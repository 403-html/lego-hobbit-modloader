// Loader bootstrap.
//
// Windows loads dinput8.dll (us) while resolving LEGOHobbit.exe's imports,
// inside DllMain under the loader lock, where loading other DLLs is unsafe.
// So DllMain only patches the exe's entry point with a jump to entry_stub.
// When the main thread reaches the entry point, every import is initialised,
// the loader lock is free, and no game code (not even the CRT) has run yet:
// that is where mods are loaded. The stub then restores the original bytes
// and jumps back to the real entry point with all registers intact.
#include "internal.h"

#include <MinHook.h>

#include <cstdlib>
#include <cstring>

namespace hml {
namespace {

LoaderConfig g_config;
uint8_t g_entry_saved[5];
volatile LONG g_started = 0;
volatile LONG g_shut_down = 0;

using ExitProcess_t = void(WINAPI*)(UINT);
ExitProcess_t o_exit_process = nullptr;

HmlLogLevel parse_level(const char* s) {
    if (!_stricmp(s, "debug")) return HML_LOG_DEBUG;
    if (!_stricmp(s, "warn")) return HML_LOG_WARN;
    if (!_stricmp(s, "error")) return HML_LOG_ERROR;
    return HML_LOG_INFO;
}

// Written next to the exe on first start, with every setting explained.
const char kDefaultIni[] =
    "; Hobbit Mod Loader settings. Lives next to LEGOHobbit.exe.\r\n"
    "[Loader]\r\n"
    "; 0 = load no mods (the game still runs normally through the loader)\r\n"
    "Enabled=1\r\n"
    "; 1 = open a console window that shows the log live\r\n"
    "Console=0\r\n"
    "; debug | info | warn | error\r\n"
    "LogLevel=info\r\n"
    "; 1 = also load plain *.asi / non-HML DLLs from mods\\ (no API, just LoadLibrary)\r\n"
    "LoadAsi=1\r\n"
    "; Comma-separated mod ids or file names to skip, e.g.  Disabled=hello, oldmod.asi\r\n"
    "; (the Mods menu edits this when you switch a mod off)\r\n"
    "Disabled=\r\n"
    "; The Mods menu is in the game's pause menu. Shortcut that opens it anywhere\r\n"
    "; (virtual-key code, 0x70 = F1) and gamepad combo\r\n"
    "; (XInput bits: Back 0x20 + Start 0x10)\r\n"
    "MenuKey=0x70\r\n"
    "MenuGamepad=0x30\r\n"
    "; 1 = draw the Mods menu with the game's own font (0 = plain overlay font)\r\n"
    "GameFont=1\r\n"
    "; 1 = reload a mod when its DLL in mods\\ changes, and load new DLLs (hot reload)\r\n"
    "HotReload=1\r\n"
    "; 1 = add \"Mods\" to the game's pause menu, as entry number PauseMenuPosition+1\r\n"
    "; (2 = right below Options). The pause menu is found by its first entry in the\r\n"
    "; game's own language; PauseMenuFirst overrides that text if ever needed.\r\n"
    "PauseMenuEntry=1\r\n"
    "PauseMenuPosition=2\r\n"
    "PauseMenuFirst=\r\n"
    "; 1 = log what the game's own menus draw and do to hml.log (development aid)\r\n"
    "GuiProbe=0\r\n"
;

void write_default_ini(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;  // already there
    DWORD written;
    WriteFile(h, kDefaultIni, sizeof kDefaultIni - 1, &written, nullptr);
    CloseHandle(h);
}

void read_config() {
    std::string ini = path_join(game_info()->game_dir, "hml.ini");
    g_config.ini_path = ini;
    write_default_ini(ini);
    const char* s = "Loader";
    char buf[1024];
    GetPrivateProfileStringA(s, "MenuKey", "0x70", buf, sizeof buf, ini.c_str());
    g_config.menu_key = (unsigned)strtoul(buf, nullptr, 0);
    GetPrivateProfileStringA(s, "MenuGamepad", "0x30", buf, sizeof buf, ini.c_str());
    g_config.menu_pad = (unsigned)strtoul(buf, nullptr, 0);
    g_config.enabled = GetPrivateProfileIntA(s, "Enabled", 1, ini.c_str()) != 0;
    g_config.console = GetPrivateProfileIntA(s, "Console", 0, ini.c_str()) != 0;
    g_config.load_asi = GetPrivateProfileIntA(s, "LoadAsi", 1, ini.c_str()) != 0;
    GetPrivateProfileStringA(s, "LogLevel", "info", buf, sizeof buf, ini.c_str());
    g_config.log_level = parse_level(buf);
    GetPrivateProfileStringA(s, "Disabled", "", buf, sizeof buf, ini.c_str());
    g_config.disabled = buf;
}

void shutdown_once() {
    if (InterlockedExchange(&g_shut_down, 1)) return;
    LOG_I("game exiting, shutting down mods");
    native::on_exit();
    mods_shutdown_all();
    LOG_I("bye");
    log_shutdown();
}

void WINAPI hk_exit_process(UINT code) {
    shutdown_once();
    o_exit_process(code);
}

}  // namespace

const LoaderConfig& loader_config() { return g_config; }

// Everything that needs a free loader lock. Runs once.
void startup() {
    if (InterlockedExchange(&g_started, 1)) return;

    bool x86 = game_init();
    read_config();
    log_init(path_join(game_info()->game_dir, "hml.log"), g_config.console, g_config.log_level);

    const HmlGameInfo* g = game_info();
    LOG_I("Hobbit Mod Loader %s (API v%d)", HML_LOADER_VERSION, HML_API_VERSION);
    LOG_I("exe: %s", g->exe_path);
    LOG_I("base %08X, image size %08X, timestamp %08X -> %s", (unsigned)g->image_base,
          g->image_size, g->timestamp, g->build_name);
    if (!x86) {
        LOG_E("not a 32-bit game executable; mods disabled");
        return;
    }
    if (g->build == HML_BUILD_UNKNOWN)
        LOG_W("unrecognised game build: mods relying on fixed addresses should stay inactive");
    if (!g_config.enabled) {
        LOG_I("disabled in hml.ini ([Loader] Enabled=0)");
        return;
    }
    if (!hooks_init()) return;

    if (HMODULE k32 = GetModuleHandleA("kernel32.dll"))
        if (void* fn = (void*)GetProcAddress(k32, "ExitProcess"))
            hooks_function(loader_self(), fn, (void*)&hk_exit_process, (void**)&o_exit_process);
    d3d9_install();
    native::install();
    pause::install();
    probe::install();
    input_init();
    mods_load_all();
}

void on_process_detach() {
    // Normal exits go through ExitProcess and were handled already. Here other
    // DLLs (including mods) may be gone, so only close the log.
    if (!g_shut_down) {
        LOG_I("process detaching");
        log_shutdown();
    }
}

}  // namespace hml

// ---- entry point trampoline ------------------------------------------------

extern "C" {
void* hml_original_entry = nullptr;

#if defined(__GNUC__)
__attribute__((force_align_arg_pointer))
#endif
void hml_on_entry() {
    hml::write_protected(hml_original_entry, hml::g_entry_saved, sizeof hml::g_entry_saved);
    hml::startup();
}
}

#if defined(_MSC_VER)
extern "C" __declspec(naked) void entry_stub() {
    __asm {
        pushad
        pushfd
        call hml_on_entry
        popfd
        popad
        jmp dword ptr [hml_original_entry]
    }
}
#else
extern "C" void entry_stub();
__asm__(
    ".text\n"
    ".globl _entry_stub\n"
    "_entry_stub:\n"
    "    pushal\n"
    "    pushfl\n"
    "    call _hml_on_entry\n"
    "    popfl\n"
    "    popal\n"
    "    jmp *_hml_original_entry\n");
#endif

static bool patch_entry_point() {
    uint8_t* entry = static_cast<uint8_t*>(hml::game_entry_point());
    hml_original_entry = entry;
    std::memcpy(hml::g_entry_saved, entry, sizeof hml::g_entry_saved);
    uint8_t jmp[5] = {0xE9};
    int32_t rel = (int32_t)((uint8_t*)&entry_stub - (entry + 5));
    std::memcpy(jmp + 1, &rel, 4);
    return hml::write_protected(entry, jmp, sizeof jmp);
}

// Called from the proxy exports: if we were somehow loaded after the entry
// point already ran (e.g. injected late), start up on first use instead.
extern "C" void hml_ensure_started() { hml::startup(); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (!patch_entry_point()) {
            OutputDebugStringA("[HML] could not patch the game entry point\n");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        hml::on_process_detach();
    }
    return TRUE;
}
