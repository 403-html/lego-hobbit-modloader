// Development probe for the game's GUI2 menus (pause menu, options...).
// Logs what the game's menu system draws and does, so a "Mods" entry can be
// added to the game's own menus. Enabled with [Loader] GuiProbe=1; capped to a
// few hundred log lines per run.
//
//   sub_3E0580  GUI text print: (utf8 text, x, y, z, sy, sx, colour, button_font, flags) -> width
//   sub_3E0180  GUI2Menu::HandleEvent (vtable slot 70): (int event, void* data), thiscall
//               menu+0x190 = selected GUI2MenuEntry; events 8..11 = navigation
#include "internal.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>

namespace hml {
namespace probe {
namespace {

constexpr uint32_t RVA_GUI_TEXT = 0x3E0580;
constexpr uint32_t RVA_MENU_EVENT = 0x3E0180;
constexpr uint32_t RVA_PAUSED = 0x1696E35;

int g_lines = 0;
constexpr int kMaxLines = 600;
CRITICAL_SECTION g_lock;

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

bool budget() { return g_lines < kMaxLines; }
#define PLOG(...)                       \
    do {                                \
        if (budget()) {                 \
            ++g_lines;                  \
            LOG_I("[probe] " __VA_ARGS__); \
        }                               \
    } while (0)

std::string printable(const char* s, size_t max = 60) {
    std::string o;
    if (!readable(s, 1)) return o;
    for (size_t i = 0; i < max; ++i) {
        if (!readable(s + i, 1)) break;
        unsigned char c = (unsigned char)s[i];
        if (!c) return o;
        if (c < 32 && c != '\t') return std::string();
        o += (char)c;
    }
    return o;
}

// Pointers inside an object: other engine objects (by RTTI) and C strings.
void dump_object(const char* what, const void* obj, size_t size) {
    const char* cls = rtti_class_name_of(obj);
    PLOG("%s %p (%s)", what, obj, cls ? cls : "?");
    for (size_t off = 0; off < size; off += 4) {
        const uint8_t* p = (const uint8_t*)obj + off;
        if (!readable(p, 4)) break;
        uint32_t v = *(const uint32_t*)p;
        void* vp = (void*)(uintptr_t)v;
        if (v < 0x10000) continue;
        if (const char* c = rtti_class_name_of(vp)) {
            PLOG("    +%03X -> %p %s", (unsigned)off, vp, c);
            continue;
        }
        std::string s = printable((const char*)vp);
        if (s.size() >= 3) PLOG("    +%03X -> \"%s\"", (unsigned)off, s.c_str());
    }
}

// ---- GUI text --------------------------------------------------------------------
using GuiText_t = float(__cdecl*)(const char*, float, float, float, float, float, uint32_t, int, int);
GuiText_t o_gui_text = nullptr;
std::set<std::string> g_seen_text;

float __cdecl hk_gui_text(const char* t, float x, float y, float z, float a, float b, uint32_t col,
                          int btn, int flags) {
    float r = o_gui_text(t, x, y, z, a, b, col, btn, flags);
    if (budget()) {
        std::string s = printable(t, 80);
        EnterCriticalSection(&g_lock);
        bool fresh = g_seen_text.size() < 400 && g_seen_text.insert(s).second;
        LeaveCriticalSection(&g_lock);
        if (fresh) {
            PLOG("text \"%s\" at (%.3f, %.3f, %.3f) scale %.3f/%.3f colour %08X btn %d flags %d -> w %.3f",
                 s.c_str(), x, y, z, a, b, col, btn, flags, r);
            // likely callers: code addresses on the stack above us
            Section text{};
            if (game_section(".text", &text)) {
                const uint32_t* sp = reinterpret_cast<const uint32_t*>(&t);
                uintptr_t base = (uintptr_t)game_rva(0);
                char line[256];
                int n = 0, len = 0;
                for (int i = 0; i < 160 && n < 10 && readable(sp + i, 4); ++i) {
                    uint8_t* v = (uint8_t*)(uintptr_t)sp[i];
                    if (v > text.begin + 5 && v < text.end && v[-5] == 0xE8) {  // after a call
                        len += std::snprintf(line + len, sizeof line - len, " %06X", (unsigned)((uintptr_t)v - base));
                        ++n;
                    }
                }
                if (n) PLOG("    callers:%s", line);
            }
        }
    }
    return r;
}

// ---- GUI2Menu events ---------------------------------------------------------------
using MenuEvent_t = void(__fastcall*)(void*, void*, int, void*);
MenuEvent_t o_menu_event = nullptr;
std::set<std::pair<void*, int>> g_seen_ev;
std::set<void*> g_seen_menu, g_seen_entry;
int g_nav_logs = 0;
uint8_t g_paused_prev = 0;

void __fastcall hk_menu_event(void* menu, void* edx, int ev, void* data) {
    void* before = readable((uint8_t*)menu + 0x190, 4) ? *(void**)((uint8_t*)menu + 0x190) : nullptr;
    o_menu_event(menu, edx, ev, data);
    if (!budget()) return;
    EnterCriticalSection(&g_lock);
    uint8_t paused = *(volatile uint8_t*)game_rva(RVA_PAUSED);
    if (paused != g_paused_prev) {
        PLOG("paused flag %d -> %d", g_paused_prev, paused);
        g_paused_prev = paused;
    }
    if (g_seen_menu.insert(menu).second) dump_object("menu", menu, 0x1C0);
    if (g_seen_ev.insert({menu, ev}).second) PLOG("menu %p event %d data %p", menu, ev, data);
    void* after = *(void**)((uint8_t*)menu + 0x190);
    if ((ev >= 8 && ev <= 11) || after != before) {
        if (g_nav_logs++ < 80) {
            const float* m = after && readable((uint8_t*)after + 0x90, 64) ? (const float*)((uint8_t*)after + 0x90) : nullptr;
            PLOG("menu %p event %d: selected %p -> %p%s pos (%.3f, %.3f, %.3f)", menu, ev, before, after,
                 after ? "" : " (none)", m ? m[12] : 0.f, m ? m[13] : 0.f, m ? m[14] : 0.f);
        }
        if (after && g_seen_entry.insert(after).second) dump_object("  entry", after, 0x200);
    }
    LeaveCriticalSection(&g_lock);
}

}  // namespace

void install() {
    InitializeCriticalSection(&g_lock);
    if (game_info()->build != HML_BUILD_STEAM_DX9) return;
    if (!GetPrivateProfileIntA("Loader", "GuiProbe", 0, loader_config().ini_path.c_str())) return;
    hooks_function(loader_self(), game_rva(RVA_GUI_TEXT), (void*)&hk_gui_text, (void**)&o_gui_text);
    hooks_function(loader_self(), game_rva(RVA_MENU_EVENT), (void*)&hk_menu_event, (void**)&o_menu_event);
    LOG_I("GUI probe on: logging the game's menus (up to %d lines)", kMaxLines);
}

}  // namespace probe
}  // namespace hml
