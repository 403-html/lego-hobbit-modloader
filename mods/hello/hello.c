/*
 * hello - the smallest useful HML mod.
 *
 * - logs what the loader knows about the game
 * - resolves a few engine classes through RTTI
 * - draws a small green square in the top-left corner for the first few
 *   seconds after the game window appears, as a visible "mods are loaded" sign
 * - optionally logs the frame rate
 *
 * Settings (mods\hello.ini):
 *   [hello]
 *   IndicatorSeconds=8     ; 0 = never draw the square
 *   FpsLogSeconds=0        ; log the frame rate every N seconds, 0 = off
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

#include <hml/hml.h>

HML_MOD("hello", "Hello World", "0.1.0", "HML", "Example mod: proves the loader works")

static const HmlApi* g_api;
static HmlMod* g_self;
static int g_indicator_seconds;
static int g_fps_seconds;
static DWORD g_device_tick;
static DWORD g_fps_tick;
static uint64_t g_fps_frame;

#define LOG(level, ...) g_api->log(g_self, level, __VA_ARGS__)

static void on_device(HmlEvent ev, const void* data, void* user) {
    const HmlD3D9Event* d = (const HmlD3D9Event*)data;
    (void)ev;
    (void)user;
    g_device_tick = GetTickCount();
    g_fps_tick = g_device_tick;
    g_fps_frame = d->frame;
    LOG(HML_LOG_INFO, "D3D9 device ready: %p", d->device);
}

static void on_frame(HmlEvent ev, const void* data, void* user) {
    const HmlD3D9Event* d = (const HmlD3D9Event*)data;
    IDirect3DDevice9* dev = (IDirect3DDevice9*)d->device;
    DWORD now = GetTickCount();
    (void)ev;
    (void)user;

    if (g_indicator_seconds > 0 && now - g_device_tick < (DWORD)g_indicator_seconds * 1000) {
        D3DRECT r = {8, 8, 24, 24};
        IDirect3DDevice9_Clear(dev, 1, &r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(40, 200, 60), 1.0f, 0);
    }

    if (g_fps_seconds > 0 && now - g_fps_tick >= (DWORD)g_fps_seconds * 1000) {
        double secs = (now - g_fps_tick) / 1000.0;
        LOG(HML_LOG_INFO, "%.1f fps", (double)(d->frame - g_fps_frame) / secs);
        g_fps_tick = now;
        g_fps_frame = d->frame;
    }
}

static void report_class(const char* name) {
    void** vt = g_api->find_vtable(name);
    if (vt)
        LOG(HML_LOG_INFO, "  %-30s vtable %p (rva %08X)", name, (void*)vt,
            (unsigned)((uintptr_t)vt - g_api->game->image_base));
    else
        LOG(HML_LOG_WARN, "  %-30s not found", name);
}

int hml_mod_init(const HmlApi* api, HmlMod* self) {
    g_api = api;
    g_self = self;

    LOG(HML_LOG_INFO, "hello from a C mod; loader %s, API v%u", api->loader_version,
        api->version);
    LOG(HML_LOG_INFO, "game: %s, base %p", api->game->build_name, (void*)api->game->image_base);

    g_indicator_seconds = api->config_get_int(self, "IndicatorSeconds", 8);
    g_fps_seconds = api->config_get_int(self, "FpsLogSeconds", 0);

    LOG(HML_LOG_INFO, "engine classes:");
    report_class("HobbitGame");
    report_class("GameFramework");
    report_class("ScriptManager");
    report_class("HobbitInstructionBuildSystem");

    api->subscribe(self, HML_EVENT_D3D9_DEVICE_CREATED, on_device, NULL);
    api->subscribe(self, HML_EVENT_FRAME, on_frame, NULL);
    return HML_OK;
}

/* Hot reload: nothing to put back (the loader drops the subscriptions). */
void hml_mod_unload(void) { LOG(HML_LOG_INFO, "unloading for reload"); }

void hml_mod_shutdown(void) {
    LOG(HML_LOG_INFO, "goodbye");
}
