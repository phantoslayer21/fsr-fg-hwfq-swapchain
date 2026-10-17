// common.cpp - configuration (fgswap.ini next to the DLL), log file and timing helpers.
#include "common.h"
#include <cstdarg>
#include <mutex>

namespace fgswap {

static Config g_cfg;
static std::mutex g_logLock;
static FILE* g_logFile;

Config& Cfg() { return g_cfg; }

int64_t QpcFreq()
{
    static int64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return (int64_t)x.QuadPart; }();
    return f;
}

static std::wstring IniString(const std::wstring& ini, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"fgswap", key, def, buf, 1024, ini.c_str());
    return buf;
}

void LoadConfig(HMODULE self)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    std::wstring dir = p.substr(0, p.find_last_of(L"\\/") + 1);
    std::wstring name = p.substr(dir.size());
    std::wstring ini = dir + L"fgswap.ini";
    Config& c = g_cfg;
    c.dir = dir;
    std::wstring pacing = IniString(ini, L"pacing", L"spin");
    if (pacing == L"early") c.pacing = Pacing::Early;
    else if (pacing == L"queue") c.pacing = Pacing::Queue;
    else if (pacing == L"amd") c.pacing = Pacing::Amd;
    else c.pacing = Pacing::Spin;
    c.earlyMs = _wtof(IniString(ini, L"early_ms", L"1.5").c_str());
    c.spinUs = GetPrivateProfileIntW(L"fgswap", L"spin_us", 500, ini.c_str());
    c.queueInterval = GetPrivateProfileIntW(L"fgswap", L"queue_interval", 0, ini.c_str());
    std::wstring prio = IniString(ini, L"presenter_priority", L"highest");
    c.presenterPriority = prio == L"normal" ? THREAD_PRIORITY_NORMAL
                        : prio == L"time_critical" ? THREAD_PRIORITY_TIME_CRITICAL
                        : prio == L"above_normal" ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_HIGHEST;
    c.maxQueuedFrames = GetPrivateProfileIntW(L"fgswap", L"max_queued_frames", 2, ini.c_str());
    if (c.maxQueuedFrames < 1) c.maxQueuedFrames = 1;
    if (c.maxQueuedFrames > 4) c.maxQueuedFrames = 4;
    c.allowTearing = GetPrivateProfileIntW(L"fgswap", L"allow_tearing", 1, ini.c_str()) != 0;
    c.log = GetPrivateProfileIntW(L"fgswap", L"log", 1, ini.c_str()) != 0;
    c.presentLog = GetPrivateProfileIntW(L"fgswap", L"present_log", 0, ini.c_str()) != 0;
    // Default location of AMD's original: a subfolder that keeps its real file name (AMD's DLL looks itself up by name).
    c.original = IniString(ini, L"original", (L"fgswap\\" + name).c_str());
    if (c.original.size() < 2 || c.original[1] != L':') c.original = dir + c.original;
    if (c.log) {
        std::lock_guard<std::mutex> l(g_logLock);
        g_logFile = _wfopen((dir + L"fgswap.log").c_str(), L"w");
    }
}

void Log(const char* fmt, ...)
{
    if (!g_cfg.log) return;
    std::lock_guard<std::mutex> l(g_logLock);
    if (!g_logFile) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_logFile, fmt, a);
    va_end(a);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

void WaitUntil(int64_t t, bool spin)
{
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t spinWindow = spin ? MsToQpc(g_cfg.spinUs / 1000.0) : 0;
    for (;;) {
        int64_t rem = t - Qpc();
        if (rem <= 0) return;
        if (rem > spinWindow + MsToQpc(0.25)) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(QpcToMs(rem - spinWindow) * 10000.0);
            if (due.QuadPart >= 0) due.QuadPart = -1;
            SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(timer, INFINITE);
        } else if (spin) {
            YieldProcessor();
        } else {
            return;  // early mode: the display holds the frame; sub-quarter-ms precision is not needed
        }
    }
}

}  // namespace fgswap
