# Writing mods

A mod is a **32-bit** Windows DLL in the game's `mods\` folder that exports
two C functions (a third is optional). Everything you can call lives in the
`HmlApi` table declared in [`include/hml/hml.h`](../include/hml/hml.h).

## Minimal mod

```c
#include <hml/hml.h>

HML_MOD("my-mod", "My Mod", "0.1.0", "me", "One-line description")

int hml_mod_init(const HmlApi* api, HmlMod* self) {
    api->log(self, HML_LOG_INFO, "hello");
    return HML_OK;          /* anything else: the loader disables the mod */
}

void hml_mod_shutdown(void) { /* optional: game is exiting */ }
```

`HML_MOD` defines `hml_mod_info`. The id must be unique and use only
`a-z 0-9 - _`. It names the settings file and tags the mod's log lines.

### Building

With this repository's CMake, add one line:

```cmake
hml_add_mod(my-mod mods/my-mod/my_mod.c)
```

Outside it, any 32-bit compiler works. Point the include path at
`include/` and link statically so the DLL has no extra dependencies:

```
i686-w64-mingw32-gcc -shared -O2 -I include my_mod.c -o my-mod.dll -static
```

In C++ just include the header and define the functions; the header already
declares them `extern "C"`. In Rust, build a `cdylib` for
`i686-pc-windows-msvc` or `i686-pc-windows-gnu` and export the functions with
`#[no_mangle] pub extern "C"`; the header is plain C so `bindgen` handles it.

## When code runs

| Moment | What exists | Do |
|---|---|---|
| `hml_mod_init` | all DLLs loaded; **no game code has run**, not even its static constructors | read settings, install hooks, subscribe to events |
| `HML_EVENT_D3D9_DEVICE_CREATED` | game window and D3D9 device | create your D3D resources |
| `HML_EVENT_FRAME` | everything; game thread, right before Present | per-frame logic, drawing |
| `HML_EVENT_D3D9_PRE_RESET` / `POST_RESET` | device being reset (alt-tab, resolution) | release / recreate `D3DPOOL_DEFAULT` resources |
| `HML_EVENT_SHUTDOWN`, `hml_mod_shutdown` | game calling `ExitProcess` | flush files; keep it short |

Game objects do not exist during `hml_mod_init`. Hook the functions you care
about there, and touch objects from inside your hooks or from events.

## Finding game code

Three ways to locate something, from most to least robust:

1. **By class, through RTTI.** The exe keeps MSVC RTTI for about 6.6k classes.
   `api->find_vtable("HobbitInstructionBuildSystem")` returns that class's
   vtable, then hook a method with `api->hook_vtable(self, vt, index, ...)` or
   detour `vt[index]` with `hook_function`. `api->class_name_of(ptr)` tells
   you what a live object is, which makes exploring much easier. Run
   `tools/re/rtti_vtables.py` for the full class list.
2. **By byte pattern.** `api->find_pattern("8B 0D ?? ?? ?? ?? 85 C9")` scans
   the code section. Survives small updates if the pattern is chosen well.
3. **By address.** `api->rva(0x6EB2C3)` turns an address from Ghidra/IDA
   (image base 0x400000, so subtract 0x400000) into a pointer. The game uses
   ASLR, so never use absolute addresses. Only do this when
   `api->game->build == HML_BUILD_STEAM_DX9`, and stay inactive otherwise.

## Hooking

```c
typedef void (__fastcall *Update_t)(void* self, void* edx, float dt);
static Update_t orig_update;

static void __fastcall my_update(void* self, void* edx, float dt) {
    /* before */
    orig_update(self, edx, dt);
    /* after */
}

/* in hml_mod_init */
void** vt = api->find_vtable("SomeClass");
if (vt) api->hook_function(self, vt[5], (void*)my_update, (void**)&orig_update);
```

- MSVC member functions are `__thiscall` (`this` in ECX). In C, and in C++
  for free functions, write the detour as `__fastcall` with a dummy second
  parameter (EDX), as above.
- If two mods hook the same function, the hooks chain: the later one runs
  first, and its `original` leads to the earlier hook, then to the game.
- `hook_function` on `vt[i]` affects every class that shares that
  implementation. `hook_vtable` changes only that class's slot.
- `patch_bytes` overwrites code or read-only data directly.

Always call `original` unless you deliberately replace the game's behaviour,
and keep the exact calling convention and argument list.

## Input (API v2)

The game reads the keyboard and mouse through DirectInput and controllers
through XInput; the loader sits in front of both.

```c
/* Hide input from the game while your mod uses it (e.g. a free camera). */
api->input_block(self, HML_INPUT_KEYBOARD | HML_INPUT_MOUSE | HML_INPUT_GAMEPAD);
api->input_block(self, 0);                 /* give it back */

/* Read input yourself, blocked or not. */
HmlGamepad pad;
if (api->gamepad_state(0, &pad) && (pad.buttons & 0x1000 /* A */)) { ... }
int32_t dx, dy, wheel;
api->mouse_delta(&dx, &dy, &wheel);        /* since the previous call */
if (api->game_has_focus() && (GetAsyncKeyState('W') & 0x8000)) { ... }
```

`HmlGamepad` has the same layout and button bits as `XINPUT_GAMEPAD`. Always
check `game_has_focus()` before acting on `GetAsyncKeyState`, which also sees
keys pressed in other windows.

## Settings in the Mods menu (API v3)

Players find the loader's Mods menu in the game's pause menu (**Mods**, below
Options; F1 or Back + Start opens it anywhere else). Each mod gets a page; add
your settings to it and the menu edits your variables directly and saves them
to `mods\<id>.ini`:

```c
static int32_t g_enabled = 1;   /* BOOL, INT and CHOICE use int32_t */
static float   g_strength = 0.5f;
static int32_t g_mode = 0;
static const char* const kModes[] = {"Gentle", "Normal", "Wild"};

/* in hml_mod_init, after reading your ini */
HmlOption o = HML_OPTION_BOOL_INIT("Enabled", "Enabled", "Turns the effect on", &g_enabled);
api->option_add(self, &o);
HmlOption s = HML_OPTION_FLOAT_INIT("Strength", "Strength", NULL, &g_strength, 0.0f, 1.0f, 0.05f);
api->option_add(self, &s);
HmlOption m = {HML_OPTION_CHOICE, "Mode", "Mode", NULL, &g_mode, 0, 0, 1, kModes, 3, NULL, NULL};
api->option_add(self, &m);
```

Types: `BOOL`, `INT`, `FLOAT` (min/max/step), `CHOICE` (saved as the choice
text), `ACTION` (a button: `on_change` runs when picked) and `LABEL` (a
heading). `on_change` is also called after any other value changes. Read your
values from the ini at start-up as usual (`config_get_*`); the menu only
writes. While the menu is open, `api->menu_is_open()` returns 1 and your
hotkeys should do nothing; `HML_EVENT_MENU_OPENED` / `HML_EVENT_MENU_CLOSED`
tell you when that changes. `tests/menudemo` is an example.

## On-screen UI in the game's look (API v4)

Draw during `HML_EVENT_UI`; the loader has the game draw it, after its own
HUD and menus, with the game's font:

```c
static void on_ui(HmlEvent ev, const void* data, void* user) {
    const HmlUiEvent* u = data;                 /* width, height, native */
    float size = u->height / 24.0f;             /* line height in pixels */
    api->ui_rect(me, 20, 20, 300, size * 2.2f, api->ui_color(HML_UI_BACKDROP));
    api->ui_text(me, 30, 26, "My Mod", size, api->ui_color(HML_UI_WHITE), HML_ALIGN_LEFT);
    api->ui_text(me, 30, 26 + size, "Turbo: On", size, api->ui_color(HML_UI_GOLD), HML_ALIGN_LEFT);
}
/* in hml_mod_init */
api->subscribe(self, HML_EVENT_UI, on_ui, NULL);
```

- Coordinates are back-buffer pixels; `ui_measure` gives a line's size.
- `ui_color`: `HML_UI_GOLD` (entries), `HML_UI_WHITE`, `HML_UI_SELECTED`
  (pulses like the game's selected entry), `HML_UI_DIM`, `HML_UI_BACKDROP`.
- `ui_toast(self, "Saved", "slot 2", 3000)`: notification in the top-right.
- `hud_hide(self, 1)`: hides the game's HUD; your UI still shows.
- The UI is part of the frame (it is in screenshots taken from the back
  buffer). Skip drawing for a frame before capturing a screenshot.
- Where the game draws no GUI (loading screens) a plain overlay stands in;
  `HmlUiEvent.native` says which.
- Text is UTF-8; characters the game's font lacks are skipped.

## Hot reload

The loader runs mods from a copy (`mods\.hml\`), so the DLL in `mods\` is
never locked: rebuild it while the game runs and, with `HotReload=1` (the
default) in `hml.ini`, the loader notices the new file and swaps it in. Each
mod's page in the Mods menu also has **Reload now**. New DLLs dropped into
`mods\` are loaded too.

A mod opts in by exporting `hml_mod_unload`:

```c
void hml_mod_unload(void) {
    /* put back what you changed in the game, free D3D resources,
       stop your threads; hooks, patches, event subscriptions, settings and
       input blocks are removed by the loader afterwards */
}
```

After the swap `hml_mod_init` runs again, this time with the game already
running, and the loader sends `HML_EVENT_D3D9_DEVICE_CREATED` (and
`HML_EVENT_MENU_OPENED` if the menu is open) so the mod can set itself up.
A reload is refused when another mod has hooked one of the same functions
after this one (its hook would point into the unloaded code). Mods without
`hml_mod_unload` keep working but need a game restart to update.
`GetModuleFileName` on your own module returns the copy's path; use
`api->game->mods_dir` to find files next to your DLL.

## Settings

`api->config_get_int(self, "Key", default)` and `config_get_string` read
`mods\<id>.ini`, section `[<id>]`:

```ini
[my-mod]
Speed=3
```

## Logging

`api->log(self, HML_LOG_INFO, "fmt %d", x)` writes to `hml.log` (and to a
console window with `Console=1` in `hml.ini`). `LogLevel=debug` there also
shows every hook the loader and mods install.

## Compatibility rules

- Check `api->game->build` before using hard-coded addresses.
- Prefer `find_vtable` and patterns over addresses.
- Fields are only ever appended to `HmlApi`; set `api_version` to the
  `HML_API_VERSION` you built against and the loader refuses to load you if
  it is older.
