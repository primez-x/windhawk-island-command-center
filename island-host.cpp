// island-host.exe — a minimal, dedicated host process for the Island Command
// Center Windhawk mod. Windhawk injects the mod into this process (matched by
// name via @include island-host.exe); the mod runs its overlay + worker threads
// here, isolated from explorer.exe and windhawk.exe. The host just stays alive.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Per-monitor DPI aware process-wide so the mod's monitor/coordinate math is
    // consistent across the multi-DPI display setup.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Single instance.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"IslandCommandCenterHost");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    // Stay alive so Windhawk keeps the mod loaded. The mod creates its own
    // window + message pump on its render thread; this thread just waits.
    Sleep(INFINITE);
    return 0;
}
