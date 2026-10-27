// Facts about the running game executable.
#include "internal.h"

#include <cstring>

namespace hml {
namespace {

struct KnownBuild {
    HmlGameBuild id;
    uint32_t timestamp;
    uint32_t image_size;
    uint32_t checksum;
    const char* name;
};

// Add new builds here once they are verified.
const KnownBuild kKnownBuilds[] = {
    {HML_BUILD_STEAM_DX9, 0x543C1BFA, 0x01E72000, 0x01CCE17D,
     "LEGO The Hobbit, Steam DX9 (v0.53o_hotfix_7146292)"},
};

HmlGameInfo g_info{};
std::string g_exe_path, g_game_dir, g_mods_dir;
uint8_t* g_base = nullptr;

// Usable before game_init(): DllMain needs the entry point early.
uint8_t* base() {
    if (!g_base) g_base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    return g_base;
}

IMAGE_NT_HEADERS* nt() {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base());
    return reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
}

}  // namespace

bool game_init() {
    base();
    char buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    g_exe_path.assign(buf, n);
    size_t slash = g_exe_path.find_last_of("\\/");
    g_game_dir = slash == std::string::npos ? "." : g_exe_path.substr(0, slash);
    g_mods_dir = path_join(g_game_dir, "mods");

    IMAGE_NT_HEADERS* h = nt();
    g_info.image_base = reinterpret_cast<uintptr_t>(g_base);
    g_info.image_size = h->OptionalHeader.SizeOfImage;
    g_info.timestamp = h->FileHeader.TimeDateStamp;
    g_info.exe_path = g_exe_path.c_str();
    g_info.game_dir = g_game_dir.c_str();
    g_info.mods_dir = g_mods_dir.c_str();
    g_info.build = HML_BUILD_UNKNOWN;
    g_info.build_name = "unknown build";

    for (const KnownBuild& b : kKnownBuilds) {
        if (b.timestamp == h->FileHeader.TimeDateStamp &&
            b.image_size == h->OptionalHeader.SizeOfImage &&
            b.checksum == h->OptionalHeader.CheckSum) {
            g_info.build = b.id;
            g_info.build_name = b.name;
            break;
        }
    }
    return h->FileHeader.Machine == IMAGE_FILE_MACHINE_I386;
}

const HmlGameInfo* game_info() { return &g_info; }

bool game_section(const char* name, Section* out) {
    IMAGE_NT_HEADERS* h = nt();
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(h);
    for (WORD i = 0; i < h->FileHeader.NumberOfSections; ++i, ++s) {
        char sname[9] = {};
        std::memcpy(sname, s->Name, 8);
        if (std::strcmp(sname, name) == 0) {
            out->begin = g_base + s->VirtualAddress;
            out->end = out->begin + s->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

void* game_entry_point() { return base() + nt()->OptionalHeader.AddressOfEntryPoint; }

void* game_rva(uint32_t rva) {
    if (rva >= g_info.image_size) return nullptr;
    return g_base + rva;
}

void** game_iat_slot(const char* dll, const char* func) {
    IMAGE_DATA_DIRECTORY dir = nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(g_base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(g_base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            g_base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(g_base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(g_base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(ibn->Name), func) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    return (last == '\\' || last == '/') ? a + b : a + "\\" + b;
}

}  // namespace hml
