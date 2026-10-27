// The loader's Mods menu: a page of the game's pause screen when opened from
// its "Mods" entry (pause_entry.cpp), an overlay when opened with F1 /
// Back+Start anywhere else.
//
// Page 1 lists the loader and every file in mods\ with its status. Page 2
// shows one mod: details, "Enabled at next start", and the settings it
// registered with HmlApi::option_add. Changes are written to the mod's ini
// at once. While the menu is open the game gets no keyboard/mouse/gamepad
// input; mods are told through HML_EVENT_MENU_OPENED/CLOSED.
#include "internal.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace hml {
namespace {

// ---- palette -----------------------------------------------------------------
constexpr unsigned kDim = 0x8C000000;
constexpr unsigned kPanel = 0xE6111317;
constexpr unsigned kRowSel = 0x4C6FC3FF;
constexpr unsigned kText = 0xFFF2F2F2;
constexpr unsigned kMuted = 0xFFA0A6AE;
constexpr unsigned kAccent = 0xFF6FC3FF;
constexpr unsigned kGood = 0xFF6FDC8C;
constexpr unsigned kWarn = 0xFFFFB347;
constexpr unsigned kBad = 0xFFFF6B6B;

// ---- state -----------------------------------------------------------------------
bool g_open = false;
bool g_classic = false;  // opened with Shift+F1: plain overlay look
DWORD g_opened_tick = 0;
// Opened from the pause menu: the menu becomes a page of the pause screen,
// printed by the game itself in place of the pause entries (pause_entry.cpp).
bool g_from_pause = false;
bool g_release_pending = false;  // keep the keyboard blocked until menu keys are up
enum class Page { List, Mod } g_page = Page::List;
int g_list_sel = 0, g_list_top = 0;
int g_row_sel = 0, g_row_top = 0;
std::string g_flash_msg;
DWORD g_flash_until = 0;

// ---- loader's own settings (page of the "Hobbit Mod Loader" entry) ----------------
int32_t g_opt_console = 0, g_opt_asi = 1, g_opt_level = 1, g_opt_hot_reload = 1;
const char* const kLevels[] = {"debug", "info", "warn", "error"};
bool g_loader_options_ready = false;

void open_path(const std::string& path) {
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void act_open_mods(HmlMod*, const HmlOption*, void*) { open_path(game_info()->mods_dir); }
void act_open_log(HmlMod*, const HmlOption*, void*) {
    open_path(path_join(game_info()->game_dir, "hml.log"));
}

void on_hot_reload(HmlMod*, const HmlOption*, void*) { mods_set_hot_reload(g_opt_hot_reload != 0); }

void register_loader_options() {
    if (g_loader_options_ready) return;
    g_loader_options_ready = true;
    HmlMod* self = loader_self();
    self->ini_path = loader_config().ini_path;
    g_opt_console = loader_config().console;
    g_opt_asi = loader_config().load_asi;
    g_opt_level = (int)loader_config().log_level;
    g_opt_hot_reload = mods_hot_reload_enabled() ? 1 : 0;
    HmlOption o{};
    o = {HML_OPTION_BOOL, "HotReload", "Reload changed mods", "Rebuilt mod files are loaded again while the game runs",
         &g_opt_hot_reload, 0, 1, 1, nullptr, 0, &on_hot_reload, nullptr};
    mods_option_add(self, &o);
    o = {HML_OPTION_LABEL, nullptr, "Applies at next start", nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr};
    mods_option_add(self, &o);
    o = {HML_OPTION_BOOL, "Console", "Console window", "Show the log live in a console window",
         &g_opt_console, 0, 1, 1, nullptr, 0, nullptr, nullptr};
    mods_option_add(self, &o);
    o = {HML_OPTION_CHOICE, "LogLevel", "Log detail", "How much goes into hml.log",
         &g_opt_level, 0, 0, 1, kLevels, 4, nullptr, nullptr};
    mods_option_add(self, &o);
    o = {HML_OPTION_BOOL, "LoadAsi", "Load plain .asi files", "Also load non-HML modules from mods",
         &g_opt_asi, 0, 1, 1, nullptr, 0, nullptr, nullptr};
    mods_option_add(self, &o);
    o = {HML_OPTION_LABEL, nullptr, "Files", nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr};
    mods_option_add(self, &o);
    o = {HML_OPTION_ACTION, nullptr, "Open mods folder", "Opens it in Explorer", nullptr, 0, 0, 0,
         nullptr, 0, &act_open_mods, nullptr};
    mods_option_add(self, &o);
    o = {HML_OPTION_ACTION, nullptr, "Open hml.log", "Opens the loader log", nullptr, 0, 0, 0,
         nullptr, 0, &act_open_log, nullptr};
    mods_option_add(self, &o);
}

// ---- input ---------------------------------------------------------------------------
struct Repeat {
    bool down = false;
    DWORD since = 0, last = 0;
    // true on press, then repeatedly while held
    bool tick(bool now) {
        DWORD t = GetTickCount();
        bool fire = false;
        if (now && !down) {
            fire = true;
            since = last = t;
        } else if (now && t - since > 380 && t - last > 70) {
            fire = true;
            last = t;
        }
        down = now;
        return fire;
    }
};
Repeat r_up, r_down, r_left, r_right;
bool p_enter = false, p_back = false, p_toggle = false;
unsigned g_pad_prev = 0;

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

struct Input {
    bool up, down, left, right, enter, back, toggle, classic;
    bool up_held, down_held;
};

Input read_input() {
    bool focus = input_game_has_focus() != 0;
    HmlGamepad pad{};
    unsigned b = input_gamepad_state(0, &pad) ? pad.buttons : 0;
    unsigned combo = loader_config().menu_pad;
    bool pad_toggle = combo && (b & combo) == combo && (g_pad_prev & combo) != combo;

    Input in{};
    auto edge = [](bool& prev, bool now) {
        bool e = now && !prev;
        prev = now;
        return e;
    };
    bool k_toggle = focus && loader_config().menu_key && key((int)loader_config().menu_key);
    in.toggle = edge(p_toggle, k_toggle) || pad_toggle;
    in.classic = in.toggle && (key(VK_SHIFT));
    {
        in.up = r_up.tick(focus && (key(VK_UP) || key('W'))) || ((b & 0x1) && !(g_pad_prev & 0x1));
        in.down = r_down.tick(focus && (key(VK_DOWN) || key('S'))) || ((b & 0x2) && !(g_pad_prev & 0x2));
        in.left = r_left.tick(focus && (key(VK_LEFT) || key('A'))) || ((b & 0x4) && !(g_pad_prev & 0x4));
        in.right = r_right.tick(focus && (key(VK_RIGHT) || key('D'))) || ((b & 0x8) && !(g_pad_prev & 0x8));
        in.enter = edge(p_enter, focus && (key(VK_RETURN) || key(VK_SPACE))) ||
                   ((b & 0x1000) && !(g_pad_prev & 0x1000));
        // left stick, for the pause-menu entry (the game navigates with it too)
        static bool stick_up = false, stick_down = false;
        bool su = pad.thumb_ly > 16000, sd = pad.thumb_ly < -16000;
        in.up = in.up || (su && !stick_up);
        in.down = in.down || (sd && !stick_down);
        stick_up = su;
        stick_down = sd;
        in.up_held = (focus && (key(VK_UP) || key('W') || key(VK_NUMPAD8))) || (b & 0x1) || su;
        in.down_held = (focus && (key(VK_DOWN) || key('S') || key(VK_NUMPAD2))) || (b & 0x2) || sd;
        in.back = edge(p_back, focus && (key(VK_ESCAPE) || key(VK_BACK))) ||
                  ((b & 0x2000) && !(g_pad_prev & 0x2000));
    }
    g_pad_prev = b;
    return in;
}

bool menu_keys_held() {
    return key(VK_ESCAPE) || key(VK_BACK) || key(VK_RETURN) || key(VK_SPACE) || key(VK_UP) ||
           key(VK_DOWN) || key(VK_LEFT) || key(VK_RIGHT) ||
           (loader_config().menu_key && key((int)loader_config().menu_key));
}

void set_open(bool open) {
    if (open == g_open) return;
    g_open = open;
    if (open) {
        register_loader_options();
        g_page = Page::List;
        g_list_sel = 0;
        g_opened_tick = GetTickCount();
        input_block(loader_self(), HML_INPUT_KEYBOARD | HML_INPUT_MOUSE | HML_INPUT_GAMEPAD);
        LOG_I("Mods menu opened%s", g_classic ? " (classic look)" : "");
        events_dispatch(HML_EVENT_MENU_OPENED, nullptr);
    } else {
        g_from_pause = false;
        pause::clear_page();
        g_release_pending = true;  // unblock once Esc/Enter/F1 are released
        LOG_I("Mods menu closed");
        events_dispatch(HML_EVENT_MENU_CLOSED, nullptr);
    }
}

// ---- model -------------------------------------------------------------------------
// The menu lists the mods first and the loader's own page last.
std::vector<HmlMod*> entries() {
    std::vector<HmlMod*> all = mods_list(), out;
    for (HmlMod* m : all)
        if (m != loader_self()) out.push_back(m);
    out.push_back(loader_self());
    return out;
}

const char* status_text(const HmlMod* m, unsigned* color) {
    if (m == loader_self()) return *color = kAccent, "loader";
    switch (m->status) {
        case ModStatus::Active: return *color = kGood, "active";
        case ModStatus::Failed: return *color = kBad, "failed";
        case ModStatus::Disabled: return *color = kMuted, "disabled";
        case ModStatus::Plain: return *color = kAccent, "plain module";
        default: return *color = kMuted, "not a mod";
    }
}

// Rows on a mod page: optional "Enabled at next start", then its options.
struct Row {
    OptionEntry* e;  // null = the enable toggle
};
// "Reload now" (hot reload) at the end of a reloadable mod's page.
void act_reload(HmlMod*, const HmlOption*, void* user) {
    HmlMod* mod = static_cast<HmlMod*>(user);
    std::string why;
    bool ok = mods_reload(mod, &why);
    g_flash_msg = ok ? mod->name + " " + mod->version + " reloaded" : "Reload failed: " + why;
    g_flash_until = GetTickCount() + 4000;
}
OptionEntry* reload_row(HmlMod* m) {
    static std::map<HmlMod*, OptionEntry*> rows;
    OptionEntry*& e = rows[m];
    if (!e) {
        e = new OptionEntry;
        e->label = "Reload now";
        e->help = "Loads the mod's file again, e.g. after rebuilding it";
        e->opt = HmlOption{HML_OPTION_ACTION, nullptr, e->label.c_str(), e->help.c_str(), nullptr, 0, 0, 1,
                           nullptr, 0, &act_reload, m};
    }
    return e;
}

std::vector<Row> rows_for(HmlMod* m) {
    std::vector<Row> rows;
    if (m != loader_self() && !m->file.empty()) rows.push_back({nullptr});
    for (OptionEntry* e : m->options) rows.push_back({e});
    bool reloadable = (m->module && m->unload) || (!m->module && m->status == ModStatus::Failed);
    if (m != loader_self() && reloadable) rows.push_back({reload_row(m)});
    return rows;
}
bool selectable(const Row& r) { return !r.e || r.e->opt.type != HML_OPTION_LABEL; }

int decimals(float step) { return step < 0.01f ? 3 : step < 0.1f ? 2 : step < 1.0f ? 1 : 0; }

std::string value_text(HmlMod* m, const Row& r, unsigned* color) {
    *color = kText;
    if (!r.e) {
        bool on = mods_enabled_next_start(m);
        *color = on ? kGood : kMuted;
        return on ? "On" : "Off";
    }
    const HmlOption& o = r.e->opt;
    char b[64];
    switch (o.type) {
        case HML_OPTION_BOOL: {
            bool on = *(int32_t*)o.value != 0;
            *color = on ? kGood : kMuted;
            return on ? "On" : "Off";
        }
        case HML_OPTION_INT: std::snprintf(b, sizeof b, "%d", (int)*(int32_t*)o.value); return b;
        case HML_OPTION_FLOAT:
            std::snprintf(b, sizeof b, "%.*f", decimals(o.step), *(float*)o.value);
            return b;
        case HML_OPTION_CHOICE: {
            int i = *(int32_t*)o.value;
            return i >= 0 && i < o.num_choices ? o.choices[i] : "?";
        }
        case HML_OPTION_ACTION: *color = kAccent; return "Enter";
        default: return "";
    }
}

void changed(HmlMod* m, OptionEntry* e) {
    mods_option_save(m, *e);
    if (e->opt.on_change) e->opt.on_change(m, &e->opt, e->opt.user);
}

// dir: -1 / +1 for left/right, 0 for Enter
void apply(HmlMod* m, const Row& r, int dir) {
    if (!r.e) {
        bool on = mods_enabled_next_start(m);
        if (mods_set_enabled_next_start(m, !on)) {
            g_flash_msg = std::string(m->name) + (on ? " will be off" : " will be on") +
                          " after the game restarts";
            g_flash_until = GetTickCount() + 3500;
        }
        return;
    }
    HmlOption& o = r.e->opt;
    switch (o.type) {
        case HML_OPTION_BOOL: *(int32_t*)o.value = !*(int32_t*)o.value; break;
        case HML_OPTION_INT: {
            if (!dir) return;
            int v = *(int32_t*)o.value + dir * (int)std::max(1.0f, o.step);
            *(int32_t*)o.value = (int32_t)std::min(std::max((float)v, o.min), o.max);
            break;
        }
        case HML_OPTION_FLOAT: {
            if (!dir) return;
            float v = *(float*)o.value + dir * o.step;
            v = std::round(v / o.step) * o.step;  // stay on the step grid
            *(float*)o.value = std::min(std::max(v, o.min), o.max);
            break;
        }
        case HML_OPTION_CHOICE: {
            int i = *(int32_t*)o.value + (dir ? dir : 1);
            *(int32_t*)o.value = (i % o.num_choices + o.num_choices) % o.num_choices;
            break;
        }
        case HML_OPTION_ACTION:
            if (dir) return;
            if (o.on_change) o.on_change(m, &o, o.user);
            return;
        default: return;
    }
    changed(m, r.e);
}

// ---- drawing ---------------------------------------------------------------------------
void keycap(int x, int y, const char* k, int* out_w) {
    int u = ui::unit(), w, h;
    ui::measure(ui::Font::Small, k, &w, &h);
    ui::rect(x, y, w + u, h + u / 3, 0x40FFFFFF);
    ui::text(ui::Font::Small, x + u / 2, y + u / 6, k, 0xFFFFFFFF);
    *out_w = w + u;
}

void footer(int x, int y, const char* const* items, int n) {
    int u = ui::unit(), cx = x;
    for (int i = 0; i < n; ++i) {
        int kw, lw;
        keycap(cx, y, items[2 * i], &kw);
        ui::measure(ui::Font::Small, items[2 * i + 1], &lw, nullptr);
        ui::text(ui::Font::Small, cx + kw + u / 3, y + u / 6, items[2 * i + 1], kMuted);
        cx += kw + u / 3 + lw + u * 3 / 2;
    }
}

void scroll_into_view(int sel, int* top, int visible) {
    if (sel < *top) *top = sel;
    if (sel >= *top + visible) *top = sel - visible + 1;
    if (*top < 0) *top = 0;
}

// ---- game-style drawing (game font, laid out like the game's pause menu) -----------
constexpr unsigned kGold = 0xFFF8B800;     // unselected item, as in the pause menu
constexpr unsigned kGoldDim = 0xFFA07A10;  // inactive item
constexpr unsigned kWhite = 0xFFFFFFFF;    // selected item
constexpr unsigned kSoft = 0xFFD8D8D8;     // secondary text
const std::string kLoaderLabel = "Loader settings";

void centered(ui::Font f, int cx, int y, const std::string& s, unsigned c) {
    int w;
    ui::measure(f, s.c_str(), &w, nullptr);
    ui::text(f, cx - w / 2, y, s.c_str(), c);
}

void footer_native(int cx, int y, const char* const* items, int n) {
    std::string line;
    for (int i = 0; i < n; ++i) {
        if (i) line += "     ";
        line += std::string(items[2 * i]) + "  " + items[2 * i + 1];
    }
    centered(ui::Font::Small, cx, y, line, kSoft);
}

void draw_native() {
    const int W = ui::w(), H = ui::h(), cx = W / 2;
    ui::rect(0, 0, W, H, 0xA0000000);
    int th, bh, sh;
    ui::measure(ui::Font::Title, "Ag", nullptr, &th);
    ui::measure(ui::Font::Body, "Ag", nullptr, &bh);
    ui::measure(ui::Font::Small, "Ag", nullptr, &sh);
    const int top = H * 12 / 100, footer_y = H - H * 6 / 100 - sh;
    int y = top;
    std::vector<HmlMod*> mods = entries();
    if (g_list_sel >= (int)mods.size()) g_list_sel = (int)mods.size() - 1;

    if (g_page == Page::List) {
        centered(ui::Font::Title, cx, y, "Mods", kWhite);
        y += th;
        centered(ui::Font::Small, cx, y, std::string("Hobbit Mod Loader ") + HML_LOADER_VERSION, kSoft);
        y += sh + bh / 2;
        int info_y = footer_y - sh * 3;
        int visible = std::max(1, (info_y - bh / 2 - y) / bh);
        scroll_into_view(g_list_sel, &g_list_top, visible);
        for (int i = g_list_top; i < (int)mods.size() && i < g_list_top + visible; ++i) {
            HmlMod* m = mods[i];
            bool live = m == loader_self() || m->status == ModStatus::Active || m->status == ModStatus::Plain;
            const std::string& label = m == loader_self() ? kLoaderLabel : m->name;
            centered(ui::Font::Body, cx, y, label, i == g_list_sel ? kWhite : live ? kGold : kGoldDim);
            y += bh;
        }
        HmlMod* m = mods[g_list_sel];
        unsigned sc;
        std::string info = m->version.empty() ? "" : m->version;
        std::string st = status_text(m, &sc);
        info += (info.empty() ? "" : "  -  ") + st;
        if (!m->author.empty()) info += "  -  by " + m->author;
        if (!m->options.empty())
            info += "  -  " + std::to_string(m->options.size()) + (m->options.size() == 1 ? " setting" : " settings");
        if (!mods_enabled_next_start(m) && m->status == ModStatus::Active) info += "  -  off after restart";
        centered(ui::Font::Small, cx, info_y, info, kSoft);
        const std::string& d = !m->note.empty() ? m->note : m->description;
        if (!d.empty()) centered(ui::Font::Small, cx, info_y + sh, d, m->note.empty() ? kSoft : kGold);
        static const char* const keys[] = {"Up/Down", "Select", "Enter", "Open", "Esc", "Back"};
        footer_native(cx, footer_y, keys, 3);
    } else {
        HmlMod* m = mods[g_list_sel];
        std::vector<Row> rows = rows_for(m);
        centered(ui::Font::Title, cx, y, m->name, kWhite);
        y += th;
        std::string info = m->version;
        if (!m->author.empty()) info += (info.empty() ? "by " : "  -  by ") + m->author;
        if (!info.empty()) centered(ui::Font::Small, cx, y, info, kSoft);
        y += sh;
        if (!m->description.empty()) {
            centered(ui::Font::Small, cx, y, m->description, kSoft);
            y += sh;
        }
        if (!m->note.empty()) {
            centered(ui::Font::Small, cx, y, m->note, kGold);
            y += sh;
        }
        y += bh / 2;
        if (!rows.empty()) {
            g_row_sel = std::min(std::max(g_row_sel, 0), (int)rows.size() - 1);
            int guard = 0;
            while (!selectable(rows[g_row_sel]) && guard++ < (int)rows.size())
                g_row_sel = (g_row_sel + 1) % (int)rows.size();
        }
        int help_y = footer_y - sh * 2;
        int visible = std::max(1, (help_y - bh / 2 - y) / bh);
        scroll_into_view(g_row_sel, &g_row_top, visible);
        if (rows.empty()) centered(ui::Font::Body, cx, y, "No settings", kGoldDim);
        const int gap = ui::unit() * 2;
        for (int i = g_row_top; i < (int)rows.size() && i < g_row_top + visible; ++i) {
            const Row& r = rows[i];
            if (r.e && r.e->opt.type == HML_OPTION_LABEL) {
                centered(ui::Font::Small, cx, y + bh - sh - sh / 4, r.e->opt.label, kSoft);
                y += bh;
                continue;
            }
            bool sel = i == g_row_sel;
            unsigned c = sel ? kWhite : kGold;
            const char* label = r.e ? r.e->opt.label : "Enabled at next start";
            unsigned vc;
            std::string v = value_text(m, r, &vc);
            if (r.e && r.e->opt.type == HML_OPTION_ACTION) {
                centered(ui::Font::Body, cx, y, label, c);
            } else {
                bool arrows = r.e && (r.e->opt.type == HML_OPTION_INT || r.e->opt.type == HML_OPTION_FLOAT ||
                                      r.e->opt.type == HML_OPTION_CHOICE);
                if (sel && arrows) v = "< " + v + " >";
                int lw;
                ui::measure(ui::Font::Body, label, &lw, nullptr);
                ui::text(ui::Font::Body, cx - gap / 2 - lw, y, label, c);
                ui::text(ui::Font::Body, cx + gap / 2, y, v.c_str(), c);
            }
            y += bh;
        }
        if (!rows.empty()) {
            const Row& r = rows[g_row_sel];
            const char* help = r.e ? r.e->opt.help : "Takes effect the next time the game starts";
            if (help) centered(ui::Font::Small, cx, help_y, help, kSoft);
        }
        static const char* const keys[] = {"Up/Down", "Select", "Left/Right", "Change", "Enter", "Toggle",
                                           "Esc", "Back"};
        footer_native(cx, footer_y, keys, 4);
    }
    if (GetTickCount() < g_flash_until) centered(ui::Font::Small, cx, footer_y - sh * 3, g_flash_msg, kGold);
}


void scroll_rows(std::vector<pause::PageRow>& rows, int sel, int* top, int max_rows) {
    if ((int)rows.size() <= max_rows) return;
    scroll_into_view(sel, top, max_rows);
    if (*top + max_rows > (int)rows.size()) *top = (int)rows.size() - max_rows;
    rows = std::vector<pause::PageRow>(rows.begin() + *top, rows.begin() + *top + max_rows);
}

void build_page() {
    pause::Page page;
    std::vector<HmlMod*> mods = entries();
    if (g_list_sel >= (int)mods.size()) g_list_sel = (int)mods.size() - 1;
    const int kMaxRows = 12;
    if (g_page == Page::List) {
        for (int i = 0; i < (int)mods.size(); ++i) {
            HmlMod* m = mods[i];
            bool live = m == loader_self() || m->status == ModStatus::Active || m->status == ModStatus::Plain;
            auto role = i == g_list_sel ? pause::PageRow::Selected
                        : live         ? pause::PageRow::Normal
                                       : pause::PageRow::Inactive;
            page.rows.push_back({role, m == loader_self() ? kLoaderLabel : m->name});
        }
        scroll_rows(page.rows, g_list_sel, &g_list_top, kMaxRows);
        HmlMod* m = mods[g_list_sel];
        page.help = !m->note.empty() ? m->note : m->description;
    } else {
        HmlMod* m = mods[g_list_sel];
        std::vector<Row> rows = rows_for(m);
        if (!rows.empty()) {
            g_row_sel = std::min(std::max(g_row_sel, 0), (int)rows.size() - 1);
            int guard = 0;
            while (!selectable(rows[g_row_sel]) && guard++ < (int)rows.size())
                g_row_sel = (g_row_sel + 1) % (int)rows.size();
        }
        for (int i = 0; i < (int)rows.size(); ++i) {
            const Row& r = rows[i];
            if (r.e && r.e->opt.type == HML_OPTION_LABEL) {
                page.rows.push_back({pause::PageRow::Header, r.e->opt.label});
                continue;
            }
            unsigned vc;
            std::string text = r.e ? r.e->opt.label : "Enabled at next start";
            if (!r.e || r.e->opt.type != HML_OPTION_ACTION) text += ": " + value_text(m, r, &vc);
            page.rows.push_back({i == g_row_sel ? pause::PageRow::Selected : pause::PageRow::Normal, text});
        }
        if (rows.empty()) page.rows.push_back({pause::PageRow::Inactive, "No settings"});
        scroll_rows(page.rows, g_row_sel, &g_row_top, kMaxRows);
        if (!rows.empty()) {
            const Row& r = rows[g_row_sel];
            const char* help = r.e ? r.e->opt.help : "Takes effect the next time the game starts";
            page.help = help ? help : "";
            int t = r.e ? r.e->opt.type : HML_OPTION_BOOL;
            page.adjust = t == HML_OPTION_BOOL || t == HML_OPTION_INT || t == HML_OPTION_FLOAT ||
                          t == HML_OPTION_CHOICE;
        }
    }
    if (GetTickCount() < g_flash_until) page.help = g_flash_msg;
    pause::set_page(page);
}

void draw(void* dev) {
    if (g_from_pause && !g_classic && pause::visible()) {
        build_page();
        return;
    }
    pause::clear_page();
    if (!ui::begin(dev)) return;
    if (ui::native_now()) {
        draw_native();
        ui::end();
        return;
    }
    int u = ui::unit();
    ui::rect(0, 0, ui::w(), ui::h(), kDim);

    int pw = std::min(ui::w() - 4 * u, u * 52), ph = std::min(ui::h() - 4 * u, u * 40);
    int px = (ui::w() - pw) / 2, py = (ui::h() - ph) / 2, pad = u * 3 / 2;
    ui::rect(px, py, pw, ph, kPanel);
    ui::rect(px, py, pw, 3, kAccent);

    int th, bh, sh;
    ui::measure(ui::Font::Title, "Ag", nullptr, &th);
    ui::measure(ui::Font::Body, "Ag", nullptr, &bh);
    ui::measure(ui::Font::Small, "Ag", nullptr, &sh);
    int x = px + pad, y = py + pad, right = px + pw - pad;
    int row_h = bh + u * 2 / 3;
    int footer_y = py + ph - pad - sh - u / 3;
    std::vector<HmlMod*> mods = entries();
    if (g_list_sel >= (int)mods.size()) g_list_sel = (int)mods.size() - 1;

    if (g_page == Page::List) {
        ui::text(ui::Font::Title, x, y, "MODS", kAccent);
        char ver[80];
        std::snprintf(ver, sizeof ver, "Hobbit Mod Loader %s", HML_LOADER_VERSION);
        int vw;
        ui::measure(ui::Font::Small, ver, &vw, nullptr);
        ui::text(ui::Font::Small, right - vw, y + (th - sh) / 2, ver, kMuted);
        y += th + u;

        int visible = std::max(1, (footer_y - u - y) / (row_h + sh));
        scroll_into_view(g_list_sel, &g_list_top, visible);
        for (int i = g_list_top; i < (int)mods.size() && i < g_list_top + visible; ++i) {
            HmlMod* m = mods[i];
            int rh = row_h + sh;
            if (i == g_list_sel) ui::rect(px + u / 2, y - u / 3, pw - u, rh, kRowSel);
            std::string title = m->name + (m->version.empty() ? "" : "  " + m->version);
            ui::text(ui::Font::Body, x, y, title.c_str(), kText);
            unsigned sc;
            const char* st = status_text(m, &sc);
            int sw;
            ui::measure(ui::Font::Small, st, &sw, nullptr);
            ui::text(ui::Font::Small, right - sw, y + (bh - sh) / 2, st, sc);
            std::string sub;
            if (!m->author.empty()) sub = "by " + m->author;
            if (!m->options.empty())
                sub += (sub.empty() ? "" : "  \xB7  ") + std::to_string(m->options.size()) +
                       (m->options.size() == 1 ? " setting" : " settings");
            if (!m->note.empty()) sub += (sub.empty() ? "" : "  \xB7  ") + m->note;
            if (!mods_enabled_next_start(m) && m->status == ModStatus::Active)
                sub += "  \xB7  off after restart";
            ui::text(ui::Font::Small, x, y + bh, sub.c_str(), kMuted);
            y += rh + u / 3;
        }
        static const char* const keys[] = {"Up / Down", "Select", "Enter", "Open", "Esc", "Close"};
        footer(x, footer_y, keys, 3);
    } else {
        HmlMod* m = mods[g_list_sel];
        std::vector<Row> rows = rows_for(m);
        ui::text(ui::Font::Title, x, y, m->name.c_str(), kAccent);
        int nw;
        ui::measure(ui::Font::Title, m->name.c_str(), &nw, nullptr);
        if (!m->version.empty())
            ui::text(ui::Font::Body, x + nw + u, y + (th - bh) / 2, m->version.c_str(), kMuted);
        unsigned sc;
        const char* st = status_text(m, &sc);
        int sw;
        ui::measure(ui::Font::Small, st, &sw, nullptr);
        ui::text(ui::Font::Small, right - sw, y + (th - sh) / 2, st, sc);
        y += th + u / 3;
        std::string info;
        if (!m->author.empty()) info = "by " + m->author;
        if (!m->file.empty()) {
            size_t slash = m->file.find_last_of('\\');
            info += (info.empty() ? "" : "  \xB7  ") + m->file.substr(slash + 1);
        }
        ui::text(ui::Font::Small, x, y, info.c_str(), kMuted);
        y += sh + u / 4;
        if (!m->description.empty()) {
            ui::text(ui::Font::Body, x, y, m->description.c_str(), kText);
            y += bh + u / 4;
        }
        if (!m->note.empty()) {
            ui::text(ui::Font::Small, x, y, m->note.c_str(), sc);
            y += sh + u / 4;
        }
        y += u / 2;
        ui::rect(x, y, right - x, 1, 0x30FFFFFF);
        y += u;

        // keep the selection on a selectable row
        if (!rows.empty()) {
            g_row_sel = std::min(std::max(g_row_sel, 0), (int)rows.size() - 1);
            int guard = 0;
            while (!selectable(rows[g_row_sel]) && guard++ < (int)rows.size())
                g_row_sel = (g_row_sel + 1) % (int)rows.size();
        }
        int help_y = footer_y - u - sh;
        int visible = std::max(1, (help_y - u / 2 - y) / row_h);
        scroll_into_view(g_row_sel, &g_row_top, visible);
        if (rows.empty())
            ui::text(ui::Font::Body, x, y, "This mod has no settings.", kMuted);
        for (int i = g_row_top; i < (int)rows.size() && i < g_row_top + visible; ++i) {
            const Row& r = rows[i];
            if (r.e && r.e->opt.type == HML_OPTION_LABEL) {
                ui::text(ui::Font::Small, x, y + (bh - sh), r.e->opt.label, kAccent);
                y += row_h;
                continue;
            }
            bool sel = i == g_row_sel;
            if (sel) ui::rect(px + u / 2, y - u / 4, pw - u, row_h, kRowSel);
            const char* label = r.e ? r.e->opt.label : "Enabled at next start";
            ui::text(ui::Font::Body, x + u / 2, y, label, kText);
            unsigned vc;
            std::string v = value_text(m, r, &vc);
            bool arrows = r.e && (r.e->opt.type == HML_OPTION_INT || r.e->opt.type == HML_OPTION_FLOAT ||
                                  r.e->opt.type == HML_OPTION_CHOICE);
            if (sel && arrows) v = "\x8B  " + v + "  \x9B";
            int vw;
            ui::measure(ui::Font::Body, v.c_str(), &vw, nullptr);
            ui::text(ui::Font::Body, right - vw, y, v.c_str(), vc);
            y += row_h;
        }
        // help for the selected row
        if (!rows.empty()) {
            const Row& r = rows[g_row_sel];
            const char* help = r.e ? r.e->opt.help : "Takes effect the next time the game starts";
            if (help) ui::text(ui::Font::Small, x, help_y, help, kMuted);
        }
        static const char* const keys[] = {"Up / Down", "Select", "Left / Right", "Change",
                                           "Enter", "Toggle", "Esc", "Back"};
        footer(x, footer_y, keys, 4);
    }

    if (GetTickCount() < g_flash_until) {
        int w;
        ui::measure(ui::Font::Small, g_flash_msg.c_str(), &w, nullptr);
        ui::rect(px + (pw - w) / 2 - u, py + ph + u / 2, w + 2 * u, sh + u / 2, kPanel);
        ui::text(ui::Font::Small, px + (pw - w) / 2, py + ph + u / 2 + u / 4, g_flash_msg.c_str(), kWarn);
    }
    ui::end();
}

}  // namespace

bool menu_open() { return g_open; }

void menu_frame(void* dev) {
    Input in = read_input();
    if (g_release_pending && !menu_keys_held()) {
        g_release_pending = false;
        if (!g_open) input_block(loader_self(), 0);
    }
    if (in.toggle) {
        if (!g_open) g_classic = in.classic;
        ui::set_classic(g_classic);
        set_open(!g_open);
        in = Input{};
    }
    if (!g_open) {
        // "Mods" entry in the game's pause menu (pause_entry.cpp)
        if (pause::input(in.up, in.down, in.enter, in.up_held, in.down_held)) {
            g_classic = false;
            ui::set_classic(false);
            set_open(true);
            g_from_pause = true;
            LOG_I("Mods menu opened from the pause menu");
        }
        return;
    }
    // A press that is still the one that opened the menu does nothing.
    if (GetTickCount() - g_opened_tick < 250) in.enter = false;

    std::vector<HmlMod*> mods = entries();
    if (g_page == Page::List) {
        int n = (int)mods.size();
        if (in.up) g_list_sel = (g_list_sel + n - 1) % n;
        if (in.down) g_list_sel = (g_list_sel + 1) % n;
        if (in.up || in.down) pause::play_sound(0);
        if (in.enter || in.right) {
            g_page = Page::Mod;
            g_row_sel = 0;
            g_row_top = 0;
            pause::play_sound(1);
        }
        if (in.back) {
            set_open(false);
            pause::play_sound(2);
        }
    } else {
        HmlMod* m = mods[std::min(g_list_sel, (int)mods.size() - 1)];
        std::vector<Row> rows = rows_for(m);
        int n = (int)rows.size();
        auto step = [&](int d) {
            for (int k = 0; k < n; ++k) {
                g_row_sel = (g_row_sel + d + n) % n;
                if (selectable(rows[g_row_sel])) break;
            }
        };
        if (n) {
            if (in.up) step(-1);
            if (in.down) step(+1);
            if (in.up || in.down || in.left || in.right || in.enter) pause::play_sound(in.enter ? 1 : 0);
            g_row_sel = std::min(std::max(g_row_sel, 0), n - 1);
            const Row& r = rows[g_row_sel];
            if (selectable(r)) {
                if (in.left) apply(m, r, -1);
                if (in.right) apply(m, r, +1);
                if (in.enter) apply(m, r, 0);
            }
        }
        if (in.back) {
            g_page = Page::List;
            pause::play_sound(2);
        }
    }
    if (g_open) draw(dev);
}

}  // namespace hml
