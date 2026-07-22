// ==WindhawkMod==
// @id              island-host-watchdog
// @name            Island Host Watchdog
// @description     Keeps island-host.exe (Island Command Center's dedicated host process) running -- relaunches it if it ever exits, since Windhawk itself only injects into existing processes and has no notion of supervising a standalone one.
// @version         1.0.0
// @author          Matt Pincoski
// @include         explorer.exe
// @architecture    x86-64
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Island Host Watchdog

Companion to **Island Command Center**. That mod runs inside a dedicated `island-host.exe` process
(not explorer.exe), since it draws its own standalone overlay rather than hooking Explorer. Windhawk
injects mods into processes that already exist -- it doesn't launch or supervise a standalone one, so
if `island-host.exe` ever exits (a crash, the user closing it by hand, anything else) nothing brings it
back until the next logon, when its Startup-folder shortcut fires once.

This mod runs a lightweight timer thread *inside explorer.exe* -- which is already alive for your whole
session anyway -- that checks every 20 seconds whether `island-host.exe` is running, and relaunches it if
not. No separate background process, no Scheduled Task: it reuses the Explorer process Windhawk is
already injected into for your other shell mods, so the marginal cost is one sleeping thread and an
occasional single syscall.

The check is a plain `OpenMutex` against `island-host.exe`'s own single-instance mutex
(`IslandCommandCenterHost`) rather than scanning the process list, and skips relaunching entirely while
Island Command Center itself is disabled in Windhawk -- so disabling that mod also quiets this one.
*/
// ==/WindhawkModReadme==

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <iterator>
#include <string>

namespace {

constexpr wchar_t kHostMutexName[] = L"IslandCommandCenterHost";
constexpr wchar_t kIslandModIniPath[] =
    L"C:\\Users\\m.pincoski\\AppData\\Local\\Windhawk\\AppData\\Engine\\Mods\\island-command-center.ini";
constexpr DWORD kCheckIntervalMs = 20000;
constexpr DWORD kInitialDelayMs = 5000;  // let the desktop settle after logon/Explorer restart first

HANDLE g_stopEvent = nullptr;
HANDLE g_thread = nullptr;

// island-host.exe takes this same mutex (non-abandoning, CreateMutexW with bInitialOwner=TRUE) as its
// own single-instance guard; a plain existence check is a cheaper and more precise liveness signal than
// scanning the system process list by executable name.
bool IsHostRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kHostMutexName);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// Best-effort: don't bother keeping the host alive if the user has disabled Island Command Center
// itself in Windhawk. Read directly via the native (BOM-aware) profile APIs -- the mod ini is UTF-16.
bool IslandModDisabled() {
    wchar_t buf[8]{};
    GetPrivateProfileStringW(L"Mod", L"Disabled", L"0", buf, static_cast<DWORD>(std::size(buf)),
                              kIslandModIniPath);
    return buf[0] == L'1';
}

std::wstring GetHostExePath() {
    wchar_t localAppData[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (n == 0 || n >= std::size(localAppData)) return L"";
    return std::wstring(localAppData) + L"\\IslandCommandCenter\\island-host.exe";
}

void LaunchHost() {
    std::wstring exePath = GetHostExePath();
    if (exePath.empty()) { Wh_Log(L"Island Host Watchdog: couldn't resolve %%LOCALAPPDATA%%."); return; }
    std::wstring workDir = exePath.substr(0, exePath.find_last_of(L'\\'));

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    if (CreateProcessW(exePath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.c_str(), &si, &pi)) {
        Wh_Log(L"Island Host Watchdog: relaunched island-host.exe (pid %lu).", pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        Wh_Log(L"Island Host Watchdog: CreateProcess failed (%u).", GetLastError());
    }
}

DWORD WINAPI WatchdogThreadProc(void*) {
    if (WaitForSingleObject(g_stopEvent, kInitialDelayMs) == WAIT_OBJECT_0) return 0;
    while (WaitForSingleObject(g_stopEvent, kCheckIntervalMs) == WAIT_TIMEOUT) {
        if (!IslandModDisabled() && !IsHostRunning()) LaunchHost();
    }
    return 0;
}

}  // namespace

BOOL Wh_ModInit() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) return FALSE;
    g_thread = CreateThread(nullptr, 0, WatchdogThreadProc, nullptr, 0, nullptr);
    if (!g_thread) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; return FALSE; }
    return TRUE;
}

void Wh_ModUninit() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) { WaitForSingleObject(g_thread, 4000); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
}
