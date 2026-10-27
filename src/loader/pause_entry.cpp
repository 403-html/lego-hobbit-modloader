// "Mods" as the last entry of the game's pause menu.
//
// The in-game pause menu is not a GUI2 menu the loader can add children to:
// its entries are printed one by one through the game's GUI text function
// (sub_3E0580) every frame, gold, with the selected one in a pulsing white.
// So the loader:
//   - recognises the pause menu in those prints (a centred column starting
//     with the "Resume" entry, fixed spacing),
//   - prints one more entry, "Mods", right after the last one, through the
//     same function with the same size and colours, so it is drawn exactly
//     like the game's own entries,
//   - moves the selection onto it when Down is pressed on the last entry
//     (the game never sees that Down), shows the game's last entry as
//     unselected meanwhile, and opens the Mods menu on Enter / A.
#include "internal.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace hml {
namespace pause {
namespace {

constexpr uint32_t RVA_GUI_TEXT = 0x3E0580;

using GuiText_t = float(__cdecl*)(const char*, float, float, float, float, float, uint32_t, int, int);
GuiText_t o_gui_text = nullptr;

constexpr uint32_t RVA_GUI2_MANAGER = 0x1799310;  // GUI2Manager*
constexpr uint32_t RVA_PLAY_SOUND = 0x3D6E20;     // GUI2Manager::PlaySound(int): 0 move, 1 select, 2 back

bool g_enabled = true;
// The pause menu is recognised by its first entry, "Resume" in the player's
// language: the game's own text for the RESUME text id, unless hml.ini sets
// PauseMenuFirst.
std::string g_first_override;
constexpr uint32_t RVA_TEXTS = 0x132FBA4;  // text table: +4 const char*[id], +0xC count
constexpr int kTextResume = 0x21;          // RESUME (ids: table at RVA 0x132FFE0)

const char* game_text(int id) {
    uint8_t* t = static_cast<uint8_t*>(game_rva(RVA_TEXTS));
    const char* const* strings = *reinterpret_cast<const char* const**>(t + 4);
    int count = *reinterpret_cast<int*>(t + 0xC);
    if (!strings || id < 0 || id >= count) return nullptr;
    return strings[id];
}

bool is_first_entry(const char* t) {
    if (!g_first_override.empty()) return g_first_override == t;
    const char* resume = game_text(kTextResume);
    static const char* logged = nullptr;
    if (resume && resume != logged) {
        logged = resume;
        LOG_I("pause menu: recognised by its first entry, \"%s\" (the game's RESUME text)", resume);
    }
    return resume && std::strcmp(resume, t) == 0;
}
int g_at = 2;                    // our entry's position (0 = top; hml.ini PauseMenuPosition)

struct Item {
    std::string text;
    float y;          // as the game printed it
    uint32_t colour;  // as the game asked for it
};
struct Batch {
    std::vector<Item> items;
    float step = 0;
    int selected = -1;  // the game's selected entry
    uint32_t selected_colour = 0, gold = 0x00005F7F;
};
Batch g_cur;   // entries printed since the last "Resume" (game thread)
Batch g_last;  // the previous complete pass over the menu (game thread)

// Shared with the input hooks and Present.
volatile LONG g_visible_tick = 0;  // GetTickCount when the pause menu was last printed
volatile LONG g_game_sel = -1;     // the game's selected entry
volatile LONG g_mods_sel = 0;      // our entry is selected
volatile LONG g_anchor = -1;       // the game's selection while ours is shown (g_at - 1 or g_at)
volatile LONG g_sticky = 0;        // SUPPRESS_* kept until that input is released
volatile LONG g_sound = -1;        // sound to play on the game thread
volatile LONG g_pass = 0;          // SUPPRESS_* bits let through until released (the game's own step)
volatile LONG g_in_print = 0;      // re-entrancy guard

// The Mods page (set from Present, drawn on the game thread).
CRITICAL_SECTION g_page_lock;
Page g_page;
volatile LONG g_page_set_tick = 0;  // when Present last handed a page over
volatile LONG g_page_drawn_tick = 0;

bool close_to(float a, float b, float eps) { return std::fabs(a - b) < eps; }

// Selected entries pulse between grey and white (R = G = B); others are gold.
bool is_selected_colour(uint32_t c) {
    uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    return r == g && g == b;
}

// The menu is printed top to bottom every frame, so the pass that started
// with the previous "Resume" is complete when the next one starts.
void finish_batch() {
    if (g_cur.items.size() >= 2 && g_at <= (int)g_cur.items.size()) {
        Batch& b = g_cur;
        for (size_t i = 0; i < b.items.size(); ++i) {
            if (is_selected_colour(b.items[i].colour)) {
                b.selected = (int)i;
                b.selected_colour = b.items[i].colour;
            } else {
                b.gold = b.items[i].colour;
            }
        }
        g_last = b;
        InterlockedExchange(&g_game_sel, b.selected);
    }
    g_cur = Batch{};
}

void play_sound_now(int index) {
    void* mgr = *static_cast<void**>(game_rva(RVA_GUI2_MANAGER));
    if (!mgr || !*reinterpret_cast<void**>((uint8_t*)mgr + 0xE0)) return;
    using Play_t = void(__fastcall*)(void*, void*, int);
    reinterpret_cast<Play_t>(game_rva(RVA_PLAY_SOUND))(mgr, nullptr, index);
}

void print_mods(float y, float z, float sy, float sx, int flags) {
    uint32_t c = g_mods_sel ? (g_last.selected_colour ? g_last.selected_colour : 0x007F7F7F) : g_last.gold;
    InterlockedExchange(&g_in_print, 1);
    float w = o_gui_text("Mods", 0.0f, y - 1000.0f, z, sy, sx, c, 0, flags);  // measure off screen
    o_gui_text("Mods", -w / 2, y, z, sy, sx, c, 0, flags);
    InterlockedExchange(&g_in_print, 0);
}

bool page_active() {
    LONG t = g_page_set_tick;
    return t && GetTickCount() - (DWORD)t < 250;
}

float print_off_screen(const char* t, float z, float sy, float sx, uint32_t c, int btn, int flags) {
    return o_gui_text(t, 0.0f, -1000.0f, z, sy, sx, c, btn, flags);
}

float print_centred(const std::string& t, float y, float z, float s, uint32_t c, int flags) {
    float w = print_off_screen(t.c_str(), z, s, s, c, 0, flags);
    o_gui_text(t.c_str(), -w / 2, y, z, s, s, c, 0, flags);
    return w;
}

// Button prompts in the bottom-right corner, as on the game's Options page
// ("Adjust <icon>/<icon>"); icons are characters of the button font.
struct Seg {
    const char* text;
    int btn;
};
void print_prompts(float right, float y, const std::vector<Seg>& segs, int flags) {
    const float s = 0.35f;
    std::vector<float> w(segs.size());
    float total = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        uint32_t c = segs[i].btn ? 0x00808080 : 0x007F7F7F;
        w[i] = o_gui_text(segs[i].text, 0.0f, -1000.0f, 0.0f, s, s, c, segs[i].btn, flags);
        total += w[i];
    }
    float x = right - total;
    for (size_t i = 0; i < segs.size(); ++i) {
        uint32_t c = segs[i].btn ? 0x00808080 : 0x007F7F7F;
        o_gui_text(segs[i].text, x, y, 0.0f, s, s, c, segs[i].btn, flags);
        x += w[i];
    }
}

// Our page, laid out like the game's Options page: entries centred, one
// step apart, gold, the selected one pulsing white; help below; prompts in
// the corner.
void draw_page(float z, float s, int flags) {
    Page page;
    EnterCriticalSection(&g_page_lock);
    page = g_page;
    LeaveCriticalSection(&g_page_lock);
    float step = g_last.step > 0 ? g_last.step : 0.1067f;
    int n = (int)page.rows.size();
    float top = -0.027f + (n - 1) * step / 2;  // centred where the pause entries are
    if (top > 0.763f) top = 0.763f;            // the Options page's first line
    uint32_t gold = g_last.gold, sel = g_last.selected_colour ? g_last.selected_colour : 0x007F7F7F;
    InterlockedExchange(&g_in_print, 1);
    float y = top;
    for (const PageRow& r : page.rows) {
        uint32_t c = r.role == PageRow::Selected ? sel
                   : r.role == PageRow::Header   ? 0x00505050
                   : r.role == PageRow::Inactive ? 0x00003050
                                                 : gold;
        if (!r.text.empty()) print_centred(r.text, y, z, s, c, flags);
        y -= step;
    }
    if (!page.help.empty()) {
        float hy = y - step * 0.3f;
        if (hy > -0.666f) hy = -0.666f;
        print_centred(page.help, hy, z, s, 0x7C00007F, flags);
    }
    // Button prompts (button-font characters, as the game's own prompts use).
    // U+0531 select, U+0532 back, U+053B / U+053A left / right
    if (page.adjust)
        print_prompts(0.916f, -0.778f,
                      {{"Adjust ", 0}, {"\xD4\xBB", 1}, {"/", 0}, {"\xD4\xBA", 1}, {"    Back ", 0}, {"\xD4\xB2", 1}},
                      flags);
    else
        print_prompts(0.916f, -0.778f, {{"Select ", 0}, {"\xD4\xB1", 1}, {"    Back ", 0}, {"\xD4\xB2", 1}},
                      flags);
    InterlockedExchange(&g_in_print, 0);
    InterlockedExchange(&g_page_drawn_tick, (LONG)GetTickCount());
}

float __cdecl hk_gui_text(const char* t, float x, float y, float z, float sy, float sx, uint32_t col,
                          int btn, int flags) {
    if (g_in_print || !g_enabled || !t) return o_gui_text(t, x, y, z, sy, sx, col, btn, flags);

    // Pause-menu entries: full-size GUI text, not a button icon, one step apart.
    bool style = !btn && close_to(z, 1.0f, 0.01f) && close_to(sx, 0.5f, 0.01f) && close_to(sy, 0.5f, 0.01f);
    int index = -1;
    if (style && is_first_entry(t)) {
        finish_batch();
        index = 0;
        InterlockedExchange(&g_visible_tick, (LONG)GetTickCount());
    } else if (style && !g_cur.items.empty()) {
        float gap = g_cur.items.back().y - y;
        float step = g_cur.items.size() >= 2 ? g_cur.step : gap;
        if (step > 0.03f && step < 0.3f && close_to(gap, step, 0.01f)) {
            g_cur.step = step;
            index = (int)g_cur.items.size();
        }
    }
    if (index < 0) return o_gui_text(t, x, y, z, sy, sx, col, btn, flags);
    g_cur.items.push_back({t, y, col});

    // Our page replaces the pause entries, as the game's Options page does.
    if (page_active()) {
        if (index == 0) draw_page(z, sx, flags);
        return print_off_screen(t, z, sy, sx, col, btn, flags);
    }

    bool ready = g_last.step > 0 && g_at <= (int)g_last.items.size();
    float step = ready ? g_last.step : 0.0f;
    uint32_t c = col;
    if (ready && g_mods_sel && index == g_anchor) c = g_last.gold;  // the selection is on "Mods"
    float shift = ready && index >= g_at ? step : 0.0f;
    if (ready && index == g_at) print_mods(y, z, sy, sx, flags);  // goes where this entry was
    float w = o_gui_text(t, x, y - shift, z, sy, sx, c, btn, flags);
    if (ready && index == g_at - 1 && g_at == (int)g_last.items.size())  // "Mods" last
        print_mods(y - step, z, sy, sx, flags);
    return w;
}

}  // namespace

void install() {
    InitializeCriticalSection(&g_page_lock);
    if (game_info()->build != HML_BUILD_STEAM_DX9) return;
    const char* ini = loader_config().ini_path.c_str();
    g_enabled = GetPrivateProfileIntA("Loader", "PauseMenuEntry", 1, ini) != 0;
    char buf[128];
    GetPrivateProfileStringA("Loader", "PauseMenuFirst", "", buf, sizeof buf, ini);
    g_first_override = buf;
    g_at = GetPrivateProfileIntA("Loader", "PauseMenuPosition", 2, ini);
    if (g_at < 1) g_at = 1;
    if (!g_enabled) return;
    if (hooks_function(loader_self(), game_rva(RVA_GUI_TEXT), (void*)&hk_gui_text, (void**)&o_gui_text) ==
        HML_OK)
        LOG_I("pause menu: \"Mods\" entry at position %d%s", g_at + 1,
              g_first_override.empty() ? "" : (" of the menu starting with \"" + g_first_override + "\"").c_str());
}

bool visible() {
    LONG t = g_visible_tick;
    return t && GetTickCount() - (DWORD)t < 200;
}
bool mods_selected() { return visible() && g_mods_sel; }

// Up / down / accept presses and what is held, from Present. Returns true
// when "Mods" was picked.
bool input(bool up, bool down, bool accept, bool up_held, bool down_held) {
    LONG sticky = g_sticky, pass = g_pass;
    if (!up_held) sticky &= ~SUPPRESS_UP, pass &= ~SUPPRESS_UP;
    if (!down_held) sticky &= ~SUPPRESS_DOWN, pass &= ~SUPPRESS_DOWN;
    InterlockedExchange(&g_sticky, sticky);
    InterlockedExchange(&g_pass, pass);
    if (!visible()) {
        InterlockedExchange(&g_mods_sel, 0);
        return false;
    }
    LONG sel = g_game_sel;
    if (!g_mods_sel) {
        if ((down && sel == g_at - 1) || (up && sel == g_at)) {
            InterlockedExchange(&g_anchor, sel);
            InterlockedExchange(&g_mods_sel, 1);
            InterlockedOr(&g_sticky, down ? SUPPRESS_DOWN : SUPPRESS_UP);
            InterlockedExchange(&g_sound, 0);
        }
        return false;
    }
    if (accept) {
        InterlockedExchange(&g_sound, 1);
        return true;
    }
    if (down || up) {
        // Moving on past the anchor is the game's own step (it moves and
        // plays its sound); moving back to the anchor is ours.
        bool back_to_anchor = down ? g_anchor == g_at : g_anchor == g_at - 1;
        if (back_to_anchor) {
            InterlockedOr(&g_sticky, down ? SUPPRESS_DOWN : SUPPRESS_UP);
            InterlockedExchange(&g_sound, 0);
        } else {
            // The game may not have taken this press yet; once our entry is
            // off, its selection (the anchor) would make us swallow it.
            InterlockedOr(&g_pass, down ? SUPPRESS_DOWN : SUPPRESS_UP);
        }
        InterlockedExchange(&g_mods_sel, 0);
    }
    return false;
}

// What the game must not see right now.
uint32_t suppress() {
    uint32_t s = (uint32_t)g_sticky;
    if (!visible()) return s & ~(uint32_t)g_pass;
    LONG sel = g_game_sel;
    if (!g_mods_sel) {
        if (sel == g_at - 1) s |= SUPPRESS_DOWN;
        if (sel == g_at) s |= SUPPRESS_UP;
    } else {
        s |= SUPPRESS_ACCEPT;
        if (g_anchor == g_at) s |= SUPPRESS_DOWN;
        if (g_anchor == g_at - 1) s |= SUPPRESS_UP;
    }
    return s & ~(uint32_t)g_pass;
}

void play_sound(int index) { InterlockedExchange(&g_sound, index); }

void set_page(const Page& page) {
    EnterCriticalSection(&g_page_lock);
    g_page = page;
    LeaveCriticalSection(&g_page_lock);
    InterlockedExchange(&g_page_set_tick, (LONG)GetTickCount());
}
void clear_page() { InterlockedExchange(&g_page_set_tick, 0); }
bool page_shown() {
    LONG t = g_page_drawn_tick;
    return page_active() && t && GetTickCount() - (DWORD)t < 250;
}

// Game thread, before the game's GUI is drawn.
void game_thread_tick() {
    LONG snd = InterlockedExchange(&g_sound, -1);
    if (snd >= 0) play_sound_now(snd);
}

}  // namespace pause
}  // namespace hml
