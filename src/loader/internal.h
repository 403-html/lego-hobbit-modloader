// Internal declarations shared by the loader's translation units.
#pragma once

#define HML_LOADER_INTERNAL
#include <hml/hml.h>

#include <windows.h>
#include <string>
#include <vector>

#ifndef HML_LOADER_VERSION
#define HML_LOADER_VERSION "0.1.0-dev"
#endif

// A setting registered through HmlApi::option_add (strings owned here).
struct OptionEntry {
    HmlOption opt;  // pointers below point into the std::strings
    std::string key, label, help;
};

enum class ModStatus { Active, Failed, Disabled, Plain, NotAMod };

// One entry per file in mods\ (and one for the loader itself, which logs as
// this pseudo-mod).
struct HmlMod {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string file;      // full path of the DLL ("" for the loader itself)
    std::string ini_path;  // mods\<id>.ini
    std::string note;      // why it failed / is disabled
    ModStatus status = ModStatus::Active;
    HMODULE module = nullptr;
    HmlModShutdownFn shutdown = nullptr;
    HmlModUnloadFn unload = nullptr;   // present: can be hot-reloaded
    std::string loaded_copy;           // the shadow copy actually loaded (mods\.hml\)
    FILETIME file_time{};              // of `file` when it was loaded
    uint64_t file_size = 0;
    std::vector<OptionEntry*> options;
};

namespace hml {

// ---- log.cpp --------------------------------------------------------------
void log_init(const std::string& path, bool console, HmlLogLevel min_level);
void log_write(const HmlMod* mod, HmlLogLevel level, const char* fmt, va_list ap);
void logf(HmlLogLevel level, const char* fmt, ...);
#define LOG_D(...) ::hml::logf(HML_LOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) ::hml::logf(HML_LOG_INFO, __VA_ARGS__)
#define LOG_W(...) ::hml::logf(HML_LOG_WARN, __VA_ARGS__)
#define LOG_E(...) ::hml::logf(HML_LOG_ERROR, __VA_ARGS__)
void log_shutdown();

// ---- game.cpp -------------------------------------------------------------
struct Section {
    uint8_t* begin;
    uint8_t* end;
};
bool game_init();               // fills game_info(); call once
const HmlGameInfo* game_info();
bool game_section(const char* name, Section* out);
void* game_entry_point();
void* game_rva(uint32_t rva);
// Address of the exe's import table slot for dll!func (or nullptr).
void** game_iat_slot(const char* dll, const char* func);

// ---- rtti.cpp -------------------------------------------------------------
void** rtti_find_vtable(const char* class_name);
const char* rtti_class_name_of(const void* obj);

// ---- pattern.cpp ----------------------------------------------------------
void* pattern_find(const char* pattern, const uint8_t* begin, const uint8_t* end);

// ---- hooks.cpp ------------------------------------------------------------
bool hooks_init();
int  hooks_function(HmlMod* owner, void* target, void* detour, void** original);
int  hooks_vtable(HmlMod* owner, void** vtable, int index, void* detour, void** original);
int  hooks_patch(HmlMod* owner, void* address, const void* bytes, size_t size);
bool write_protected(void* dst, const void* src, size_t size);
// Hot reload: undo all of one mod's hooks and patches (check_only: just say if possible).
bool hooks_remove_owner(HmlMod* owner, void* reserved, bool check_only, std::string* why);
void hooks_shutdown();

// ---- d3d9_hooks.cpp -------------------------------------------------------
void d3d9_install();
void* d3d9_device();
void* d3d9_hwnd();

// ---- input.cpp ------------------------------------------------------------
void input_init();
void input_on_dinput_created(void* dinput);
void input_block(HmlMod* self, uint32_t devices);
int  input_gamepad_state(uint32_t index, HmlGamepad* out);
void input_mouse_delta(int32_t* dx, int32_t* dy, int32_t* wheel);
int  input_game_has_focus();

// ---- ui.cpp / menu.cpp --------------------------------------------------------
// The menu is drawn as a display list. ui.cpp either replays it with D3D9 at
// Present (classic look) or hands it to native_ui.cpp, which draws it on the
// game thread with the game's own font and 2D primitives.
namespace ui {
enum class Font { Body, Title, Small };
struct Op {
    enum Kind { Rect, Text } kind;
    int x, y, w, h;
    unsigned color;  // ARGB
    Font font;
    float size;      // text line height in px (API text); 0 = by font
    bool utf8;       // text is UTF-8 (API) rather than the menu's cp1252
    std::string text;
};
void on_device(void* dev);
void on_pre_reset();
void on_post_reset(void* dev);
void frame_begin(void* dev);  // Present: start this frame's list
void frame_end();             // Present: hand it to the game renderer or draw it
bool begin(void* dev);        // true while the frame's list is open
void end();
bool active();
bool native_now();        // this frame's list goes to the game renderer
void set_classic(bool on);  // force the plain overlay look
float native_scale(Font f);
float native_scale(const Op& o);
int w();
int h();
int unit();
void measure(Font f, const char* s, int* w, int* h);
void text(Font f, int x, int y, const char* s, unsigned argb);
void rect(int x, int y, int w, int h, unsigned argb);
void measure_px(float size, const char* utf8, float* w, float* h);
void text_px(float size, float x, float y, const char* utf8, unsigned argb);
}  // namespace ui
namespace native {
void install();
void on_exit();
bool game_paused();
long frame_id();  // game frames so far (game thread)
void hud_hide(HmlMod* mod, bool hide);  // any mod hiding it hides the game's HUD
bool enabled();
void set_enabled(bool on);
bool active();
void submit(const std::vector<ui::Op>& ops, bool native);
bool measure(float scale, const char* s, float* w_px, float* h_px, bool utf8 = false);
float line_px();
}  // namespace native
namespace pause {
enum : uint32_t { SUPPRESS_DOWN = 1, SUPPRESS_UP = 2, SUPPRESS_ACCEPT = 4 };
void install();          // "Mods" entry in the game's pause menu
bool visible();          // the pause menu is on screen
bool mods_selected();    // our "Mods" entry is selected
// Navigation presses (edges) and held state from Present; true = "Mods" picked.
bool input(bool up, bool down, bool accept, bool up_held, bool down_held);
uint32_t suppress();     // SUPPRESS_* the game must not see now
void play_sound(int index);  // the game's menu sounds: 0 move, 1 select, 2 back
void game_thread_tick();     // plays queued sounds; game thread
// The Mods menu as a page of the game's own pause screen, printed with the
// game's GUI text in place of the pause entries (like its Options page).
struct PageRow {
    enum Role { Normal, Selected, Header, Inactive } role;
    std::string text;
};
struct Page {
    std::vector<PageRow> rows;  // already scrolled to what fits
    std::string help;           // under the rows
    bool adjust = false;        // the selected row takes Left/Right
};
void set_page(const Page& page);  // from Present, every frame while shown
void clear_page();
bool page_shown();                // the game drew the page recently
}  // namespace pause
namespace probe {
void install();  // [Loader] GuiProbe=1: log the game's GUI2 menus
}
void menu_frame(void* dev);  // input + drawing, once per frame after mods
// ui_api.cpp
void ui_api_fill(HmlApi* api);
void ui_frame(void* dev, uint64_t frame);  // Present: mods' UI, menu, toasts
void toast(const std::string& title, const std::string& detail, unsigned ms);
bool menu_open();

// ---- mods.cpp -------------------------------------------------------------
HmlMod* loader_self();
// All entries for the menu: loader first, then files in load order.
std::vector<HmlMod*> mods_list();
int  mods_option_add(HmlMod* mod, const HmlOption* opt);
void mods_option_save(HmlMod* mod, const OptionEntry& e);
// Enable/disable a mod for the next start (edits hml.ini [Loader] Disabled).
bool mods_set_enabled_next_start(HmlMod* mod, bool enabled);
bool mods_enabled_next_start(const HmlMod* mod);
void mods_load_all();
// Hot reload (mods.cpp): unload and load the mod's DLL again; why = reason on failure.
bool mods_reload(HmlMod* mod, std::string* why);
bool mods_hot_reload_enabled();
void mods_set_hot_reload(bool on);
void mods_watch();  // once per frame from Present: reload changed files, load new ones
void mods_shutdown_all();
void events_dispatch(HmlEvent event, const void* data);

// ---- config ---------------------------------------------------------------
struct LoaderConfig {
    std::string ini_path;
    unsigned menu_key = VK_F1;
    unsigned menu_pad = 0x0030;  // Back + Start
    bool enabled = true;
    bool console = false;
    HmlLogLevel log_level = HML_LOG_INFO;
    bool load_asi = true;
    std::string disabled;  // comma separated mod file names / ids
};
const LoaderConfig& loader_config();

std::string path_join(const std::string& a, const std::string& b);

}  // namespace hml
