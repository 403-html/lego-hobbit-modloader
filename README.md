# Hobbit Mod Loader (HML)

> Status: early (0.6.0). Supports the Steam `LEGOHobbit.exe`
> (32-bit, DirectX 9). The DX11 `LEGOHobbit_DX11.exe` is 64-bit and not
> supported yet.

A native mod loader for **LEGO The Hobbit** (PC, Steam). It loads mods
written in C, C++ or anything else that can build a 32-bit Windows DLL, and
gives them a small, stable API for hooking game code, finding engine classes,
reading settings, logging, running code every frame, and taking over input.

This repository holds the loader, its SDK and one example mod (**hello**, a
green square for a few seconds after start). Mods are separate projects; one
checked out under `mods/<name>/` with its own `CMakeLists.txt` builds along
with the loader.

## How it works

1. The loader is a `dinput8.dll` placed next to the game. The game imports
   `DINPUT8.dll`, so Windows loads ours first; it forwards everything to the
   real system DLL.
2. While Windows is loading DLLs it is unsafe to load more, so the loader only
   patches the game's entry point with a jump. When the game is about to start
   (all DLLs ready, no game code run yet) the jump lands in the loader, which
   restores the original bytes, loads every mod in `mods\` and then lets the
   game start normally.
3. It recognises the exact game build from the PE header, so mods can refuse
   to apply hard-coded addresses to an unknown executable.
4. It hooks Direct3D 9 to tell mods when the device is created, reset, and
   when each frame is presented.
5. It sits in front of the game's DirectInput (keyboard, mouse) and XInput
   (gamepad), so a mod can hide input from the game while it uses it.
6. It has an in-game **Mods menu** (F1, or Back + Start on a gamepad): every
   mod with its version and status, each mod's settings (changed with the
   arrow keys / d-pad and saved at once), and a switch to turn a mod off for
   the next start. It is also an entry of the game's own pause menu, drawn
   by the game like its Options page (in any game language).
7. Mods can draw their own on-screen UI in the game's look (its font and
   colours, drawn by the game itself) and hide the game's HUD (API v4).
8. Mods can be rebuilt while the game runs: changed DLLs in `mods\` are
   reloaded (hot reload) and new ones are loaded.

## Install

1. Build it (below) or take a release zip.
2. Copy into the game folder, usually
   `C:\Program Files (x86)\Steam\steamapps\common\LEGO - The Hobbit`:
   ```
   dinput8.dll
   mods\<mod>.dll      (the mods you want)
   mods\hello.dll      (optional example)
   ```
   `hml.ini`, with every setting explained, is written on the first start.
3. Start the game from Steam as usual. With `hello.dll` installed, a small
   green square appears in the top-left corner for 8 seconds once the game
   window opens. The log is written to `hml.log` in the game folder.

**Uninstall:** delete `dinput8.dll` (and `hml.ini`, `hml.log`, `mods\`).

**Another `dinput8.dll` mod already installed?** Only one file can have that
name. Plain DLL mods can be renamed to `mods\<name>.asi`; the loader loads
`.asi` files as they are (`LoadAsi=1`), without the mod API.

## Writing a mod

See [docs/writing-mods.md](docs/writing-mods.md). The short version:

```c
#include <hml/hml.h>

HML_MOD("my-mod", "My Mod", "0.1.0", "me", "What it does")

static void on_frame(HmlEvent ev, const void* data, void* user) { /* every frame */ }

int hml_mod_init(const HmlApi* api, HmlMod* self) {
    api->log(self, HML_LOG_INFO, "running on %s", api->game->build_name);
    void** vt = api->find_vtable("HobbitGame");   /* engine class via RTTI */
    api->subscribe(self, HML_EVENT_FRAME, on_frame, NULL);
    return HML_OK;
}
```

Build it as a 32-bit DLL and drop it into `mods\`.

## Build

Requires CMake 3.16+ and a 32-bit toolchain. MinHook is downloaded at
configure time (pinned to v1.3.4).

**MinGW-w64** (Linux, WSL, or MSYS2):
```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix dist/game
```
In MSYS2 use the `mingw32` shell and drop the toolchain file.

**Visual Studio** (x86):
```
cmake -B build -A Win32
cmake --build build --config Release
cmake --install build --config Release --prefix dist/game
```
The MSVC build has not been compiled yet; MinGW is the tested path.

`dist/game` then holds exactly what goes into the game folder.

### Self-test

`-DHML_BUILD_TESTS=ON` also builds `mods/selftest.dll`. Put it in `mods\` and
start the game; `hml.log` gets a `PASS`/`FAIL` line per loader API check. It
is a development tool, not something to ship. It also builds `shot_test.exe`,
which writes test PNG/JPG/BMP files through freecam's screenshot encoder.

## Layout

```
include/hml/hml.h      the SDK: everything a mod needs (plain C)
src/loader/            the loader (dinput8.dll)
mods/hello/            example mod (C)
tests/selftest/        API self-test mod (C++)
tests/menudemo/        Mods menu / UI test mod (C)
tests/menu_host.cpp    test host that loads dinput8.dll like the game
tools/re/              reverse-engineering helpers
docs/                  mod guide and reverse-engineering notes
```

## Reverse engineering

[docs/re-notes.md](docs/re-notes.md) collects what is known about the
executable and the engine so far. `tools/re/rtti_vtables.py` lists all ~6.6k
engine classes with their vtable addresses.

## License

MIT, see [LICENSE](LICENSE). MinHook is BSD-2-Clause. This project contains no
game code or assets; you need your own copy of the game.
