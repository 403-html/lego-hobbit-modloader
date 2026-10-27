#include "internal.h"

#include <cstdarg>
#include <cstdio>

namespace hml {
namespace {

CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
FILE* g_file = nullptr;
HANDLE g_console = nullptr;
HmlLogLevel g_min = HML_LOG_INFO;

const char* level_tag(HmlLogLevel l) {
    switch (l) {
        case HML_LOG_DEBUG: return "DBG";
        case HML_LOG_INFO: return "INF";
        case HML_LOG_WARN: return "WRN";
        default: return "ERR";
    }
}

}  // namespace

void log_init(const std::string& path, bool console, HmlLogLevel min_level) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    g_min = min_level;
    g_file = std::fopen(path.c_str(), "w");
    if (console && AllocConsole()) {
        g_console = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTitleA("Hobbit Mod Loader");
    }
}

void log_write(const HmlMod* mod, HmlLogLevel level, const char* fmt, va_list ap) {
    if (level < g_min || !fmt) return;

    char msg[2048];
    std::vsnprintf(msg, sizeof msg, fmt, ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2300];
    int n = std::snprintf(line, sizeof line, "[%02u:%02u:%02u.%03u] %s [%s] %s\n", t.wHour,
                          t.wMinute, t.wSecond, t.wMilliseconds, level_tag(level),
                          mod ? mod->id.c_str() : "?", msg);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;

    if (g_lock_ready) EnterCriticalSection(&g_lock);
    if (g_file) {
        std::fwrite(line, 1, (size_t)n, g_file);
        std::fflush(g_file);  // keep the log useful if the game crashes
    }
    if (g_console) {
        DWORD w;
        WriteConsoleA(g_console, line, (DWORD)n, &w, nullptr);
    }
    OutputDebugStringA(line);
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

void logf(HmlLogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(loader_self(), level, fmt, ap);
    va_end(ap);
}

void log_shutdown() {
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace hml
