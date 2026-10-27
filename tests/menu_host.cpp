// Stands in for the game: imports DINPUT8 (so the loader loads like it does in
// LEGOHobbit.exe), creates a D3D9 device, presses keys to drive the Mods
// menu and saves what is on screen after each step. Usage: menu_host.exe
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d9.h>
#include <dinput.h>

#include <cstdio>
#include <string>


static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void tap(WORD vk) {
    keybd_event((BYTE)vk, 0, 0, 0);
}
static void untap(WORD vk) { keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0); }

static void frame(IDirect3DDevice9* dev, int w, int h, int i) {
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(40, 60, 38), 1.0f, 0);
    dev->BeginScene();
    for (int k = 0; k < 10; ++k) {
        D3DRECT r = {k * w / 10, h / 3 + (k % 3) * h / 10, k * w / 10 + w / 12, h};
        dev->Clear(1, &r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(90 + k * 15, 120 + (k * 31 + i) % 90, 80 + (k * 47) % 120), 1.0f, 0);
    }
    dev->EndScene();
    dev->Present(nullptr, nullptr, nullptr, nullptr);
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    Sleep(30);
}

// Saves the back buffer as PNG through d3dx9_43.dll (D3DXIFF_PNG = 3).
static void save(IDirect3DDevice9* dev, const char* name) {
    using Save_t = HRESULT(WINAPI*)(const char*, int, IDirect3DSurface9*, const void*, const RECT*);
    HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll");
    auto save_fn = d3dx ? (Save_t)(void*)GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA") : nullptr;
    IDirect3DSurface9* bb = nullptr;
    bool ok = save_fn && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) &&
              SUCCEEDED(save_fn(name, 3, bb, nullptr, nullptr));
    if (bb) bb->Release();
    std::printf("%s: %s\n", name, ok ? "ok" : "FAILED");
}

int main() {
    const int w = 1920, h = 810;
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "menu_host";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("menu_host", "menu_host", WS_POPUP | WS_VISIBLE, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(wnd);
    SetFocus(wnd);
    IDirectInput8A* di = nullptr;
    DirectInput8Create(wc.hInstance, DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di, nullptr);
    LoadLibraryA("d3dx9_43.dll");  // the game imports it; the menu uses it for text
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;  // keep the presented image for capture
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) return 3;

    struct Step { WORD key; const char* shot; } steps[] = {
        {VK_F1, "menu_list.png"},          // open: list page
        {VK_DOWN, nullptr}, {VK_DOWN, nullptr}, {VK_DOWN, nullptr},
        {VK_RETURN, "menu_page.png"},      // a mod page
        {VK_DOWN, nullptr}, {VK_DOWN, nullptr}, {VK_DOWN, nullptr}, {VK_RIGHT, nullptr}, {VK_RIGHT, "menu_page_edit.png"},
        {VK_ESCAPE, nullptr}, {VK_UP, nullptr}, {VK_UP, nullptr}, {VK_UP, nullptr}, {VK_RETURN, "menu_loader.png"},
        {VK_ESCAPE, nullptr}, {VK_ESCAPE, "menu_closed.png"},
    };
    for (int i = 0; i < 10; ++i) frame(dev, w, h, i);
    int n = 0;
    for (auto& s : steps) {
        tap(s.key);
        for (int i = 0; i < 3; ++i) frame(dev, w, h, n++);
        untap(s.key);
        for (int i = 0; i < 3; ++i) frame(dev, w, h, n++);
        if (s.shot) save(dev, s.shot);
    }
    dev->Release();
    d3d->Release();
    if (di) di->Release();
    return 0;
}
