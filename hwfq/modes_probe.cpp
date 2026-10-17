// modes_probe: list the refresh rates the primary display offers at its current resolution. Read-only.
#include <windows.h>
#include <cstdio>
#include <set>

int main()
{
    DEVMODEW cur = { sizeof(cur) };
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur);
    std::set<DWORD> hz;
    DEVMODEW m = { sizeof(m) };
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &m); ++i)
        if (m.dmPelsWidth == cur.dmPelsWidth && m.dmPelsHeight == cur.dmPelsHeight) hz.insert(m.dmDisplayFrequency);
    printf("current %lux%lu @ %lu Hz; available at this size:", cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency);
    for (DWORD h : hz) printf(" %lu", h);
    printf("\n");
    return 0;
}
