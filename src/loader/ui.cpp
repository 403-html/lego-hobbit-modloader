// On-screen UI of the loader and of mods (API v4). Each Present builds one
// display list: mods draw into it during HML_EVENT_UI, then the loader's
// toasts and Mods menu go on top. When the game renderer is available
// (native_ui.cpp) the list is drawn by the game in its GUI pass, with its own
// font; otherwise it is replayed here at Present: text through d3dx9_43.dll
// (loaded by the game), boxes as alpha quads, device state saved and restored.
#include "internal.h"

#include <d3d9.h>
#include <d3dx9core.h>

#include <map>

namespace hml {
namespace ui {
namespace {

using CreateFontA_t = HRESULT(WINAPI*)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD,
                                       DWORD, DWORD, DWORD, LPCSTR, ID3DXFont**);
IDirect3DDevice9* g_dev = nullptr;
ID3DXFont* g_fonts[3] = {};  // Body, Title, Small
std::map<int, ID3DXFont*> g_sized;  // fallback fonts for API text sizes (px)
IDirect3DStateBlock9* g_state = nullptr;
int g_w = 1920, g_h = 1080;
bool g_in = false;
bool g_native = false;          // this frame's list goes to the game renderer
bool g_classic = false;         // forced plain overlay
std::vector<Op> g_ops;

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
};

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void read_size() {
    IDirect3DSurface9* bb = nullptr;
    if (g_dev && SUCCEEDED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC d;
        bb->GetDesc(&d);
        g_w = (int)d.Width;
        g_h = (int)d.Height;
        bb->Release();
    }
}

bool preview() {
    static int p = -1;
    if (p < 0) {
        char b[8] = {};
        p = GetEnvironmentVariableA("HML_PREVIEW_GAME_STYLE", b, sizeof b) && b[0] == '1';
    }
    return p == 1;
}

void create_fonts() {
    HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll");
    auto create = d3dx ? (CreateFontA_t)(void*)GetProcAddress(d3dx, "D3DXCreateFontA") : nullptr;
    if (!create) {
        LOG_W("d3dx9_43.dll not available: the Mods menu cannot draw text");
        return;
    }
    int u = unit();
    const int sizes[3] = {u * 14 / 10, u * 20 / 10, u * 12 / 10};
    // HML_PREVIEW_GAME_STYLE=1 (development): lay the menu out as for the
    // game's font, drawn with a stand-in font, to check the layout without the game.
    if (preview()) {
        const int big[3] = {u * 31 / 10, u * 40 / 10, u * 19 / 10};
        for (int i = 0; i < 3; ++i)
            create(g_dev, big[i], 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Georgia", &g_fonts[i]);
        return;
    }
    const UINT weights[3] = {FW_NORMAL, FW_BOLD, FW_NORMAL};
    for (int i = 0; i < 3; ++i)
        create(g_dev, sizes[i], 0, weights[i], 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI", &g_fonts[i]);
}

ID3DXFont* font(Font f) { return g_fonts[f == Font::Title ? 1 : f == Font::Small ? 2 : 0]; }

// Fallback font for a pixel size (API text), made on first use.
ID3DXFont* sized_font(float px) {
    int size = (int)(px + 0.5f);
    if (size < 6) size = 6;
    auto it = g_sized.find(size);
    if (it != g_sized.end()) return it->second;
    ID3DXFont* f = nullptr;
    HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll");
    auto create = d3dx ? (CreateFontA_t)(void*)GetProcAddress(d3dx, "D3DXCreateFontA") : nullptr;
    if (create && g_dev && g_sized.size() < 16)
        create(g_dev, size, 0, FW_SEMIBOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
               DEFAULT_PITCH | FF_DONTCARE, "Segoe UI", &f);
    g_sized[size] = f;
    return f;
}

void release_sized() {
    for (auto& kv : g_sized) release(kv.second);
    g_sized.clear();
}

ID3DXFont* op_font(const Op& o) { return o.size > 0 ? sized_font(o.size) : font(o.font); }

std::wstring widen_utf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// D3DX draw / measure for one op's text (ANSI menu text or UTF-8 API text).
int d3dx_text(ID3DXFont* f, const Op& o, RECT* r, DWORD flags, D3DCOLOR c) {
    if (o.utf8) return f->DrawTextW(nullptr, widen_utf8(o.text).c_str(), -1, r, flags, c);
    return f->DrawTextA(nullptr, o.text.c_str(), -1, r, flags, c);
}

void quad_state() {
    g_dev->SetVertexShader(nullptr);
    g_dev->SetPixelShader(nullptr);
    g_dev->SetTexture(0, nullptr);
    g_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
}

}  // namespace

void on_device(void* dev) {
    for (auto& f : g_fonts) release(f);
    release_sized();
    release(g_state);
    g_dev = static_cast<IDirect3DDevice9*>(dev);
    read_size();
    create_fonts();
}

void on_pre_reset() {
    release(g_state);
    for (auto* f : g_fonts)
        if (f) f->OnLostDevice();
    release_sized();
}

void on_post_reset(void* dev) {
    g_dev = static_cast<IDirect3DDevice9*>(dev);
    int old = g_h;
    read_size();
    if (g_h != old) {
        for (auto& f : g_fonts) release(f);
        create_fonts();
    } else {
        for (auto* f : g_fonts)
            if (f) f->OnResetDevice();
    }
}

int w() { return g_w; }
int h() { return g_h; }
int unit() { return g_h / 60 > 10 ? g_h / 60 : 10; }

namespace {

bool d3d_begin() {
    if (!g_state && FAILED(g_dev->CreateStateBlock(D3DSBT_ALL, &g_state))) return false;
    if (FAILED(g_dev->BeginScene())) return false;
    g_state->Capture();
    quad_state();
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    D3DVIEWPORT9 vp = {0, 0, (DWORD)g_w, (DWORD)g_h, 0.0f, 1.0f};
    g_dev->SetViewport(&vp);
    return true;
}

void d3d_rect(const Op& o) {
    if (o.w <= 0 || o.h <= 0) return;
    quad_state();  // the font sprite changes state
    float l = o.x - 0.5f, t = o.y - 0.5f, r = o.x + o.w - 0.5f, b = o.y + o.h - 0.5f;
    Vertex v[4] = {{l, t, 0, 1, o.color}, {r, t, 0, 1, o.color}, {l, b, 0, 1, o.color}, {r, b, 0, 1, o.color}};
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}

void d3d_text(const Op& o) {
    ID3DXFont* fnt = op_font(o);
    if (!fnt) return;
    unsigned shadow = (o.color & 0xFF000000u) / 4 * 3;
    RECT rs = {o.x + 1, o.y + 1, o.x + 4000, o.y + 4000};
    RECT r = {o.x, o.y, o.x + 4000, o.y + 4000};
    d3dx_text(fnt, o, &rs, DT_LEFT | DT_TOP | DT_NOCLIP, shadow);
    d3dx_text(fnt, o, &r, DT_LEFT | DT_TOP | DT_NOCLIP, o.color);
}

// Line heights (px) of the menu's text styles in the game's font, sized like
// the game's own pause menu (items about 1/19 of the screen height apart).
float target_px(Font f) {
    int u = unit();
    return f == Font::Title ? u * 4.0f : f == Font::Small ? u * 1.9f : u * 3.1f;
}

}  // namespace

void frame_begin(void* dev) {
    g_ops.clear();
    g_in = false;
    if (!g_dev || dev != g_dev) return;
    g_native = !g_classic && native::active();
    g_in = g_native || g_fonts[0] != nullptr;
}

void frame_end() {
    if (!g_in) {
        native::submit({}, false);
        return;
    }
    g_in = false;
    if (g_native) {
        native::submit(g_ops, true);
        return;
    }
    native::submit({}, false);
    if (g_ops.empty() || !d3d_begin()) return;
    for (const Op& o : g_ops) {
        if (o.kind == Op::Rect) d3d_rect(o);
        else d3d_text(o);
    }
    g_state->Apply();
    g_dev->EndScene();
}

bool begin(void*) { return g_in; }
void end() {}
bool active() { return g_in; }

bool native_now() { return g_native || (preview() && !g_classic); }
void set_classic(bool on) { g_classic = on; }

float native_scale(const Op& o) {
    float line = native::line_px();
    float px = o.size > 0 ? o.size : target_px(o.font);
    return line > 0 ? px / line : 1.0f;
}
float native_scale(Font f) {
    float line = native::line_px();
    return line > 0 ? target_px(f) / line : 1.0f;
}

void rect(int x, int y, int w, int h, unsigned argb) {
    if (!g_in || w <= 0 || h <= 0) return;
    Op o{};
    o.kind = Op::Rect;
    o.x = x, o.y = y, o.w = w, o.h = h, o.color = argb;
    g_ops.push_back(o);
}

void measure(Font f, const char* s, int* w, int* h) {
    if (g_native) {
        float fw = 0, fh = 0;
        native::measure(native_scale(f), s ? s : "", &fw, &fh);
        if (w) *w = (int)(fw + 0.5f);
        if (h) *h = (int)(fh + 0.5f);
        return;
    }
    RECT r = {0, 0, 0, 0};
    ID3DXFont* fnt = font(f);
    if (fnt && s) fnt->DrawTextA(nullptr, s, -1, &r, DT_CALCRECT | DT_LEFT | DT_NOCLIP, 0);
    if (w) *w = r.right - r.left;
    if (h) *h = r.bottom - r.top;
}

void text(Font f, int x, int y, const char* s, unsigned argb) {
    if (!g_in || !s || !*s) return;
    Op o{};
    o.kind = Op::Text;
    o.x = x, o.y = y, o.color = argb, o.font = f;
    o.text = s;
    g_ops.push_back(o);
}

// ---- API v4 text: any pixel size, UTF-8 ----------------------------------------
void measure_px(float size, const char* s, float* w, float* h) {
    if (size <= 0) size = target_px(Font::Body);
    if (g_native) {
        float line = native::line_px();
        native::measure(line > 0 ? size / line : 1.0f, s ? s : "", w, h, true);
        return;
    }
    Op o{};
    o.size = size;
    o.utf8 = true;
    o.text = s ? s : "";
    RECT r = {0, 0, 0, 0};
    ID3DXFont* f = sized_font(size);
    if (f) d3dx_text(f, o, &r, DT_CALCRECT | DT_LEFT | DT_NOCLIP, 0);
    if (w) *w = (float)(r.right - r.left);
    if (h) *h = (float)(r.bottom - r.top);
}

void text_px(float size, float x, float y, const char* s, unsigned argb) {
    if (!g_in || !s || !*s) return;
    Op o{};
    o.kind = Op::Text;
    o.x = (int)(x + 0.5f), o.y = (int)(y + 0.5f), o.color = argb;
    o.size = size > 0 ? size : target_px(Font::Body);
    o.utf8 = true;
    o.text = s;
    g_ops.push_back(o);
}

}  // namespace ui
}  // namespace hml
