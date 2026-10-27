// Function detours (MinHook), vtable slot swaps and byte patches, all tracked
// per mod so they can be logged and undone on shutdown.
#include "internal.h"

#include <MinHook.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace hml {
namespace {

CRITICAL_SECTION g_lock;
bool g_ready = false;

struct FunctionHook {
    HmlMod* owner;
    void* target;    // the function the mod asked for
    void* patch_at;  // what MinHook actually patched (that function or an older detour)
    void* detour;
};
std::vector<FunctionHook> g_functions;
// game function -> newest detour installed on it, used to chain hooks.
std::map<void*, void*> g_chain_head;

struct SavedBytes {
    HmlMod* owner;
    void* address;
    std::vector<uint8_t> original;
    std::vector<uint8_t> written;
};
std::vector<SavedBytes> g_patches;  // byte patches and vtable slots, in install order

const char* owner_id(HmlMod* m) { return m ? m->id.c_str() : "?"; }

struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

}  // namespace

bool hooks_init() {
    InitializeCriticalSection(&g_lock);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        LOG_E("MinHook init failed: %s", MH_StatusToString(s));
        return false;
    }
    g_ready = true;
    return true;
}

bool write_protected(void* dst, const void* src, size_t size) {
    DWORD old;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, size);
    VirtualProtect(dst, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    return true;
}

int hooks_function(HmlMod* owner, void* target, void* detour, void** original) {
    if (!g_ready) return HML_ERR_GENERIC;
    if (!target || !detour || !original) return HML_ERR_BAD_ARG;
    Lock lock;

    // Chain: if this function is already detoured, hook the newest detour
    // instead. The new detour runs first; its original() reaches the older one.
    void* patch_at = target;
    auto head = g_chain_head.find(target);
    if (head != g_chain_head.end()) patch_at = head->second;

    MH_STATUS s = MH_CreateHook(patch_at, detour, original);
    if (s == MH_OK) s = MH_EnableHook(patch_at);
    if (s != MH_OK) {
        LOG_E("[%s] hook at %p failed: %s", owner_id(owner), target, MH_StatusToString(s));
        return HML_ERR_HOOK_FAILED;
    }
    g_chain_head[target] = detour;
    g_functions.push_back({owner, target, patch_at, detour});
    LOG_D("%s hooked function %p%s", owner_id(owner), target,
          patch_at != target ? " (chained)" : "");
    return HML_OK;
}

int hooks_vtable(HmlMod* owner, void** vtable, int index, void* detour, void** original) {
    if (!vtable || index < 0 || !detour || !original) return HML_ERR_BAD_ARG;
    Lock lock;
    void** slot = vtable + index;
    void* previous = *slot;
    SavedBytes saved{owner, slot, std::vector<uint8_t>(sizeof(void*)), std::vector<uint8_t>(sizeof(void*))};
    std::memcpy(saved.original.data(), slot, sizeof(void*));
    std::memcpy(saved.written.data(), &detour, sizeof(void*));
    // Publish *original before the slot so a racing call never sees a null.
    *original = previous;
    if (!write_protected(slot, &detour, sizeof(void*))) {
        LOG_E("[%s] vtable write at %p failed (%lu)", owner_id(owner), (void*)slot,
              GetLastError());
        return HML_ERR_HOOK_FAILED;
    }
    g_patches.push_back(std::move(saved));
    LOG_D("%s hooked vtable %p[%d]", owner_id(owner), (void*)vtable, index);
    return HML_OK;
}

int hooks_patch(HmlMod* owner, void* address, const void* bytes, size_t size) {
    if (!address || !bytes || !size) return HML_ERR_BAD_ARG;
    Lock lock;
    SavedBytes saved{owner, address, std::vector<uint8_t>(size),
                     std::vector<uint8_t>(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + size)};
    std::memcpy(saved.original.data(), address, size);
    if (!write_protected(address, bytes, size)) {
        LOG_E("[%s] patch at %p failed (%lu)", owner_id(owner), address, GetLastError());
        return HML_ERR_HOOK_FAILED;
    }
    g_patches.push_back(std::move(saved));
    LOG_D("%s patched %u bytes at %p", owner_id(owner), (unsigned)size, address);
    return HML_OK;
}

// Hot reload: take back everything one mod installed. Fails (and changes
// nothing) when another mod has since hooked on top of one of its detours,
// because that hook lives in the code about to be unloaded.
bool hooks_remove_owner(HmlMod* owner, void*, bool check_only, std::string* why) {
    if (!g_ready) return true;
    Lock lock;
    for (const FunctionHook& h : g_functions) {
        if (h.owner != owner) continue;
        auto head = g_chain_head.find(h.target);
        if (head == g_chain_head.end() || head->second != h.detour) {
            if (why) *why = "another mod hooked the same game function after it";
            return false;
        }
    }
    if (check_only) return true;
    for (auto it = g_functions.rbegin(); it != g_functions.rend(); ++it) {
        if (it->owner != owner) continue;
        MH_DisableHook(it->patch_at);
        MH_RemoveHook(it->patch_at);
        if (it->patch_at == it->target) g_chain_head.erase(it->target);
        else g_chain_head[it->target] = it->patch_at;
    }
    g_functions.erase(std::remove_if(g_functions.begin(), g_functions.end(),
                                     [owner](const FunctionHook& h) { return h.owner == owner; }),
                      g_functions.end());
    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it) {
        if (it->owner != owner) continue;
        if (std::memcmp(it->address, it->written.data(), it->written.size()) == 0)
            write_protected(it->address, it->original.data(), it->original.size());
        else
            LOG_W("[%s] bytes at %p were changed again since; left as they are", owner_id(owner), it->address);
    }
    g_patches.erase(std::remove_if(g_patches.begin(), g_patches.end(),
                                   [owner](const SavedBytes& p) { return p.owner == owner; }),
                    g_patches.end());
    return true;
}

void hooks_shutdown() {
    if (!g_ready) return;
    Lock lock;
    // Undo in reverse install order so overlapping patches restore cleanly.
    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it)
        write_protected(it->address, it->original.data(), it->original.size());
    g_patches.clear();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_functions.clear();
    g_chain_head.clear();
    g_ready = false;
}

}  // namespace hml
