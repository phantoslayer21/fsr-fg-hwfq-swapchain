// vblank_probe: measure the real vblank cadence of each output (IDXGIOutput::WaitForVBlank) and DWM's timing info.
// Presents nothing. Own code; Windows SDK only.
#include <windows.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    DWM_TIMING_INFO ti = { sizeof(ti) };
    HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &ti);
    printf("DwmGetCompositionTimingInfo hr=0x%08lx rate=%u/%u qpcRefreshPeriod=%.3f ms cRefresh=%llu cFrame=%llu cFramesDisplayed=%llu cFramesDropped=%llu\n",
           hr, ti.rateRefresh.uiNumerator, ti.rateRefresh.uiDenominator, ti.qpcRefreshPeriod * 1000.0 / f.QuadPart,
           ti.cRefresh, ti.cFrame, ti.cFramesDisplayed, ti.cFramesDropped);
    Sleep(1000);
    DWM_TIMING_INFO t2 = { sizeof(t2) };
    DwmGetCompositionTimingInfo(nullptr, &t2);
    printf("after 1 s: cRefresh +%llu, cFrame +%llu, cFramesDisplayed +%llu\n", t2.cRefresh - ti.cRefresh, t2.cFrame - ti.cFrame,
           t2.cFramesDisplayed - ti.cFramesDisplayed);

    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            std::vector<double> d;
            LARGE_INTEGER prev, now;
            out->WaitForVBlank();
            QueryPerformanceCounter(&prev);
            for (int i = 0; i < 60; ++i) {
                out->WaitForVBlank();
                QueryPerformanceCounter(&now);
                d.push_back((now.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart);
                prev = now;
            }
            std::sort(d.begin(), d.end());
            double sum = 0;
            for (double x : d) sum += x;
            printf("adapter %u output %u: WaitForVBlank interval mean %.3f ms, min %.3f, median %.3f, max %.3f\n", a, o,
                   sum / d.size(), d.front(), d[d.size() / 2], d.back());
        }
    }
    return 0;
}
