# Reverse-engineering notes

What is known about LEGO The Hobbit's PC executable so far. "Verified" means
checked against the binary or a running game. Everything else is a lead.

## Executables (Steam)

| File | Arch | PE timestamp | Notes |
|---|---|---|---|
| `LEGOHobbit.exe` | x86, DX9 | `0x543C1BFA` (2014-10-13) | SizeOfImage `0x01E72000`, CheckSum `0x01CCE17D`, entry RVA `0x00BBEFC1`. **Supported.** |
| `LEGOHobbit_DX11.exe` | x64, DX11 | 2014-10-13 | Also imports `DINPUT8.dll`, so a 64-bit build of the same loader could support it later. |

Both: MSVC 2010 (linker 10.0), ASLR on (runtime base differs every launch),
DEP on, no exports. Steam app id `285160` (verified: passed to
`SteamAPI_RestartAppIfNecessary`). Internal build name, from assert paths:
`legohobbit-28-05-14-v0.53o_hotfix_7146292`.

Sections of `LEGOHobbit.exe` (RVA, size): `.text 001000 CB0FFA`,
`.rdata CB2000 3E29D8`, `.data 1095000 78AC0C`, `.tls 1820000`, `.shr`,
`.rsrc`, `.reloc`.

### Imports worth knowing

- `DINPUT8!DirectInput8Create`: why a `dinput8.dll` proxy loads first.
- `d3d9!Direct3DCreate9`: the only D3D9 entry point; the loader hooks it.
- `XINPUT1_3` ordinals 2/3/4 (`XInputGetState`, `XInputSetState`,
  `XInputGetCapabilities`): controller input.
- `steam_api`: 11 imports (init, user stats, cloud, callbacks).
- `dbghelp!SymFromAddr` etc.: probably a crash reporter that symbolises
  stacks. Lead only.
- `WS2_32`, `IPHLPAPI`: network. RTTI shows `NuTMClientSock`, `NuSocket`,
  `NuClientServer`, possibly developer tooling. Lead only.
- `AVIFIL32`: AVI writing, likely a capture feature. Lead only.

## Engine: TT Games "Nu2" / NuTT

The binary keeps **MSVC RTTI**: 6,772 type names and ~6,600 vtables
(4,405 complete-object vtables). Class names show the engine's layers:

| Prefix | Count | Layer |
|---|---|---|
| `Nu*` | ~256 | Core engine (`nu2api\nucore`, `nusound`, …): files, memory, render, sound, reflection |
| `Mech*`, `Api*` | ~770 | "mechanicsApi": characters, vehicles, abilities, AI |
| `Lego*` | ~174 | Shared LEGO-game layer: gizmos, studs, cutscenes, saves, cheats |
| `Hobbit*` | ~121 | Title-specific: inventory, loot, instruction builds, mining, frontend |
| `Scr*`, `Script*` | ~120 | Script system (`ScriptManager`, `ScriptProgram`, `ScrAction_*`) |
| `Game*`, `Framework*` | | `GameFramework`, `HobbitGame` (330 virtual methods), level containers |

Notable systems to explore:

- **Reflection:** `NuClass`, `NuClassDefinition`, `NuClassCoreField`,
  `ApiClass`, `ApiClassField`. If classes describe their fields at runtime,
  a mod could enumerate and edit object properties by name. Probably the most
  valuable target for a general modding API.
- **Scripts:** `ScriptManager`, `ScriptProgram`, `ScriptFunction` and ~100
  `ScrAction_*` nodes. Hooking these could expose game logic to mods without
  per-function reverse engineering.
- **Cheats:** `LegoCheatListenerAddOn`, `LootOverrideCheatsAddOn`, strings
  `CHEAT_PERFECT_FORGE`, `CHEAT_DOUBLE_LOOT`, `CHEAT_FASTHACK`, ….
- **Add-on pattern:** many `*AddOn` classes (`ApiAddOnsManager`,
  `LegoAddOnsManager`) attached to characters and levels. Likely the engine's
  component system.
- **Instruction builds** (Auto Perfect Build's area): `HobbitInstructionBuildSystem`
  (`E30644`, 57 methods), `InstructionBuildPlayBubble` (`EF4EA4`, 12 methods),
  `PlayBubbleSystem`, `HobbitInstructionBuildUI`.

Selected vtables (RVA). The first four were also resolved in-process by the
loader on the real exe; all match the offline script:

| Class | vtable RVA | methods |
|---|---|---|
| `HobbitGame` | `E1D274` | 330 |
| `GameFramework` | `EA78BC` | 41 |
| `ScriptManager` | `EAE69C` | 24 |
| `HobbitInstructionBuildSystem` | `E30644` | 57 |
| `InstructionBuildPlayBubble` | `EF4EA4` | 12 |
| `ApiSystemBase` | `E1BF94` | 37 |

Full list: `python tools/re/rtti_vtables.py LEGOHobbit.exe`.

### RTTI layout (x86 MSVC)

```
object+0           -> vtable
vtable[-1]         -> CompleteObjectLocator  (.rdata)
                        +0  signature (0)
                        +4  offset of this vtable in the complete object
                        +12 TypeDescriptor*   (.data)
TypeDescriptor+8   -> ".?AVHobbitGame@@"
```
Pointers are absolute and relocated by Windows, so the walk works the same
at any ASLR base. Nested names read inner-first: `Helper@PlayBubbleObjectManager`
is `PlayBubbleObjectManager::Helper`.

## Camera

Found statically on the Steam DX9 exe; names are ours. Used by the `freecam`
mod. The hooks install cleanly (Wine test); behaviour in game is **not yet
verified**.

**NuCamera** (0x370 bytes; constructor `1A9370`, 69 callers):

| Offset | Meaning | Evidence |
|---|---|---|
| `+000` | world matrix 4×4 (camera→world). Row-major, row vectors: row0 right, row1 up, row2 forward, row3 position | ctor sets identity; `ICameraSource::v3` returns `&cam+0x30` (position) |
| `+040` | view = inverse(world) | `CalcMatrices` calls a 4×4 inverse `512170(dst=+40, src=+0)` |
| `+080` | view × projection | `5119F0(+80, +40, +100)` |
| `+0C0`, `+140`, `+180` | further products / inverses used for frustum | `CalcMatrices` |
| `+100` | projection | built from FOV/aspect/near/far by `1A9BD0` (perspective) or inline (ortho) |
| `+1C0…+33F` | probably frustum data | written at the end of `CalcMatrices` |
| `+340` | FOV (default 0.75, halved before `tan`) | ctor, `CalcMatrices` |
| `+344` | ortho size, 0 = perspective | branch in `CalcMatrices` |
| `+348` | aspect factor | set by `NuCamera::Update` from the screen mode |
| `+34C` / `+350` | near / far (0.1 / 5000) | ctor, `ICameraSource::v12/v13` |
| `+36C`, `+36D` | flags (`+36D` = keep aspect) | `Update` |

The projection has `m[2][3] = +1` and `m[3][2] = −near·…`, i.e. D3D
**left-handed**: the camera looks along **+row2**. Y is up (editor strings
`Cam Y Min/Max`, `Cam_Raise_In_Y`).

**Functions**

| RVA | Our name | Notes |
|---|---|---|
| `1A9370` | `NuCamera::NuCamera()` | |
| `1A93E0` | `NuCamera::CopyTo(NuCamera* dst)` | thiscall, `ret 4`; copies everything incl. derived matrices; 62 callers |
| `1AA900` | `NuCamera::CalcMatrices()` | thiscall, no args; rebuilds `+040…+358` from world/FOV/aspect/near/far |
| `1AD790` | `NuCamera::Update(bool)` | refreshes aspect unless `+36D`, then `CalcMatrices` |
| `508AE0` | `Viewports::SetCameraSource(ICameraSource*, short idx)` | thiscall, `ret 8`; stores the source at `[this+0x13D4+idx*4]` |
| `508F20` | `Viewports::SetCamera(ICameraSource*, short idx)` | wraps `508AE0` |
| `9EA5F0`, `9EA780` | camera manager per-frame code | copy the chosen game camera into the static source, register it with the viewport |
| `A36F10` | active-camera selection | owns the string "Game is running but not setting any valid cameras!" |
| `4592C0` / `459300` | add / remove camera in an 8-entry list at `17C6970` (count `17C6718`) | |

**Camera sources** (what the renderer asks for a camera). Both classes have
15 virtuals; `v2` = `GetCamera()` returns `this+0x10`, `v3` = position,
`v9` FOV, `v12/v13` near/far.

- `SplitScreenGameCameraAdaptor` (vtable `F32D44`): created in pairs by
  `A5C320` (split-screen manager). Its `GetCamera` (`9C7580`) copies the
  gameplay camera's matrix from `[this+4]+0x80`, sets FOV/aspect, then calls
  `NuCamera::Update(1)`, so its camera passes through `CalcMatrices` whenever
  the renderer asks for it.
- `SplitScreenCutsceneCameraAdaptor` (vtable `F32D84`): a static array of two
  at `1806C70` (stride `0x3A0`, ctor `9C8340`); filled by `NuCamera::CopyTo`
  from the camera manager.

**Free camera recipe** (as implemented in the Free Camera mod): remember viewport
0's source from `SetCameraSource`; when `CalcMatrices` runs on that source's
camera, or `CopyTo` writes into it, overwrite `+000` (and `+340`), then let
`CalcMatrices` rebuild the rest.

**Confirmed in game (freecam 0.1.2):** the gameplay source is a
`SplitScreenGameCameraAdaptor`. Most of the renderer does not use its
NuCamera but reads the gameplay camera record `G = [adaptor+4]` through the
adaptor's virtuals: `v3` → `G+0xF0` position, `v4` → `G+0x100` focus,
`v5` → `G+0x130` look-at point (position + forward × 4), `v7` → `G+0x80`
world matrix. `G` has no vtable. A free camera has to write `G` (done in
those virtuals) as well as the NuCamera. The game ran at ~200 fps with
~2 `SetCameraSource` and ~2 `GetCamera` calls per frame.

## Time

`Timer::Update(float dt)` (`4AE010`, timer = `game+0x23C0`, game =
`[17D69CC]`, `HobbitGame`) does `scaled = dt * time_scale` with
`time_scale` a float at `120E16C` (default 1.0, read only here, written by
nothing), smooths it into `timer+0x18`, and `GameFramework::v7` (`4E3390`)
copies that to `game+0x2870`, the dt broadcast to every system. Setting the
time scale near 0 freezes gameplay while the camera and renderer keep
working (used by freecam). `Timer::SetPaused(int)` (`4B0E60`, `timer+0x3C`)
only stops the accumulated game time and pauses audio.

The pause menu path (`A3E380` pause / `A3E5A0` resume, flag `1696E35`) also
opens the menu. `IsPaused()` (`152A20`) = flag `||` an extra condition
function stored at `1695964` (the game installs `4AF550`); chaining that
function did **not** stop gameplay in testing, it is only consulted by hints
and the renderer.

## Render pipes and HUD

`sub_184810` renders the frame: it walks a pointer array of render pipes at
`16A1990` (current index `16A19DC`) and calls `RenderPipe::Render(int)`
(`1838A0`, thiscall, `ret 4`) for each. It also tests the pause flag at
`1696E35`. Inside `Render`, stages are bracketed by strings such as
`"RenderPipe - AFTER_DEFERRED"`, `"RenderPipe = POST_EFFECTS"`,
`"RenderPipe - FINAL_POST_EFFECTS"` and `"RenderPipe - HUD"`. The HUD stage is

```
if (pipe->hud_fn /* +0x74 */) { pipe->stage = 8; ...; pipe->hud_fn(pipe->hud_arg /* +0x98 */); ... }
```

so clearing `+0x74` around the call hides the whole 2D HUD (used by freecam).
The other stage callbacks are at `+0x54 + 4*i` (args `+0x78 + 4*i`), i = 0..8.
In game, pipe 4 had a full set of callbacks, pipe 2 only stage 8
(`17E2D0`), pipe 1 only the HUD stage. The minimap's black backing survives
hiding the HUD stage; its source is still unknown (freecam's F7 layer probe
is meant to find it).

**3D interaction icons** (button glyphs, tool icons over objects): singleton
`MechIcons*` at `167C5F4` (vtable `ECCFCC`); virtual 37 `Suppress(int pipe)`
with `-1` sets bit 1 of `+0xE4`, honoured by the per-frame update (`632220`).
Confirmed in game to hide them.

## Text and 2D drawing

The engine draws text with **QFont** bitmap fonts (module around RVA
`1A2000`-`1A9370`). All functions are `cdecl`; a `NULL` font means the
built-in system font.

| What | RVA | Notes |
|---|---|---|
| GUI font (`QFont*`) | `17992B8` | the game's menu/HUD font; set by the frontend from `[[1799310]+0xAC]` |
| Button-icon font | `17992D0` | `framework-data\gui\Font\buttons_*`; `[[1799310]+0xB0]` |
| System font | `16A325C` | built in (`system_qfont`, embedded data), used by debug text |
| `PushFlags(int)` / `PopFlags()` | `1A6D20` / `1A6D60` | flags at `16A3264`. 2 = screen text; 4 = text placed by a 4x4 matrix (the GUI uses this) |
| `PushMode(int)` / `PopMode()` | `1A6EA0` / `1A6ED0` | coordinate mode at `16A3260`: 3 = -1..1 with y up, 4 = raw virtual units |
| `SpaceFromFont(font)` | `1A70B0` | copies the font's space width to `16A3258`; call before printing |
| `SetScale(font, sx, sy)` | `1A70D0` | without flag 4, y is halved (the virtual space is not square) |
| `SetColour(font, c)` | `1A7150` | `0xAABBGGRR`, alpha `0x80` = opaque |
| `SetPosition(font, x, y, z)` | `1A73A0` | top-left of the line |
| `Print(font, utf8)` | `1A9030` | up to ~1000 glyphs |
| `TextWidth(font, utf8)` -> float | `1A7990` | in the current mode's units |
| `LineHeight(font)` -> float | `1A7050` | negative in modes 3/4 (y up) |

Text and other 2D geometry are not drawn at once: they become render
commands in per-thread frame buffers (TLS slot `+0x6784`), so they must be
issued on the game thread during the frame. The debug overlay shows the
pattern (`sub_150A90`): push flags 2, push mode 3, `SpaceFromFont`,
`SetScale(1,1)`, then `SetColour` / `SetPosition` / `Print` per line, pop both.

**Virtual 2D space** is 640 x 224 units for the whole screen (mode 0 of the
primitive transform: `x/320 - 1`, `1 - y/112`); the engine maps it to the
current viewport and corrects the aspect ratio.

**Frame order** (`sub_184810`, every frame): each render pipe is drawn
(`RenderPipe::Render`), followed by `sub_180080` (screen fade, debug text via
`sub_154310`); the flag `16A19D8` is 1 during this loop. Then `sub_153780`
calls `sub_180080` once more outside the loop and ends the frame in
`sub_1F1B50`, but in normal play that last `sub_180080` call does not
happen. 2D recorded outside a pipe's `sub_180080` (e.g. in `sub_1F1B50`)
never reaches the screen, so the loader draws its UI right after the
in-loop `sub_180080` of the first pipe each frame.

**GUI pass**: the render pipe's HUD stage callback `sub_4C2000` (slot 9,
`+0x74`) tells the HUD systems to draw (stage 8, `sub_4C1BB0`) and then
jumps to the GUI dispatch `sub_3DC790` (also called from the pause screen).
Inside it `sub_3DA570` and `GUI2Manager::Render` (`sub_3E55B0`, vtable slot
15) each wrap their layers (`sub_3D6E60`) in a full-screen viewport guard
(`sub_1A57E0` .. restore `sub_1A58B0`) with the GUI camera
(`[[1799310]+0x60]` set through `sub_152F00` on `1695960`; the current
camera, `1696AC0`, is saved with `sub_1A9370` + `sub_1A93E0` and put back).
The loader draws mod and menu UI just before that restore in
`GUI2Manager::Render`, or in its own copy of the wrapper when the HUD is
hidden.

**GUI text** (menus, HUD numbers): `sub_3E0580(utf8, x, y, z, scale_y,
scale_x, colour, button_font, flags) -> width`, cdecl, in the GUI's own
space (pause menu: x centred on 0, y from about +0.24 down in steps of
0.1067, z 1, scale 0.5). Gold entries use colour `0x00005F7F`; the selected
one pulses grey to white (R = G = B). The in-game pause menu is printed
through it every frame and is not a GUI2 menu; the front-end menus are
(`GUI2Menu::HandleEvent` = `sub_3E0180`, vtable slot 70, selected entry at
`+0x190`, object name string at `+0x0C`).

**2D primitives** (as used by `sub_18045C`):

```
Prim* p;  PrimBegin(&p, 0 /*triangle list*/, 5, &material, vertex_count);   // 1863E0
per vertex:
    fmt = *(u8**)(p+4); w = *(u8**)(p+8);
    *(u32*)(w + *(u16*)(fmt+0xA)) = PrimColour(p, c);   // 185700, thiscall
    w[*(u16*)(fmt+8) + 3] = alpha;
    PrimVertex(p, x, y, z);                              // 401DE0, thiscall, virtual units
PrimSubmit(p);                                           // 185BB0, thiscall
p->vtable[3](p);                                         // Release
```

Default materials (globals holding `Material*`): `16A22A8`
`numtl_defaultmtl2d`, `16A22B8` `numtl_defaultmtl2d_alpha`, `16A22BC`
`numtl_defaultmtl2d_alpha_nozupd` (used for the loader's boxes), `16A22C0`
`numtl_defaulttexturemtl2d`.

## Data files

`GAME.DAT`, `GAME0-3.DAT`, `DLC*.DAT`: TT Games DAT archives (~8.5 GB).
Strings reference loose paths such as `chars\abilities.txt`,
`Audio\Localisation.txt`, `stuff\chasecam\*.txt`, `appdata:pcconfig.txt`.
Whether the game prefers loose files over the archives (the usual route for
data mods in TT games) is **not verified** for this title.

## Known code sites (from Auto Perfect Build)

Taken from that mod's log on this exe (runtime base `0x970000`, converted to
RVAs). The names are that mod's labels, not the game's.

| Label | RVA | Kind |
|---|---|---|
| IsSelectPressed (P1) | `6EB2C3` | 5-byte call patch |
| IsSelectPressed (P2) | `6EB337` | 5-byte call patch |
| OnSlotSelected | `78F49E` | 5-byte patch |
| PotLose (wrong pick) | `765CEF` | 5-byte patch |
| PotLose (timer) | `766158` | 5-byte patch |
| Complete | `765D4A` | 5-byte patch |
| AnimPlayTo (advance) | `72F1E4` | 5-byte patch |
| AnimPlayTo (resume) | `776561` | 5-byte patch |
| PlayBubble::Update | `EF4EC0` | vtable slot = `InstructionBuildPlayBubble` vtable + `0x1C` (index 7) |
| game paused flag | `1696E35` | byte in `.data` |

The `PlayBubble::Update` slot lies inside the RTTI-resolved
`InstructionBuildPlayBubble` vtable, which cross-checks both analyses.

## Next steps

1. Load the exe in Ghidra and apply class names to vtables and their methods
   from `rtti_vtables.py` (a Ghidra script that consumes its JSON is the
   obvious next tool).
2. Find singletons: who constructs `HobbitGame` / `GameFramework`, and the
   globals holding them. That gives mods a way to the game state.
3. Map the `NuClass` reflection data. A loader API like
   `get_field(obj, "Studs")` would serve most gameplay mods.
4. Find the main loop / per-frame update, to give mods a game-thread tick
   that does not depend on rendering.
5. Check whether loose files override `.DAT` contents.
