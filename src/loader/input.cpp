// Input plumbing for mods: lets them hide keyboard / mouse / gamepad input from
// the game while still reading it themselves.
//
// Keyboard and mouse: the game reads them through DirectInput 8 (it imports
// DINPUT8!DirectInput8Create, which is us). We detour the device methods
// GetDeviceState / GetDeviceData and blank the result for blocked devices.
// Gamepads: XInputGetState (xinput1_3.dll, imported by ordinal 2) is detoured.
#include "internal.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <cstring>
#include <map>

namespace hml {
namespace {

// {6F1D2B61-D5A0-11CF-BFC7-444553540000}, {6F1D2B60-...}: defined here so we
// do not need dxguid.lib.
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

CRITICAL_SECTION g_lock;
bool g_ready = false;
std::map<HmlMod*, uint32_t> g_requests;
volatile LONG g_blocked = 0;  // union of g_requests

std::map<void*, uint32_t> g_devices;  // IDirectInputDevice8* -> HML_INPUT_KEYBOARD/MOUSE
volatile LONG g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_wheel = 0;
volatile LONG g_have_di_mouse = 0;

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void* self, REFGUID, void** out, void* outer);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD size, void* data);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD obj_size,
                                                    DIDEVICEOBJECTDATA* data, DWORD* inout,
                                                    DWORD flags);
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

CreateDevice_t o_create_device = nullptr;
GetDeviceState_t o_get_state = nullptr;
GetDeviceData_t o_get_data = nullptr;
XInputGetState_t o_xinput_get_state = nullptr;
XInputGetState_t g_xinput_direct = nullptr;  // fallback when the hook is not installed

uint32_t device_kind(void* dev) {
    EnterCriticalSection(&g_lock);
    auto it = g_devices.find(dev);
    uint32_t kind = it == g_devices.end() ? 0 : it->second;
    LeaveCriticalSection(&g_lock);
    return kind;
}

bool blocked(uint32_t kind) { return kind && (static_cast<uint32_t>(g_blocked) & kind); }

// Mouse axes can be relative (movement since last read, the default) or
// absolute (running position) depending on DIPROP_AXISMODE. Mods always get
// movement, so absolute readings are differenced per device.
struct MouseState {
    bool checked = false, absolute = false;
    bool have[3] = {};
    LONG last[3] = {};
};
std::map<void*, MouseState> g_mice;

using GetProperty_t = HRESULT(STDMETHODCALLTYPE*)(void* self, const GUID* prop, DIPROPHEADER* hdr);

// Caller holds g_lock.
MouseState& mouse_state(void* dev) {
    MouseState& m = g_mice[dev];
    if (!m.checked) {
        m.checked = true;
        DIPROPDWORD pd{};
        pd.diph.dwSize = sizeof pd;
        pd.diph.dwHeaderSize = sizeof pd.diph;
        pd.diph.dwHow = DIPH_DEVICE;
        auto get_property = (GetProperty_t)(*static_cast<void***>(dev))[5];
        // DIPROP_AXISMODE is MAKEDIPROP(2): the GUID "pointer" is the value 2.
        if (SUCCEEDED(get_property(dev, reinterpret_cast<const GUID*>(2), &pd.diph)))
            m.absolute = pd.dwData == DIPROPAXISMODE_ABS;
        LOG_I("DirectInput mouse %p reports %s axes", dev, m.absolute ? "absolute" : "relative");
    }
    return m;
}

// axis: 0 = x, 1 = y, 2 = wheel. Caller holds g_lock.
void mouse_feed(MouseState& m, int axis, LONG value) {
    LONG delta = value;
    if (m.absolute) {
        delta = m.have[axis] ? value - m.last[axis] : 0;
        m.last[axis] = value;
        m.have[axis] = true;
    }
    volatile LONG* acc = axis == 0 ? &g_mouse_dx : axis == 1 ? &g_mouse_dy : &g_mouse_wheel;
    InterlockedExchangeAdd(acc, delta);
}

// Keys the pause-menu "Mods" entry takes over (see pause_entry.cpp).
const uint8_t kDownKeys[] = {0xD0 /*Down*/, 0x1F /*S*/, 0x50 /*Num2*/};
const uint8_t kUpKeys[] = {0xC8 /*Up*/, 0x11 /*W*/, 0x48 /*Num8*/};
const uint8_t kAcceptKeys[] = {0x1C /*Enter*/, 0x39 /*Space*/, 0x9C /*NumEnter*/};

bool suppressed_key(uint32_t mask, DWORD dik) {
    auto in = [dik](const uint8_t* k, size_t n) {
        for (size_t i = 0; i < n; ++i)
            if (k[i] == dik) return true;
        return false;
    };
    return ((mask & pause::SUPPRESS_DOWN) && in(kDownKeys, sizeof kDownKeys)) ||
           ((mask & pause::SUPPRESS_UP) && in(kUpKeys, sizeof kUpKeys)) ||
           ((mask & pause::SUPPRESS_ACCEPT) && in(kAcceptKeys, sizeof kAcceptKeys));
}

HRESULT STDMETHODCALLTYPE hk_get_state(void* dev, DWORD size, void* data) {
    HRESULT hr = o_get_state(dev, size, data);
    uint32_t kind = device_kind(dev);
    if (SUCCEEDED(hr) && data) {
        if (kind == HML_INPUT_MOUSE && size >= sizeof(DIMOUSESTATE)) {
            auto* st = static_cast<DIMOUSESTATE*>(data);
            EnterCriticalSection(&g_lock);
            MouseState& m = mouse_state(dev);
            mouse_feed(m, 0, st->lX);
            mouse_feed(m, 1, st->lY);
            mouse_feed(m, 2, st->lZ);
            LeaveCriticalSection(&g_lock);
        }
        if (blocked(kind)) {
            std::memset(data, 0, size);
        } else if (kind == HML_INPUT_KEYBOARD && size >= 256) {
            if (uint32_t mask = pause::suppress())
                for (DWORD k = 0; k < 256; ++k)
                    if (suppressed_key(mask, k)) static_cast<uint8_t*>(data)[k] = 0;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_get_data(void* dev, DWORD obj_size, DIDEVICEOBJECTDATA* data,
                                      DWORD* inout, DWORD flags) {
    HRESULT hr = o_get_data(dev, obj_size, data, inout, flags);
    uint32_t kind = device_kind(dev);
    if (SUCCEEDED(hr) && inout) {
        if (kind == HML_INPUT_MOUSE && data && !(flags & DIGDD_PEEK) &&
            obj_size >= 2 * sizeof(DWORD)) {
            auto* p = reinterpret_cast<uint8_t*>(data);
            EnterCriticalSection(&g_lock);
            MouseState& m = mouse_state(dev);
            for (DWORD i = 0; i < *inout; ++i, p += obj_size) {
                auto* item = reinterpret_cast<DIDEVICEOBJECTDATA*>(p);
                LONG v = static_cast<LONG>(item->dwData);
                if (item->dwOfs == DIMOFS_X) mouse_feed(m, 0, v);
                else if (item->dwOfs == DIMOFS_Y) mouse_feed(m, 1, v);
                else if (item->dwOfs == DIMOFS_Z) mouse_feed(m, 2, v);
            }
            LeaveCriticalSection(&g_lock);
        }
        if (blocked(kind)) {
            // Hand the events over as releases / no movement rather than
            // dropping them: a key let go while blocked must still reach the
            // game as released, or it stays held there.
            if (kind == HML_INPUT_KEYBOARD && data && obj_size >= 2 * sizeof(DWORD)) {
                auto* p = reinterpret_cast<uint8_t*>(data);
                for (DWORD i = 0; i < *inout; ++i, p += obj_size)
                    reinterpret_cast<DIDEVICEOBJECTDATA*>(p)->dwData = 0;
            } else {
                *inout = 0;
            }
        } else if (kind == HML_INPUT_KEYBOARD && data && obj_size >= sizeof(DWORD)) {
            if (uint32_t mask = pause::suppress()) {
                auto* p = reinterpret_cast<uint8_t*>(data);
                for (DWORD i = 0; i < *inout; ++i, p += obj_size) {
                    auto* item = reinterpret_cast<DIDEVICEOBJECTDATA*>(p);
                    if (suppressed_key(mask, item->dwOfs)) item->dwData = 0;  // reads as "released"
                }
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_device(void* di, REFGUID guid, void** out, void* outer) {
    HRESULT hr = o_create_device(di, guid, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;

    uint32_t kind = 0;
    if (IsEqualGUID(guid, kSysKeyboard)) kind = HML_INPUT_KEYBOARD;
    else if (IsEqualGUID(guid, kSysMouse)) kind = HML_INPUT_MOUSE;
    if (!kind) return hr;

    EnterCriticalSection(&g_lock);
    g_devices[*out] = kind;
    LeaveCriticalSection(&g_lock);
    if (kind == HML_INPUT_MOUSE) InterlockedExchange(&g_have_di_mouse, 1);
    LOG_I("game created DirectInput %s device %p", kind == HML_INPUT_MOUSE ? "mouse" : "keyboard",
          *out);

    if (!o_get_state) {
        void** vt = *reinterpret_cast<void***>(*out);
        hooks_function(loader_self(), vt[9], (void*)&hk_get_state, (void**)&o_get_state);
        hooks_function(loader_self(), vt[10], (void*)&hk_get_data, (void**)&o_get_data);
    }
    return hr;
}

DWORD WINAPI hk_xinput_get_state(DWORD index, XINPUT_STATE* state) {
    DWORD rc = o_xinput_get_state(index, state);
    if (rc == ERROR_SUCCESS && state) {
        if (static_cast<uint32_t>(g_blocked) & HML_INPUT_GAMEPAD) {
            std::memset(&state->Gamepad, 0, sizeof state->Gamepad);
        } else if (uint32_t mask = pause::suppress()) {
            XINPUT_GAMEPAD& g = state->Gamepad;
            if (mask & pause::SUPPRESS_DOWN) {
                g.wButtons &= ~XINPUT_GAMEPAD_DPAD_DOWN;
                if (g.sThumbLY < 0) g.sThumbLY = 0;
            }
            if (mask & pause::SUPPRESS_UP) {
                g.wButtons &= ~XINPUT_GAMEPAD_DPAD_UP;
                if (g.sThumbLY > 0) g.sThumbLY = 0;
            }
            if (mask & pause::SUPPRESS_ACCEPT) g.wButtons &= ~(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START);
        }
    }
    return rc;
}

}  // namespace

void input_init() {
    InitializeCriticalSection(&g_lock);
    g_ready = true;
    HMODULE xi = GetModuleHandleA("xinput1_3.dll");
    if (!xi) xi = GetModuleHandleA("xinput1_4.dll");
    if (xi) {
        void* fn = (void*)GetProcAddress(xi, "XInputGetState");
        if (!fn) fn = (void*)GetProcAddress(xi, MAKEINTRESOURCEA(2));
        g_xinput_direct = (XInputGetState_t)fn;
        if (fn)
            hooks_function(loader_self(), fn, (void*)&hk_xinput_get_state,
                           (void**)&o_xinput_get_state);
    } else {
        LOG_W("XInput not loaded; gamepad blocking unavailable");
    }
}

void input_on_dinput_created(void* dinput) {
    if (!g_ready || !dinput || o_create_device) return;
    void** vt = *static_cast<void***>(dinput);
    hooks_function(loader_self(), vt[3], (void*)&hk_create_device, (void**)&o_create_device);
}

// ---- API ----------------------------------------------------------------

void input_block(HmlMod* self, uint32_t devices) {
    if (!g_ready) return;
    EnterCriticalSection(&g_lock);
    if (devices) g_requests[self] = devices;
    else g_requests.erase(self);
    uint32_t all = 0;
    for (auto& r : g_requests) all |= r.second;
    LONG before = InterlockedExchange(&g_blocked, static_cast<LONG>(all));
    LeaveCriticalSection(&g_lock);
    if (static_cast<uint32_t>(before) != all)
        LOG_D("input blocked from game: keyboard=%d mouse=%d gamepad=%d",
              !!(all & HML_INPUT_KEYBOARD), !!(all & HML_INPUT_MOUSE), !!(all & HML_INPUT_GAMEPAD));
}

int input_gamepad_state(uint32_t index, HmlGamepad* out) {
    XInputGetState_t fn = o_xinput_get_state ? o_xinput_get_state : g_xinput_direct;
    if (!fn || !out || index > 3) return 0;
    XINPUT_STATE st{};
    if (fn(index, &st) != ERROR_SUCCESS) return 0;
    static_assert(sizeof(HmlGamepad) == sizeof(XINPUT_GAMEPAD), "layout must match XINPUT_GAMEPAD");
    std::memcpy(out, &st.Gamepad, sizeof *out);
    return 1;
}

void input_mouse_delta(int32_t* dx, int32_t* dy, int32_t* wheel) {
    LONG x = InterlockedExchange(&g_mouse_dx, 0);
    LONG y = InterlockedExchange(&g_mouse_dy, 0);
    LONG w = InterlockedExchange(&g_mouse_wheel, 0);
    if (dx) *dx = x;
    if (dy) *dy = y;
    if (wheel) *wheel = w;
}

int input_game_has_focus() {
    HWND game = static_cast<HWND>(d3d9_hwnd());
    if (!game) return 0;
    HWND fg = GetForegroundWindow();
    return fg && (fg == game || GetAncestor(fg, GA_ROOTOWNER) == GetAncestor(game, GA_ROOTOWNER));
}

}  // namespace hml
