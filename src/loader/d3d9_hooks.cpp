// Direct3D 9 lifecycle -> loader events.
//
// Direct3DCreate9 -> IDirect3D9::CreateDevice (vtable 16)
//                 -> IDirect3DDevice9::Reset (16) / Present (17)
// The d3d9.dll implementations are detoured (not the vtable slots), so every
// device, including one recreated later, goes through the same hooks.
#include "internal.h"

#include <d3d9.h>

namespace hml {
namespace {

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                              const RGNDATA*);

Direct3DCreate9_t o_create = nullptr;
CreateDevice_t o_create_device = nullptr;
Reset_t o_reset = nullptr;
Present_t o_present = nullptr;

IDirect3DDevice9* volatile g_device = nullptr;
HWND g_hwnd = nullptr;
uint64_t g_frame = 0;

HmlD3D9Event make_event(IDirect3DDevice9* dev) { return HmlD3D9Event{dev, g_hwnd, g_frame}; }

HRESULT STDMETHODCALLTYPE hk_present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst,
                                     HWND wnd, const RGNDATA* dirty) {
    if (dev == g_device) {
        HmlD3D9Event ev = make_event(dev);
        events_dispatch(HML_EVENT_FRAME, &ev);
        ui_frame(dev, g_frame);  // mods' UI (HML_EVENT_UI), the Mods menu, toasts
        mods_watch();
        ++g_frame;
    }
    return o_present(dev, src, dst, wnd, dirty);
}

HRESULT STDMETHODCALLTYPE hk_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    HmlD3D9Event ev = make_event(dev);
    if (dev == g_device) {
        events_dispatch(HML_EVENT_D3D9_PRE_RESET, &ev);
        ui::on_pre_reset();
    }
    HRESULT hr = o_reset(dev, pp);
    if (dev == g_device) {
        LOG_D("IDirect3DDevice9::Reset -> 0x%08lX", (unsigned long)hr);
        ui::on_post_reset(dev);
        events_dispatch(HML_EVENT_D3D9_POST_RESET, &ev);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_device(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type,
                                           HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
    HRESULT hr = o_create_device(d3d, adapter, type, focus, flags, pp, out);
    if (FAILED(hr) || !out || !*out) {
        LOG_W("IDirect3D9::CreateDevice failed: 0x%08lX", (unsigned long)hr);
        return hr;
    }
    IDirect3DDevice9* dev = *out;
    void** vt = *reinterpret_cast<void***>(dev);
    if (!o_present) {
        hooks_function(loader_self(), vt[16], (void*)&hk_reset, (void**)&o_reset);
        hooks_function(loader_self(), vt[17], (void*)&hk_present, (void**)&o_present);
    }
    g_hwnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;
    g_device = dev;
    LOG_I("Direct3D 9 device created: %p, window %p, %ux%u %s", (void*)dev, (void*)g_hwnd,
          pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
          (pp && pp->Windowed) ? "windowed" : "fullscreen");
    ui::on_device(dev);
    HmlD3D9Event ev = make_event(dev);
    events_dispatch(HML_EVENT_D3D9_DEVICE_CREATED, &ev);
    return hr;
}

IDirect3D9* WINAPI hk_create(UINT sdk) {
    IDirect3D9* d3d = o_create(sdk);
    if (d3d && !o_create_device) {
        void** vt = *reinterpret_cast<void***>(d3d);
        hooks_function(loader_self(), vt[16], (void*)&hk_create_device, (void**)&o_create_device);
    }
    return d3d;
}

}  // namespace

void d3d9_install() {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    void* fn = d3d9 ? (void*)GetProcAddress(d3d9, "Direct3DCreate9") : nullptr;
    if (!fn) {
        LOG_W("d3d9.dll not loaded; frame events disabled");
        return;
    }
    hooks_function(loader_self(), fn, (void*)&hk_create, (void**)&o_create);
}

void* d3d9_device() { return g_device; }
void* d3d9_hwnd() { return g_hwnd; }

}  // namespace hml
