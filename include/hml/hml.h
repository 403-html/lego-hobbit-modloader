/*
 * Hobbit Mod Loader (HML) - public SDK for native mods.
 *
 * A mod is a 32-bit Windows DLL placed in <game>/mods/. The loader looks for
 * three exported C functions:
 *
 *   const HmlModInfo* hml_mod_info(void);                       (required)
 *   int  hml_mod_init(const HmlApi* api, HmlMod* self);          (required)
 *   void hml_mod_shutdown(void);                                 (optional)
 *   void hml_mod_unload(void);                                   (optional)
 *
 * hml_mod_init runs once, very early: after every DLL the game imports is
 * loaded, but before the game's C runtime starts and before any game code or
 * static constructor runs. Install hooks here; do not touch game objects yet
 * (they do not exist). Return HML_OK to stay loaded, anything else to be
 * unloaded again.
 *
 * Everything crossing the boundary is plain C, so mods can be written in C,
 * C++ or any language that can export cdecl functions (Rust: extern "C").
 *
 * hml_mod_unload makes a mod reloadable while the game runs (hot reload): the
 * loader calls it, then removes the mod's hooks, patches, event subscriptions,
 * settings and input blocks itself, unloads the DLL and loads the new build.
 * In it, put back any game state the mod changed and release what the loader
 * does not know about (D3D resources, threads, timers). After a reload,
 * hml_mod_init runs again with the game already running; the loader then
 * sends HML_EVENT_D3D9_DEVICE_CREATED if the device exists.
 *
 * Use the HML_MOD macro at the bottom for the boilerplate.
 */
#ifndef HML_H
#define HML_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any change to HmlApi. New fields are only ever appended, so a mod
 * built against version N works on loaders >= N. Check api->version (or
 * api->size) before using a field newer than the version you require. */
#define HML_API_VERSION 4

#if defined(_WIN32)
#  define HML_EXPORT __declspec(dllexport)
#else
#  define HML_EXPORT
#endif

/* ------------------------------------------------------------------------ */
/* Status codes                                                              */
/* ------------------------------------------------------------------------ */
typedef enum HmlStatus {
    HML_OK               = 0,
    HML_ERR_GENERIC      = -1,
    HML_ERR_BAD_ARG      = -2,
    HML_ERR_NOT_FOUND    = -3,
    HML_ERR_HOOK_FAILED  = -4,
    HML_ERR_UNSUPPORTED  = -5, /* e.g. unknown game executable */
    HML_ERR_MEMORY       = -6
} HmlStatus;

typedef enum HmlLogLevel {
    HML_LOG_DEBUG = 0,
    HML_LOG_INFO  = 1,
    HML_LOG_WARN  = 2,
    HML_LOG_ERROR = 3
} HmlLogLevel;

/* ------------------------------------------------------------------------ */
/* Mod description                                                           */
/* ------------------------------------------------------------------------ */
typedef struct HmlModInfo {
    uint32_t    api_version; /* set to HML_API_VERSION                      */
    const char* id;          /* unique, [a-z0-9_-], e.g. "hello-world"       */
    const char* name;        /* human readable                               */
    const char* version;     /* e.g. "0.1.0"                                 */
    const char* author;
    const char* description; /* may be NULL                                  */
} HmlModInfo;

/* Opaque handle identifying the calling mod in API calls. */
typedef struct HmlMod HmlMod;

/* ------------------------------------------------------------------------ */
/* Game information                                                          */
/* ------------------------------------------------------------------------ */
typedef enum HmlGameBuild {
    HML_BUILD_UNKNOWN   = 0,
    /* LEGOHobbit.exe, Steam, 32-bit DX9, PE timestamp 0x543C1BFA,
     * internal build "legohobbit-28-05-14-v0.53o_hotfix_7146292". */
    HML_BUILD_STEAM_DX9 = 1
} HmlGameBuild;

typedef struct HmlGameInfo {
    HmlGameBuild build;       /* HML_BUILD_UNKNOWN: do not use hard-coded RVAs */
    const char*  build_name;  /* human readable                              */
    uintptr_t    image_base;  /* runtime base of the exe (ASLR applies)      */
    uint32_t     image_size;
    uint32_t     timestamp;   /* PE header TimeDateStamp                     */
    const char*  exe_path;    /* full path of the running exe                */
    const char*  game_dir;    /* directory of the exe, no trailing slash     */
    const char*  mods_dir;    /* <game_dir>\mods                             */
} HmlGameInfo;

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */
typedef enum HmlEvent {
    /* data: HmlD3D9Event*. The game's Direct3D 9 device was just created. */
    HML_EVENT_D3D9_DEVICE_CREATED = 1,
    /* data: HmlD3D9Event*. Once per frame, right before Present. Drawing is
     * allowed (wrap it in BeginScene/EndScene). */
    HML_EVENT_FRAME = 2,
    /* data: HmlD3D9Event*. Before/after IDirect3DDevice9::Reset (alt-tab,
     * resolution change). Release/recreate D3DPOOL_DEFAULT resources here. */
    HML_EVENT_D3D9_PRE_RESET  = 3,
    HML_EVENT_D3D9_POST_RESET = 4,
    /* data: NULL. Process is exiting. Only flush files / free memory. */
    HML_EVENT_SHUTDOWN = 5,
    /* data: NULL. The loader's Mods menu (pause menu > Mods, or F1) opened /
     * closed (API v3). While it
     * is open, keys and buttons belong to the menu: ignore your hotkeys. */
    HML_EVENT_MENU_OPENED = 6,
    HML_EVENT_MENU_CLOSED = 7,
    /* data: HmlUiEvent*. Once per frame (API v4): draw on-screen UI with the
     * api->ui_* functions; they only work while this event runs. */
    HML_EVENT_UI = 8
} HmlEvent;

/* Data of HML_EVENT_UI. */
typedef struct HmlUiEvent {
    int      width, height; /* back buffer size in pixels; ui_* coordinates  */
    int      native;        /* 1: drawn by the game in its own font this      */
                            /* frame; 0: plain overlay (loading screens, ...) */
    uint64_t frame;
} HmlUiEvent;

/* Text alignment for ui_text */
#define HML_ALIGN_LEFT   0
#define HML_ALIGN_CENTER 1
#define HML_ALIGN_RIGHT  2

/* Colours of the game's own UI, for ui_color */
#define HML_UI_GOLD      0  /* menu entries, values             */
#define HML_UI_WHITE     1  /* titles, plain text               */
#define HML_UI_SELECTED  2  /* the selected entry: pulses grey-white like the game's */
#define HML_UI_DIM       3  /* secondary text                   */
#define HML_UI_BACKDROP  4  /* translucent dark box behind text */

typedef struct HmlD3D9Event {
    void*    device; /* IDirect3DDevice9* */
    void*    hwnd;   /* HWND of the game window */
    uint64_t frame;  /* frames presented so far */
} HmlD3D9Event;

typedef void (*HmlEventFn)(HmlEvent event, const void* data, void* user);

/* ------------------------------------------------------------------------ */
/* Settings shown in the loader's Mods menu (API v3)                         */
/* ------------------------------------------------------------------------ */
typedef enum HmlOptionType {
    HML_OPTION_BOOL = 0,   /* value: int32_t*, 0 / 1                              */
    HML_OPTION_INT = 1,    /* value: int32_t*, min..max by step                   */
    HML_OPTION_FLOAT = 2,  /* value: float*,   min..max by step                   */
    HML_OPTION_CHOICE = 3, /* value: int32_t*, index into choices                 */
    HML_OPTION_ACTION = 4, /* no value; on_change runs when the player picks it   */
    HML_OPTION_LABEL = 5   /* no value; a heading or note in the list             */
} HmlOptionType;

typedef struct HmlOption HmlOption;
typedef void (*HmlOptionFn)(HmlMod* self, const HmlOption* option, void* user);

struct HmlOption {
    HmlOptionType type;
    const char* key;   /* ini key in mods\<id>.ini [<id>]; NULL = not saved      */
    const char* label; /* text in the menu                                     */
    const char* help;  /* one line shown for the selected option; may be NULL   */
    void* value;       /* mod-owned storage, see HmlOptionType; read live        */
    float min, max, step;         /* INT / FLOAT                                */
    const char* const* choices;   /* CHOICE: display strings (also saved)       */
    int32_t num_choices;
    HmlOptionFn on_change;        /* called after the menu changed the value   */
    void* user;
};

/* Convenience initialisers */
#define HML_OPTION_BOOL_INIT(KEY, LABEL, HELP, PTR) \
    { HML_OPTION_BOOL, KEY, LABEL, HELP, PTR, 0, 1, 1, NULL, 0, NULL, NULL }
#define HML_OPTION_FLOAT_INIT(KEY, LABEL, HELP, PTR, MIN, MAX, STEP) \
    { HML_OPTION_FLOAT, KEY, LABEL, HELP, PTR, MIN, MAX, STEP, NULL, 0, NULL, NULL }

/* ------------------------------------------------------------------------ */
/* Input (API v2)                                                            */
/* ------------------------------------------------------------------------ */
/* Devices for HmlApi::input_block. */
#define HML_INPUT_KEYBOARD 0x1u /* game's DirectInput keyboard              */
#define HML_INPUT_MOUSE    0x2u /* game's DirectInput mouse                 */
#define HML_INPUT_GAMEPAD  0x4u /* XInput controllers                        */

/* Same layout and button bits as XINPUT_GAMEPAD (XINPUT_GAMEPAD_A = 0x1000…). */
typedef struct HmlGamepad {
    uint16_t buttons;
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  thumb_lx;
    int16_t  thumb_ly;
    int16_t  thumb_rx;
    int16_t  thumb_ry;
} HmlGamepad;

/* ------------------------------------------------------------------------ */
/* The API table handed to hml_mod_init                                       */
/* ------------------------------------------------------------------------ */
typedef struct HmlApi {
    uint32_t size;    /* sizeof(HmlApi) as built into the loader */
    uint32_t version; /* HML_API_VERSION of the loader           */
    const char* loader_version;

    const HmlGameInfo* game;

    /* --- logging: written to hml.log (and the console if enabled) --- */
    void (*log)(HmlMod* self, HmlLogLevel level, const char* fmt, ...);

    /* --- addresses ---------------------------------------------------- */
    /* Relative virtual address (as seen in Ghidra/IDA with image base
     * 0x400000, minus 0x400000) to a runtime pointer. */
    void* (*rva)(uint32_t rva);

    /* Vtable of an engine class by its RTTI name, e.g. "HobbitGame" or
     * the mangled ".?AVHobbitGame@@". Returns the vtable of the complete
     * object (this-offset 0), or NULL. Survives game updates far better than
     * a hard-coded address. */
    void** (*find_vtable)(const char* class_name);

    /* RTTI class name of a live polymorphic object (its vtable at offset 0),
     * e.g. "HobbitGame", or NULL if obj does not look like one. */
    const char* (*class_name_of)(const void* obj);

    /* IDA-style byte pattern search in the exe's .text section, e.g.
     * "8B 0D ?? ?? ?? ?? 85 C9 74 ??". Returns the first match or NULL. */
    void* (*find_pattern)(const char* pattern);

    /* --- hooks ---------------------------------------------------------- */
    /* Detour a function. *original receives a callable pointer to the
     * previous implementation. If another mod already hooked target, the
     * hooks chain: the newer detour runs first and its *original leads to the
     * older detour. Hooks are enabled immediately. */
    int (*hook_function)(HmlMod* self, void* target, void* detour, void** original);

    /* Replace slot `index` of a vtable. *original receives the previous
     * entry. Affects every object of that exact class. */
    int (*hook_vtable)(HmlMod* self, void** vtable, int index, void* detour, void** original);

    /* Write bytes into code/read-only memory (handles page protection and
     * instruction cache). The original bytes are recorded per mod. */
    int (*patch_bytes)(HmlMod* self, void* address, const void* bytes, size_t size);

    /* --- events --------------------------------------------------------- */
    int (*subscribe)(HmlMod* self, HmlEvent event, HmlEventFn fn, void* user);

    /* --- configuration: mods\<mod id>.ini, section [<mod id>] ----------- */
    int (*config_get_int)(HmlMod* self, const char* key, int default_value);
    /* Copies into buf (always NUL-terminated); returns string length. */
    size_t (*config_get_string)(HmlMod* self, const char* key, const char* default_value,
                                char* buf, size_t buf_size);

    /* --- Direct3D ------------------------------------------------------- */
    /* IDirect3DDevice9* once created, else NULL. */
    void* (*d3d9_device)(void);

    /* ===== API v2 ======================================================= */

    /* --- input ------------------------------------------------------------ */
    /* Hide devices (HML_INPUT_* bits) from the game; 0 = hide nothing. The
     * loader blocks the union of what all mods ask for. Blocked devices read
     * as idle to the game; the functions below still see the real input.
     * Keyboard: use GetAsyncKeyState together with game_has_focus(). */
    void (*input_block)(HmlMod* self, uint32_t devices);

    /* Real state of XInput controller 0-3, even while blocked.
     * Returns 1 if connected (out filled), 0 otherwise. */
    int (*gamepad_state)(uint32_t index, HmlGamepad* out);

    /* Mouse movement the game's DirectInput mouse produced since the previous
     * call, even while blocked. All zero if the game has no DirectInput mouse;
     * fall back to cursor movement then. Any pointer may be NULL. */
    void (*mouse_delta)(int32_t* dx, int32_t* dy, int32_t* wheel);

    /* --- window ----------------------------------------------------------- */
    void* (*game_window)(void);  /* HWND, NULL before the device exists */
    int (*game_has_focus)(void); /* game window is the foreground window */

    /* ===== API v3 ======================================================= */

    /* --- Mods menu (F1 in game) --------------------------------------------- */
    /* Show a setting on this mod's page. The struct and strings are copied;
     * `value`, `choices` and `user` must stay valid. Changes made in the menu
     * are written to `value` and saved to mods\<id>.ini at once. Options appear
     * in the order they are added. */
    int (*option_add)(HmlMod* self, const HmlOption* option);
    /* 1 while the Mods menu is open (input belongs to the menu). */
    int (*menu_is_open)(void);

    /* ---- v4: on-screen UI in the game's own look --------------------------
     * Call these during HML_EVENT_UI only. Coordinates are back-buffer pixels
     * (HmlUiEvent width/height). The UI is drawn by the game itself, after its
     * own HUD and menus, with its own font; where the game draws no GUI
     * (loading screens) a plain overlay stands in. It is part of the frame,
     * so it shows up in screenshots taken from the back buffer. */
    /* Text, `size` = line height in pixels (0 = the game's menu size), ARGB
     * colour, align = HML_ALIGN_*; (x, y) is the top-left / top-centre /
     * top-right of the line. */
    void (*ui_text)(HmlMod* self, float x, float y, const char* utf8, float size, uint32_t argb,
                    int align);
    /* Filled box (ARGB, alpha blended). */
    void (*ui_rect)(HmlMod* self, float x, float y, float w, float h, uint32_t argb);
    /* Size of a line of text in pixels (either pointer may be NULL). */
    void (*ui_measure)(const char* utf8, float size, float* w, float* h);
    /* ARGB of one of the game's UI colours (HML_UI_*). */
    uint32_t (*ui_color)(int which);
    /* A short notification in the top-right corner, in the game's style. */
    void (*ui_toast)(HmlMod* self, const char* title, const char* detail, unsigned ms);
    /* Hide the game's HUD (portraits, hearts, counters, prompts, minimap)
     * while `hidden` is 1; your ui_* drawing still shows. Undone on unload. */
    void (*hud_hide)(HmlMod* self, int hidden);
} HmlApi;

/* Signatures of the mod exports */
typedef const HmlModInfo* (*HmlModInfoFn)(void);
typedef int  (*HmlModInitFn)(const HmlApi* api, HmlMod* self);
typedef void (*HmlModShutdownFn)(void);
typedef void (*HmlModUnloadFn)(void);

#ifndef HML_LOADER_INTERNAL
/* Declared here with C linkage so a C++ mod's plain definitions export
 * unmangled names. */
HML_EXPORT const HmlModInfo* hml_mod_info(void);
HML_EXPORT int  hml_mod_init(const HmlApi* api, HmlMod* self);
HML_EXPORT void hml_mod_shutdown(void);
HML_EXPORT void hml_mod_unload(void);
#endif

/* ------------------------------------------------------------------------ */
/* Convenience                                                               */
/* ------------------------------------------------------------------------ */
/*
 *   HML_MOD("hello-world", "Hello World", "0.1.0", "you", "Says hello")
 *
 *   int hml_mod_init(const HmlApi* api, HmlMod* self) { ... }
 */
#define HML_MOD(ID, NAME, VERSION, AUTHOR, DESCRIPTION)                         \
    HML_EXPORT const HmlModInfo* hml_mod_info(void) {                           \
        static const HmlModInfo info = { HML_API_VERSION, ID, NAME, VERSION,   \
                                         AUTHOR, DESCRIPTION };                 \
        return &info;                                                           \
    }

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HML_H */
