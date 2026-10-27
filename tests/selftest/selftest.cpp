// selftest - exercises the loader API at startup and logs PASS/FAIL lines.
// Built with -DHML_BUILD_TESTS=ON; not meant to be shipped to players.
// Written in C++ on purpose: checks that a C++ mod exports unmangled names.
#include <windows.h>

#include <hml/hml.h>

#include <cstring>
#include <string>

HML_MOD("selftest", "Loader self-test", "0.1.0", "HML", "Checks loader APIs; dev only")

namespace {

const HmlApi* api;
HmlMod* self;
int failures = 0;
std::string order;

void check(bool ok, const char* what) {
    api->log(self, ok ? HML_LOG_INFO : HML_LOG_ERROR, "%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// Two detours on the same function, to test chaining. The game calls
// SteamAPI_RestartAppIfNecessary(appid) early in WinMain.
using Restart_t = bool(__cdecl*)(uint32_t);
Restart_t orig_a = nullptr, orig_b = nullptr;
bool __cdecl detour_a(uint32_t id) {
    order += "A";
    return orig_a(id);
}
bool __cdecl detour_b(uint32_t id) {
    order += "B";
    bool r = orig_b(id);
    api->log(self, HML_LOG_INFO, "SteamAPI_RestartAppIfNecessary(%u) -> %d, call order %s", id,
             (int)r, order.c_str());
    check(order == "BA", "chained hooks run newest first, then older, then the game");
    return r;
}

void on_shutdown(HmlEvent, const void*, void*) {
    check(!order.empty(), "hooked function was called by the game");
    api->log(self, failures ? HML_LOG_ERROR : HML_LOG_INFO, "selftest done, %d failure(s)",
             failures);
}

}  // namespace

int hml_mod_init(const HmlApi* a, HmlMod* s) {
    api = a;
    self = s;

    check(a->size >= sizeof(HmlApi) && a->version == HML_API_VERSION, "API table size/version");
    check(a->game->build == HML_BUILD_STEAM_DX9, "game build recognised");

    // rva()
    auto* mz = static_cast<const char*>(a->rva(0));
    check(mz && mz[0] == 'M' && mz[1] == 'Z', "rva(0) points at the MZ header");
    check(a->rva(a->game->image_size) == nullptr, "rva() rejects out-of-image offsets");

    // RTTI
    void** vt = a->find_vtable("HobbitGame");
    check(vt && (uintptr_t)vt - a->game->image_base == 0x00E1D274, "find_vtable(HobbitGame) == rva E1D274");
    check(a->find_vtable(".?AVGameFramework@@") == a->find_vtable("GameFramework"),
          "find_vtable accepts mangled and plain names");
    check(a->find_vtable("NoSuchClass") == nullptr, "find_vtable unknown -> NULL");
    struct Fake { void** vtable; } fake{vt};
    const char* cn = a->class_name_of(&fake);
    check(cn && std::strcmp(cn, "HobbitGame") == 0, "class_name_of(fake HobbitGame object)");
    int not_an_object = 1234;
    check(a->class_name_of(&not_an_object) == nullptr, "class_name_of(non-object) -> NULL");
    check(a->class_name_of(reinterpret_cast<void*>(0x10)) == nullptr, "class_name_of(bad ptr) -> NULL");

    // Pattern scan: the exe entry point (call security_init_cookie; jmp tmainCRTStartup).
    void* ep = a->find_pattern("E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? CC CC CC CC CC 83");
    check(ep && (uintptr_t)ep - a->game->image_base == 0x00BBEFC1, "find_pattern finds the entry point");
    check(a->find_pattern("ZZ") == nullptr, "find_pattern rejects a malformed pattern");

    // vtable hook: swap a slot and swap it back.
    void* slot_orig = nullptr;
    void* before = vt[1];
    check(a->hook_vtable(self, vt, 1, reinterpret_cast<void*>(&detour_a), &slot_orig) == HML_OK &&
              slot_orig == before && vt[1] == reinterpret_cast<void*>(&detour_a),
          "hook_vtable swaps the slot and returns the old entry");
    check(a->patch_bytes(self, &vt[1], &before, sizeof before) == HML_OK && vt[1] == before,
          "patch_bytes writes read-only memory");

    // Function hooks with chaining.
    HMODULE steam = GetModuleHandleA("steam_api.dll");
    void* restart = steam ? (void*)GetProcAddress(steam, "SteamAPI_RestartAppIfNecessary") : nullptr;
    check(restart != nullptr, "steam_api.dll!SteamAPI_RestartAppIfNecessary found");
    if (restart) {
        check(a->hook_function(self, restart, (void*)&detour_a, (void**)&orig_a) == HML_OK, "hook_function #1");
        check(a->hook_function(self, restart, (void*)&detour_b, (void**)&orig_b) == HML_OK, "hook_function #2 (chained)");
    }
    check(a->hook_function(self, nullptr, (void*)&detour_a, (void**)&orig_a) == HML_ERR_BAD_ARG,
          "hook_function(NULL) -> HML_ERR_BAD_ARG");

    // Config defaults when the ini is missing.
    char buf[16];
    check(a->config_get_int(self, "Missing", 42) == 42, "config_get_int default");
    check(a->config_get_string(self, "Missing", "dflt", buf, sizeof buf) == 4 &&
              std::strcmp(buf, "dflt") == 0, "config_get_string default");
    check(a->d3d9_device() == nullptr, "no D3D device before the game creates one");

    a->subscribe(self, HML_EVENT_SHUTDOWN, on_shutdown, nullptr);
    return HML_OK;
}
