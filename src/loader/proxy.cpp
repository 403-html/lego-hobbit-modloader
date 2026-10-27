// dinput8.dll proxy: forwards the six exports to the real system DLL.
#include "internal.h"

#include <unknwn.h>

extern "C" void hml_ensure_started();

namespace {

HMODULE g_real = nullptr;

FARPROC real(const char* name) {
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for a 32-bit process
        if (n == 0 || n > MAX_PATH - 14) return nullptr;
        lstrcatA(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

}  // namespace

extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out,
                                  LPUNKNOWN outer) {
    hml_ensure_started();
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = (Fn)real("DirectInput8Create");
    HRESULT hr = fn ? fn(inst, version, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out) hml::input_on_dinput_created(*out);
    return hr;
}

HRESULT WINAPI DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = (Fn)real("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = (Fn)real("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = (Fn)real("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = (Fn)real("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

LPCVOID WINAPI GetdfDIJoystick() {
    using Fn = LPCVOID(WINAPI*)();
    auto fn = (Fn)real("GetdfDIJoystick");
    return fn ? fn() : nullptr;
}

}  // extern "C"
