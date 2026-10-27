// Mod discovery, the API table, events and per-mod configuration.
#include "internal.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

namespace hml {
namespace {

HmlMod g_self;
std::vector<std::unique_ptr<HmlMod>> g_mods;

struct Subscription {
    HmlMod* mod;
    HmlEvent event;
    HmlEventFn fn;
    void* user;
};
std::vector<Subscription> g_subs;
CRITICAL_SECTION g_event_lock;
bool g_event_lock_ready = false;

// ---- API implementations --------------------------------------------------

void api_log(HmlMod* self, HmlLogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(self ? self : &g_self, level, fmt, ap);
    va_end(ap);
}

void* api_rva(uint32_t rva) { return game_rva(rva); }

void* api_find_pattern(const char* pattern) {
    Section text;
    if (!game_section(".text", &text)) return nullptr;
    return pattern_find(pattern, text.begin, text.end);
}

int api_subscribe(HmlMod* self, HmlEvent event, HmlEventFn fn, void* user) {
    if (!fn) return HML_ERR_BAD_ARG;
    EnterCriticalSection(&g_event_lock);
    g_subs.push_back({self, event, fn, user});
    LeaveCriticalSection(&g_event_lock);
    return HML_OK;
}

int api_config_get_int(HmlMod* self, const char* key, int def) {
    if (!self || !key) return def;
    return (int)GetPrivateProfileIntA(self->id.c_str(), key, def, self->ini_path.c_str());
}

size_t api_config_get_string(HmlMod* self, const char* key, const char* def, char* buf,
                             size_t size) {
    if (!buf || !size) return 0;
    if (!self || !key) {
        std::snprintf(buf, size, "%s", def ? def : "");
        return std::strlen(buf);
    }
    return GetPrivateProfileStringA(self->id.c_str(), key, def ? def : "", buf, (DWORD)size,
                                    self->ini_path.c_str());
}

void* api_d3d9_device() { return d3d9_device(); }
void* api_game_window() { return d3d9_hwnd(); }
int api_option_add(HmlMod* self, const HmlOption* opt) { return mods_option_add(self, opt); }
int api_menu_is_open() { return menu_open() ? 1 : 0; }

HmlApi make_api() {
    HmlApi a{};
    a.size = sizeof(HmlApi);
    a.version = HML_API_VERSION;
    a.loader_version = HML_LOADER_VERSION;
    a.game = game_info();
    a.log = &api_log;
    a.rva = &api_rva;
    a.find_vtable = &rtti_find_vtable;
    a.class_name_of = &rtti_class_name_of;
    a.find_pattern = &api_find_pattern;
    a.hook_function = &hooks_function;
    a.hook_vtable = &hooks_vtable;
    a.patch_bytes = &hooks_patch;
    a.subscribe = &api_subscribe;
    a.config_get_int = &api_config_get_int;
    a.config_get_string = &api_config_get_string;
    a.d3d9_device = &api_d3d9_device;
    // v2
    a.input_block = &input_block;
    a.gamepad_state = &input_gamepad_state;
    a.mouse_delta = &input_mouse_delta;
    a.game_window = &api_game_window;
    a.game_has_focus = &input_game_has_focus;
    // v3
    a.option_add = &api_option_add;
    a.menu_is_open = &api_menu_is_open;
    // v4
    ui_api_fill(&a);
    return a;
}

HmlApi g_api;

// ---- discovery ------------------------------------------------------------

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool ends_with(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool is_disabled(const std::string& file_lower, const std::string& id_lower) {
    std::string list = lower(loader_config().disabled);
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos
                                                                        : comma - pos);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty() && (item == file_lower || item == id_lower)) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

std::vector<std::string> list_files(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path_join(dir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        std::string l = lower(name);
        if (ends_with(l, ".dll") || ends_with(l, ".asi")) out.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(),
              [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

bool valid_id(const char* id) {
    if (!id || !*id) return false;
    for (const char* p = id; *p; ++p)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-' || *p == '_'))
            return false;
    return true;
}

bool g_hot_reload = true;  // [Loader] HotReload

// Set while a mod is being loaded again (hot reload): its entry is reused so
// the HmlMod* the mod keeps stays valid.
HmlMod* g_reuse = nullptr;

// Every file gets an entry so the Mods menu can show what happened to it.
HmlMod* add_entry(const std::string& path, const std::string& file, ModStatus status,
                  const std::string& note) {
    if (HmlMod* e = g_reuse) {
        g_reuse = nullptr;
        e->id = lower(file);
        e->name = file;
        e->file = path;
        e->status = status;
        e->note = note;
        e->version.clear();
        e->author.clear();
        e->description.clear();
        return e;
    }
    auto mod = std::make_unique<HmlMod>();
    mod->id = lower(file);
    mod->name = file;
    mod->file = path;
    mod->status = status;
    mod->note = note;
    HmlMod* raw = mod.get();
    g_mods.push_back(std::move(mod));
    return raw;
}

bool file_stamp(const std::string& path, FILETIME* t, uint64_t* size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return false;
    *t = d.ftLastWriteTime;
    *size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

std::string shadow_dir(const std::string& dir) { return path_join(dir, ".hml"); }

// .hml/<name>.<pid>-<n>.dll in the mods folder, a fresh name every time so an old copy that is
// still mapped never blocks the new one.
std::string shadow_copy(const std::string& dir, const std::string& file) {
    static unsigned counter = 0;
    std::string sd = shadow_dir(dir);
    if (GetFileAttributesA(sd.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryA(sd.c_str(), nullptr);
        SetFileAttributesA(sd.c_str(), FILE_ATTRIBUTE_HIDDEN);
    }
    std::string stem = file.substr(0, file.find_last_of('.'));
    std::string copy = path_join(sd, stem + "." + std::to_string(GetCurrentProcessId()) + "-" +
                                         std::to_string(++counter) + ".dll");
    if (!CopyFileA(path_join(dir, file).c_str(), copy.c_str(), FALSE)) {
        LOG_W("could not copy %s to %s (error %lu); loading it in place (no hot reload)", file.c_str(),
              copy.c_str(), GetLastError());
        return std::string();
    }
    return copy;
}

// Copies left by earlier sessions.
void clean_shadow_dir(const std::string& dir) {
    std::string sd = shadow_dir(dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path_join(sd, "*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileA(path_join(sd, fd.cFileName).c_str());
    while (FindNextFileA(h, &fd));
    FindClose(h);
}

void load_one(const std::string& dir, const std::string& file) {
    const std::string path = path_join(dir, file);
    const std::string file_l = lower(file);
    const bool asi = ends_with(file_l, ".asi");

    if (asi && !loader_config().load_asi) {
        add_entry(path, file, ModStatus::Disabled, "plain modules are off (LoadAsi=0)");
        return;
    }
    if (is_disabled(file_l, "")) {
        LOG_I("skipping %s (disabled in hml.ini)", file.c_str());
        add_entry(path, file, ModStatus::Disabled, "disabled in hml.ini");
        return;
    }

    // HML mods are loaded from a copy in the hidden .hml folder, so the file
    // in the mods folder stays unlocked: it can be rebuilt while the game runs (hot reload).
    // Plain modules are loaded as they are.
    std::string load_path = asi ? path : shadow_copy(dir, file);
    if (load_path.empty()) load_path = path;
    // Lets a mod's own dependencies sit next to it in mods\.
    SetDllDirectoryA(dir.c_str());
    HMODULE m = LoadLibraryExA(load_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    SetDllDirectoryA(nullptr);
    auto drop_copy = [&]() {
        if (load_path != path) DeleteFileA(load_path.c_str());
    };
    if (!m) {
        LOG_E("could not load %s (error %lu)", file.c_str(), GetLastError());
        DWORD err = GetLastError();
        drop_copy();
        add_entry(path, file, ModStatus::Failed, "could not be loaded (error " + std::to_string(err) + ")");
        return;
    }
    if (asi) {
        LOG_I("loaded plain module %s", file.c_str());
        add_entry(path, file, ModStatus::Plain, "plain module, no HML API");
        return;
    }

    auto info_fn = (HmlModInfoFn)GetProcAddress(m, "hml_mod_info");
    auto init_fn = (HmlModInitFn)GetProcAddress(m, "hml_mod_init");
    if (!info_fn || !init_fn) {
        LOG_W("%s does not export hml_mod_info/hml_mod_init; not a mod, unloading", file.c_str());
        FreeLibrary(m);
        drop_copy();
        add_entry(path, file, ModStatus::NotAMod, "not an HML mod (no hml_mod_init)");
        return;
    }
    const HmlModInfo* info = info_fn();
    if (!info || !valid_id(info->id)) {
        LOG_E("%s: missing or invalid mod id (use [a-z0-9_-])", file.c_str());
        FreeLibrary(m);
        drop_copy();
        add_entry(path, file, ModStatus::Failed, "missing or invalid mod id");
        return;
    }
    auto fill = [&](HmlMod* e) {
        e->id = info->id;
        e->name = info->name ? info->name : info->id;
        e->version = info->version ? info->version : "?";
        e->author = info->author ? info->author : "";
        e->description = info->description ? info->description : "";
        e->ini_path = path_join(dir, e->id + ".ini");
    };
    if (info->api_version == 0 || info->api_version > HML_API_VERSION) {
        LOG_E("%s (%s) needs API v%u, this loader provides v%u; update the loader",
              file.c_str(), info->id, info->api_version, HML_API_VERSION);
        HmlMod* e = add_entry(path, file, ModStatus::Failed,
                              "needs loader API v" + std::to_string(info->api_version) +
                                  ", this loader has v" + std::to_string(HML_API_VERSION));
        fill(e);
        FreeLibrary(m);
        drop_copy();
        return;
    }
    if (is_disabled(file_l, lower(info->id))) {
        LOG_I("skipping %s (disabled in hml.ini)", info->id);
        HmlMod* e = add_entry(path, file, ModStatus::Disabled, "disabled in hml.ini");
        fill(e);
        FreeLibrary(m);
        drop_copy();
        return;
    }
    for (auto& other : g_mods) {
        if (other.get() != g_reuse && other->status == ModStatus::Active && other->id == info->id) {
            LOG_E("%s: mod id '%s' already used by %s; skipping", file.c_str(), info->id,
                  other->file.c_str());
            HmlMod* e = add_entry(path, file, ModStatus::Failed,
                                  "id '" + std::string(info->id) + "' already used by another mod");
            fill(e);
            e->id = lower(file);  // keep ids unique in the list
            FreeLibrary(m);
            drop_copy();
            return;
        }
    }

    HmlMod* raw = add_entry(path, file, ModStatus::Active, "");
    fill(raw);
    raw->module = m;
    raw->shutdown = (HmlModShutdownFn)GetProcAddress(m, "hml_mod_shutdown");
    raw->unload = (HmlModUnloadFn)GetProcAddress(m, "hml_mod_unload");
    raw->loaded_copy = load_path != path ? load_path : std::string();
    file_stamp(path, &raw->file_time, &raw->file_size);

    LOG_I("loading %s %s (%s) by %s", raw->name.c_str(), raw->version.c_str(), file.c_str(),
          info->author ? info->author : "?");
    int rc = init_fn(&g_api, raw);
    if (rc != HML_OK) {
        // The mod may already have installed hooks into its own code, so the
        // DLL stays mapped; we only drop its event subscriptions.
        LOG_W("%s: init returned %d; mod disabled", raw->id.c_str(), rc);
        EnterCriticalSection(&g_event_lock);
        g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(),
                                    [raw](const Subscription& s) { return s.mod == raw; }),
                     g_subs.end());
        LeaveCriticalSection(&g_event_lock);
        raw->shutdown = nullptr;
        raw->status = ModStatus::Failed;
        raw->note = rc == HML_ERR_UNSUPPORTED ? "not supported on this game build"
                    : rc == HML_ERR_HOOK_FAILED ? "could not hook the game; see hml.log"
                    : "did not start (error " + std::to_string(rc) + "); see hml.log";
    }
}

// Mods switched on/off in the menu this session (applies at next start).
std::vector<std::string> g_disabled_next;  // lower-case ids / file names
bool g_disabled_next_ready = false;

void disabled_next_init() {
    if (g_disabled_next_ready) return;
    g_disabled_next_ready = true;
    std::string list = lower(loader_config().disabled);
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) g_disabled_next.push_back(item);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

std::string file_name(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

HmlMod* loader_self() {
    if (g_self.id.empty()) {
        g_self.id = "loader";
        g_self.name = "Hobbit Mod Loader";
        g_self.version = HML_LOADER_VERSION;
        g_self.author = "HML";
        g_self.description = "Loads native mods from the mods folder";
    }
    return &g_self;
}

std::vector<HmlMod*> mods_list() {
    std::vector<HmlMod*> out{loader_self()};
    for (auto& m : g_mods) out.push_back(m.get());
    return out;
}

int mods_option_add(HmlMod* mod, const HmlOption* opt) {
    if (!mod || !opt || !opt->label) return HML_ERR_BAD_ARG;
    bool needs_value = opt->type != HML_OPTION_ACTION && opt->type != HML_OPTION_LABEL;
    if (needs_value && !opt->value) return HML_ERR_BAD_ARG;
    if (opt->type == HML_OPTION_CHOICE && (!opt->choices || opt->num_choices <= 0))
        return HML_ERR_BAD_ARG;
    auto* e = new OptionEntry;  // lives for the process
    e->opt = *opt;
    e->key = opt->key ? opt->key : "";
    e->label = opt->label;
    e->help = opt->help ? opt->help : "";
    e->opt.key = opt->key ? e->key.c_str() : nullptr;
    e->opt.label = e->label.c_str();
    e->opt.help = opt->help ? e->help.c_str() : nullptr;
    if (e->opt.step <= 0) e->opt.step = 1;
    mod->options.push_back(e);
    return HML_OK;
}

void mods_option_save(HmlMod* mod, const OptionEntry& e) {
    if (!e.opt.key || !e.opt.value || mod->ini_path.empty()) return;
    char buf[64];
    const HmlOption& o = e.opt;
    switch (o.type) {
        case HML_OPTION_BOOL:
        case HML_OPTION_INT: std::snprintf(buf, sizeof buf, "%d", (int)*(int32_t*)o.value); break;
        case HML_OPTION_FLOAT: std::snprintf(buf, sizeof buf, "%g", *(float*)o.value); break;
        case HML_OPTION_CHOICE: {
            int i = *(int32_t*)o.value;
            std::snprintf(buf, sizeof buf, "%s", i >= 0 && i < o.num_choices ? o.choices[i] : "");
            break;
        }
        default: return;
    }
    const char* section = mod == loader_self() ? "Loader" : mod->id.c_str();
    WritePrivateProfileStringA(section, o.key, buf, mod->ini_path.c_str());
}

bool mods_enabled_next_start(const HmlMod* mod) {
    disabled_next_init();
    std::string id = lower(mod->id), file = lower(file_name(mod->file));
    for (const std::string& d : g_disabled_next)
        if (d == id || d == file) return false;
    return true;
}

bool mods_set_enabled_next_start(HmlMod* mod, bool enabled) {
    if (mod == loader_self() || mod->file.empty()) return false;
    disabled_next_init();
    std::string id = lower(mod->id), file = lower(file_name(mod->file));
    g_disabled_next.erase(std::remove_if(g_disabled_next.begin(), g_disabled_next.end(),
                                         [&](const std::string& d) { return d == id || d == file; }),
                          g_disabled_next.end());
    if (!enabled) g_disabled_next.push_back(mod->status == ModStatus::Plain ||
                                                    mod->status == ModStatus::NotAMod
                                                ? file
                                                : id);
    std::string joined;
    for (const std::string& d : g_disabled_next) joined += (joined.empty() ? "" : ", ") + d;
    return WritePrivateProfileStringA("Loader", "Disabled", joined.c_str(),
                                      loader_config().ini_path.c_str()) != 0;
}

void mods_load_all() {
    if (!g_event_lock_ready) {
        InitializeCriticalSection(&g_event_lock);
        g_event_lock_ready = true;
    }
    g_api = make_api();

    const char* dir = game_info()->mods_dir;
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryA(dir, nullptr);
        LOG_I("created %s", dir);
    }
    clean_shadow_dir(dir);
    g_hot_reload = GetPrivateProfileIntA("Loader", "HotReload", 1, loader_config().ini_path.c_str()) != 0;
    std::vector<std::string> files = list_files(dir);
    LOG_I("%u file(s) in mods\\", (unsigned)files.size());
    for (const std::string& f : files) load_one(dir, f);
    unsigned active = 0;
    for (auto& m : g_mods) active += m->status == ModStatus::Active;
    LOG_I("%u mod(s) active", active);
}

void events_dispatch(HmlEvent event, const void* data) {
    if (!g_event_lock_ready) return;
    EnterCriticalSection(&g_event_lock);
    // Index loop: a handler may subscribe (append) while we iterate.
    for (size_t i = 0; i < g_subs.size(); ++i) {
        Subscription s = g_subs[i];
        if (s.event == event) s.fn(event, data, s.user);
    }
    LeaveCriticalSection(&g_event_lock);
}

// ---- hot reload -------------------------------------------------------------

namespace {

// Sends one event to a single mod's handlers (for a mod that started late).
void dispatch_to(HmlMod* mod, HmlEvent event, const void* data) {
    EnterCriticalSection(&g_event_lock);
    for (size_t i = 0; i < g_subs.size(); ++i) {
        Subscription s = g_subs[i];
        if (s.mod == mod && s.event == event) s.fn(event, data, s.user);
    }
    LeaveCriticalSection(&g_event_lock);
}

// A mod loaded after the game started has missed the device and menu events.
void catch_up(HmlMod* mod) {
    if (mod->status != ModStatus::Active) return;
    if (void* dev = d3d9_device()) {
        HmlD3D9Event ev{dev, d3d9_hwnd(), 0};
        dispatch_to(mod, HML_EVENT_D3D9_DEVICE_CREATED, &ev);
    }
    if (menu_open()) dispatch_to(mod, HML_EVENT_MENU_OPENED, nullptr);
}

struct Pending {
    FILETIME time{};
    uint64_t size = 0;
    DWORD since = 0;
};
std::map<std::string, Pending> g_pending;  // lower-case file name -> change being watched
DWORD g_last_scan = 0;

bool same_stamp(const FILETIME& a, uint64_t as, const FILETIME& b, uint64_t bs) {
    return as == bs && CompareFileTime(&a, &b) == 0;
}

}  // namespace

bool mods_hot_reload_enabled() { return g_hot_reload; }
void mods_set_hot_reload(bool on) { g_hot_reload = on; }

bool mods_reload(HmlMod* mod, std::string* why) {
    std::string dummy;
    std::string& reason = why ? *why : dummy;
    if (!mod || mod == loader_self() || mod->file.empty()) return reason = "not a mod file", false;
    if (mod->status == ModStatus::Plain || mod->status == ModStatus::NotAMod)
        return reason = "plain modules cannot be reloaded", false;
    if (mod->module && !mod->unload)
        return reason = "this mod does not support reloading (no hml_mod_unload); restart the game", false;
    std::string dir = game_info()->mods_dir, file = file_name(mod->file);
    if (GetFileAttributesA(mod->file.c_str()) == INVALID_FILE_ATTRIBUTES)
        return reason = "its file is gone", false;

    if (mod->module) {
        if (!hooks_remove_owner(mod, nullptr, true, &reason)) return false;  // check only
        LOG_I("reloading %s (%s)", mod->name.c_str(), file.c_str());
        if (mod->status == ModStatus::Active) mod->unload();
        EnterCriticalSection(&g_event_lock);
        g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(),
                                    [mod](const Subscription& s) { return s.mod == mod; }),
                     g_subs.end());
        LeaveCriticalSection(&g_event_lock);
        input_block(mod, 0);
        native::hud_hide(mod, false);
        hooks_remove_owner(mod, nullptr, false, nullptr);
        mod->options.clear();  // the entries point into the old DLL; never touched again
        Sleep(20);              // let any thread still returning through its code leave
        FreeLibrary(mod->module);
        if (!mod->loaded_copy.empty()) DeleteFileA(mod->loaded_copy.c_str());
        mod->module = nullptr;
        mod->shutdown = nullptr;
        mod->unload = nullptr;
        mod->loaded_copy.clear();
    } else {
        LOG_I("loading %s again", file.c_str());
        mod->options.clear();
    }
    g_reuse = mod;
    load_one(dir, file);
    g_reuse = nullptr;
    catch_up(mod);
    if (mod->status != ModStatus::Active) return reason = mod->note, false;
    LOG_I("%s %s reloaded", mod->name.c_str(), mod->version.c_str());
    return true;
}

// Once per frame from Present. Every second: mods whose file changed are
// reloaded (once the file has stopped changing), new files are loaded.
void mods_watch() {
    if (!g_hot_reload || !g_event_lock_ready) return;
    DWORD now = GetTickCount();
    if (now - g_last_scan < 500) return;
    g_last_scan = now;
    std::string dir = game_info()->mods_dir;

    auto settled = [&](const std::string& key, const FILETIME& t, uint64_t size) {
        Pending& p = g_pending[key];
        if (!same_stamp(p.time, p.size, t, size)) {
            p.time = t;
            p.size = size;
            p.since = now;
            return false;
        }
        return now - p.since >= 700;  // unchanged for a moment: the build is done
    };

    std::vector<std::string> files = list_files(dir);
    for (const std::string& f : files) {
        std::string path = path_join(dir, f), key = lower(f);
        FILETIME t;
        uint64_t size;
        if (!file_stamp(path, &t, &size)) continue;
        HmlMod* known = nullptr;
        for (auto& m : g_mods)
            if (lower(file_name(m->file)) == key) known = m.get();
        if (known) {
            bool reloadable = known->status == ModStatus::Active ? known->unload != nullptr
                              : known->status == ModStatus::Failed;
            if (!reloadable || same_stamp(known->file_time, known->file_size, t, size)) {
                g_pending.erase(key);
                continue;
            }
            if (!settled(key, t, size)) continue;
            g_pending.erase(key);
            std::string why;
            if (!mods_reload(known, &why)) {
                LOG_W("hot reload of %s failed: %s", f.c_str(), why.c_str());
                known->file_time = t;  // do not retry until it changes again
                known->file_size = size;
            }
        } else {
            if (!settled(key, t, size)) continue;
            g_pending.erase(key);
            LOG_I("new file in mods\\: %s", f.c_str());
            size_t before = g_mods.size();
            load_one(dir, f);
            if (g_mods.size() > before) catch_up(g_mods.back().get());
        }
    }
}

void mods_shutdown_all() {
    events_dispatch(HML_EVENT_SHUTDOWN, nullptr);
    for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it)
        if ((*it)->status == ModStatus::Active && (*it)->shutdown) (*it)->shutdown();
}

}  // namespace hml
