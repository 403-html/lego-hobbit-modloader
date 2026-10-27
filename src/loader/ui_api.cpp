// API v4: on-screen UI for mods in the game's own look (see hml.h), the
// loader's toasts, and the per-Present UI frame that puts it all together.
#include "internal.h"

#include <cmath>
#include <deque>
#include <string>

namespace hml {
namespace {

// Colours of the game's UI (taken from its pause menu and HUD).
constexpr uint32_t kGold = 0xFFF8B800;
constexpr uint32_t kWhite = 0xFFFFFFFF;
constexpr uint32_t kDim = 0xFFC8C8C8;
constexpr uint32_t kBackdrop = 0xA0000000;

struct Toast {
    std::string title, detail;
    DWORD start, ms;
};
CRITICAL_SECTION g_toast_lock;
bool g_toast_ready = false;
std::deque<Toast> g_toasts;

uint32_t with_alpha(uint32_t argb, float a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return ((uint32_t)((argb >> 24) * a) << 24) | (argb & 0x00FFFFFF);
}

// Selected entries in the game pulse between grey and white.
uint32_t pulse() {
    float t = (std::sin(GetTickCount() * 0.0063f) + 1.0f) * 0.5f;  // about one beat a second
    uint32_t v = 0xAE + (uint32_t)(t * (0xFF - 0xAE));
    return 0xFF000000 | (v << 16) | (v << 8) | v;
}

void draw_toasts() {
    if (!g_toast_ready) return;
    EnterCriticalSection(&g_toast_lock);
    DWORD now = GetTickCount();
    while (!g_toasts.empty() && now - g_toasts.front().start > g_toasts.front().ms) g_toasts.pop_front();
    std::deque<Toast> list = g_toasts;
    LeaveCriticalSection(&g_toast_lock);
    if (list.empty()) return;

    const float u = (float)ui::unit(), ts = u * 2.2f, ds = u * 1.7f, pad = u * 0.8f;
    float y = u * 1.5f;
    for (const Toast& t : list) {
        DWORD age = now - t.start;
        float a = 1.0f;
        if (age < 150) a = age / 150.0f;
        if (t.ms - age < 500) a = (t.ms - age) / 500.0f;
        float tw = 0, th = 0, dw = 0, dh = 0;
        ui::measure_px(ts, t.title.c_str(), &tw, &th);
        if (!t.detail.empty()) ui::measure_px(ds, t.detail.c_str(), &dw, &dh);
        float w = (tw > dw ? tw : dw) + 2 * pad, h = th + dh + pad;
        float x = ui::w() - u * 1.5f - w;
        ui::rect((int)x, (int)y, (int)w, (int)h, with_alpha(kBackdrop, a));
        ui::text_px(ts, x + pad, y + pad / 2, t.title.c_str(), with_alpha(kGold, a));
        if (dh > 0) ui::text_px(ds, x + pad, y + pad / 2 + th, t.detail.c_str(), with_alpha(kWhite, a));
        y += h + u * 0.6f;
    }
}

// ---- API ------------------------------------------------------------------------

void api_ui_text(HmlMod*, float x, float y, const char* s, float size, uint32_t argb, int align) {
    if (!ui::active() || !s) return;
    if (align == HML_ALIGN_CENTER || align == HML_ALIGN_RIGHT) {
        float w = 0;
        ui::measure_px(size, s, &w, nullptr);
        x -= align == HML_ALIGN_CENTER ? w / 2 : w;
    }
    ui::text_px(size, x, y, s, argb);
}

void api_ui_rect(HmlMod*, float x, float y, float w, float h, uint32_t argb) {
    if (!ui::active()) return;
    ui::rect((int)(x + 0.5f), (int)(y + 0.5f), (int)(w + 0.5f), (int)(h + 0.5f), argb);
}

void api_ui_measure(const char* s, float size, float* w, float* h) {
    float fw = 0, fh = 0;
    ui::measure_px(size, s ? s : "", &fw, &fh);
    if (w) *w = fw;
    if (h) *h = fh;
}

uint32_t api_ui_color(int which) {
    switch (which) {
        case HML_UI_GOLD: return kGold;
        case HML_UI_WHITE: return kWhite;
        case HML_UI_SELECTED: return pulse();
        case HML_UI_DIM: return kDim;
        case HML_UI_BACKDROP: return kBackdrop;
        default: return kWhite;
    }
}

void api_ui_toast(HmlMod*, const char* title, const char* detail, unsigned ms) {
    toast(title ? title : "", detail ? detail : "", ms);
}

void api_hud_hide(HmlMod* self, int hidden) { native::hud_hide(self, hidden != 0); }

}  // namespace

void toast(const std::string& title, const std::string& detail, unsigned ms) {
    if (!g_toast_ready) {
        InitializeCriticalSection(&g_toast_lock);
        g_toast_ready = true;
    }
    EnterCriticalSection(&g_toast_lock);
    for (auto it = g_toasts.begin(); it != g_toasts.end(); ++it)
        if (it->title == title) {
            g_toasts.erase(it);
            break;
        }
    g_toasts.push_back({title, detail, GetTickCount(), ms ? ms : 3000});
    while (g_toasts.size() > 4) g_toasts.pop_front();
    LeaveCriticalSection(&g_toast_lock);
}

void ui_api_fill(HmlApi* a) {
    a->ui_text = &api_ui_text;
    a->ui_rect = &api_ui_rect;
    a->ui_measure = &api_ui_measure;
    a->ui_color = &api_ui_color;
    a->ui_toast = &api_ui_toast;
    a->hud_hide = &api_hud_hide;
}

// Once per Present, after HML_EVENT_FRAME: mods' UI, toasts, then the menu.
void ui_frame(void* dev, uint64_t frame) {
    ui::frame_begin(dev);
    if (ui::active()) {
        HmlUiEvent ev{ui::w(), ui::h(), ui::native_now() ? 1 : 0, frame};
        events_dispatch(HML_EVENT_UI, &ev);
    }
    menu_frame(dev);  // input every frame; draws when open
    if (ui::active()) draw_toasts();
    ui::frame_end();
}

}  // namespace hml
