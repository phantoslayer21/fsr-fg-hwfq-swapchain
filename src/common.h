// common.h - configuration, logging and timing shared by the fgswap DLL.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace fgswap {

enum class Pacing { Spin, Early, Queue, Amd };

struct Config {
    Pacing pacing = Pacing::Spin;  // spin: absolute schedule, timer then busy-spin the last spin_us; early: present
                                   // early_ms before each slot from a plain timer (refresh-matched FreeSync);
                                   // queue: present each batch back to back with sync interval queue_interval
                                   // (fixed refresh); amd: hand the swapchain to AMD's original DLL unchanged.
    double earlyMs = 1.5;
    int spinUs = 500;
    int queueInterval = 0;          // 0 = derive from refresh rate / output rate
    int presenterPriority = THREAD_PRIORITY_HIGHEST;
    int maxQueuedFrames = 2;        // real frames the game may run ahead of the presenter
    bool allowTearing = true;       // use tearing presents when the game asks for sync interval 0
    bool log = true;
    bool presentLog = false;        // per-present CSV (fgswap_presents.csv) for pacing analysis
    std::wstring original;          // path of AMD's original DLL, relative to this DLL's folder unless absolute
    std::wstring dir;               // folder of this DLL (with trailing backslash)
};

Config& Cfg();
void LoadConfig(HMODULE self);
void Log(const char* fmt, ...);

inline int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t QpcFreq();
inline double QpcToMs(int64_t d) { return d * 1000.0 / (double)QpcFreq(); }
inline int64_t MsToQpc(double ms) { return (int64_t)(ms * (double)QpcFreq() / 1000.0); }

// Sleep with a high-resolution waitable timer until `spinBefore` before t, then (if spin) busy-wait to t.
void WaitUntil(int64_t t, bool spin);

}  // namespace fgswap
