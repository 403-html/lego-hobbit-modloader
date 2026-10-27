// Draws the loader's UI through the game's own renderer: text with the game's
// GUI font (QFont), boxes as the engine's 2D primitives. See the "Text and 2D
// drawing" section of docs/re-notes.md.
//
// The engine records drawing into per-frame command lists on the game thread,
// so this cannot run from Present. The menu is built into a display list at
// Present (ui.cpp); the game-thread hook below replays the newest list in the
// game's 2D overlay pass of the on-screen render pipe (sub_180080), after the
// game's HUD and menus, so it lands on top of them.
//
// Steam DX9 build only. If anything goes wrong the D3D fallback in ui.cpp
// draws instead; a guard file turns this off after a crash during the first
// native frames.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hml {
namespace native {
namespace {

// ---- game addresses (RVAs, Steam DX9 build) --------------------------------------
constexpr uint32_t RVA_END_FRAME = 0x1F1B50;     // int sub_1F1B50(void): ends the frame
constexpr uint32_t RVA_PAUSED = 0x1696E35;        // byte: game paused (pause menu)
constexpr uint32_t RVA_OVERLAY_PASS = 0x180080;  // void sub_180080(void): HUD-level 2D + debug text
constexpr uint32_t RVA_GUI_DISPATCH = 0x3DC790;   // void sub_3DC790(void): draws the GUI
constexpr uint32_t RVA_HUD_STAGE = 0x4C2000;      // render pipe HUD stage callback (cdecl, 1 arg)
constexpr uint32_t RVA_STAGE_CONTEXT = 0x17D69FC; // its context object (+0x10 = argument)
constexpr uint32_t RVA_GUI2_MANAGER = 0x1799310;  // GUI2Manager* (+0x60 GUI camera)
constexpr uint32_t RVA_CURRENT_CAMERA = 0x1696AC0;// the camera currently set (NuCamera)
constexpr uint32_t RVA_VIEWS = 0x1695960;         // object whose sub_152F00 sets the camera
constexpr uint32_t RVA_GUI_RENDER = 0x3E55B0;     // GUI2Manager::Render (vtable slot 15), thiscall, 3 args
constexpr uint32_t RVA_VIEW_RESTORE = 0x1A58B0;   // viewport guard: restore (thiscall)
constexpr uint32_t RVA_CUR_PIPE = 0x16A19DC;      // int: index of the pipe being drawn
constexpr uint32_t RVA_IN_PIPE_LOOP = 0x16A19D8;  // int: 1 while the render pipes are drawn
constexpr uint32_t RVA_GUI_FONT = 0x17992B8;      // QFont*: the game's GUI font
constexpr uint32_t RVA_SYSTEM_FONT = 0x16A325C;   // QFont*: built-in debug font (fallback)
constexpr uint32_t RVA_MTL_2D_ALPHA = 0x16A22BC;  // Material*: numtl_defaultmtl2d_alpha_nozupd
constexpr uint32_t RVA_SPACE_OVERRIDE = 0x16A3258;  // float: space width used by Print

// QFont (cdecl; font NULL = system font)
using PushInt_t = void(__cdecl*)(int);
using Pop_t = void(__cdecl*)();
using FontFn_t = void(__cdecl*)(void*);
using SetScale_t = void(__cdecl*)(void*, float, float);
using SetColour_t = void(__cdecl*)(void*, uint32_t);
using SetPos_t = void(__cdecl*)(void*, float, float, float);
using Print_t = void(__cdecl*)(void*, const char*);
using Width_t = float(__cdecl*)(void*, const char*);
using LineH_t = float(__cdecl*)(void*);
// 2D primitives
using PrimBegin_t = void**(__cdecl*)(void** out, int type, int flags, void** material, int count);
using PrimColour_t = uint32_t(__fastcall*)(void* prim, void* edx, uint32_t game_colour);
using PrimVertex_t = void(__fastcall*)(void* prim, void* edx, float x, float y, float z);
using PrimThis_t = void(__fastcall*)(void* prim, void* edx);

struct Api {
    PushInt_t push_flags, push_mode;
    Pop_t pop_flags, pop_mode;
    FontFn_t space_from_font;
    SetScale_t set_scale;
    SetColour_t set_colour;
    SetPos_t set_pos;
    Print_t print;
    Width_t width;
    LineH_t line_height;
    PrimBegin_t prim_begin;
    PrimColour_t prim_colour;
    PrimVertex_t prim_vertex;
    PrimThis_t prim_submit;
} A;

template <class T>
T fn(uint32_t rva) {
    return reinterpret_cast<T>(game_rva(rva));
}

using Pass_t = void(__cdecl*)();
Pass_t o_pass = nullptr;

bool g_installed = false;
bool g_enabled = true;   // user setting
bool g_broken = false;   // set after a crash guard trip
CRITICAL_SECTION g_lock;

// ---- display list handed over from Present -------------------------------------------
std::vector<ui::Op> g_pending;  // under g_lock
DWORD g_pending_tick = 0;
bool g_pending_native = false;
volatile LONG g_draws = 0;  // game-thread passes that drew (or could have)

// ---- metrics (game thread computes, any thread reads) ---------------------------------
struct Metrics {
    void* font = nullptr;
    int scr_w = 0, scr_h = 0;
    float adv[128] = {};   // virtual units at scale 1
    float spacing = 0;     // between two glyphs at scale 1
    float line = 0;        // line height, virtual units at scale 1
};
Metrics g_m;            // under g_lock
volatile LONG g_ready = 0;

std::string g_guard;  // see guard_arm()

void* current_font() {
    void* f = *static_cast<void**>(game_rva(RVA_GUI_FONT));
    return f ? f : *static_cast<void**>(game_rva(RVA_SYSTEM_FONT));
}

// cp1252 / UTF-8 safe subset: the menus only use ASCII plus a few symbols.
std::string to_game_text(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x80) o += (char)c;
        else if (c == 0x8B) o += '<';
        else if (c == 0x9B) o += '>';
        else if (c == 0xB7) o += '-';
        else o += '?';
    }
    return o;
}

// ARGB -> engine colour: 0xAABBGGRR with alpha 0..0x80
uint32_t game_colour(uint32_t argb) {
    uint32_t a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    a = (a * 128 + 127) / 255;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// The game sets a font's scale, colour and position before each use, but put
// them back anyway so nothing of ours leaks into its next print.
struct FontState {
    void* font;
    float sx, sy;
    uint8_t colour_pos[16];
    float space;
    explicit FontState(void* f) : font(f) {
        sx = **reinterpret_cast<float**>((uint8_t*)f + 0x2C);
        sy = **reinterpret_cast<float**>((uint8_t*)f + 0x30);
        std::memcpy(colour_pos, *reinterpret_cast<uint8_t**>((uint8_t*)f + 0x48), 16);
        space = *static_cast<float*>(game_rva(RVA_SPACE_OVERRIDE));
    }
    ~FontState() {
        **reinterpret_cast<float**>((uint8_t*)font + 0x2C) = sx;
        **reinterpret_cast<float**>((uint8_t*)font + 0x30) = sy;
        std::memcpy(*reinterpret_cast<uint8_t**>((uint8_t*)font + 0x48), colour_pos, 16);
        *static_cast<float*>(game_rva(RVA_SPACE_OVERRIDE)) = space;
    }
};

void compute_metrics(void* font, int w, int h) {
    FontState keep(font);
    Metrics m;
    m.font = font;
    m.scr_w = w;
    m.scr_h = h;
    A.push_flags(2);
    A.push_mode(4);
    A.space_from_font(font);
    A.set_scale(font, 1.0f, 1.0f);
    char s[3] = {0, 0, 0};
    for (int c = 32; c < 127; ++c) {
        s[0] = (char)c;
        m.adv[c] = A.width(font, s);
    }
    float wi = A.width(font, "i"), wii = A.width(font, "ii");
    m.spacing = wii - 2 * wi;
    m.line = std::fabs(A.line_height(font));
    A.pop_mode();
    A.pop_flags();
    bool sane = m.line > 0.01f && m.line < 1000.0f && m.adv['M'] > 0.01f && m.adv['M'] < 1000.0f;
    LOG_I("game font %p: line %.2f, 'M' %.2f, spacing %.2f (virtual units)%s", font, m.line,
          m.adv['M'], m.spacing, sane ? "" : " - implausible, using the classic menu font");
    EnterCriticalSection(&g_lock);
    g_m = m;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_ready, sane ? 1 : 0);
    if (!sane) g_broken = true;
}

// px -> the engine's virtual 640 x 224 2D space
struct Space {
    float sx, sy;
};

void draw_rects(const std::vector<const ui::Op*>& rects, Space sp) {
    void** mtl = static_cast<void**>(game_rva(RVA_MTL_2D_ALPHA));
    size_t i = 0;
    while (i < rects.size()) {
        size_t n = rects.size() - i;
        if (n > 32) n = 32;
        void* prim = nullptr;
        A.prim_begin(&prim, 0, 5, mtl, (int)(6 * n));
        if (!prim) return;
        for (size_t k = 0; k < n; ++k) {
            const ui::Op& r = *rects[i + k];
            float l = r.x * sp.sx, t = r.y * sp.sy, rr = (r.x + r.w) * sp.sx, b = (r.y + r.h) * sp.sy;
            uint32_t c = A.prim_colour(prim, nullptr, game_colour(r.color));
            const float vx[6] = {l, rr, l, rr, rr, l};
            const float vy[6] = {t, t, b, t, b, b};
            for (int v = 0; v < 6; ++v) {
                uint8_t* fmt = *reinterpret_cast<uint8_t**>((uint8_t*)prim + 4);
                uint8_t* wp = *reinterpret_cast<uint8_t**>((uint8_t*)prim + 8);
                *reinterpret_cast<uint32_t*>(wp + *reinterpret_cast<uint16_t*>(fmt + 0xA)) = c;
                wp[*reinterpret_cast<uint16_t*>(fmt + 8) + 3] = (uint8_t)(c >> 24);
                A.prim_vertex(prim, nullptr, vx[v], vy[v], 0.0f);
            }
        }
        A.prim_submit(prim, nullptr);
        void** vt = *reinterpret_cast<void***>(prim);
        reinterpret_cast<PrimThis_t>(vt[3])(prim, nullptr);  // Release
        i += n;
    }
}

void draw_list(const std::vector<ui::Op>& ops, const Metrics& m) {
    void* font = m.font;
    FontState keep(font);
    Space sp{640.0f / (float)m.scr_w, 224.0f / (float)m.scr_h};
    A.push_flags(2);
    A.push_mode(4);
    A.space_from_font(font);
    float cur_scale = -1;
    std::vector<const ui::Op*> rects;
    for (size_t i = 0; i <= ops.size(); ++i) {
        const ui::Op* op = i < ops.size() ? &ops[i] : nullptr;
        if (op && op->kind == ui::Op::Rect) {
            rects.push_back(op);
            continue;
        }
        if (!rects.empty()) {
            draw_rects(rects, sp);
            rects.clear();
        }
        if (!op) break;
        float s = ui::native_scale(*op);
        if (s != cur_scale) {
            A.set_scale(font, s, s);
            cur_scale = s;
        }
        A.set_colour(font, game_colour(op->color));
        A.set_pos(font, op->x * sp.sx, op->y * sp.sy, 0.0f);
        A.print(font, op->utf8 ? op->text.c_str() : to_game_text(op->text).c_str());
    }
    A.pop_mode();
    A.pop_flags();
}

// The guard file exists only while game code we have not seen run cleanly yet
// is being called: the first metrics pass and the first 300 menu frames. If
// the game dies in between, the next start finds it and stays classic.
bool g_guard_armed = false;
int g_guard_frames = 0;  // clean native menu frames so far (-1: done watching)

void guard_arm() {
    if (g_guard_armed) return;
    HANDLE f = CreateFileA(g_guard.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    g_guard_armed = true;
}

void guard_disarm() {
    if (!g_guard_armed) return;
    DeleteFileA(g_guard.c_str());
    g_guard_armed = false;
}

void guard_frame() {
    if (g_guard_frames < 0) return;
    guard_arm();
    if (++g_guard_frames >= 300) {
        guard_disarm();
        g_guard_frames = -1;
        LOG_I("game-font menu drawn for 300 frames without trouble");
    }
}

// Our drawing, on the game thread, in a render pipe's overlay pass.
int g_diag = 0;          // passes still to describe in the log
bool g_had_ops = false;
void draw_frame(int) {
    if (!g_enabled || g_broken) return;
    void* font = current_font();
    if (!font) return;
    int w = ui::w(), h = ui::h();
    if (w <= 0 || h <= 0) return;
    if (!g_ready || g_m.font != font || g_m.scr_w != w || g_m.scr_h != h) {
        bool watch = g_guard_frames >= 0 && !g_guard_armed;
        if (watch) guard_arm();
        compute_metrics(font, w, h);
        if (watch) guard_disarm();
        if (!g_ready) return;
    }
    InterlockedIncrement(&g_draws);

    std::vector<ui::Op> ops;
    Metrics m;
    EnterCriticalSection(&g_lock);
    bool fresh = g_pending_native && GetTickCount() - g_pending_tick < 250;
    if (fresh) ops = g_pending;
    m = g_m;
    LeaveCriticalSection(&g_lock);
    if (ops.empty()) {
        g_had_ops = false;
        return;
    }
    if (!g_had_ops) g_diag = 6;  // the menu just appeared: say where it goes
    g_had_ops = true;
    if (g_diag > 0) {
        --g_diag;
        LOG_I("menu drawn in the game's GUI pass (pipe %d current), game frame %ld, %zu items",
              *static_cast<int*>(game_rva(RVA_CUR_PIPE)), (long)native::frame_id(), ops.size());
    }
    if (g_guard_frames == 0) LOG_I("drawing the menu with the game's font");
    guard_frame();
    draw_list(ops, m);
}

// sub_180080: the game's 2D overlay pass (screen fade, debug text), run after
// each on-screen render pipe while that pipe's viewport is current. 2D
// recorded anywhere else does not reach the screen. Our UI goes into every
// such pass (once per pipe per frame).
volatile LONG g_pass_in = 0, g_pass_out = 0, g_end_frames = 0;
void __cdecl hk_pass() {
    o_pass();
    if (*static_cast<int*>(game_rva(RVA_IN_PIPE_LOOP)) == 0) {
        InterlockedIncrement(&g_pass_out);
        return;
    }
    InterlockedIncrement(&g_pass_in);
}

// GUI2Manager::Render (vtable slot 15) draws the game's menus and HUD text
// with a full-screen viewport, which it sets up with a viewport guard
// (sub_1A57E0) and restores with sub_1A58B0 once its layers are drawn. Our
// UI goes in just before that restore: same context as the game's menus, on
// top of them. (A render pipe's overlay pass is not it: while paused the game
// only renders an off-screen pipe there.)
using GuiRender_t = void(__fastcall*)(void*, void*, void*, void*, void*);
GuiRender_t o_gui_render = nullptr;
using ViewRestore_t = void(__fastcall*)(void*, void*);
ViewRestore_t o_view_restore = nullptr;
volatile LONG g_in_gui = 0;  // inside GUI2Manager::Render, our UI not drawn yet
volatile LONG g_drawn = 0;   // our UI went in during this GUI pass
void __fastcall hk_gui_render(void* mgr, void* edx, void* a, void* b, void* c) {
    g_in_gui = 1;
    o_gui_render(mgr, edx, a, b, c);
    g_in_gui = 0;
}
void __fastcall hk_view_restore(void* guard, void* edx) {
    if (g_in_gui) {
        g_in_gui = 0;
        g_drawn = 1;
        draw_frame(-1);
    }
    o_view_restore(guard, edx);
}

bool have_pending() {
    EnterCriticalSection(&g_lock);
    bool r = g_pending_native && !g_pending.empty() && GetTickCount() - g_pending_tick < 250;
    LeaveCriticalSection(&g_lock);
    return r;
}

// Our own GUI pass, for when the game's does not reach GUI2Manager::Render
// (e.g. its HUD is hidden): the same set-up the game's GUI dispatch
// (sub_3DA570) uses around its layers - a full-screen viewport and the GUI
// camera - with only our UI inside.
using Ctor_t = void(__fastcall*)(void*, void*);
using CopyTo_t = void(__fastcall*)(void*, void*, void*);
using ViewGuard_t = void(__fastcall*)(void*, void*, float, float, float, float, float, float, float, float);
using SetCamera_t = void(__fastcall*)(void*, void*, void*, int);
alignas(16) uint8_t g_saved_cam[0x380];
int g_own_passes = 0;  // own passes so far (the first ones run under the crash guard)
void own_gui_pass() {
    void* mgr = *static_cast<void**>(game_rva(RVA_GUI2_MANAGER));
    void* gui_cam = mgr ? *reinterpret_cast<void**>(static_cast<uint8_t*>(mgr) + 0x60) : nullptr;
    void* cur_cam = game_rva(RVA_CURRENT_CAMERA);
    void* views = game_rva(RVA_VIEWS);
    if (!gui_cam) return;
    if (g_own_passes < 120) {
        if (g_own_passes == 0) LOG_I("drawing the UI in the loader's own GUI pass (game HUD hidden)");
        guard_arm();
    }
    reinterpret_cast<Ctor_t>(game_rva(0x1A9370))(g_saved_cam, nullptr);
    reinterpret_cast<CopyTo_t>(game_rva(0x1A93E0))(cur_cam, nullptr, g_saved_cam);
    alignas(16) float guard[8];
    reinterpret_cast<ViewGuard_t>(game_rva(0x1A57E0))(guard, nullptr, 0, 0, 1, 1, 0, 0, 1, 1);
    reinterpret_cast<SetCamera_t>(game_rva(0x152F00))(views, nullptr, gui_cam, 0);
    draw_frame(-1);
    o_view_restore(guard, nullptr);
    reinterpret_cast<SetCamera_t>(game_rva(0x152F00))(views, nullptr, g_saved_cam, 0);
    if (g_own_passes < 120 && ++g_own_passes == 120 && g_guard_frames < 0) guard_disarm();
}

// sub_3DC790: the game's GUI dispatch (from the HUD stage of the render pipe,
// and from the pause screen). Our UI is drawn inside it, after the game's.
using Dispatch_t = void(__cdecl*)();
Dispatch_t o_gui_dispatch = nullptr;
volatile LONG g_hud_hidden = 0;  // a mod asked for the game's HUD to be hidden
void __cdecl hk_gui_dispatch() {
    pause::game_thread_tick();
    g_drawn = 0;
    if (!g_hud_hidden) o_gui_dispatch();
    if (!g_drawn && have_pending()) own_gui_pass();
    g_drawn = 0;
}

// sub_4C2000: the render pipe's HUD stage: tells the HUD systems to draw
// (stage 8), then runs the GUI dispatch. With the HUD hidden only our part runs.
using Stage_t = void(__cdecl*)(int);
Stage_t o_hud_stage = nullptr;
void __cdecl hk_hud_stage(int arg) {
    if (!g_hud_hidden) return o_hud_stage(arg);
    uint8_t* ctx = *static_cast<uint8_t**>(game_rva(RVA_STAGE_CONTEXT));
    if (ctx) *reinterpret_cast<int*>(ctx + 0x10) = arg;
    hk_gui_dispatch();
}

// sub_1F1B50: ends the frame. Counts frames.
using EndFrame_t = int(__cdecl*)();
EndFrame_t o_end_frame = nullptr;
int __cdecl hk_end_frame() {
    LONG n = InterlockedIncrement(&g_end_frames);
    if (n == 1) LOG_I("game renderer: first frame reached the loader's UI hook");
    if (n == 600)
        LOG_I("game renderer: 600 frames, overlay passes %ld in pipes / %ld after, UI %s", (long)g_pass_in,
              (long)g_pass_out, g_ready ? "ready" : "not ready");
    return o_end_frame();
}

}  // namespace

void install() {
    InitializeCriticalSection(&g_lock);
    const HmlGameInfo* g = game_info();
    g_guard = path_join(g->game_dir, "hml_gamefont.guard");
    g_enabled = GetPrivateProfileIntA("Loader", "GameFont", 1, loader_config().ini_path.c_str()) != 0;
    if (g->build != HML_BUILD_STEAM_DX9) {
        LOG_I("game font for menus: not available on this build");
        return;
    }
    if (GetFileAttributesA(g_guard.c_str()) != INVALID_FILE_ATTRIBUTES) {
        LOG_W("the game stopped while the menu was being drawn with the game's font last time; "
              "using the classic menu font (GameFont=1 in hml.ini turns it back on)");
        DeleteFileA(g_guard.c_str());
        WritePrivateProfileStringA("Loader", "GameFont", "0", loader_config().ini_path.c_str());
        g_enabled = false;
    }
    A.push_flags = fn<PushInt_t>(0x1A6D20);
    A.pop_flags = fn<Pop_t>(0x1A6D60);
    A.push_mode = fn<PushInt_t>(0x1A6EA0);
    A.pop_mode = fn<Pop_t>(0x1A6ED0);
    A.space_from_font = fn<FontFn_t>(0x1A70B0);
    A.set_scale = fn<SetScale_t>(0x1A70D0);
    A.set_colour = fn<SetColour_t>(0x1A7150);
    A.set_pos = fn<SetPos_t>(0x1A73A0);
    A.print = fn<Print_t>(0x1A9030);
    A.width = fn<Width_t>(0x1A7990);
    A.line_height = fn<LineH_t>(0x1A7050);
    A.prim_begin = fn<PrimBegin_t>(0x1863E0);
    A.prim_colour = fn<PrimColour_t>(0x185700);
    A.prim_vertex = fn<PrimVertex_t>(0x001DE0);
    A.prim_submit = fn<PrimThis_t>(0x185BB0);
    g_installed = hooks_function(loader_self(), game_rva(RVA_END_FRAME), (void*)&hk_end_frame,
                                 (void**)&o_end_frame) == HML_OK &&
                  hooks_function(loader_self(), game_rva(RVA_OVERLAY_PASS), (void*)&hk_pass,
                                 (void**)&o_pass) == HML_OK &&
                  hooks_function(loader_self(), game_rva(RVA_GUI_RENDER), (void*)&hk_gui_render,
                                 (void**)&o_gui_render) == HML_OK &&
                  hooks_function(loader_self(), game_rva(RVA_VIEW_RESTORE), (void*)&hk_view_restore,
                                 (void**)&o_view_restore) == HML_OK &&
                  hooks_function(loader_self(), game_rva(RVA_GUI_DISPATCH), (void*)&hk_gui_dispatch,
                                 (void**)&o_gui_dispatch) == HML_OK &&
                  hooks_function(loader_self(), game_rva(RVA_HUD_STAGE), (void*)&hk_hud_stage,
                                 (void**)&o_hud_stage) == HML_OK;
    LOG_I("game font for menus: %s", !g_installed ? "hook failed" : g_enabled ? "on" : "off");
}

void on_exit() { guard_disarm(); }

long frame_id() { return g_end_frames; }

// HUD hiding requested by mods (api->hud_hide).
namespace {
CRITICAL_SECTION g_hud_lock;
bool g_hud_lock_ready = false;
std::vector<HmlMod*> g_hud_requests;
}  // namespace
void hud_hide(HmlMod* mod, bool hide) {
    if (!g_hud_lock_ready) {
        InitializeCriticalSection(&g_hud_lock);
        g_hud_lock_ready = true;
    }
    EnterCriticalSection(&g_hud_lock);
    auto it = std::find(g_hud_requests.begin(), g_hud_requests.end(), mod);
    if (hide && it == g_hud_requests.end()) g_hud_requests.push_back(mod);
    if (!hide && it != g_hud_requests.end()) g_hud_requests.erase(it);
    LONG now = g_hud_requests.empty() ? 0 : 1;
    LeaveCriticalSection(&g_hud_lock);
    if (InterlockedExchange(&g_hud_hidden, now) != now) LOG_D("game HUD %s", now ? "hidden" : "shown");
}

bool game_paused() {
    return g_installed && *static_cast<volatile uint8_t*>(game_rva(RVA_PAUSED)) != 0;
}

bool enabled() { return g_enabled; }
void set_enabled(bool on) {
    g_enabled = on;
    if (on) g_broken = false;
    if (on && g_guard_frames < 0) g_guard_frames = 0;  // watch the first frames again
}

// True when the game thread is drawing our lists (recently and reliably).
bool active() {
    static LONG last_draws = 0;
    static int idle_presents = 1000;
    if (!g_installed || !g_enabled || g_broken || !g_ready) return false;
    LONG d = g_draws;
    if (d != last_draws) {
        last_draws = d;
        idle_presents = 0;
    } else if (idle_presents < 1000) {
        ++idle_presents;
    }
    return idle_presents < 10;
}

void submit(const std::vector<ui::Op>& ops, bool native) {
    if (!g_installed) return;
    EnterCriticalSection(&g_lock);
    g_pending = ops;
    g_pending_native = native;
    g_pending_tick = GetTickCount();
    LeaveCriticalSection(&g_lock);
}

bool measure(float scale, const char* s, float* w_px, float* h_px, bool utf8) {
    if (!g_ready) return false;
    EnterCriticalSection(&g_lock);
    float sx = (float)g_m.scr_w / 640.0f, sy = (float)g_m.scr_h / 224.0f;
    float w = 0;
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; p && *p; ++p, ++n) {
        unsigned c = *p;
        if (utf8 && c >= 0x80) {
            if ((c & 0xC0) == 0x80) {  // continuation byte: part of the previous character
                --n;
                continue;
            }
            c = '?';
        } else if (c == 0x8B) c = '<';
        else if (c == 0x9B) c = '>';
        else if (c == 0xB7) c = '-';
        else if (c < 32 || c > 126) c = '?';
        w += g_m.adv[c];
    }
    if (n > 1) w += (n - 1) * g_m.spacing;
    float line = g_m.line;
    LeaveCriticalSection(&g_lock);
    if (w_px) *w_px = w * scale * sx;
    if (h_px) *h_px = line * scale * sy;
    return true;
}

// Line height in px at scale 1.
float line_px() {
    if (!g_ready) return 0;
    return g_m.line * (float)g_m.scr_h / 224.0f;
}

}  // namespace native
}  // namespace hml
