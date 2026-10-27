// MSVC x86 RTTI walker.
//
// LEGOHobbit.exe was built with RTTI enabled (6.7k type names survive). Layout:
//   vtable[-1]              -> RTTICompleteObjectLocator (COL, in .rdata)
//   COL { sig=0, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor* }
//   TypeDescriptor { vftable*, spare, char name[] = ".?AVHobbitGame@@" } (.data)
// On x86 these are absolute pointers, already relocated by the Windows loader.
#include "internal.h"

#include <cstring>
#include <unordered_map>

namespace hml {
namespace {

struct Col {
    uint32_t signature;
    uint32_t offset;
    uint32_t cd_offset;
    const char* type_descriptor;  // points at TypeDescriptor
    const void* hierarchy;
};

CRITICAL_SECTION g_lock;
bool g_built = false;
std::unordered_map<std::string, void**> g_vtables;          // ".?AVName@@" -> vtable
std::unordered_map<const void*, std::string> g_pretty;      // TypeDescriptor -> "Name"
Section g_text{}, g_rdata{}, g_data{};

bool in(const Section& s, const void* p, size_t n = 1) {
    // Compare as integers: arbitrary dwords (e.g. 0xFFFFFFFF) must not wrap.
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    uintptr_t b = reinterpret_cast<uintptr_t>(s.begin), e = reinterpret_cast<uintptr_t>(s.end);
    return a >= b && a < e && n <= e - a;
}

const char* td_name(const char* td) {
    const char* name = td + 8;  // skip vftable* and spare
    if (!in(g_data, name, 5) || std::strncmp(name, ".?A", 3) != 0) return nullptr;
    return name;
}

std::string pretty(const char* mangled) {
    // ".?AVHobbitGame@@" -> "HobbitGame"; nested names keep MSVC order "Inner@Outer".
    std::string s(mangled + 4);
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, "@@") == 0) s.resize(s.size() - 2);
    return s;
}

void build() {
    if (!game_section(".text", &g_text) || !game_section(".rdata", &g_rdata) ||
        !game_section(".data", &g_data)) {
        LOG_W("rtti: exe sections not found, RTTI lookups disabled");
        return;
    }
    // Every dword in .rdata that points at a valid COL (offset 0, i.e. the
    // complete object's primary vtable) and is followed by a code pointer is
    // the slot just before a vtable.
    auto first = reinterpret_cast<uintptr_t*>(g_rdata.begin);
    auto last = reinterpret_cast<uintptr_t*>(g_rdata.end) - 2;
    for (uintptr_t* p = first; p < last; ++p) {
        auto col = reinterpret_cast<const Col*>(p[0]);
        if (!in(g_rdata, col, sizeof(Col))) continue;
        if (col->signature != 0 || col->offset != 0) continue;
        if (!in(g_text, reinterpret_cast<void*>(p[1]))) continue;
        if (!in(g_data, col->type_descriptor, 12)) continue;
        const char* name = td_name(col->type_descriptor);
        if (!name) continue;
        g_vtables.emplace(name, reinterpret_cast<void**>(p + 1));
    }
    LOG_D("rtti: indexed %u class vtables", (unsigned)g_vtables.size());
}

void ensure_built() {
    static bool lock_ready = [] {
        InitializeCriticalSection(&g_lock);
        return true;
    }();
    (void)lock_ready;
    EnterCriticalSection(&g_lock);
    if (!g_built) {
        build();
        g_built = true;
    }
    LeaveCriticalSection(&g_lock);
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const uint8_t*>(p) + n <= end;
}

}  // namespace

void** rtti_find_vtable(const char* class_name) {
    if (!class_name || !*class_name) return nullptr;
    ensure_built();
    if (std::strncmp(class_name, ".?A", 3) == 0) {
        auto it = g_vtables.find(class_name);
        return it == g_vtables.end() ? nullptr : it->second;
    }
    for (const char* prefix : {".?AV", ".?AU"}) {
        auto it = g_vtables.find(std::string(prefix) + class_name + "@@");
        if (it != g_vtables.end()) return it->second;
    }
    return nullptr;
}

const char* rtti_class_name_of(const void* obj) {
    ensure_built();
    if (!obj || !readable(obj, sizeof(void*))) return nullptr;
    auto vtable = *static_cast<void** const*>(obj);
    if (!in(g_rdata, vtable - 1, sizeof(void*))) return nullptr;
    auto col = static_cast<const Col*>(vtable[-1]);
    if (!in(g_rdata, col, sizeof(Col)) || col->signature != 0) return nullptr;
    if (!in(g_data, col->type_descriptor, 12)) return nullptr;
    const char* name = td_name(col->type_descriptor);
    if (!name) return nullptr;
    EnterCriticalSection(&g_lock);
    auto it = g_pretty.find(col->type_descriptor);
    if (it == g_pretty.end()) it = g_pretty.emplace(col->type_descriptor, pretty(name)).first;
    const char* result = it->second.c_str();  // node-stable for the process lifetime
    LeaveCriticalSection(&g_lock);
    return result;
}

}  // namespace hml
