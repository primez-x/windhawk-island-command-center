// ==WindhawkMod==
// @id              island-command-center
// @name            Island Command Center
// @description     An interactive island overlay: a unified, functional control & notification center inspired by the Dynamic Island design language.
// @version         1.0.0
// @author          Matt Pincoski
// @include         island-host.exe
// @compilerOptions -lole32 -loleaut32 -lshcore -ld2d1 -ldwrite -ldwmapi -lgdi32 -luser32 -lshell32 -lruntimeobject -lwindowscodecs -lavrt -lsetupapi -lwinhttp -lpdh -ldxva2 -ladvapi32 -lwinmm
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Island Command Center

A ground-up reimagining of the Dynamic Island as a genuinely *interactive*,
unified surface for native Windows features — instant volume & brightness
sliders, a real notification center (history, dismiss, clear-all), a functional
calendar backed by your Outlook published feed, media with live waveform, and
the island's signature transient pills.

Windhawk **tool mod**: runs in a dedicated `windhawk.exe` process and draws a
layered, click-through overlay. It does not modify Explorer.

> Build status: **interactive core + volume**. Hover the pill to peek; click to
> open the control center; drag the volume slider; click outside to dismiss.

## Hiding the native taskbar duplicates
This mod *owns* these surfaces, so hide the taskbar equivalents with the
companion **Taskbar tray system icon tweaks** mod (bell, volume, battery,
network) and a clock mod / the Taskbar Styler (clock).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- Position: top-center
  $name: Position
  $options:
  - top-center: Top center
  - top-left: Top left
  - top-right: Top right
  - bottom-center: Bottom center
- TargetMonitor: "0"
  $name: Target monitor
  $description: 0 = primary, 1-8 = that monitor, -1 = follow the mouse.
- OffsetX: 0
  $name: Horizontal offset (px)
- OffsetY: 0
  $name: Vertical offset (px)
- SizeScale: "1.0"
  $name: Size scale
  $description: Overall scale of the island (e.g. 0.9, 1.0, 1.25).
- AutoDpiScale: true
  $name: Auto DPI scaling
  $description: Scale with the monitor's DPI in addition to Size scale.
- AlwaysOnTop: true
  $name: Always on top
- PillOpacity: "1.0"
  $name: Pill opacity
  $description: 0.35 - 1.0.
- CalendarIcsUrl: ""
  $name: Calendar ICS URL
  $description: >-
    Your Outlook published-calendar .ics feed URL. The island fetches and
    parses this to show your real events. Treated as private; never shared.
- CalendarRefreshMinutes: 20
  $name: Calendar refresh interval (minutes)
- BrightnessEnabled: true
  $name: Enable brightness control
  $description: Show a brightness slider for the island's monitor (DDC/CI). Hidden automatically if the monitor doesn't support it.
- WeatherCity: ""
  $name: Weather city
  $description: City for weather (e.g. "Colorado Springs, CO"). Leave blank to auto-detect by IP.
- WeatherFahrenheit: true
  $name: Use Fahrenheit
  $description: Show temperature in °F (off = °C).
- CaptureVolumeKeys: true
  $name: Capture volume keys (skin the volume OSD)
  $description: >-
    Intercept the keyboard volume up/down/mute keys so the island shows its own
    volume popup and the native Windows volume flyout never appears. Turn off to
    use the standard Windows volume OSD.
*/
// ==/WindhawkModSettings==

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shcore.h>
#include <windowsx.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#if __has_include(<winrt/Windows.UI.Notifications.Management.h>) && \
    __has_include(<winrt/Windows.UI.Notifications.h>)
#define ICC_HAS_NOTIFICATION_LISTENER 1
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#else
#define ICC_HAS_NOTIFICATION_LISTENER 0
#endif

using Microsoft::WRL::ComPtr;

// mingw's endpointvolume.h only forward-declares IAudioMeterInformation; supply the
// full vtable + IID so we can read live output peak levels for the waveform.
struct IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount, float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* pdwHardwareSupportMask) = 0;
};
static const GUID ICC_IID_IAudioMeterInformation = {
    0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

namespace {

constexpr wchar_t kWindowClass[] = L"Windhawk.IslandCommandCenter";
constexpr UINT WM_APP_NEW_EVENT = WM_APP + 0x443;
constexpr UINT WM_APP_DISMISS   = WM_APP + 0x444;
constexpr UINT WM_APP_VOLKEY    = WM_APP + 0x445;  // wParam = VK_VOLUME_UP/DOWN/MUTE (from the key hook)

constexpr float kRenderPadX = 26.0f;
constexpr float kRenderPadY = 22.0f;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
bool EqualsNoCase(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

std::wstring GetStringSettingCopy(PCWSTR name) {
    PCWSTR value = Wh_GetStringSetting(name);
    std::wstring copy = value ? value : L"";
    Wh_FreeStringSetting(value);
    return copy;
}

// ===========================================================================
// iCalendar (.ics) parser — validated standalone against the Outlook feed.
// Handles line folding, VEVENT, TZID (Windows tz names via registry),
// RRULE expansion (DAILY/WEEKLY/MONTHLY/YEARLY + INTERVAL/COUNT/UNTIL/BYDAY/
// BYMONTHDAY/BYMONTH), EXDATE, RECURRENCE-ID overrides, all-day, de-dup.
// ===========================================================================
struct IcsEvent {
    std::wstring subject;
    std::wstring location;
    SYSTEMTIME start{};   // user local
    SYSTEMTIME end{};     // user local
    bool allDay = false;
    std::wstring uid;
};

namespace ics_detail {

inline std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::vector<std::string> Unfold(const std::string& in) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    auto flush = [&] { lines.push_back(cur); cur.clear(); };
    while (i < in.size()) {
        char c = in[i];
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            if (i + 1 < in.size() && (in[i + 1] == ' ' || in[i + 1] == '\t')) { i += 2; continue; }
            flush(); ++i; continue;
        }
        cur.push_back(c); ++i;
    }
    if (!cur.empty()) flush();
    return lines;
}

struct Prop {
    std::string name;
    std::map<std::string, std::string> params;
    std::string value;
};
inline std::string Upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
inline Prop SplitProp(const std::string& line) {
    Prop p;
    size_t colon = line.find(':');
    if (colon == std::string::npos) { p.name = Upper(line); return p; }
    std::string head = line.substr(0, colon);
    p.value = line.substr(colon + 1);
    size_t semi = head.find(';');
    p.name = Upper(head.substr(0, semi));
    while (semi != std::string::npos) {
        size_t next = head.find(';', semi + 1);
        std::string kv = head.substr(semi + 1, (next == std::string::npos ? std::string::npos : next - semi - 1));
        size_t eq = kv.find('=');
        if (eq != std::string::npos) p.params[Upper(kv.substr(0, eq))] = kv.substr(eq + 1);
        semi = next;
    }
    return p;
}
inline std::wstring UnescapeText(const std::string& v) {
    std::wstring w = Utf8ToW(v), out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L'\\' && i + 1 < w.size()) {
            wchar_t n = w[i + 1];
            if (n == L'n' || n == L'N') { out.push_back(L' '); ++i; }
            else if (n == L',' || n == L';' || n == L'\\') { out.push_back(n); ++i; }
            else out.push_back(w[i]);
        } else out.push_back(w[i]);
    }
    return out;
}
inline int N(const std::string& s, size_t off, int len) {
    int v = 0;
    for (int i = 0; i < len && off + i < s.size(); ++i) { char c = s[off + i]; if (c < '0' || c > '9') return v; v = v * 10 + (c - '0'); }
    return v;
}
#pragma pack(push, 1)
struct RegTzi { LONG Bias; LONG StandardBias; LONG DaylightBias; SYSTEMTIME StandardDate; SYSTEMTIME DaylightDate; };
#pragma pack(pop)
inline bool GetTzi(const std::wstring& name, TIME_ZONE_INFORMATION& out) {
    static std::map<std::wstring, TIME_ZONE_INFORMATION> cache;
    static std::unordered_set<std::wstring> missing;
    auto it = cache.find(name);
    if (it != cache.end()) { out = it->second; return true; }
    if (missing.count(name)) return false;
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\" + name;
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) { missing.insert(name); return false; }
    RegTzi rtzi{}; DWORD sz = sizeof(rtzi), type = 0;
    LONG r = RegQueryValueExW(h, L"TZI", nullptr, &type, (LPBYTE)&rtzi, &sz);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS) { missing.insert(name); return false; }
    TIME_ZONE_INFORMATION tzi{};
    tzi.Bias = rtzi.Bias; tzi.StandardBias = rtzi.StandardBias; tzi.DaylightBias = rtzi.DaylightBias;
    tzi.StandardDate = rtzi.StandardDate; tzi.DaylightDate = rtzi.DaylightDate;
    cache[name] = tzi; out = tzi; return true;
}
inline uint64_t StToU64(const SYSTEMTIME& st) {
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; return u.QuadPart;
}
inline SYSTEMTIME U64ToSt(uint64_t v) {
    ULARGE_INTEGER u; u.QuadPart = v;
    FILETIME ft; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME st; FileTimeToSystemTime(&ft, &st); return st;
}
inline SYSTEMTIME AddDays(const SYSTEMTIME& st, int days) { return U64ToSt(StToU64(st) + (uint64_t)days * 86400ULL * 10000000ULL); }
inline int DayOfWeek(const SYSTEMTIME& st) { SYSTEMTIME n = U64ToSt(StToU64(st)); return n.wDayOfWeek; }

struct Dt { SYSTEMTIME wall{}; bool valid = false; bool dateOnly = false; int mode = 0; TIME_ZONE_INFORMATION tzi{}; };
inline Dt ParseDt(const std::string& value, const std::map<std::string, std::string>& params) {
    Dt d;
    if (value.size() < 8) return d;
    SYSTEMTIME st{};
    st.wYear = (WORD)N(value, 0, 4); st.wMonth = (WORD)N(value, 4, 2); st.wDay = (WORD)N(value, 6, 2);
    bool dateOnly = true;
    if (value.size() >= 15 && (value[8] == 'T' || value[8] == 't')) {
        st.wHour = (WORD)N(value, 9, 2); st.wMinute = (WORD)N(value, 11, 2); st.wSecond = (WORD)N(value, 13, 2);
        dateOnly = false;
    }
    d.wall = st;
    d.valid = (st.wYear > 1900 && st.wMonth >= 1 && st.wMonth <= 12 && st.wDay >= 1 && st.wDay <= 31);
    d.dateOnly = dateOnly;
    auto vt = params.find("VALUE");
    if (vt != params.end() && Upper(vt->second) == "DATE") d.dateOnly = true;
    if (!value.empty() && (value.back() == 'Z' || value.back() == 'z')) d.mode = 1;
    else {
        auto tz = params.find("TZID");
        if (tz != params.end()) {
            TIME_ZONE_INFORMATION tzi;
            if (GetTzi(Utf8ToW(tz->second), tzi)) { d.mode = 2; d.tzi = tzi; } else d.mode = 0;
        } else d.mode = 0;
    }
    return d;
}
inline uint64_t ToUtc(const Dt& d) {
    if (d.dateOnly) { SYSTEMTIME utc{}; TzSpecificLocalTimeToSystemTime(nullptr, &d.wall, &utc); return StToU64(utc); }
    if (d.mode == 1) return StToU64(d.wall);
    SYSTEMTIME utc{};
    if (d.mode == 2) { TIME_ZONE_INFORMATION tzi = d.tzi; TzSpecificLocalTimeToSystemTime(&tzi, &d.wall, &utc); }
    else TzSpecificLocalTimeToSystemTime(nullptr, &d.wall, &utc);
    return StToU64(utc);
}
inline uint64_t WallToUtc(const SYSTEMTIME& wall, const Dt& frame) {
    SYSTEMTIME utc{};
    if (frame.mode == 2) { TIME_ZONE_INFORMATION tzi = frame.tzi; TzSpecificLocalTimeToSystemTime(&tzi, &wall, &utc); }
    else if (frame.mode == 1) utc = wall;
    else TzSpecificLocalTimeToSystemTime(nullptr, &wall, &utc);
    return StToU64(utc);
}
inline SYSTEMTIME UtcToUserLocal(uint64_t utc) { SYSTEMTIME u = U64ToSt(utc), local{}; SystemTimeToTzSpecificLocalTime(nullptr, &u, &local); return local; }

struct RRule {
    std::string freq; int interval = 1; int count = -1; uint64_t until = 0; bool hasUntil = false;
    std::vector<std::pair<int, int>> byday; std::vector<int> bymonthday; std::vector<int> bymonth;
};
inline int WeekdayCode(const std::string& s) {
    static const char* names[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; ++i) if (s == names[i]) return i;
    return -1;
}
inline RRule ParseRRule(const std::string& v) {
    RRule r; size_t i = 0;
    while (i < v.size()) {
        size_t semi = v.find(';', i);
        std::string tok = v.substr(i, (semi == std::string::npos ? std::string::npos : semi - i));
        size_t eq = tok.find('=');
        if (eq != std::string::npos) {
            std::string k = Upper(tok.substr(0, eq)), val = tok.substr(eq + 1);
            if (k == "FREQ") r.freq = Upper(val);
            else if (k == "INTERVAL") r.interval = std::max(1, atoi(val.c_str()));
            else if (k == "COUNT") r.count = atoi(val.c_str());
            else if (k == "UNTIL") { Dt d = ParseDt(val, {}); if (d.valid) { r.until = (d.mode == 1) ? StToU64(d.wall) : ToUtc(d); r.hasUntil = true; } }
            else if (k == "BYDAY") {
                size_t p = 0;
                while (p < val.size()) {
                    size_t c = val.find(',', p);
                    std::string item = val.substr(p, (c == std::string::npos ? std::string::npos : c - p));
                    int ord = 0; size_t q = 0;
                    if (!item.empty() && (item[0] == '+' || item[0] == '-' || isdigit((unsigned char)item[0]))) {
                        int sign = 1; if (item[0] == '+') q = 1; else if (item[0] == '-') { sign = -1; q = 1; }
                        int num = 0; while (q < item.size() && isdigit((unsigned char)item[q])) { num = num * 10 + (item[q] - '0'); ++q; }
                        ord = sign * num;
                    }
                    int wd = WeekdayCode(item.substr(q));
                    if (wd >= 0) r.byday.push_back({ord, wd});
                    if (c == std::string::npos) break;
                    p = c + 1;
                }
            } else if (k == "BYMONTHDAY") { size_t p = 0; while (p < val.size()) { size_t c = val.find(',', p); r.bymonthday.push_back(atoi(val.substr(p, c - p).c_str())); if (c == std::string::npos) break; p = c + 1; } }
            else if (k == "BYMONTH") { size_t p = 0; while (p < val.size()) { size_t c = val.find(',', p); r.bymonth.push_back(atoi(val.substr(p, c - p).c_str())); if (c == std::string::npos) break; p = c + 1; } }
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return r;
}
inline int DaysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[(m - 1) % 12];
}
inline int NthWeekdayOfMonth(int y, int m, int ord, int wd) {
    int dim = DaysInMonth(y, m); std::vector<int> days;
    for (int d = 1; d <= dim; ++d) { SYSTEMTIME st{}; st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d; st.wHour = 12; if (DayOfWeek(st) == wd) days.push_back(d); }
    if (days.empty()) return -1;
    if (ord > 0 && ord <= (int)days.size()) return days[ord - 1];
    if (ord < 0 && -ord <= (int)days.size()) return days[days.size() + ord];
    return -1;
}
struct RawEvent {
    std::string uid, summary, location, status;
    Dt dtstart, dtend; bool hasRRule = false; RRule rrule;
    std::vector<uint64_t> exdates; bool hasRecurId = false; uint64_t recurId = 0; bool hasDtEnd = false;
};

}  // namespace ics_detail

inline std::vector<IcsEvent> ParseIcsEvents(const std::string& icsUtf8, const SYSTEMTIME& windowStartLocal, int days) {
    using namespace ics_detail;
    std::vector<IcsEvent> result;
    SYSTEMTIME ws = windowStartLocal; ws.wHour = ws.wMinute = ws.wSecond = ws.wMilliseconds = 0;
    SYSTEMTIME we = AddDays(ws, days);
    uint64_t winStartUtc, winEndUtc;
    { SYSTEMTIME u{}; TzSpecificLocalTimeToSystemTime(nullptr, &ws, &u); winStartUtc = StToU64(u); }
    { SYSTEMTIME u{}; TzSpecificLocalTimeToSystemTime(nullptr, &we, &u); winEndUtc = StToU64(u); }

    auto lines = Unfold(icsUtf8);
    std::vector<RawEvent> raws; RawEvent cur; bool in = false;
    for (auto& ln : lines) {
        if (ln == "BEGIN:VEVENT") { in = true; cur = RawEvent(); continue; }
        if (ln == "END:VEVENT") { if (in) raws.push_back(cur); in = false; continue; }
        if (!in) continue;
        Prop p = SplitProp(ln);
        if (p.name == "UID") cur.uid = p.value;
        else if (p.name == "SUMMARY") cur.summary = p.value;
        else if (p.name == "LOCATION") cur.location = p.value;
        else if (p.name == "STATUS") cur.status = Upper(p.value);
        else if (p.name == "DTSTART") cur.dtstart = ParseDt(p.value, p.params);
        else if (p.name == "DTEND") { cur.dtend = ParseDt(p.value, p.params); cur.hasDtEnd = true; }
        else if (p.name == "RRULE") { cur.rrule = ParseRRule(p.value); cur.hasRRule = true; }
        else if (p.name == "RECURRENCE-ID") { Dt d = ParseDt(p.value, p.params); cur.recurId = ToUtc(d); cur.hasRecurId = true; }
        else if (p.name == "EXDATE") {
            size_t q = 0;
            while (q < p.value.size()) {
                size_t c = p.value.find(',', q);
                std::string item = p.value.substr(q, (c == std::string::npos ? std::string::npos : c - q));
                Dt d = ParseDt(item, p.params);
                if (d.valid) cur.exdates.push_back(ToUtc(d));
                if (c == std::string::npos) break;
                q = c + 1;
            }
        }
    }

    std::unordered_map<std::string, std::unordered_set<uint64_t>> overrides;
    auto emit = [&](const RawEvent& e, uint64_t startUtc, uint64_t durTicks) {
        if (startUtc >= winEndUtc || (startUtc + durTicks) <= winStartUtc) return;
        IcsEvent ev;
        ev.subject = UnescapeText(e.summary);
        ev.location = UnescapeText(e.location);
        ev.allDay = e.dtstart.dateOnly;
        ev.start = UtcToUserLocal(startUtc);
        ev.end = UtcToUserLocal(startUtc + durTicks);
        ev.uid = Utf8ToW(e.uid);
        result.push_back(ev);
    };

    for (auto& e : raws) {
        if (!e.dtstart.valid) continue;
        uint64_t startUtc = ToUtc(e.dtstart);
        uint64_t durTicks;
        if (e.hasDtEnd && e.dtend.valid) { uint64_t endUtc = ToUtc(e.dtend); durTicks = (endUtc > startUtc) ? (endUtc - startUtc) : (e.dtstart.dateOnly ? 86400ULL * 10000000ULL : 3600ULL * 10000000ULL); }
        else durTicks = e.dtstart.dateOnly ? 86400ULL * 10000000ULL : 3600ULL * 10000000ULL;
        if (e.hasRecurId) { overrides[e.uid].insert(e.recurId); if (e.status != "CANCELLED") emit(e, startUtc, durTicks); continue; }
        if (!e.hasRRule) { emit(e, startUtc, durTicks); continue; }
    }

    for (auto& e : raws) {
        if (!e.hasRRule || e.hasRecurId || !e.dtstart.valid) continue;
        uint64_t startUtc0 = ToUtc(e.dtstart);
        uint64_t durTicks;
        if (e.hasDtEnd && e.dtend.valid) { uint64_t eu = ToUtc(e.dtend); durTicks = eu > startUtc0 ? eu - startUtc0 : 3600ULL * 10000000ULL; }
        else durTicks = e.dtstart.dateOnly ? 86400ULL * 10000000ULL : 3600ULL * 10000000ULL;
        const RRule& r = e.rrule;
        const SYSTEMTIME base = e.dtstart.wall;
        std::unordered_set<uint64_t> exset(e.exdates.begin(), e.exdates.end());
        auto& ovset = overrides[e.uid];
        int emitted = 0, guard = 0; const int kCap = 1500;
        auto consider = [&](const SYSTEMTIME& wall) -> bool {
            uint64_t occUtc = WallToUtc(wall, e.dtstart);
            if (r.hasUntil && occUtc > r.until) return false;
            if (occUtc >= winEndUtc) return false;
            ++emitted;
            if (r.count >= 0 && emitted > r.count) return false;
            if (occUtc + durTicks > winStartUtc) { if (!exset.count(occUtc) && !ovset.count(occUtc)) emit(e, occUtc, durTicks); }
            return true;
        };
        if (r.freq == "DAILY") {
            SYSTEMTIME w = base; while (guard++ < kCap) { if (!consider(w)) break; w = AddDays(w, r.interval); }
        } else if (r.freq == "WEEKLY") {
            std::vector<int> wds; for (auto& bd : r.byday) wds.push_back(bd.second);
            if (wds.empty()) wds.push_back(DayOfWeek(base));
            std::sort(wds.begin(), wds.end());
            SYSTEMTIME weekStart = AddDays(base, -DayOfWeek(base)); bool stop = false;
            while (!stop && guard++ < kCap) {
                for (int wd : wds) {
                    SYSTEMTIME occ = AddDays(weekStart, wd);
                    occ.wHour = base.wHour; occ.wMinute = base.wMinute; occ.wSecond = base.wSecond;
                    if (StToU64(occ) < StToU64(base)) continue;
                    if (!consider(occ)) { stop = true; break; }
                }
                weekStart = AddDays(weekStart, 7 * r.interval);
            }
        } else if (r.freq == "MONTHLY") {
            int y = base.wYear, m = base.wMonth;
            while (guard++ < kCap) {
                bool stop = false; std::vector<int> dom;
                if (!r.bymonthday.empty()) dom = r.bymonthday;
                else if (!r.byday.empty()) { for (auto& bd : r.byday) { int d = NthWeekdayOfMonth(y, m, bd.first ? bd.first : 1, bd.second); if (d > 0) dom.push_back(d); } }
                else dom.push_back(base.wDay);
                std::sort(dom.begin(), dom.end());
                for (int d : dom) { if (d < 1 || d > DaysInMonth(y, m)) continue; SYSTEMTIME occ = base; occ.wYear = (WORD)y; occ.wMonth = (WORD)m; occ.wDay = (WORD)d; if (StToU64(occ) < StToU64(base)) continue; if (!consider(occ)) { stop = true; break; } }
                if (stop) break;
                m += r.interval; while (m > 12) { m -= 12; ++y; }
            }
        } else if (r.freq == "YEARLY") {
            int y = base.wYear;
            while (guard++ < kCap) {
                bool stop = false; std::vector<int> months = r.bymonth.empty() ? std::vector<int>{base.wMonth} : r.bymonth;
                for (int m : months) {
                    std::vector<int> dom;
                    if (!r.bymonthday.empty()) dom = r.bymonthday;
                    else if (!r.byday.empty()) { for (auto& bd : r.byday) { int d = NthWeekdayOfMonth(y, m, bd.first ? bd.first : 1, bd.second); if (d > 0) dom.push_back(d); } }
                    else dom.push_back(base.wDay);
                    for (int d : dom) { if (d < 1 || d > DaysInMonth(y, m)) continue; SYSTEMTIME occ = base; occ.wYear = (WORD)y; occ.wMonth = (WORD)m; occ.wDay = (WORD)d; if (StToU64(occ) < StToU64(base)) continue; if (!consider(occ)) { stop = true; break; } }
                    if (stop) break;
                }
                if (stop) break;
                y += r.interval;
            }
        } else {
            if (startUtc0 + durTicks > winStartUtc && startUtc0 < winEndUtc) emit(e, startUtc0, durTicks);
        }
    }

    std::sort(result.begin(), result.end(), [](const IcsEvent& a, const IcsEvent& b) {
        FILETIME fa, fb; SystemTimeToFileTime(&a.start, &fa); SystemTimeToFileTime(&b.start, &fb);
        LONG c = CompareFileTime(&fa, &fb); if (c != 0) return c < 0; return a.uid < b.uid;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const IcsEvent& a, const IcsEvent& b) {
        FILETIME fa, fb; SystemTimeToFileTime(&a.start, &fa); SystemTimeToFileTime(&b.start, &fb);
        return !a.uid.empty() && a.uid == b.uid && CompareFileTime(&fa, &fb) == 0;
    }), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
enum class Position { TopCenter, TopLeft, TopRight, BottomCenter };

struct Settings {
    Position position = Position::TopCenter;
    int targetMonitor = 0;
    int offsetX = 0;
    int offsetY = 0;
    float sizeScale = 1.0f;
    bool autoDpiScale = true;
    bool alwaysOnTop = true;
    float pillOpacity = 1.0f;
    std::wstring calendarIcsUrl;
    int calendarRefreshMinutes = 20;
    bool brightnessEnabled = true;
    std::wstring weatherCity;
    bool weatherFahrenheit = true;
    bool captureVolumeKeys = true;
};

Settings g_settings;
std::mutex g_settingsMutex;
std::atomic<bool> g_captureVolKeys{true};  // lock-free mirror of Settings.captureVolumeKeys for the LL key hook

void LoadSettings() {
    Settings next;
    const std::wstring pos = GetStringSettingCopy(L"Position");
    if (EqualsNoCase(pos, L"top-left")) next.position = Position::TopLeft;
    else if (EqualsNoCase(pos, L"top-right")) next.position = Position::TopRight;
    else if (EqualsNoCase(pos, L"bottom-center")) next.position = Position::BottomCenter;
    else next.position = Position::TopCenter;

    const std::wstring mon = GetStringSettingCopy(L"TargetMonitor");
    next.targetMonitor = mon.empty() ? 0 : _wtoi(mon.c_str());
    next.offsetX = Wh_GetIntSetting(L"OffsetX");
    next.offsetY = Wh_GetIntSetting(L"OffsetY");
    const std::wstring scale = GetStringSettingCopy(L"SizeScale");
    next.sizeScale = scale.empty() ? 1.0f : Clamp(static_cast<float>(_wtof(scale.c_str())), 0.6f, 3.0f);
    next.autoDpiScale = Wh_GetIntSetting(L"AutoDpiScale") != 0;
    next.alwaysOnTop = Wh_GetIntSetting(L"AlwaysOnTop") != 0;
    const std::wstring opacity = GetStringSettingCopy(L"PillOpacity");
    next.pillOpacity = opacity.empty() ? 1.0f : Clamp(static_cast<float>(_wtof(opacity.c_str())), 0.35f, 1.0f);
    next.calendarIcsUrl = GetStringSettingCopy(L"CalendarIcsUrl");
    next.calendarRefreshMinutes = ClampInt(Wh_GetIntSetting(L"CalendarRefreshMinutes"), 5, 240);
    next.brightnessEnabled = Wh_GetIntSetting(L"BrightnessEnabled") != 0;
    next.weatherCity = GetStringSettingCopy(L"WeatherCity");
    next.weatherFahrenheit = Wh_GetIntSetting(L"WeatherFahrenheit") != 0;
    next.captureVolumeKeys = Wh_GetIntSetting(L"CaptureVolumeKeys") != 0;
    g_captureVolKeys = next.captureVolumeKeys;  // lock-free mirror for the key hook

    std::lock_guard lock(g_settingsMutex);
    g_settings = std::move(next);
}
Settings SettingsSnapshot() { std::lock_guard lock(g_settingsMutex); return g_settings; }

// ---------------------------------------------------------------------------
// Spring
// ---------------------------------------------------------------------------
struct SpringValue {
    float value = 0.0f, velocity = 0.0f, target = 0.0f;
    void Reset(float v) { value = target = v; velocity = 0.0f; }
    void Step(float dt, float stiffness, float damping) {
        const float disp = value - target;
        velocity += (-stiffness * disp - damping * velocity) * dt;
        value += velocity * dt;
        if (std::fabs(value - target) < 0.05f && std::fabs(velocity) < 0.05f) { value = target; velocity = 0.0f; }
    }
    bool AtRest() const { return value == target && velocity == 0.0f; }
};

// ---------------------------------------------------------------------------
// Shared interactive state (render thread + WndProc both on the render thread)
// ---------------------------------------------------------------------------
std::atomic<float> g_volScalar{0.0f};
std::atomic<float> g_volDisplayScalar{0.0f};  // smoothed level the volume popup animates toward
std::atomic<bool>  g_volMuted{false};
std::atomic<bool>  g_volValid{false};
double g_volSuppressPollUntil = 0.0;  // render-thread only

HWND g_hwnd = nullptr;
RECT g_pillRect = {};  // screen rect of the VISIBLE panel (not the window) — drives hover detection
HANDLE g_stopEvent = nullptr;
HANDLE g_renderThread = nullptr;
HANDLE g_notifThread = nullptr;
std::atomic<bool> g_layoutDirty{true};
std::atomic<double> g_lastInputSec{-100.0};  // last pointer input; drives event-based repaint
std::atomic<bool> g_waveWanted{false};       // true only when the live waveform is on screen
HHOOK g_dismissHook = nullptr;               // WH_MOUSE_LL: click-outside dismiss (on the input thread)
std::atomic<bool> g_dismissActive{false};    // only act on the mouse hook while the panel is Open
HHOOK g_keyHook = nullptr;                    // WH_KEYBOARD_LL: volume-key capture
HANDLE g_inputHookThread = nullptr;          // dedicated thread hosting BOTH low-level hooks
DWORD g_inputHookTid = 0;

void TriggerNudge() {
    static std::atomic<double> last{-1.0};
    const double now = NowSeconds();
    if (now - last.load() < 0.10) return;
    last = now;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_NEW_EVENT, 0, 0);
}

// ---------------------------------------------------------------------------
// Volume controller (synchronous; push-callbacks added in a later stage)
// ---------------------------------------------------------------------------
class VolumeController {
   public:
    bool Get(float& scalar, bool& muted) {
        if (!Ensure()) return false;
        float lvl = 0.0f; BOOL mu = FALSE;
        if (SUCCEEDED(vol_->GetMasterVolumeLevelScalar(&lvl)) && SUCCEEDED(vol_->GetMute(&mu))) {
            scalar = lvl; muted = mu != FALSE; return true;
        }
        vol_.Reset(); device_.Reset();
        return false;
    }
    void Set(float scalar) {
        if (!Ensure()) return;
        vol_->SetMasterVolumeLevelScalar(Clamp(scalar, 0.0f, 1.0f), nullptr);
    }
    void SetMute(bool muted) {
        if (!Ensure()) return;
        vol_->SetMute(muted ? TRUE : FALSE, nullptr);
    }
   private:
    bool Ensure() {
        if (vol_) return true;
        if (!enum_) {
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&enum_)))) return false;
        }
        if (FAILED(enum_->GetDefaultAudioEndpoint(eRender, eConsole, &device_))) return false;
        if (FAILED(device_->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(vol_.GetAddressOf())))) return false;
        return true;
    }
    ComPtr<IMMDeviceEnumerator> enum_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioEndpointVolume> vol_;
};
VolumeController g_volume;

void ShowVolumeTransient(int pct, bool muted);  // defined in the transient section below

void PollVolume() {
    float s = 0.0f; bool m = false;
    if (g_volume.Get(s, m)) {
        if (NowSeconds() >= g_volSuppressPollUntil) {  // ignore our own drag echo
            float prev = g_volScalar.load();
            bool pm = g_volMuted.load();
            g_volScalar = s;
            g_volMuted = m;
            int pct = (int)(s * 100.0f + 0.5f);
            static int lastPct = -1, lastMuted = -1;
            if (lastPct >= 0 && (std::abs(pct - lastPct) >= 1 || (int)m != lastMuted))
                ShowVolumeTransient(pct, m);   // external change -> transient pill
            lastPct = pct; lastMuted = (int)m;
            if (std::fabs(prev - s) > 0.001f || pm != m) g_layoutDirty = true;
        }
        g_volValid = true;
    }
}

// ---------------------------------------------------------------------------
// Notification center data layer (UserNotificationListener)
// ---------------------------------------------------------------------------
struct NotificationItem {
    uint32_t id = 0;
    std::wstring app, title, body, aumid;
};

std::mutex g_notifMutex;
std::vector<NotificationItem> g_notifs;          // newest-first (listener order)
std::unordered_set<uint32_t> g_notifSuppressed;  // locally dismissed ids
std::atomic<bool> g_notifSupported{false};
std::atomic<uint64_t> g_notifGen{0};             // bumps on any list change (add/clear/dismiss) -> rebuild Open panel

std::vector<NotificationItem> NotificationsSnapshot() {
    std::lock_guard lock(g_notifMutex);
    return g_notifs;
}

#if ICC_HAS_NOTIFICATION_LISTENER
void DismissNotification(uint32_t id) {
    {
        std::lock_guard lock(g_notifMutex);
        g_notifSuppressed.insert(id);
        g_notifs.erase(std::remove_if(g_notifs.begin(), g_notifs.end(),
                       [id](const NotificationItem& n) { return n.id == id; }),
                       g_notifs.end());
    }
    std::thread([id] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            winrt::Windows::UI::Notifications::Management::UserNotificationListener::Current()
                .RemoveNotification(id);
        } catch (...) {}
        winrt::uninit_apartment();
    }).detach();
    g_notifGen.fetch_add(1);
    g_layoutDirty = true;
}

void ClearAllNotifications() {
    std::vector<uint32_t> ids;
    {
        std::lock_guard lock(g_notifMutex);
        for (auto& n : g_notifs) { ids.push_back(n.id); g_notifSuppressed.insert(n.id); }
        g_notifs.clear();
    }
    std::thread([ids] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            auto l = winrt::Windows::UI::Notifications::Management::UserNotificationListener::Current();
            for (uint32_t id : ids) { try { l.RemoveNotification(id); } catch (...) {} }
        } catch (...) {}
        winrt::uninit_apartment();
    }).detach();
    g_notifGen.fetch_add(1);
    g_layoutDirty = true;
}

DWORD WINAPI NotificationThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace winrt::Windows::UI::Notifications;
    using namespace winrt::Windows::UI::Notifications::Management;
    try {
        auto listener = UserNotificationListener::Current();
        if (listener.RequestAccessAsync().get() != UserNotificationListenerAccessStatus::Allowed) {
            g_notifSupported = false;
            winrt::uninit_apartment();
            return 0;
        }
        g_notifSupported = true;
        uint64_t lastSig = 0;
        while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
            try {
                auto list = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
                std::vector<NotificationItem> items;
                std::unordered_set<uint32_t> present;
                uint64_t sig = 0;
                for (uint32_t i = 0; i < list.Size(); ++i) {
                    auto un = list.GetAt(i);
                    uint32_t id = un.Id();
                    present.insert(id);
                    {
                        std::lock_guard lock(g_notifMutex);
                        if (g_notifSuppressed.count(id)) continue;
                    }
                    NotificationItem item;
                    item.id = id;
                    try {
                        auto info = un.AppInfo();
                        item.app = info.DisplayInfo().DisplayName().c_str();
                        item.aumid = info.AppUserModelId().c_str();
                    } catch (...) {}
                    try {
                        auto binding = un.Notification().Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                        if (binding) {
                            auto texts = binding.GetTextElements();
                            if (texts.Size() > 0) item.title = texts.GetAt(0).Text().c_str();
                            if (texts.Size() > 1) item.body = texts.GetAt(1).Text().c_str();
                        }
                    } catch (...) {}
                    if (item.title.empty()) item.title = item.app.empty() ? L"Notification" : item.app;
                    sig = sig * 1000003u + (id + 1u);
                    items.push_back(std::move(item));
                }
                {
                    std::lock_guard lock(g_notifMutex);
                    for (auto it = g_notifSuppressed.begin(); it != g_notifSuppressed.end();) {
                        if (!present.count(*it)) it = g_notifSuppressed.erase(it); else ++it;
                    }
                    g_notifs = std::move(items);
                }
                if (sig != lastSig) { lastSig = sig; g_notifGen.fetch_add(1); TriggerNudge(); g_layoutDirty = true; }
            } catch (...) {}
            WaitForSingleObject(g_stopEvent, 1000);
        }
    } catch (...) {
        g_notifSupported = false;
    }
    winrt::uninit_apartment();
    return 0;
}
#endif  // ICC_HAS_NOTIFICATION_LISTENER

void LaunchByAumid(const std::wstring& aumid) {
    if (aumid.empty()) return;
    std::wstring path = L"shell:AppsFolder\\" + aumid;
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Calendar data layer (Outlook published .ics over WinHTTP)
// ---------------------------------------------------------------------------
std::mutex g_calMutex;
std::vector<IcsEvent> g_calEvents;
std::atomic<uint64_t> g_calGen{0};  // bumps whenever g_calEvents is replaced (cache invalidation)
std::atomic<bool> g_calLoaded{false};
std::atomic<bool> g_calError{false};
std::atomic<bool> g_calConfigured{false};
// View/selection state (interaction-owned; persists across root rebuilds). 0 = uninit.
std::atomic<int> g_calViewYear{0};
std::atomic<int> g_calViewMonth{0};
std::atomic<int> g_calSelYear{0};
std::atomic<int> g_calSelMonth{0};
std::atomic<int> g_calSelDay{0};
HANDLE g_calThread = nullptr;
HANDLE g_calRefreshEvent = nullptr;

std::vector<IcsEvent> CalendarSnapshot() { std::lock_guard lock(g_calMutex); return g_calEvents; }

std::string HttpGetUtf8(const std::wstring& url) {
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {}, path[8192] = {}, extra[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return "";
    host[std::min<DWORD>(uc.dwHostNameLength, ARRAYSIZE(host) - 1)] = 0;
    path[std::min<DWORD>(uc.dwUrlPathLength, ARRAYSIZE(path) - 1)] = 0;
    extra[std::min<DWORD>(uc.dwExtraInfoLength, ARRAYSIZE(extra) - 1)] = 0;
    std::wstring fullPath = std::wstring(path) + extra;  // path + query (?format=j1 etc.)
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    HINTERNET hSession = WinHttpOpen(L"IslandCommandCenter/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    std::string result;
    if (HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0)) {
        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, nullptr)) {
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (WinHttpReadData(hReq, chunk.data(), avail, &read) && read) { chunk.resize(read); result += chunk; }
                    else break;
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

DWORD WINAPI CalendarThreadProc(void*) {
    HANDLE waits[2] = {g_stopEvent, g_calRefreshEvent};
    for (;;) {
        Settings s = SettingsSnapshot();
        g_calConfigured = !s.calendarIcsUrl.empty();
        if (g_calConfigured.load()) {
            std::string ics = HttpGetUtf8(s.calendarIcsUrl);
            if (!ics.empty() && ics.find("BEGIN:VCALENDAR") != std::string::npos) {
                SYSTEMTIME now; GetLocalTime(&now);
                SYSTEMTIME firstOfMonth = now; firstOfMonth.wDay = 1;
                SYSTEMTIME w0 = ics_detail::AddDays(firstOfMonth, -45);
                auto evs = ParseIcsEvents(ics, w0, 210);
                { std::lock_guard lock(g_calMutex); g_calEvents = std::move(evs); }
                g_calGen.fetch_add(1);  // invalidate agenda/calendar caches
                g_calLoaded = true; g_calError = false;
            } else {
                g_calError = true; g_calLoaded = true;
            }
            TriggerNudge(); g_layoutDirty = true;
        }
        int mins = std::max(5, SettingsSnapshot().calendarRefreshMinutes);
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, (DWORD)mins * 60000);
        if (wr == WAIT_OBJECT_0) break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Media (GSMTC) + album art + live audio waveform
// ---------------------------------------------------------------------------
struct BitmapPixels { int w = 0, h = 0; std::vector<uint8_t> bgra; uint64_t gen = 0; };
std::atomic<uint64_t> g_artGen{0};

std::vector<uint8_t> ReadWinRtStreamBytes(winrt::Windows::Storage::Streams::IRandomAccessStreamReference ref) {
    using namespace winrt::Windows::Storage::Streams;
    std::vector<uint8_t> out;
    try {
        auto stream = ref.OpenReadAsync().get();
        uint64_t size = stream.Size();
        if (!size || size > 8u * 1024 * 1024) return out;
        DataReader reader(stream);
        reader.LoadAsync((uint32_t)size).get();
        out.resize((size_t)size);
        reader.ReadBytes(winrt::array_view<uint8_t>(out.data(), out.data() + out.size()));
    } catch (...) {}
    return out;
}

bool DecodeImageBytesToPixels(const std::vector<uint8_t>& bytes, BitmapPixels* out) {
    if (bytes.empty() || !out) return false;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) return false;
    void* p = GlobalLock(mem); memcpy(p, bytes.data(), bytes.size()); GlobalUnlock(mem);
    ComPtr<IStream> s;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &s))) { GlobalFree(mem); return false; }
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return false;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(&cv))) return false;
    if (FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0; cv->GetSize(&w, &h);
    if (!w || !h || w > 2048 || h > 2048) return false;
    out->w = (int)w; out->h = (int)h; out->bgra.resize((size_t)w * h * 4);
    if (FAILED(cv->CopyPixels(nullptr, w * 4, (UINT)out->bgra.size(), out->bgra.data()))) return false;
    out->gen = ++g_artGen;
    return true;
}

struct MediaState {
    bool active = false, playing = false;
    std::wstring title, artist, aumid;
    int64_t posTicks = 0, endTicks = 0;  // 100ns
    ULONGLONG capturedTick = 0;
    BitmapPixels art;
};
std::mutex g_mediaMutex;
MediaState g_media;
MediaState MediaSnapshot() { std::lock_guard l(g_mediaMutex); return g_media; }

constexpr int kWaveN = 56;
std::mutex g_waveMutex;
std::array<float, kWaveN> g_wave{};
int g_waveWrite = 0;
void WavePush(float v) { std::lock_guard l(g_waveMutex); g_wave[g_waveWrite % kWaveN] = v; ++g_waveWrite; }
std::array<float, kWaveN> WaveSnapshot(int& writeOut) { std::lock_guard l(g_waveMutex); writeOut = g_waveWrite; return g_wave; }

HANDLE g_mediaThread = nullptr;
HANDLE g_audioThread = nullptr;

DWORD WINAPI AudioThreadProc(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev; ComPtr<IAudioMeterInformation> meter;
    auto ensure = [&]() -> bool {
        if (meter) return true;
        if (!en && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return false;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
        if (FAILED(dev->Activate(ICC_IID_IAudioMeterInformation, CLSCTX_ALL, nullptr, (void**)meter.GetAddressOf()))) { dev.Reset(); return false; }
        return true;
    };
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        // Only sample the meter fast when the waveform is actually visible; otherwise idle so we
        // don't wake ~22x/second forever feeding a strip nobody is looking at.
        bool wanted = g_waveWanted.load();
        float peak = 0.0f;
        if (wanted) {
            if (ensure()) { if (FAILED(meter->GetPeakValue(&peak))) { meter.Reset(); dev.Reset(); peak = 0.0f; } }
            WavePush(Clamp(peak, 0.0f, 1.0f));
        }
        WaitForSingleObject(g_stopEvent, wanted ? 45 : 400);
    }
    CoUninitialize();
    return 0;
}

DWORD WINAPI MediaThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace winrt::Windows::Media::Control;
    using PS = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MediaState next;
        try {
            if (!mgr) mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            if (mgr) {
                auto s = mgr.GetCurrentSession();
                if (s) {
                    auto props = s.TryGetMediaPropertiesAsync().get();
                    auto pb = s.GetPlaybackInfo();
                    auto tl = s.GetTimelineProperties();
                    next.active = true;
                    next.playing = pb.PlaybackStatus() == PS::Playing;
                    next.title = props.Title().c_str();
                    next.artist = props.Artist().c_str();
                    next.aumid = s.SourceAppUserModelId().c_str();
                    if (tl) { next.posTicks = tl.Position().count(); next.endTicks = tl.EndTime().count(); next.capturedTick = GetTickCount64(); }
                    if (auto th = props.Thumbnail()) {
                        auto bytes = ReadWinRtStreamBytes(th);
                        if (!bytes.empty()) { BitmapPixels d; if (DecodeImageBytesToPixels(bytes, &d)) next.art = std::move(d); }
                    }
                }
            }
        } catch (...) {}
        bool changed;
        {
            std::lock_guard l(g_mediaMutex);
            changed = next.active != g_media.active || next.title != g_media.title ||
                      next.artist != g_media.artist || next.playing != g_media.playing;
            if (next.art.bgra.empty() && next.title == g_media.title && next.artist == g_media.artist) next.art = g_media.art;
            g_media = std::move(next);
        }
        if (changed) { TriggerNudge(); g_layoutDirty = true; }
        WaitForSingleObject(g_stopEvent, 1000);
    }
    winrt::uninit_apartment();
    return 0;
}

void MediaCommand(int cmd) {  // 0 prev, 1 toggle, 2 next
    std::wstring aumid; { std::lock_guard l(g_mediaMutex); aumid = g_media.aumid; }
    std::thread([cmd, aumid] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            using namespace winrt::Windows::Media::Control;
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            if (mgr) {
                auto s = mgr.GetCurrentSession();
                for (auto const& sess : mgr.GetSessions())
                    if (std::wstring(sess.SourceAppUserModelId().c_str()) == aumid) { s = sess; break; }
                if (s) { if (cmd == 0) s.TrySkipPreviousAsync().get(); else if (cmd == 1) s.TryTogglePlayPauseAsync().get(); else if (cmd == 2) s.TrySkipNextAsync().get(); }
            }
        } catch (...) {}
        winrt::uninit_apartment();
    }).detach();
}

void MediaSeek(double fraction) {
    std::wstring aumid; int64_t endTicks;
    { std::lock_guard l(g_mediaMutex); aumid = g_media.aumid; endTicks = g_media.endTicks; }
    if (endTicks <= 0) return;
    int64_t target = (int64_t)(Clamp((float)fraction, 0.0f, 1.0f) * (float)endTicks);
    std::thread([aumid, target] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            using namespace winrt::Windows::Media::Control;
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            if (mgr) {
                auto s = mgr.GetCurrentSession();
                for (auto const& sess : mgr.GetSessions())
                    if (std::wstring(sess.SourceAppUserModelId().c_str()) == aumid) { s = sess; break; }
                if (s) s.TryChangePlaybackPositionAsync(target).get();
            }
        } catch (...) {}
        winrt::uninit_apartment();
    }).detach();
}

// ---------------------------------------------------------------------------
// Weather (wttr.in over WinHTTP)
// ---------------------------------------------------------------------------
struct WeatherState {
    bool hasData = false;
    int temp = 0;
    int code = 0;
    int feelsLike = 0;
    int humidity = 0;
    int windSpeed = 0;          // mph (Fahrenheit) or km/h (Celsius)
    std::wstring windDir;       // 16-point compass, e.g. "SSW"
    std::wstring desc, city;
    double updated = 0.0;
};
std::mutex g_weatherMutex;
WeatherState g_weather;
HANDLE g_weatherThread = nullptr;
WeatherState WeatherSnapshot() { std::lock_guard l(g_weatherMutex); return g_weather; }

std::wstring WeatherGlyph(int code) {
    switch (code) {
        case 113: return L"☀️";                                   // sunny
        case 116: return L"⛅";                                         // partly cloudy
        case 119: case 122: return L"☁️";                         // cloudy
        case 143: case 248: case 260: return L"\U0001F32B️";           // fog
        case 200: case 386: case 389: case 392: case 395: return L"⛈️"; // thunder
    }
    if (code >= 281 && code <= 377) return L"\U0001F327️";             // rain
    if (code >= 179 && code <= 230) return L"❄️";                 // snow
    if (code >= 320 && code <= 395) return L"❄️";                 // snow
    return L"\U0001F321️";                                            // thermometer
}

// Extract a JSON string value for `key` after position `from`, tolerating
// pretty-printed whitespace: "key" <ws> : <ws> "value". (wttr.in j1 is pretty-printed.)
static std::string JsonStr(const std::string& s, size_t from, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = s.find(needle, from);
    if (p == std::string::npos) return "";
    p = s.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    if (p >= s.size() || s[p] != '"') return "";  // only string values
    ++p;
    size_t e = s.find('"', p);
    return (e == std::string::npos) ? "" : s.substr(p, e - p);
}

DWORD WINAPI WeatherThreadProc(void*) {
    WaitForSingleObject(g_stopEvent, 2500);
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        Settings s = SettingsSnapshot();
        std::wstring url = L"https://wttr.in/";
        for (wchar_t c : s.weatherCity) url += (c == L' ') ? L'+' : c;
        url += L"?format=j1";
        std::string j = HttpGetUtf8(url);
        size_t cc = j.find("current_condition");
        if (cc != std::string::npos) {
            std::string tF = JsonStr(j, cc, "temp_F");
            std::string tC = JsonStr(j, cc, "temp_C");
            std::string code = JsonStr(j, cc, "weatherCode");
            size_t dpos = j.find("weatherDesc", cc);
            std::string desc = (dpos != std::string::npos) ? JsonStr(j, dpos, "value") : "";
            WeatherState w;
            w.hasData = !tF.empty() || !tC.empty();
            w.temp = atoi((s.weatherFahrenheit ? tF : tC).c_str());
            w.code = atoi(code.c_str());
            w.desc = ics_detail::Utf8ToW(desc);
            w.feelsLike = atoi(JsonStr(j, cc, s.weatherFahrenheit ? "FeelsLikeF" : "FeelsLikeC").c_str());
            w.humidity = atoi(JsonStr(j, cc, "humidity").c_str());
            w.windSpeed = atoi(JsonStr(j, cc, s.weatherFahrenheit ? "windspeedMiles" : "windspeedKmph").c_str());
            w.windDir = ics_detail::Utf8ToW(JsonStr(j, cc, "winddir16Point"));
            size_t na = j.find("nearest_area");
            if (na != std::string::npos) { size_t ap = j.find("areaName", na); if (ap != std::string::npos) w.city = ics_detail::Utf8ToW(JsonStr(j, ap, "value")); }
            w.updated = NowSeconds();
            { std::lock_guard l(g_weatherMutex); g_weather = w; }
            TriggerNudge(); g_layoutDirty = true;
        }
        WaitForSingleObject(g_stopEvent, 15u * 60u * 1000u);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Transient pills (volume / caps lock / clipboard / device) + privacy dots
// ---------------------------------------------------------------------------
struct Transient {
    int kind = 0;             // 0 none, 1 generic, 2 volume
    std::wstring glyph, line;
    int value = 0;            // volume percent (kind 2)
    bool muted = false;
    double expiresAt = 0.0;
};
std::mutex g_transMutex;
Transient g_trans;

void ShowTransient(const std::wstring& glyph, const std::wstring& line, double dur = 2.2) {
    { std::lock_guard l(g_transMutex); g_trans = {1, glyph, line, 0, false, NowSeconds() + dur}; }
    TriggerNudge(); g_layoutDirty = true;
}
void ShowVolumeTransient(int pct, bool muted) {
    { std::lock_guard l(g_transMutex); g_trans = {2, muted ? L"\xE74F" : L"\xE767", L"Volume", pct, muted, NowSeconds() + 1.8}; }
    TriggerNudge(); g_layoutDirty = true;
}
Transient TransientSnapshot() { std::lock_guard l(g_transMutex); return g_trans; }
bool TransientActive() { std::lock_guard l(g_transMutex); return g_trans.kind != 0 && NowSeconds() < g_trans.expiresAt; }

// Privacy (mic/camera in-use) via CapabilityAccessManager registry.
std::atomic<bool> g_micActive{false}, g_camActive{false};
HANDLE g_privacyThread = nullptr;

bool ProbeConsentStore(HKEY root, const std::wstring& path) {
    HKEY h;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    bool inUse = false;
    wchar_t name[256];
    for (DWORD idx = 0; !inUse; ++idx) {
        DWORD nlen = 256;
        if (RegEnumKeyExW(h, idx, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring child = path + L"\\" + name;
        if (wcscmp(name, L"NonPackaged") == 0) { if (ProbeConsentStore(root, child)) inUse = true; continue; }
        HKEY sub;
        if (RegOpenKeyExW(root, child.c_str(), 0, KEY_READ, &sub) == ERROR_SUCCESS) {
            ULONGLONG stop = 1; DWORD sz = sizeof(stop), tp = 0;
            if (RegQueryValueExW(sub, L"LastUsedTimeStop", nullptr, &tp, (LPBYTE)&stop, &sz) == ERROR_SUCCESS && stop == 0) inUse = true;
            RegCloseKey(sub);
        }
    }
    RegCloseKey(h);
    return inUse;
}
bool IsCapabilityInUse(const wchar_t* cap) {
    return ProbeConsentStore(HKEY_CURRENT_USER,
        std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + cap);
}
DWORD WINAPI PrivacyThreadProc(void*) {
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        bool mic = IsCapabilityInUse(L"microphone");
        bool cam = IsCapabilityInUse(L"webcam");
        if (mic != g_micActive.load() || cam != g_camActive.load()) { g_micActive = mic; g_camActive = cam; g_layoutDirty = true; }
        WaitForSingleObject(g_stopEvent, 1500);
    }
    return 0;
}

// ===========================================================================
// Widget framework
// ===========================================================================
struct DrawContext {
    ID2D1DCRenderTarget* dc = nullptr;
    IDWriteFactory* dw = nullptr;
    float scale = 1.0f;
    double now = 0.0;

    // Brushes (owned by Renderer).
    ID2D1SolidColorBrush* text = nullptr;
    ID2D1SolidColorBrush* muted = nullptr;
    ID2D1SolidColorBrush* accent = nullptr;
    ID2D1SolidColorBrush* panel = nullptr;
    ID2D1SolidColorBrush* card = nullptr;
    ID2D1SolidColorBrush* border = nullptr;
    ID2D1SolidColorBrush* divider = nullptr;
    ID2D1SolidColorBrush* track = nullptr;

    // Text formats (owned by Renderer).
    IDWriteTextFormat* fHuge = nullptr;
    IDWriteTextFormat* fTitle = nullptr;
    IDWriteTextFormat* fBody = nullptr;
    IDWriteTextFormat* fSmall = nullptr;
    IDWriteTextFormat* fPill = nullptr;
    IDWriteTextFormat* fGlyph = nullptr;
    ID2D1Bitmap* artBmp = nullptr;

    float MeasureWidth(IDWriteTextFormat* fmt, const std::wstring& s) const {
        if (!fmt || s.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt,
                                        10000.0f, 200.0f, &layout)))
            return 0.0f;
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }
    void Text(const std::wstring& s, IDWriteTextFormat* fmt, D2D1_RECT_F r,
              ID2D1SolidColorBrush* brush, float opacity = 1.0f,
              DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
              D2D1_DRAW_TEXT_OPTIONS opt = D2D1_DRAW_TEXT_OPTIONS_NONE) const {
        if (s.empty() || !fmt || !brush) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(vAlign);
        brush->SetOpacity(opacity);
        dc->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, brush, opt);
        brush->SetOpacity(1.0f);
    }
};

enum class PointerPhase { Move, Down, Up, Wheel };
struct PointerEvent {
    PointerPhase phase;
    D2D1_POINT_2F pos{};
    float wheel = 0.0f;
};

struct ScrollContainer;  // fwd

struct Widget {
    D2D1_RECT_F bounds{};
    bool visible = true;
    Widget* parent = nullptr;

    virtual ~Widget() = default;
    // Returns the height needed at the given width.
    virtual float Measure(const DrawContext& dc, float width) = 0;
    virtual float PreferredWidth(const DrawContext&) { return -1.0f; }
    virtual void Layout(D2D1_RECT_F b) { bounds = b; }
    virtual void Paint(DrawContext&) = 0;
    bool Contains(D2D1_POINT_2F p) const {
        return p.x >= bounds.left && p.x < bounds.right && p.y >= bounds.top && p.y < bounds.bottom;
    }
    virtual Widget* HitTestDeep(D2D1_POINT_2F p) {
        return (visible && Contains(p)) ? this : nullptr;
    }
    // Returns true if handled. Sets wantsCapture on a Down that begins a drag.
    virtual bool OnPointer(const PointerEvent&, bool& wantsCapture) { wantsCapture = false; return false; }
    virtual bool Tick(float) { return false; }  // returns animating
    virtual ScrollContainer* AsScroll() { return nullptr; }
};

float W(const D2D1_RECT_F& r) { return r.right - r.left; }
float H(const D2D1_RECT_F& r) { return r.bottom - r.top; }

// --- Custom (lambda-painted) leaf, used for the pill & expanded clock --------
struct Custom : Widget {
    std::function<float(const DrawContext&, float)> measure;  // optional
    std::function<float(const DrawContext&)> prefWidth;       // optional
    std::function<void(DrawContext&, D2D1_RECT_F)> paint;
    float fixedHeight = 0.0f;

    float Measure(const DrawContext& dc, float width) override {
        return measure ? measure(dc, width) : fixedHeight;
    }
    float PreferredWidth(const DrawContext& dc) override {
        return prefWidth ? prefWidth(dc) : -1.0f;
    }
    void Paint(DrawContext& dc) override { if (paint) paint(dc, bounds); }
};

// --- Label -------------------------------------------------------------------
struct Label : Widget {
    std::wstring text;
    IDWriteTextFormat* DrawContext::* fmtMember = &DrawContext::fBody;
    ID2D1SolidColorBrush* DrawContext::* brushMember = &DrawContext::text;
    float opacity = 1.0f;
    float height = 18.0f;
    float padL = 0.0f, padR = 0.0f;

    float Measure(const DrawContext& dc, float) override { return height * dc.scale; }
    void Paint(DrawContext& dc) override {
        D2D1_RECT_F r = bounds;
        r.left += padL * dc.scale; r.right -= padR * dc.scale;
        dc.Text(text, dc.*fmtMember, r, dc.*brushMember, opacity);
    }
};

// --- Slider (the capture case) ----------------------------------------------
struct Slider : Widget {
    std::function<float()> get;       // current 0..1
    std::function<void(float)> set;   // live during drag
    std::wstring label;
    std::wstring glyph;               // leading icon (Segoe Fluent Icons), optional
    IDWriteTextFormat* glyphFmt = nullptr;
    bool dragging = false;
    float height = 46.0f;

    float Measure(const DrawContext& dc, float) override { return height * dc.scale; }

    D2D1_RECT_F TrackRect(const DrawContext& dc) const {
        const float s = dc.scale;
        float left = bounds.left + 44.0f * s;
        float right = bounds.right - 16.0f * s;
        float cy = bounds.top + H(bounds) * 0.62f;
        return D2D1::RectF(left, cy - 3.0f * s, right, cy + 3.0f * s);
    }
    void Apply(const DrawContext& dc, float x) {
        D2D1_RECT_F t = TrackRect(dc);
        float v = (x - t.left) / std::max(1.0f, W(t));
        if (set) set(Clamp(v, 0.0f, 1.0f));
    }
    bool OnPointer(const PointerEvent& e, bool& wantsCapture) override {
        wantsCapture = false;
        // dc.scale isn't available here; recompute via bounds geometry below.
        if (e.phase == PointerPhase::Down) { dragging = true; wantsCapture = true; pendingX_ = e.pos.x; return true; }
        if (e.phase == PointerPhase::Move && dragging) { pendingX_ = e.pos.x; return true; }
        if (e.phase == PointerPhase::Up && dragging) { dragging = false; pendingX_ = e.pos.x; return true; }
        return false;
    }
    bool HasPending() const { return pendingX_ >= 0.0f; }
    float TakePending() { float x = pendingX_; pendingX_ = -1.0f; return x; }

    void Paint(DrawContext& dc) override {
        const float s = dc.scale;
        float value = get ? Clamp(get(), 0.0f, 1.0f) : 0.0f;
        // Icon
        if (!glyph.empty() && glyphFmt) {
            D2D1_RECT_F gr = D2D1::RectF(bounds.left + 8.0f * s, bounds.top,
                                         bounds.left + 40.0f * s, bounds.bottom);
            dc.Text(glyph, glyphFmt, gr, dc.text, 0.9f);
        }
        // Label + percent
        if (!label.empty()) {
            D2D1_RECT_F lr = D2D1::RectF(bounds.left + 44.0f * s, bounds.top + 4.0f * s,
                                         bounds.right - 16.0f * s, bounds.top + 22.0f * s);
            dc.Text(label, dc.fSmall, lr, dc.muted, 0.85f);
            wchar_t pct[8]; swprintf_s(pct, L"%d%%", (int)(value * 100.0f + 0.5f));
            dc.Text(pct, dc.fSmall, lr, dc.text, 0.9f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        // Track + fill + thumb
        D2D1_RECT_F t = TrackRect(dc);
        float rad = H(t) * 0.5f;
        dc.dc->FillRoundedRectangle(D2D1::RoundedRect(t, rad, rad), dc.track);
        D2D1_RECT_F fill = t; fill.right = t.left + W(t) * value;
        dc.dc->FillRoundedRectangle(D2D1::RoundedRect(fill, rad, rad), dc.accent);
        float thumbX = t.left + W(t) * value;
        float thumbR = 7.0f * s;
        dc.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, (t.top + t.bottom) * 0.5f), thumbR, thumbR), dc.text);
    }
   private:
    float pendingX_ = -1.0f;
};

// --- Button ------------------------------------------------------------------
struct Button : Widget {
    std::wstring text;
    std::wstring glyph;
    IDWriteTextFormat* glyphFmt = nullptr;
    std::function<void()> onClick;
    float height = 30.0f;
    bool pressed = false;

    float Measure(const DrawContext& dc, float) override { return height * dc.scale; }
    bool OnPointer(const PointerEvent& e, bool& wantsCapture) override {
        wantsCapture = false;
        if (e.phase == PointerPhase::Down) { pressed = true; return true; }
        if (e.phase == PointerPhase::Up) { pressed = false; if (onClick) onClick(); return true; }
        return false;
    }
    void Paint(DrawContext& dc) override {
        const float s = dc.scale;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(bounds, 8.0f * s, 8.0f * s);
        dc.card->SetOpacity(pressed ? 0.18f : 0.10f);
        dc.dc->FillRoundedRectangle(rr, dc.card);
        dc.card->SetOpacity(1.0f);
        std::wstring t = glyph.empty() ? text : (glyph + L"  " + text);
        dc.Text(t, dc.fSmall, bounds, dc.text, 0.92f, DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
};

// --- NotifRow (a single notification card) -----------------------------------
struct NotifRow : Widget {
    uint32_t id = 0;
    std::wstring app, title, body, aumid;
    std::function<void(uint32_t)> onDismiss;
    std::function<void(const std::wstring&)> onActivate;  // aumid
    float height = 60.0f;

    float Measure(const DrawContext& dc, float) override { return height * dc.scale; }
    bool InX(D2D1_POINT_2F p) const {
        return p.x >= bounds.right - 32.0f * lastScale_ && p.y <= bounds.top + 34.0f * lastScale_;
    }
    bool OnPointer(const PointerEvent& e, bool& wantsCapture) override {
        wantsCapture = false;
        if (e.phase == PointerPhase::Down) { armedDismiss_ = InX(e.pos); return true; }
        if (e.phase == PointerPhase::Up) {
            if (armedDismiss_ && InX(e.pos)) { armedDismiss_ = false; if (onDismiss) onDismiss(id); return true; }
            armedDismiss_ = false;
            if (onActivate && !aumid.empty()) onActivate(aumid);
            return true;
        }
        return false;
    }
    void Paint(DrawContext& dc) override {
        lastScale_ = dc.scale;
        const float s = dc.scale;
        D2D1_RECT_F card = bounds; card.bottom -= 5.0f * s;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(card, 10.0f * s, 10.0f * s);
        dc.card->SetOpacity(0.07f);
        dc.dc->FillRoundedRectangle(rr, dc.card);
        dc.card->SetOpacity(1.0f);

        D2D1_RECT_F appR = D2D1::RectF(card.left + 12.0f * s, card.top + 7.0f * s,
                                       card.right - 30.0f * s, card.top + 23.0f * s);
        dc.Text(app, dc.fSmall, appR, dc.accent, 0.92f);

        std::wstring line = title;
        if (!body.empty()) line += L"   " + body;
        D2D1_RECT_F bodyR = D2D1::RectF(card.left + 12.0f * s, card.top + 24.0f * s,
                                        card.right - 12.0f * s, card.bottom - 6.0f * s);
        dc.Text(line, dc.fBody, bodyR, dc.text, 0.92f,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        D2D1_RECT_F xr = D2D1::RectF(card.right - 30.0f * s, card.top + 6.0f * s,
                                     card.right - 8.0f * s, card.top + 28.0f * s);
        dc.Text(L"\x2715", dc.fSmall, xr, dc.muted, armedDismiss_ ? 1.0f : 0.6f,
                DWRITE_TEXT_ALIGNMENT_CENTER);
    }
   private:
    bool armedDismiss_ = false;
    float lastScale_ = 1.0f;
};

int ClockMinuteKey();  // fwd (defined with the time helpers below)

// --- CalendarView (interactive month grid) -----------------------------------
struct CalendarView : Widget {
    float Measure(const DrawContext& dc, float) override { return 214.0f * dc.scale; }

    bool OnPointer(const PointerEvent& e, bool& wc) override {
        wc = false;
        if (e.phase == PointerPhase::Down) return true;
        if (e.phase != PointerPhase::Up) return false;
        Init();
        const float s = lastScale_;
        if (e.pos.y < bounds.top + 30.0f * s) {
            if (e.pos.x < bounds.left + 44.0f * s) { Step(-1); return true; }
            if (e.pos.x > bounds.right - 44.0f * s) { Step(1); return true; }
            return true;
        }
        int d = DayAt(e.pos, s);
        if (d > 0) { g_calSelYear = g_calViewYear.load(); g_calSelMonth = g_calViewMonth.load(); g_calSelDay = d; g_layoutDirty = true; }
        return true;
    }

    void Paint(DrawContext& dc) override {
        lastScale_ = dc.scale; Init();
        const float s = dc.scale;
        int vy = g_calViewYear.load(), vm = g_calViewMonth.load();
        SYSTEMTIME mt{}; mt.wYear = (WORD)vy; mt.wMonth = (WORD)vm; mt.wDay = 1;
        wchar_t hdr[64] = {}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &mt, L"MMMM yyyy", hdr, ARRAYSIZE(hdr), nullptr);
        D2D1_RECT_F hr = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + 28.0f * s);
        dc.Text(hdr, dc.fTitle, hr, dc.text, 0.95f, DWRITE_TEXT_ALIGNMENT_CENTER);
        dc.Text(L"\x2039", dc.fTitle, D2D1::RectF(bounds.left + 6 * s, hr.top, bounds.left + 40 * s, hr.bottom), dc.muted, 0.85f, DWRITE_TEXT_ALIGNMENT_CENTER);
        dc.Text(L"\x203A", dc.fTitle, D2D1::RectF(bounds.right - 40 * s, hr.top, bounds.right - 6 * s, hr.bottom), dc.muted, 0.85f, DWRITE_TEXT_ALIGNMENT_CENTER);

        const wchar_t* wd[7] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
        float cellW = W(bounds) / 7.0f, wkY = bounds.top + 30.0f * s;
        for (int i = 0; i < 7; ++i)
            dc.Text(wd[i], dc.fSmall, D2D1::RectF(bounds.left + i * cellW, wkY, bounds.left + (i + 1) * cellW, wkY + 18 * s), dc.muted, 0.5f, DWRITE_TEXT_ALIGNMENT_CENTER);

        const std::unordered_set<int>& evDays = EventDays(vy, vm);
        SYSTEMTIME now; GetLocalTime(&now);
        int fw = FirstWeekday(vy, vm), dim = ics_detail::DaysInMonth(vy, vm);
        float gridTop = bounds.top + 52.0f * s, rowH = (H(bounds) - 52.0f * s) / 6.0f;
        int selY = g_calSelYear.load(), selM = g_calSelMonth.load(), selD = g_calSelDay.load();
        for (int d = 1; d <= dim; ++d) {
            int idx = fw + d - 1, col = idx % 7, row = idx / 7;
            float cx = bounds.left + col * cellW + cellW * 0.5f, cy = gridTop + row * rowH + rowH * 0.5f;
            bool isToday = (vy == now.wYear && vm == now.wMonth && d == now.wDay);
            bool isSel = (vy == selY && vm == selM && d == selD);
            if (isSel) { dc.accent->SetOpacity(0.9f); dc.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 11 * s, 11 * s), dc.accent); dc.accent->SetOpacity(1.0f); }
            else if (isToday) dc.dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 11 * s, 11 * s), dc.accent, 1.5f);
            wchar_t ds[4]; swprintf_s(ds, L"%d", d);
            dc.Text(ds, dc.fSmall, D2D1::RectF(cx - cellW * 0.5f, cy - rowH * 0.5f, cx + cellW * 0.5f, cy + rowH * 0.5f),
                    dc.text, isSel ? 1.0f : 0.88f, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (evDays.count(d)) dc.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 9 * s), 1.6f * s, 1.6f * s), isSel ? dc.text : dc.accent);
        }
    }
   private:
    float lastScale_ = 1.0f;
    uint64_t evGen_ = (uint64_t)-1; int evView_ = -1;
    std::unordered_set<int> evDays_;
    // Set of days-with-events for the viewed month; rebuilt only when the calendar data
    // generation or the viewed month changes (not every paint/drag frame).
    const std::unordered_set<int>& EventDays(int vy, int vm) {
        uint64_t gen = g_calGen.load(); int vk = vy * 13 + vm;
        if (gen == evGen_ && vk == evView_) return evDays_;
        evGen_ = gen; evView_ = vk; evDays_.clear();
        for (auto& ev : CalendarSnapshot()) if (ev.start.wYear == vy && ev.start.wMonth == vm) evDays_.insert(ev.start.wDay);
        return evDays_;
    }
    void Init() {
        if (g_calViewYear.load() == 0) { SYSTEMTIME n; GetLocalTime(&n); g_calViewYear = n.wYear; g_calViewMonth = n.wMonth; }
        if (g_calSelYear.load() == 0) { SYSTEMTIME n; GetLocalTime(&n); g_calSelYear = n.wYear; g_calSelMonth = n.wMonth; g_calSelDay = n.wDay; }
    }
    void Step(int dir) { int y = g_calViewYear.load(), m = g_calViewMonth.load() + dir; if (m < 1) { m = 12; --y; } if (m > 12) { m = 1; ++y; } g_calViewYear = y; g_calViewMonth = m; g_layoutDirty = true; }
    int FirstWeekday(int y, int m) { SYSTEMTIME st{}; st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = 1; st.wHour = 12; return ics_detail::DayOfWeek(st); }
    int DayAt(D2D1_POINT_2F p, float s) {
        Init();
        int vy = g_calViewYear.load(), vm = g_calViewMonth.load();
        float cellW = W(bounds) / 7.0f, gridTop = bounds.top + 52.0f * s, rowH = (H(bounds) - 52.0f * s) / 6.0f;
        if (p.y < gridTop) return -1;
        int col = (int)((p.x - bounds.left) / cellW), row = (int)((p.y - gridTop) / rowH);
        if (col < 0 || col > 6 || row < 0 || row > 5) return -1;
        int d = row * 7 + col - FirstWeekday(vy, vm) + 1, dim = ics_detail::DaysInMonth(vy, vm);
        return (d >= 1 && d <= dim) ? d : -1;
    }
};

// --- AgendaList (events for the selected day) ---------------------------------
struct AgendaList : Widget {
    float Measure(const DrawContext& dc, float) override {
        int n = (int)ForSel().size(); if (n == 0) n = 1;
        return (24.0f + n * 36.0f) * dc.scale;
    }
    void Paint(DrawContext& dc) override {
        const float s = dc.scale;
        SYSTEMTIME sd{}; sd.wYear = (WORD)g_calSelYear.load(); sd.wMonth = (WORD)g_calSelMonth.load(); sd.wDay = (WORD)g_calSelDay.load(); sd.wHour = 12;
        if (sd.wYear == 0) GetLocalTime(&sd);
        wchar_t hdr[64] = {}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &sd, L"dddd, MMMM d", hdr, ARRAYSIZE(hdr), nullptr);
        dc.Text(hdr, dc.fSmall, D2D1::RectF(bounds.left + 4 * s, bounds.top, bounds.right - 4 * s, bounds.top + 22 * s), dc.muted, 0.85f);
        const std::vector<IcsEvent>& evs = ForSel();
        float y = bounds.top + 24.0f * s;
        if (evs.empty()) {
            SYSTEMTIME n; GetLocalTime(&n);
            bool selToday = (g_calSelYear.load() == n.wYear && g_calSelMonth.load() == n.wMonth && g_calSelDay.load() == n.wDay);
            dc.Text(selToday ? L"Nothing upcoming" : L"No events", dc.fBody,
                    D2D1::RectF(bounds.left + 4 * s, y, bounds.right, y + 30 * s), dc.muted, 0.5f);
            return;
        }
        for (auto& e : evs) {
            D2D1_RECT_F row = D2D1::RectF(bounds.left + 4 * s, y, bounds.right - 4 * s, y + 34 * s);
            dc.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(row.left, row.top + 4 * s, row.left + 3 * s, row.bottom - 4 * s), 1.5f * s, 1.5f * s), dc.accent);
            wchar_t tm[24] = {};
            if (e.allDay) wcscpy_s(tm, L"All day");
            else { SYSTEMTIME es = e.start; GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &es, nullptr, tm, ARRAYSIZE(tm)); }
            dc.Text(tm, dc.fSmall, D2D1::RectF(row.left + 10 * s, row.top, row.left + 80 * s, row.bottom), dc.muted, 0.8f);
            dc.Text(e.subject, dc.fBody, D2D1::RectF(row.left + 86 * s, row.top, row.right, row.bottom), dc.text, 0.92f);
            y += 36.0f * s;
        }
    }
   private:
    static unsigned long long StampOf(const SYSTEMTIME& st) {
        FILETIME ft{}; SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    }
    uint64_t cacheGen_ = (uint64_t)-1; int cacheSel_ = -1; int cacheMin_ = -1;
    std::vector<IcsEvent> cache_;
    // Events for the selected day. When that day is today, show only what's still upcoming
    // (drop meetings that have already ended); other days show the full day. Sorted by time.
    // Cached: recomputed only when the calendar data, the selected day, or the minute changes
    // (the "upcoming" cutoff advances by the minute) — NOT on every measure/paint/drag frame.
    const std::vector<IcsEvent>& ForSel() {
        int y = g_calSelYear.load(), m = g_calSelMonth.load(), d = g_calSelDay.load();
        int selKey = (y * 13 + m) * 32 + d;
        int minKey = ClockMinuteKey();
        uint64_t gen = g_calGen.load();
        if (gen == cacheGen_ && selKey == cacheSel_ && minKey == cacheMin_) return cache_;
        cacheGen_ = gen; cacheSel_ = selKey; cacheMin_ = minKey;
        cache_.clear();
        if (y == 0) return cache_;
        SYSTEMTIME nowSt; GetLocalTime(&nowSt);
        bool selToday = (y == nowSt.wYear && m == nowSt.wMonth && d == nowSt.wDay);
        unsigned long long nowU = StampOf(nowSt);
        for (auto& e : CalendarSnapshot()) {
            if (!(e.start.wYear == y && e.start.wMonth == m && e.start.wDay == d)) continue;
            if (selToday && !e.allDay) {
                unsigned long long endU = StampOf(e.end.wYear ? e.end : e.start);
                if (endU <= nowU) continue;  // already ended -> not upcoming
            }
            cache_.push_back(e);
        }
        std::sort(cache_.begin(), cache_.end(), [](const IcsEvent& a, const IcsEvent& b) {
            if (a.allDay != b.allDay) return a.allDay;  // all-day first
            return StampOf(a.start) < StampOf(b.start);
        });
        return cache_;
    }
};

// --- MediaCard (now playing: art, transport, scrub, live waveform) -----------
struct MediaCard : Widget {
    float Measure(const DrawContext& dc, float) override { return 112.0f * dc.scale; }

    D2D1_RECT_F SeekRect(float s) const {
        return D2D1::RectF(bounds.left + 12 * s, bounds.bottom - 15 * s, bounds.right - 12 * s, bounds.bottom - 11 * s);
    }
    void Buttons(float s, D2D1_RECT_F& prev, D2D1_RECT_F& play, D2D1_RECT_F& next) const {
        float cx = (bounds.left + 84 * s + bounds.right) * 0.5f;
        float y0 = bounds.top + 60 * s, y1 = bounds.top + 88 * s, r = 15 * s;
        prev = D2D1::RectF(cx - 44 * s - r, y0, cx - 44 * s + r, y1);
        play = D2D1::RectF(cx - r, y0, cx + r, y1);
        next = D2D1::RectF(cx + 44 * s - r, y0, cx + 44 * s + r, y1);
    }
    bool OnPointer(const PointerEvent& e, bool& wc) override {
        wc = false; const float s = lastScale_;
        D2D1_RECT_F sk = SeekRect(s);
        bool inSeek = e.pos.x >= sk.left - 6 * s && e.pos.x <= sk.right + 6 * s &&
                      e.pos.y >= sk.top - 10 * s && e.pos.y <= sk.bottom + 10 * s;
        if (e.phase == PointerPhase::Down) {
            if (inSeek) { seeking_ = true; wc = true; pendingFrac_ = Clamp((e.pos.x - sk.left) / std::max(1.0f, W(sk)), 0.0f, 1.0f); }
            return true;
        }
        if (e.phase == PointerPhase::Move && seeking_) { pendingFrac_ = Clamp((e.pos.x - sk.left) / std::max(1.0f, W(sk)), 0.0f, 1.0f); return true; }
        if (e.phase == PointerPhase::Up) {
            if (seeking_) { seeking_ = false; MediaSeek(Clamp((e.pos.x - sk.left) / std::max(1.0f, W(sk)), 0.0f, 1.0f)); return true; }
            D2D1_RECT_F p, pl, n; Buttons(s, p, pl, n);
            auto in = [&](const D2D1_RECT_F& r) { return e.pos.x >= r.left && e.pos.x <= r.right && e.pos.y >= r.top && e.pos.y <= r.bottom; };
            if (in(p)) MediaCommand(0); else if (in(pl)) MediaCommand(1); else if (in(n)) MediaCommand(2);
            return true;
        }
        return false;
    }
    void Paint(DrawContext& dc) override {
        lastScale_ = dc.scale; const float s = dc.scale;
        MediaState m = MediaSnapshot();
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(bounds, 12 * s, 12 * s);
        dc.card->SetOpacity(0.06f); dc.dc->FillRoundedRectangle(rr, dc.card); dc.card->SetOpacity(1.0f);

        D2D1_RECT_F art = D2D1::RectF(bounds.left + 10 * s, bounds.top + 10 * s, bounds.left + 74 * s, bounds.top + 74 * s);
        if (dc.artBmp) dc.dc->DrawBitmap(dc.artBmp, art);
        else { dc.card->SetOpacity(0.12f); dc.dc->FillRoundedRectangle(D2D1::RoundedRect(art, 8 * s, 8 * s), dc.card); dc.card->SetOpacity(1.0f);
               dc.Text(L"\xE8D6", dc.fGlyph ? dc.fGlyph : dc.fTitle, art, dc.muted, 0.6f, DWRITE_TEXT_ALIGNMENT_CENTER); }

        float tx = bounds.left + 84 * s;
        dc.Text(m.title.empty() ? L"Nothing playing" : m.title, dc.fTitle,
                D2D1::RectF(tx, bounds.top + 12 * s, bounds.right - 12 * s, bounds.top + 34 * s), dc.text, 0.95f);
        dc.Text(m.artist, dc.fSmall, D2D1::RectF(tx, bounds.top + 34 * s, bounds.right - 12 * s, bounds.top + 52 * s), dc.muted, 0.8f);

        // Live waveform strip.
        int wcount; auto wave = WaveSnapshot(wcount);
        D2D1_RECT_F wf = D2D1::RectF(tx, bounds.top + 52 * s, bounds.right - 12 * s, bounds.top + 60 * s);
        const int bars = 20; float bw = W(wf) / bars;
        for (int i = 0; i < bars; ++i) {
            int idx = wcount - bars + i;
            float v = idx >= 0 ? wave[((idx % kWaveN) + kWaveN) % kWaveN] : 0.0f;
            if (!m.playing) v *= 0.2f;
            float h = std::max(1.0f, v * 8 * s);
            float x = wf.left + i * bw;
            dc.accent->SetOpacity(0.4f + 0.6f * v);
            dc.dc->FillRectangle(D2D1::RectF(x, wf.bottom - h, x + bw * 0.6f, wf.bottom), dc.accent);
        }
        dc.accent->SetOpacity(1.0f);

        D2D1_RECT_F bp, bpl, bn; Buttons(s, bp, bpl, bn);
        IDWriteTextFormat* g = dc.fGlyph ? dc.fGlyph : dc.fTitle;
        dc.Text(L"\xE892", g, bp, dc.text, 0.9f, DWRITE_TEXT_ALIGNMENT_CENTER);
        dc.Text(m.playing ? L"\xE769" : L"\xE768", g, bpl, dc.text, 0.95f, DWRITE_TEXT_ALIGNMENT_CENTER);
        dc.Text(L"\xE893", g, bn, dc.text, 0.9f, DWRITE_TEXT_ALIGNMENT_CENTER);

        D2D1_RECT_F sk = SeekRect(s); float rad = H(sk) * 0.5f;
        dc.dc->FillRoundedRectangle(D2D1::RoundedRect(sk, rad, rad), dc.track);
        double posSec = m.posTicks / 1e7 + ((m.playing && m.capturedTick) ? (double)(GetTickCount64() - m.capturedTick) / 1000.0 : 0.0);
        double durSec = m.endTicks / 1e7;
        float frac = (seeking_ && pendingFrac_ >= 0) ? pendingFrac_ : (durSec > 0 ? Clamp((float)(posSec / durSec), 0.0f, 1.0f) : 0.0f);
        D2D1_RECT_F fill = sk; fill.right = sk.left + W(sk) * frac;
        dc.dc->FillRoundedRectangle(D2D1::RoundedRect(fill, rad, rad), dc.accent);
    }
   private:
    float lastScale_ = 1.0f;
    bool seeking_ = false;
    float pendingFrac_ = -1.0f;
};

// --- StackPanel --------------------------------------------------------------
struct StackPanel : Widget {
    std::vector<std::unique_ptr<Widget>> children;
    float padX = 14.0f, padY = 12.0f, gap = 8.0f;
    bool drawBg = false;   // fill a rounded panel backdrop (for the Open control center)

    Widget* Add(std::unique_ptr<Widget> w) {
        w->parent = this;
        Widget* raw = w.get();
        children.push_back(std::move(w));
        return raw;
    }
    float Measure(const DrawContext& dc, float width) override {
        const float s = dc.scale;
        float inner = width - 2.0f * padX * s;
        float h = padY * s;
        bool first = true;
        for (auto& c : children) {
            if (!c->visible) continue;
            if (!first) h += gap * s;
            first = false;
            h += c->Measure(dc, inner);
        }
        h += padY * s;
        return h;
    }
    void Layout(D2D1_RECT_F b) override {
        bounds = b;
        const DrawContext* dcp = layoutDc;
        float s = dcp ? dcp->scale : 1.0f;
        float x = b.left + padX * s;
        float right = b.right - padX * s;
        float y = b.top + padY * s;
        for (auto& c : children) {
            if (!c->visible) continue;
            float ch = c->Measure(*dcp, right - x);
            c->Layout(D2D1::RectF(x, y, right, y + ch));
            y += ch + gap * s;
        }
    }
    void Paint(DrawContext& dc) override {
        if (drawBg) {
            float r = 24.0f * dc.scale;
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(bounds, r, r);
            dc.dc->FillRoundedRectangle(rr, dc.panel);
            dc.dc->DrawRoundedRectangle(rr, dc.border, 1.0f);
        }
        for (auto& c : children) if (c->visible) c->Paint(dc);
    }
    Widget* HitTestDeep(D2D1_POINT_2F p) override {
        if (!visible || !Contains(p)) return nullptr;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (!(*it)->visible) continue;
            if (Widget* h = (*it)->HitTestDeep(p)) return h;
        }
        return this;
    }
    const DrawContext* layoutDc = nullptr;  // set by Surface before Layout
};

// --- ScrollContainer ---------------------------------------------------------
struct ScrollContainer : Widget {
    std::vector<std::unique_ptr<Widget>> children;
    SpringValue scroll;
    float contentHeight = 0.0f;
    float viewportMax = 240.0f;
    float gap = 6.0f;
    const DrawContext* layoutDc = nullptr;

    Widget* Add(std::unique_ptr<Widget> w) {
        w->parent = this;
        Widget* raw = w.get();
        children.push_back(std::move(w));
        return raw;
    }
    void Clear() { children.clear(); }
    ScrollContainer* AsScroll() override { return this; }

    float Measure(const DrawContext& dc, float width) override {
        float h = 0.0f; bool first = true;
        for (auto& c : children) {
            if (!c->visible) continue;
            if (!first) h += gap * dc.scale;
            first = false;
            h += c->Measure(dc, width);
        }
        contentHeight = h;
        return std::min(h, viewportMax * dc.scale);
    }
    void Layout(D2D1_RECT_F b) override {
        bounds = b;
        const DrawContext* dcp = layoutDc;
        float s = dcp ? dcp->scale : 1.0f;
        float maxScroll = std::max(0.0f, contentHeight - H(b));
        scroll.target = Clamp(scroll.target, 0.0f, maxScroll);
        float y = b.top - scroll.value;
        for (auto& c : children) {
            if (!c->visible) continue;
            float ch = c->Measure(*dcp, W(b));
            c->Layout(D2D1::RectF(b.left, y, b.right, y + ch));
            y += ch + gap * s;
        }
    }
    void OnWheel(float wheel) {
        scroll.target = scroll.target - wheel;  // wheel>0 = up
    }
    bool Tick(float dt) override {
        scroll.Step(dt, 220.0f, 26.0f);
        return !scroll.AtRest();
    }
    void Paint(DrawContext& dc) override {
        dc.dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        for (auto& c : children) {
            if (!c->visible) continue;
            if (c->bounds.bottom < bounds.top || c->bounds.top > bounds.bottom) continue;  // cull
            c->Paint(dc);
        }
        dc.dc->PopAxisAlignedClip();
        // Scrollbar hint
        float maxScroll = std::max(0.0f, contentHeight - H(bounds));
        if (maxScroll > 1.0f) {
            float frac = H(bounds) / contentHeight;
            float barH = std::max(20.0f * dc.scale, H(bounds) * frac);
            float t = scroll.value / maxScroll;
            float barY = bounds.top + t * (H(bounds) - barH);
            D2D1_RECT_F bar = D2D1::RectF(bounds.right - 3.0f * dc.scale, barY,
                                          bounds.right - 1.0f * dc.scale, barY + barH);
            dc.track->SetOpacity(0.5f);
            dc.dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 1.5f * dc.scale, 1.5f * dc.scale), dc.track);
            dc.track->SetOpacity(1.0f);
        }
    }
    Widget* HitTestDeep(D2D1_POINT_2F p) override {
        if (!visible || !Contains(p)) return nullptr;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (!(*it)->visible) continue;
            if ((*it)->bounds.bottom < bounds.top || (*it)->bounds.top > bounds.bottom) continue;
            if (Widget* h = (*it)->HitTestDeep(p)) return h;
        }
        return this;
    }
};

// ===========================================================================
// Renderer — D2D host. Owns brushes/formats; paints a Surface.
// ===========================================================================
class Renderer {
   public:
    bool Initialize(HWND hwnd) {
        hwnd_ = hwnd;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                     reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))))
            return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()))))
            return false;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&props, &target_))) return false;
        return CreateBackingBitmap(360, 96);
    }
    void Shutdown() {
        fHuge_.Reset(); fTitle_.Reset(); fBody_.Reset(); fSmall_.Reset(); fPill_.Reset(); fGlyph_.Reset();
        bText_.Reset(); bMuted_.Reset(); bAccent_.Reset(); bPanel_.Reset(); bCard_.Reset();
        bBorder_.Reset(); bDivider_.Reset(); bTrack_.Reset();
        artBitmap_.Reset();
        contentLayer_.Reset();
        target_.Reset(); dwriteFactory_.Reset(); d2dFactory_.Reset();
        if (oldBitmap_) { SelectObject(memDc_, oldBitmap_); oldBitmap_ = nullptr; }
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        if (memDc_) { DeleteDC(memDc_); memDc_ = nullptr; }
    }

    // Cached opacity layer for the content cross-fade during a morph (created lazily post-init).
    ID2D1Layer* EnsureContentLayer() {
        if (!contentLayer_ && target_) target_->CreateLayer(nullptr, &contentLayer_);
        return contentLayer_.Get();
    }

    IDWriteFactory* DWrite() const { return dwriteFactory_.Get(); }

    DrawContext MakeContext(float scale, double now) {
        EnsureFormats(scale);
        DrawContext dc;
        dc.dc = target_.Get(); dc.dw = dwriteFactory_.Get(); dc.scale = scale; dc.now = now;
        dc.text = bText_.Get(); dc.muted = bMuted_.Get(); dc.accent = bAccent_.Get();
        dc.panel = bPanel_.Get(); dc.card = bCard_.Get(); dc.border = bBorder_.Get();
        dc.divider = bDivider_.Get(); dc.track = bTrack_.Get();
        dc.fHuge = fHuge_.Get(); dc.fTitle = fTitle_.Get(); dc.fBody = fBody_.Get();
        dc.fSmall = fSmall_.Get(); dc.fPill = fPill_.Get(); dc.fGlyph = fGlyph_.Get();
        return dc;
    }
    IDWriteTextFormat* GlyphFormat() const { return fGlyph_.Get(); }

    // Cache an ID2D1Bitmap for the current album art (by generation).
    ID2D1Bitmap* ArtBitmap(uint64_t gen, int w, int h, const std::vector<uint8_t>& bgra) {
        if (gen == 0 || bgra.empty() || w <= 0 || h <= 0) return nullptr;
        if (artBitmap_ && artGen_ == gen) return artBitmap_.Get();
        artBitmap_.Reset();
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(target_->CreateBitmap(D2D1::SizeU((UINT)w, (UINT)h), bgra.data(), (UINT)(w * 4), &bp, &artBitmap_))) {
            artGen_ = gen;
            return artBitmap_.Get();
        }
        return nullptr;
    }

    // Begin a frame. `contentW/H` is the current (animating) size we PRESENT; `targetW/H` is the
    // spring's destination and `settled` says the morph is done. To keep the expand/collapse morph
    // smooth we size the backing DIB to the TARGET (allocated once per morph) and only present the
    // current size from its top-left — this eliminates the per-frame CreateDIBSection realloc +
    // shrink/grow GDI churn that made resizing choppy. Returns the inner content rect.
    bool BeginFrame(const Settings& settings, int contentW, int contentH,
                    int targetW, int targetH, bool settled, float scale, D2D1_RECT_F& innerOut) {
        const float padX = kRenderPadX * scale, padY = kRenderPadY * scale;
        const int pxW = std::max(1, static_cast<int>(std::ceil(contentW + padX * 2.0f)));  // present
        const int pxH = std::max(1, static_cast<int>(std::ceil(contentH + padY * 2.0f)));
        const int aW = std::max(1, static_cast<int>(std::ceil(std::max(contentW, targetW) + padX * 2.0f)));
        const int aH = std::max(1, static_cast<int>(std::ceil(std::max(contentH, targetH) + padY * 2.0f)));
        // Grow the DIB straight to the target on the first morph frame; never realloc mid-morph;
        // shrink it back to the exact present size only once the spring has settled.
        const int needW = settled ? pxW : std::max(aW, bitmapWidth_);
        const int needH = settled ? pxH : std::max(aH, bitmapHeight_);
        if (needW != bitmapWidth_ || needH != bitmapHeight_) {
            if (!CreateBackingBitmap(needW, needH)) return false;
        }
        // Resize/reposition the window whenever the PRESENT size changes (each morph frame) or on
        // an external layout change (settings / display change).
        // Size/position the WINDOW to the (stable) DIB size, not the animating size. Because the DIB
        // is held at max(current,target) for the whole morph, the window never repositions per-frame
        // — the panel + content animate WITHIN a fixed window. This is what kills the horizontal
        // sub-pixel jitter of the clock text (the window's centered x-position was rounding
        // independently of the content origin every frame). At rest DIB == content, so no margin.
        if (bitmapWidth_ != presentW_ || bitmapHeight_ != presentH_ || g_layoutDirty.exchange(false)) {
            PositionOverlayWindow(settings, bitmapWidth_, bitmapHeight_);
            presentW_ = bitmapWidth_; presentH_ = bitmapHeight_;
        }
        RECT rc = {0, 0, bitmapWidth_, bitmapHeight_};
        if (FAILED(target_->BindDC(memDc_, &rc))) return false;
        EnsureBrushes();  // device-dependent: must be created after BindDC
        target_->BeginDraw();
        target_->Clear(D2D1::ColorF(0, 0.0f));
        innerOut = D2D1::RectF(padX, padY, (float)bitmapWidth_ - padX, (float)bitmapHeight_ - padY);
        return true;
    }
    bool EndFrame(const Settings& settings) {
        if (FAILED(target_->EndDraw())) return false;
        POINT src = {0, 0};
        SIZE size = {presentW_, presentH_};  // present the current size from the DIB's top-left
        RECT win = {}; GetWindowRect(hwnd_, &win);
        POINT dst = {win.left, win.top};
        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = static_cast<BYTE>(Clamp(settings.pillOpacity, 0.35f, 1.0f) * 255.0f);
        blend.AlphaFormat = AC_SRC_ALPHA;
        return UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, memDc_, &src, 0, &blend, ULW_ALPHA) != FALSE;
    }

    void DrawPanelSurface(D2D1_RECT_F inner) {
        float r = std::min(H(inner) * 0.5f, 22.0f);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(inner, r, r);
        target_->FillRoundedRectangle(rr, bPanel_.Get());
        target_->DrawRoundedRectangle(rr, bBorder_.Get(), 1.0f);
    }

   private:
    void PositionOverlayWindow(const Settings& s, int width, int height);

    bool CreateBackingBitmap(int width, int height) {
        if (oldBitmap_) { SelectObject(memDc_, oldBitmap_); oldBitmap_ = nullptr; }
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        if (!memDc_) {
            HDC screen = GetDC(nullptr);
            memDc_ = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!memDc_) return false;
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dib_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib_) return false;
        oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, dib_));
        bitmapWidth_ = width; bitmapHeight_ = height;
        return true;
    }

    void EnsureFormats(float scale) {
        if (fTitle_ && std::fabs(scale - lastScale_) < 0.001f) return;
        fHuge_.Reset(); fTitle_.Reset(); fBody_.Reset(); fSmall_.Reset(); fPill_.Reset(); fGlyph_.Reset();
        auto mk = [&](const wchar_t* family, DWRITE_FONT_WEIGHT w, float size, ComPtr<IDWriteTextFormat>& out) {
            dwriteFactory_->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, size * scale, L"", &out);
            if (out) { out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        };
        mk(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_BOLD, 32.0f, fHuge_);
        mk(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0f, fTitle_);
        mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 13.0f, fBody_);
        mk(L"Segoe UI Variable Small", DWRITE_FONT_WEIGHT_NORMAL, 11.5f, fSmall_);
        mk(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0f, fPill_);
        mk(L"Segoe Fluent Icons", DWRITE_FONT_WEIGHT_NORMAL, 16.0f, fGlyph_);
        if (fPill_) {
            fPill_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fPill_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (fHuge_) { fHuge_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); fHuge_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); }
        for (auto* f : {fTitle_.Get(), fBody_.Get(), fSmall_.Get()})
            if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (fGlyph_) { fGlyph_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); fGlyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); }
        lastScale_ = scale;
    }
    void EnsureBrushes() {
        auto mk = [&](float r, float g, float b, float a, ComPtr<ID2D1SolidColorBrush>& out) {
            if (!out) target_->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &out);
        };
        mk(1, 1, 1, 0.96f, bText_);
        mk(1, 1, 1, 0.62f, bMuted_);
        mk(0.20f, 0.55f, 1.0f, 1.0f, bAccent_);
        mk(0.043f, 0.043f, 0.051f, 0.94f, bPanel_);
        mk(1, 1, 1, 1.0f, bCard_);     // opacity varied at draw time
        mk(1, 1, 1, 0.10f, bBorder_);
        mk(1, 1, 1, 0.12f, bDivider_);
        mk(1, 1, 1, 0.16f, bTrack_);
    }

    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1Layer> contentLayer_;
    ComPtr<IDWriteTextFormat> fHuge_, fTitle_, fBody_, fSmall_, fPill_, fGlyph_;
    ComPtr<ID2D1SolidColorBrush> bText_, bMuted_, bAccent_, bPanel_, bCard_, bBorder_, bDivider_, bTrack_;
    ComPtr<ID2D1Bitmap> artBitmap_;
    uint64_t artGen_ = 0;
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr, oldBitmap_ = nullptr;
    int bitmapWidth_ = 0, bitmapHeight_ = 0;   // allocated DIB size (may exceed the presented size)
    int presentW_ = 0, presentH_ = 0;          // size actually shown via UpdateLayeredWindow
    float lastScale_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Monitor anchoring
// ---------------------------------------------------------------------------
struct MonitorEnumData { std::vector<HMONITOR> monitors; };
BOOL CALLBACK MonitorEnumProc(HMONITOR h, HDC, LPRECT, LPARAM p) {
    reinterpret_cast<MonitorEnumData*>(p)->monitors.push_back(h);
    return TRUE;
}
HMONITOR GetAnchorMonitor(const Settings& s) {
    if (s.targetMonitor == -1) { POINT pt; GetCursorPos(&pt); return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST); }
    if (s.targetMonitor > 0) {
        MonitorEnumData d;
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&d));
        int i = s.targetMonitor - 1;
        if (i >= 0 && i < (int)d.monitors.size()) return d.monitors[i];
    }
    POINT o = {0, 0};
    return MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
}
RECT GetAnchorWorkRect(const Settings& s) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(GetAnchorMonitor(s), &mi);
    return mi.rcWork;
}
void Renderer::PositionOverlayWindow(const Settings& s, int width, int height) {
    RECT work = GetAnchorWorkRect(s);
    int x = work.left + (work.right - work.left - width) / 2;
    int y = work.top + 8;
    switch (s.position) {
        case Position::TopLeft:      x = work.left + 16; y = work.top + 8; break;
        case Position::TopRight:     x = work.right - width - 16; y = work.top + 8; break;
        case Position::BottomCenter: x = work.left + (work.right - work.left - width) / 2; y = work.bottom - height - 40; break;
        default: break;
    }
    x += s.offsetX; y += s.offsetY;
    HWND z = s.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(hwnd_, z, x, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

// ---------------------------------------------------------------------------
// Brightness control (DDC/CI on the island's monitor; runs off the render thread)
// ---------------------------------------------------------------------------
std::atomic<int> g_brightPercent{-1};   // -1 = unsupported / hidden
std::atomic<int> g_brightSetRequest{-1};
std::atomic<bool> g_brightRebind{false};
HANDLE g_brightEvent = nullptr;         // auto-reset; signals set/rebind
HANDLE g_brightThread = nullptr;

DWORD WINAPI BrightnessThreadProc(void*) {
    std::vector<PHYSICAL_MONITOR> phys;
    HANDLE active = nullptr;
    DWORD minB = 0, curB = 0, maxB = 0;
    bool haveCaps = false;

    auto cleanup = [&]() {
        if (!phys.empty()) { DestroyPhysicalMonitors((DWORD)phys.size(), phys.data()); phys.clear(); }
        active = nullptr; haveCaps = false;
    };
    auto pctFrom = [&](DWORD cur) -> int {
        if (cur < minB) cur = minB;
        if (cur > maxB) cur = maxB;
        return (maxB > minB) ? (int)(((long long)(cur - minB) * 100 + (maxB - minB) / 2) / (maxB - minB)) : 0;
    };
    auto rebind = [&]() {
        cleanup();
        Settings s = SettingsSnapshot();
        if (!s.brightnessEnabled) { g_brightPercent = -1; return; }
        HMONITOR hm = GetAnchorMonitor(s);
        DWORD n = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hm, &n) || n == 0) { g_brightPercent = -1; return; }
        phys.resize(n);
        if (!GetPhysicalMonitorsFromHMONITOR(hm, n, phys.data())) { phys.clear(); g_brightPercent = -1; return; }
        for (auto& pm : phys) {
            DWORD mn = 0, cu = 0, mx = 0;
            if (GetMonitorBrightness(pm.hPhysicalMonitor, &mn, &cu, &mx)) {
                minB = mn; curB = cu; maxB = mx; haveCaps = true; active = pm.hPhysicalMonitor;
                g_brightPercent = pctFrom(cu);
                return;
            }
        }
        g_brightPercent = -1;  // no monitor honored DDC/CI
    };

    rebind();
    HANDLE waits[2] = {g_stopEvent, g_brightEvent};
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 3000);
        if (wr == WAIT_OBJECT_0) break;
        if (g_brightRebind.exchange(false)) { rebind(); g_layoutDirty = true; continue; }
        int req = g_brightSetRequest.exchange(-1);
        if (req >= 0 && haveCaps) {
            DWORD val = minB + (DWORD)((double)(maxB - minB) * Clamp(req / 100.0f, 0.0f, 1.0f) + 0.5);
            if (SetMonitorBrightness(active, val)) { curB = val; g_brightPercent = pctFrom(val); }
            else { g_brightPercent = -1; cleanup(); }
            g_layoutDirty = true;
        }
        if (wr == WAIT_TIMEOUT && haveCaps) {
            DWORD mn = 0, cu = 0, mx = 0;
            if (GetMonitorBrightness(active, &mn, &cu, &mx)) {
                minB = mn; maxB = mx;
                int p = pctFrom(cu);
                if (p != g_brightPercent.load()) { g_brightPercent = p; g_layoutDirty = true; }
            }
        }
    }
    cleanup();
    return 0;
}

// ===========================================================================
// Surface — state machine + routing
// ===========================================================================
enum class SurfaceState { Collapsed, TransientPopup, Expanded, Open };

void InstallDismissHook();
void RemoveDismissHook();

class Surface {
   public:
    SurfaceState state = SurfaceState::Collapsed;

    void BuildFor(SurfaceState s, const DrawContext& dc) {
        if (captureTarget_) ReleaseCapture();  // don't leak OS capture if we rebuild mid-drag
        state = s;
        root_ = BuildRoot(s, dc);
        captureTarget_ = nullptr;
        pressTarget_ = nullptr;
        builtNotifGen_ = g_notifGen.load();  // snapshot the notif list version this tree was built from
        builtScale_ = dc.scale;
    }

    // The initial BuildFor(Collapsed, ...) at startup uses a placeholder scale of 1.0 (no real
    // window/DPI yet). If the surface never transitions state before the real per-monitor scale is
    // known, sizes baked into the tree (e.g. Custom::fixedHeight, captured by value at build time)
    // stay wrong forever. Call this every frame; it rebuilds the CURRENT state at the real scale the
    // first time they differ (and again on any later DPI/SizeScale change).
    bool RebuildIfScaleChanged(const DrawContext& dc) {
        if (std::fabs(dc.scale - builtScale_) > 0.001f) { BuildFor(state, dc); return true; }
        return false;
    }

    // The Open panel's notification rows are baked at build time; when the list changes
    // (arrival / dismiss / clear-all) rebuild the tree in place so it refreshes without a
    // close/open. Skip while dragging so a rebuild never drops an in-flight slider capture.
    bool RebuildIfDataChanged(const DrawContext& dc) {
        if (state == SurfaceState::Open && !captureTarget_ && g_notifGen.load() != builtNotifGen_) {
            BuildFor(SurfaceState::Open, dc);
            return true;
        }
        return false;
    }

    // Returns desired content size for the current state.
    D2D1_SIZE_F MeasureContent(const DrawContext& dc) {
        SetLayoutDc(root_.get(), &dc);
        float width = StateWidth(dc);
        if (state == SurfaceState::Collapsed || state == SurfaceState::TransientPopup) {
            float pw = root_ ? root_->PreferredWidth(dc) : -1.0f;
            if (pw > 0) width = pw;
        }
        float height = root_ ? root_->Measure(dc, width) : 40.0f * dc.scale;
        float maxH = MaxOpenHeight(dc);
        if (state == SurfaceState::Open && height > maxH) height = maxH;
        return D2D1::SizeF(width, height);
    }

    void SetLocked(D2D1_SIZE_F sz) { lockedW_ = sz.width; lockedH_ = sz.height; }
    float LockedW() const { return lockedW_; }
    float LockedH() const { return lockedH_; }

    void Layout(const DrawContext& dc, D2D1_RECT_F inner) {
        SetLayoutDc(root_.get(), &dc);
        if (!root_) return;
        // Lay out at the LOCKED (settled) content size, centered in the animating inner rect, so
        // the tree never re-flows at intermediate morph sizes — it is simply revealed by the
        // growing panel (the caller clips to inner). SNAP the origin to whole device pixels and keep
        // the width CONSTANT (lockedW_) so the clock/text glyphs keep the same sub-pixel phase every
        // frame; a fractional, per-frame-shifting origin makes DirectWrite re-rasterize them, which
        // reads as horizontal shimmer/blur during the morph. At rest inner == the locked size.
        float lw = lockedW_ > 0 ? lockedW_ : W(inner);
        float lh = lockedH_ > 0 ? lockedH_ : H(inner);
        float left = std::floor((inner.left + inner.right - lw) * 0.5f + 0.5f);
        float top  = std::floor(inner.top + 0.5f);
        root_->Layout(D2D1::RectF(left, top, left + lw, top + lh));
    }
    void Paint(DrawContext& dc) { if (root_) root_->Paint(dc); }

    bool Tick(float dt) { return root_ ? TickTree(root_.get(), dt) : false; }

    // ---- pointer ----
    void OnMove(const PointerEvent& e) {
        if (captureTarget_) {
            bool wc; captureTarget_->OnPointer(e, wc);
        }
    }
    bool OnDown(const PointerEvent& e) {
        if (state != SurfaceState::Open) return false;  // clicks open the panel (handled by caller)
        Widget* hit = root_ ? root_->HitTestDeep(e.pos) : nullptr;
        pressTarget_ = hit;
        if (hit) {
            bool wc = false;
            bool handled = hit->OnPointer(e, wc);
            if (wc) { captureTarget_ = hit; SetCapture(g_hwnd); }
            return handled;
        }
        return false;
    }
    bool OnUp(const PointerEvent& e) {
        if (captureTarget_) {
            bool wc; captureTarget_->OnPointer(e, wc);
            captureTarget_ = nullptr;
            ReleaseCapture();
            return true;
        }
        if (state != SurfaceState::Open) return false;
        Widget* hit = root_ ? root_->HitTestDeep(e.pos) : nullptr;
        if (hit && hit == pressTarget_) { bool wc; return hit->OnPointer(e, wc); }
        return false;
    }
    void OnWheel(const PointerEvent& e) {
        Widget* w = root_ ? root_->HitTestDeep(e.pos) : nullptr;
        while (w) {
            if (ScrollContainer* sc = w->AsScroll()) { sc->OnWheel(e.wheel); g_layoutDirty = true; return; }
            w = w->parent;
        }
    }
    bool IsCapturing() const { return captureTarget_ != nullptr; }

    void ForEachSlider(const std::function<void(Slider*)>& fn) { ForEachSliderImpl(root_.get(), fn); }

    // Drain any pending slider drag into a real value-set (needs dc.scale).
    void FlushPendingDrag(const DrawContext& dc) {
        if (auto* sl = dynamic_cast<Slider*>(captureTarget_ ? captureTarget_ : nullptr)) {
            if (sl->HasPending()) sl->Apply(dc, sl->TakePending());
        }
    }

    void Open() {
        if (state == SurfaceState::Open) return;
        pendingState_ = SurfaceState::Open;
        InstallDismissHook();
    }
    void Dismiss() {
        if (state == SurfaceState::Collapsed) return;
        pendingState_ = SurfaceState::Collapsed;
        RemoveDismissHook();
    }
    // Drive Collapsed / TransientPopup / Expanded transitions (Open is click-driven).
    void UpdateAmbientState(bool hovered, double now) {
        if (state == SurfaceState::Open) return;
        if (hovered) {
            lastHoverTime_ = now;
            if (state != SurfaceState::Expanded) pendingState_ = SurfaceState::Expanded;
            return;
        }
        if (state == SurfaceState::Expanded && now - lastHoverTime_ < 0.30) return;  // grace
        SurfaceState want = TransientActive() ? SurfaceState::TransientPopup : SurfaceState::Collapsed;
        if (state != want) pendingState_ = want;
    }
    // Apply a queued state change (rebuilds root). Returns true if changed.
    bool ApplyPendingState(const DrawContext& dc) {
        if (!pendingState_.has_value() || *pendingState_ == state) { pendingState_.reset(); return false; }
        BuildFor(*pendingState_, dc);
        pendingState_.reset();
        return true;
    }

   private:
    float StateWidth(const DrawContext& dc) const {
        switch (state) {
            case SurfaceState::Collapsed:      return 180.0f * dc.scale;
            case SurfaceState::TransientPopup: return 230.0f * dc.scale;
            case SurfaceState::Expanded:       return 360.0f * dc.scale;
            case SurfaceState::Open:           return 360.0f * dc.scale;
        }
        return 180.0f * dc.scale;
    }
    float MaxOpenHeight(const DrawContext& dc) const {
        RECT work = GetAnchorWorkRect(SettingsSnapshot());
        return std::min(620.0f * dc.scale, (work.bottom - work.top) * 0.78f);
    }
    static void SetLayoutDc(Widget* w, const DrawContext* dc) {
        if (!w) return;
        if (auto* sp = dynamic_cast<StackPanel*>(w)) { sp->layoutDc = dc; for (auto& c : sp->children) SetLayoutDc(c.get(), dc); }
        else if (auto* sc = dynamic_cast<ScrollContainer*>(w)) { sc->layoutDc = dc; for (auto& c : sc->children) SetLayoutDc(c.get(), dc); }
    }
    static bool TickTree(Widget* w, float dt) {
        bool anim = w->Tick(dt);
        if (auto* sp = dynamic_cast<StackPanel*>(w)) for (auto& c : sp->children) anim |= TickTree(c.get(), dt);
        else if (auto* sc = dynamic_cast<ScrollContainer*>(w)) for (auto& c : sc->children) anim |= TickTree(c.get(), dt);
        return anim;
    }

    std::unique_ptr<Widget> BuildRoot(SurfaceState s, const DrawContext& dc);

    static void ForEachSliderImpl(Widget* w, const std::function<void(Slider*)>& fn) {
        if (!w) return;
        if (auto* sl = dynamic_cast<Slider*>(w)) fn(sl);
        if (auto* sp = dynamic_cast<StackPanel*>(w)) { for (auto& c : sp->children) ForEachSliderImpl(c.get(), fn); }
        else if (auto* sc = dynamic_cast<ScrollContainer*>(w)) { for (auto& c : sc->children) ForEachSliderImpl(c.get(), fn); }
    }

    std::unique_ptr<Widget> root_;
    Widget* captureTarget_ = nullptr;
    Widget* pressTarget_ = nullptr;
    std::optional<SurfaceState> pendingState_;
    double lastHoverTime_ = 0.0;
    uint64_t builtNotifGen_ = 0;
    float lockedW_ = 0.0f, lockedH_ = 0.0f;  // frozen target size during a morph
    float builtScale_ = -1.0f;  // scale the current root_ was built at; -1 forces the first real rebuild
};

// ---- formatting helpers ----
std::wstring TimeString() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t buf[32] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &t, nullptr, buf, ARRAYSIZE(buf));
    return buf;
}
// Changes once per displayed minute — used as a cheap "the clock text differs now" render trigger.
int ClockMinuteKey() {
    SYSTEMTIME t; GetLocalTime(&t);
    return t.wHour * 60 + t.wMinute;
}
std::wstring DateStringShort() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t buf[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"ddd, MMM d", buf, ARRAYSIZE(buf), nullptr);
    return buf;
}
// Compact weekday + numeric date for the collapsed pill, e.g. "Wed 7/2" (no year).
std::wstring DatePillString() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t buf[32] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"ddd M/d", buf, ARRAYSIZE(buf), nullptr);
    return buf;
}
std::wstring DateStringLong() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t buf[96] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"dddd, MMMM d", buf, ARRAYSIZE(buf), nullptr);
    return buf;
}
bool WeatherFresh() { WeatherState w = WeatherSnapshot(); return w.hasData && NowSeconds() - w.updated <= 7200.0; }
std::wstring WeatherShort() {
    WeatherState w = WeatherSnapshot();
    if (!w.hasData) return L"";
    return WeatherGlyph(w.code) + L"  " + std::to_wstring(w.temp) + L"\x00B0";
}
// Secondary weather line for the hover peek: "Feels 83°   ·   Humidity 8%   ·   11 mph SSW".
std::wstring WeatherDetail() {
    WeatherState w = WeatherSnapshot();
    if (!w.hasData) return L"";
    std::wstring s = L"Feels " + std::to_wstring(w.feelsLike) + L"\x00B0";
    s += L"    \x00B7    Humidity " + std::to_wstring(w.humidity) + L"%";
    if (w.windSpeed > 0) {
        s += L"    \x00B7    " + std::to_wstring(w.windSpeed) +
             (SettingsSnapshot().weatherFahrenheit ? L" mph" : L" km/h");
        if (!w.windDir.empty()) s += L" " + w.windDir;
    }
    return s;
}
// The next N not-yet-ended events across today + upcoming days, sorted by start time.
// Cached (render thread only) by calendar generation + minute so it isn't recomputed
// from the full ~289-event store on every measure/paint frame while hovering.
std::vector<IcsEvent> NextUpcomingEvents(int maxN) {
    static uint64_t cGen = (uint64_t)-1; static int cMin = -1; static int cN = -1;
    static std::vector<IcsEvent> cache;
    uint64_t gen = g_calGen.load(); int mk = ClockMinuteKey();
    if (gen == cGen && mk == cMin && maxN == cN) return cache;
    cGen = gen; cMin = mk; cN = maxN;
    auto stamp = [](const SYSTEMTIME& st) {
        FILETIME f{}; SystemTimeToFileTime(&st, &f);
        ULARGE_INTEGER u; u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime; return u.QuadPart;
    };
    SYSTEMTIME nowSt; GetLocalTime(&nowSt);
    unsigned long long nowU = stamp(nowSt);
    std::vector<IcsEvent> out;
    for (auto& e : CalendarSnapshot()) {
        unsigned long long endU = stamp(e.end.wYear ? e.end : e.start);
        if (endU > nowU) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [&](const IcsEvent& a, const IcsEvent& b) { return stamp(a.start) < stamp(b.start); });
    if ((int)out.size() > maxN) out.resize(maxN);
    cache = std::move(out);
    return cache;
}

std::unique_ptr<Widget> Surface::BuildRoot(SurfaceState s, const DrawContext& dc) {
    if (s == SurfaceState::Collapsed) {
        auto pill = std::make_unique<Custom>();
        pill->fixedHeight = 50.0f * dc.scale;
        // [ time / date ]  |  weather  — time is the hero with the date as small sub-text beneath.
        pill->prefWidth = [](const DrawContext& d) {
            const float s = d.scale;
            float leftW = std::max(d.MeasureWidth(d.fPill, TimeString()),
                                   d.MeasureWidth(d.fSmall, DatePillString()));
            float total = 16.0f * s + leftW;
            if (WeatherFresh())
                total += 12.0f * s + 1.2f * s + 12.0f * s + d.MeasureWidth(d.fPill, WeatherShort());
            return total + 16.0f * s;
        };
        pill->paint = [](DrawContext& d, D2D1_RECT_F b) {
            const float s = d.scale;
            float leftW = std::max(d.MeasureWidth(d.fPill, TimeString()),
                                   d.MeasureWidth(d.fSmall, DatePillString()));
            float lx = b.left + 16.0f * s;
            d.Text(TimeString(), d.fPill, D2D1::RectF(lx, b.top + 9.0f * s, lx + leftW, b.top + 32.0f * s),
                   d.text, 0.96f, DWRITE_TEXT_ALIGNMENT_CENTER);
            d.Text(DatePillString(), d.fSmall, D2D1::RectF(lx, b.top + 31.0f * s, lx + leftW, b.top + 47.0f * s),
                   d.muted, 0.8f, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (WeatherFresh()) {
                float x = lx + leftW + 12.0f * s;
                d.dc->FillRectangle(D2D1::RectF(x, b.top + 13.0f * s, x + 1.2f * s, b.bottom - 13.0f * s), d.divider);
                x += 1.2f * s + 12.0f * s;
                std::wstring w = WeatherShort();
                float ww = d.MeasureWidth(d.fPill, w);
                d.Text(w, d.fPill, D2D1::RectF(x, b.top, x + ww, b.bottom), d.text, 0.92f,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                       D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
        };
        return pill;
    }

    if (s == SurfaceState::TransientPopup) {
        auto v = std::make_unique<Custom>();
        v->fixedHeight = 44.0f * dc.scale;
        v->prefWidth = [](const DrawContext& d) {
            Transient t = TransientSnapshot();
            float tw = d.MeasureWidth(d.fPill, t.line);
            return 56.0f * d.scale + tw + (t.kind == 2 ? 70.0f * d.scale : 24.0f * d.scale);
        };
        v->paint = [](DrawContext& d, D2D1_RECT_F b) {
            Transient t = TransientSnapshot();
            D2D1_RECT_F gr = D2D1::RectF(b.left + 14.0f * d.scale, b.top, b.left + 40.0f * d.scale, b.bottom);
            d.Text(t.glyph, d.fGlyph ? d.fGlyph : d.fPill, gr, d.accent, 0.95f, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (t.kind == 2) {  // volume: label + bar + percent
                D2D1_RECT_F lr = D2D1::RectF(b.left + 44.0f * d.scale, b.top + 6.0f * d.scale, b.right - 12.0f * d.scale, b.top + 22.0f * d.scale);
                d.Text(t.muted ? L"Muted" : t.line, d.fSmall, lr, d.muted, 0.8f);
                float shown = Clamp(g_volDisplayScalar.load(), 0.0f, 1.0f);  // smoothed, animates
                wchar_t pc[8]; swprintf_s(pc, L"%d%%", (int)(shown * 100.0f + 0.5f));
                d.Text(pc, d.fSmall, lr, d.text, 0.9f, DWRITE_TEXT_ALIGNMENT_TRAILING);
                D2D1_RECT_F tr = D2D1::RectF(b.left + 44.0f * d.scale, b.bottom - 14.0f * d.scale, b.right - 12.0f * d.scale, b.bottom - 10.0f * d.scale);
                float trad = H(tr) * 0.5f;
                d.dc->FillRoundedRectangle(D2D1::RoundedRect(tr, trad, trad), d.track);
                D2D1_RECT_F fill = tr; fill.right = tr.left + W(tr) * shown;
                d.dc->FillRoundedRectangle(D2D1::RoundedRect(fill, trad, trad), d.accent);
            } else {
                D2D1_RECT_F lr = D2D1::RectF(b.left + 44.0f * d.scale, b.top, b.right - 14.0f * d.scale, b.bottom);
                d.Text(t.line, d.fPill, lr, d.text, 0.95f);
            }
        };
        return v;
    }

    if (s == SurfaceState::Expanded) {
        auto v = std::make_unique<Custom>();
        // Height grows to fit the weather detail line + the mini "Up next" list.
        v->measure = [](const DrawContext& d, float) {
            bool wf = WeatherFresh();
            int n = (int)NextUpcomingEvents(3).size();
            float h = 12.0f + 44.0f + 26.0f;      // pad + clock + date
            if (wf) h += 26.0f + 22.0f;           // weather main + detail line
            h += 10.0f + 1.0f + 10.0f + 20.0f;    // divider + "Up next" header
            h += (n > 0 ? n * 24.0f : 22.0f);     // event rows or empty state
            h += 12.0f;                           // bottom pad
            return h * d.scale;
        };
        v->paint = [](DrawContext& d, D2D1_RECT_F b) {
            const float sc = d.scale;
            float y = b.top + 12.0f * sc;
            d.Text(TimeString(), d.fHuge, D2D1::RectF(b.left, y, b.right, y + 44 * sc), d.text, 0.97f, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 44 * sc;
            d.Text(DateStringLong(), d.fBody, D2D1::RectF(b.left, y, b.right, y + 22 * sc), d.muted, 0.8f, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 26 * sc;
            if (WeatherFresh()) {
                WeatherState w = WeatherSnapshot();
                std::wstring line = WeatherShort() + (w.desc.empty() ? L"" : L"   " + w.desc);
                d.Text(line, d.fTitle, D2D1::RectF(b.left, y, b.right, y + 26 * sc), d.text, 0.92f,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                y += 26 * sc;
                d.Text(WeatherDetail(), d.fSmall, D2D1::RectF(b.left, y, b.right, y + 18 * sc), d.muted, 0.7f, DWRITE_TEXT_ALIGNMENT_CENTER);
                y += 22 * sc;
            }
            d.dc->FillRectangle(D2D1::RectF(b.left + 24 * sc, y + 5 * sc, b.right - 24 * sc, y + 6 * sc), d.divider);
            y += 16 * sc;
            d.Text(L"Up next", d.fSmall, D2D1::RectF(b.left + 20 * sc, y, b.right - 20 * sc, y + 16 * sc), d.muted, 0.55f);
            y += 20 * sc;
            auto evs = NextUpcomingEvents(3);
            if (evs.empty()) {
                d.Text(L"Nothing upcoming", d.fBody, D2D1::RectF(b.left, y, b.right, y + 22 * sc), d.muted, 0.5f, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                SYSTEMTIME nowSt; GetLocalTime(&nowSt);
                for (auto& e : evs) {
                    bool sameDay = (e.start.wYear == nowSt.wYear && e.start.wMonth == nowSt.wMonth && e.start.wDay == nowSt.wDay);
                    wchar_t tm[24] = {};
                    if (e.allDay) wcscpy_s(tm, L"All day");
                    else { SYSTEMTIME es = e.start; GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &es, nullptr, tm, ARRAYSIZE(tm)); }
                    std::wstring when = tm;
                    if (!sameDay) {  // prefix the weekday so tomorrow's times aren't mistaken for past ones
                        wchar_t dd[16] = {}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &e.start, L"ddd", dd, ARRAYSIZE(dd), nullptr);
                        when = std::wstring(dd) + L" " + tm;
                    }
                    d.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 20 * sc, y + 4 * sc, b.left + 22.5f * sc, y + 20 * sc), 1.2f * sc, 1.2f * sc), d.accent);
                    d.Text(when, d.fSmall, D2D1::RectF(b.left + 30 * sc, y, b.left + 140 * sc, y + 22 * sc), d.muted, 0.8f);
                    d.Text(e.subject, d.fBody, D2D1::RectF(b.left + 144 * sc, y, b.right - 16 * sc, y + 22 * sc), d.text, 0.92f);
                    y += 24 * sc;
                }
            }
        };
        return v;
    }

    // Open: full control center. (The rounded panel backdrop is drawn once by the render loop at
    // the animating size; the content tree is transparent so the morph reveal/clip stays clean.)
    auto panel = std::make_unique<StackPanel>();
    panel->padX = 16.0f; panel->padY = 14.0f; panel->gap = 10.0f;

    // Header: big clock + long date (custom).
    auto header = std::make_unique<Custom>();
    header->fixedHeight = 64.0f * dc.scale;
    header->paint = [](DrawContext& d, D2D1_RECT_F b) {
        D2D1_RECT_F clock = D2D1::RectF(b.left, b.top, b.right, b.top + 40.0f * d.scale);
        d.Text(TimeString(), d.fHuge, clock, d.text, 0.97f, DWRITE_TEXT_ALIGNMENT_LEADING);
        D2D1_RECT_F date = D2D1::RectF(b.left, b.top + 40.0f * d.scale, b.right, b.bottom);
        d.Text(DateStringLong(), d.fBody, date, d.muted, 0.8f, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (WeatherFresh()) {
            D2D1_RECT_F wr = D2D1::RectF(b.right - 130.0f * d.scale, b.top + 4.0f * d.scale, b.right, b.top + 30.0f * d.scale);
            d.Text(WeatherShort(), d.fTitle, wr, d.text, 0.92f, DWRITE_TEXT_ALIGNMENT_TRAILING,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            WeatherState w = WeatherSnapshot();
            if (!w.city.empty()) {
                D2D1_RECT_F cr = D2D1::RectF(b.right - 150.0f * d.scale, b.top + 30.0f * d.scale, b.right, b.top + 48.0f * d.scale);
                d.Text(w.city, d.fSmall, cr, d.muted, 0.6f, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
    };
    panel->Add(std::move(header));

    // Volume slider (real, interactive).
    auto vol = std::make_unique<Slider>();
    vol->label = L"Volume";
    vol->glyph = L"\xE767";  // Fluent: volume
    vol->glyphFmt = nullptr;  // set below via dc
    vol->get = []() { return g_volScalar.load(); };
    vol->set = [](float v) {
        g_volScalar = v;
        g_volMuted = false;
        g_volSuppressPollUntil = NowSeconds() + 0.4;
        g_volume.Set(v);
        g_layoutDirty = true;
    };
    panel->Add(std::move(vol));

    // Brightness slider (only if the island's monitor honors DDC/CI).
    if (g_brightPercent.load() >= 0) {
        auto bri = std::make_unique<Slider>();
        bri->label = L"Brightness";
        bri->glyph = L"\xE706";  // Fluent: brightness
        bri->get = []() { int p = g_brightPercent.load(); return p < 0 ? 0.0f : p / 100.0f; };
        bri->set = [](float v) {
            int pct = (int)(v * 100.0f + 0.5f);
            g_brightPercent = pct;
            g_brightSetRequest = pct;
            if (g_brightEvent) SetEvent(g_brightEvent);
            g_layoutDirty = true;
        };
        panel->Add(std::move(bri));
    }

    // Notifications section.
    auto notifs = NotificationsSnapshot();

    // Everything below the fixed controls lives in one scroll area:
    // calendar, agenda for the selected day, then the notification list.
    auto scroller = std::make_unique<ScrollContainer>();
    scroller->viewportMax = 430.0f;
    scroller->gap = 8.0f;

    if (MediaSnapshot().active) scroller->Add(std::make_unique<MediaCard>());
    scroller->Add(std::make_unique<CalendarView>());
    scroller->Add(std::make_unique<AgendaList>());

    auto secLabel = std::make_unique<Label>();
    secLabel->text = L"Notifications";
    secLabel->fmtMember = &DrawContext::fTitle;
    secLabel->height = 24.0f;
    scroller->Add(std::move(secLabel));

    if (!notifs.empty()) {
        auto clear = std::make_unique<Button>();
        clear->text = L"Clear all";
        clear->height = 26.0f;
#if ICC_HAS_NOTIFICATION_LISTENER
        clear->onClick = []() { ClearAllNotifications(); };
#endif
        scroller->Add(std::move(clear));
    }

    if (!g_notifSupported.load()) {
        auto l = std::make_unique<Label>();
        l->text = L"Notification access is off. Enable it in Windows Settings.";
        l->fmtMember = &DrawContext::fBody;
        l->brushMember = &DrawContext::muted;
        l->opacity = 0.6f;
        l->height = 50.0f;
        scroller->Add(std::move(l));
    } else if (notifs.empty()) {
        auto l = std::make_unique<Label>();
        l->text = L"No new notifications";
        l->fmtMember = &DrawContext::fBody;
        l->brushMember = &DrawContext::muted;
        l->opacity = 0.55f;
        l->height = 40.0f;
        scroller->Add(std::move(l));
    } else {
        for (auto& n : notifs) {
            auto row = std::make_unique<NotifRow>();
            row->id = n.id; row->app = n.app; row->title = n.title; row->body = n.body; row->aumid = n.aumid;
#if ICC_HAS_NOTIFICATION_LISTENER
            row->onDismiss = [](uint32_t id) { DismissNotification(id); };
#endif
            row->onActivate = [](const std::wstring& a) { LaunchByAumid(a); };
            scroller->Add(std::move(row));
        }
    }
    panel->Add(std::move(scroller));

    return panel;
}

// ---------------------------------------------------------------------------
// Globals tying WndProc to the Surface (all on render thread)
// ---------------------------------------------------------------------------
Surface* g_surface = nullptr;

// Both low-level hooks below live on the dedicated InputHookThreadProc, which does nothing
// but pump messages — so Windows can service them instantly. A low-level hook installed on
// the render thread (which spends most of its time painting or sleeping in WaitForSingleObject)
// would stall ALL system mouse input while the panel is up: the "cursor dragging through slime"
// bug. These procs must therefore stay trivial and return fast.
LRESULT CALLBACK LlMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_dismissActive.load() &&
        (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)) {
        auto* p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        RECT r;
        if (g_hwnd && GetWindowRect(g_hwnd, &r) && !PtInRect(&r, p->pt))
            PostMessageW(g_hwnd, WM_APP_DISMISS, 0, 0);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
// Just arm/disarm the (always-installed) mouse hook; no SetWindowsHookEx on the render thread.
void InstallDismissHook() { g_dismissActive = true; }
void RemoveDismissHook() { g_dismissActive = false; }

// Volume-key capture: swallow VK_VOLUME_* so the composition-rendered Win11 OSD never shows.
LRESULT CALLBACK LlKeyProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_captureVolKeys.load() && g_hwnd) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (k->vkCode == VK_VOLUME_UP || k->vkCode == VK_VOLUME_DOWN || k->vkCode == VK_VOLUME_MUTE) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
                PostMessageW(g_hwnd, WM_APP_VOLKEY, k->vkCode, 0);
            return 1;  // swallow down AND up so Windows never shows its native flyout
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
DWORD WINAPI InputHookThreadProc(void*) {
    g_inputHookTid = GetCurrentThreadId();
    HINSTANCE mod = GetModuleHandleW(nullptr);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LlKeyProc, mod, 0);
    g_dismissHook = SetWindowsHookExW(WH_MOUSE_LL, LlMouseProc, mod, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_keyHook) { UnhookWindowsHookEx(g_keyHook); g_keyHook = nullptr; }
    if (g_dismissHook) { UnhookWindowsHookEx(g_dismissHook); g_dismissHook = nullptr; }
    return 0;
}

D2D1_POINT_2F ClientPos(LPARAM lParam) {
    return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            AddClipboardFormatListener(hwnd);
            return 0;
        case WM_DISPLAYCHANGE:
            g_layoutDirty = true;
            g_brightRebind = true;
            if (g_brightEvent) SetEvent(g_brightEvent);
            return 0;
        case WM_CLIPBOARDUPDATE: {
            std::wstring preview;
            if (OpenClipboard(hwnd)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                    if (wchar_t* p = (wchar_t*)GlobalLock(h)) { preview = p; GlobalUnlock(h); }
                CloseClipboard();
            }
            for (auto& c : preview) if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
            if (preview.size() > 40) { preview.resize(40); preview += L"\x2026"; }
            ShowTransient(L"\xE8C8", preview.empty() ? L"Copied" : (L"Copied  " + preview));
            return 0;
        }
        case WM_DEVICECHANGE:
            if (wParam == 0x8000 || wParam == 0x8004)  // DBT_DEVICEARRIVAL / DBT_DEVICEREMOVECOMPLETE
                ShowTransient(L"\xE88E", wParam == 0x8000 ? L"Device connected" : L"Device disconnected");
            return 0;
        case WM_APP_DISMISS:
            if (g_surface) { g_surface->Dismiss(); g_layoutDirty = true; }
            return 0;
        case WM_APP_VOLKEY: {  // a captured hardware volume key -> drive volume + island popup
            float cur = 0.0f; bool mut = false;
            g_volume.Get(cur, mut);
            if (wParam == VK_VOLUME_MUTE) {
                bool nm = !mut;
                g_volume.SetMute(nm);
                g_volMuted = nm;
                ShowVolumeTransient((int)(cur * 100.0f + 0.5f), nm);
            } else {
                const float step = 0.02f;  // matches the native 2%-per-press step
                float nv = Clamp(cur + (wParam == VK_VOLUME_UP ? step : -step), 0.0f, 1.0f);
                g_volume.Set(nv);
                if (mut) g_volume.SetMute(false);  // Windows unmutes on volume up/down
                g_volScalar = nv; g_volMuted = false;
                ShowVolumeTransient((int)(nv * 100.0f + 0.5f), false);
            }
            g_volSuppressPollUntil = NowSeconds() + 0.4;  // avoid a duplicate popup from PollVolume
            g_layoutDirty = true;
            return 0;
        }
        case WM_MOUSEMOVE:
            // Do NOT treat bare mouse movement as a repaint trigger: the panel has no
            // hover-driven visuals, so moving over it must be free. A live slider drag keeps
            // rendering via IsCapturing(); scroll/press set their own dirty flags. This is the
            // fix for "open the panel, move the mouse, it gets laggy" (full 60fps repaints).
            if (g_surface) g_surface->OnMove({PointerPhase::Move, ClientPos(lParam)});
            return 0;
        case WM_LBUTTONDOWN:
            g_lastInputSec = NowSeconds();
            if (g_surface) {
                bool handled = g_surface->OnDown({PointerPhase::Down, ClientPos(lParam)});
                if (!handled && g_surface->state != SurfaceState::Open) g_surface->Open();
            }
            g_layoutDirty = true;
            return 0;
        case WM_LBUTTONUP:
            g_lastInputSec = NowSeconds();
            if (g_surface) g_surface->OnUp({PointerPhase::Up, ClientPos(lParam)});
            g_layoutDirty = true;
            return 0;
        case WM_MOUSEWHEEL:
            g_lastInputSec = NowSeconds();
            if (g_surface) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 36.0f;
                g_surface->OnWheel({PointerPhase::Wheel, D2D1::Point2F((float)pt.x, (float)pt.y), notches});
            }
            return 0;
        case WM_DESTROY: return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ===========================================================================
// Render thread
// ===========================================================================
void SetClickThrough(HWND hwnd, bool clickThrough);
void BindGlyphFormats(Surface& surface, IDWriteTextFormat* glyph);

DWORD WINAPI RenderThreadProc(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClass, L"Island Command Center", WS_POPUP, 0, 0, 360, 96,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Wh_Log(L"Window creation failed."); if (SUCCEEDED(hrCo)) CoUninitialize(); return 0; }
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    Renderer renderer;
    if (!renderer.Initialize(hwnd)) {
        Wh_Log(L"Renderer init failed.");
        DestroyWindow(hwnd); g_hwnd = nullptr;
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return 0;
    }

    Surface surface;
    g_surface = &surface;
    {
        DrawContext seed = renderer.MakeContext(1.0f, NowSeconds());
        surface.BuildFor(SurfaceState::Collapsed, seed);
    }

    SpringValue wSpring, hSpring;
    wSpring.Reset(180.0f);
    hSpring.Reset(40.0f);
    auto prevFrame = std::chrono::steady_clock::now();
    double nextVolPoll = 0.0;
    bool hiRes = false;                                   // 1ms timer resolution raised only while active
    auto nextDeadline = std::chrono::steady_clock::now();  // absolute 60fps pacing deadline
    float morphStartH = 40.0f;                             // panel height at the current morph's start

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP_NEW_EVENT) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const double now = NowSeconds();
        const Settings s = SettingsSnapshot();
        const UINT dpi = s.autoDpiScale ? GetDpiForWindow(hwnd) : 96;
        const float scale = (dpi / 96.0f) * s.sizeScale;

        if (now >= nextVolPoll) { PollVolume(); nextVolPoll = now + 0.15; }

        // Caps / Num lock toggles -> transient pill.
        {
            static int lastCaps = -1, lastNum = -1;
            int caps = (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
            int num = (GetKeyState(VK_NUMLOCK) & 1) ? 1 : 0;
            if (lastCaps >= 0 && caps != lastCaps) ShowTransient(L"\xE765", caps ? L"Caps Lock on" : L"Caps Lock off");
            if (lastNum >= 0 && num != lastNum) ShowTransient(L"\xE765", num ? L"Num Lock on" : L"Num Lock off");
            lastCaps = caps; lastNum = num;
        }

        DrawContext dc = renderer.MakeContext(scale, now);

        // The startup BuildFor(Collapsed, ...) used a placeholder scale (no real DPI yet); rebuild
        // at the real scale the first time they differ (and again on any later DPI/SizeScale change)
        // so sizes baked into the tree (e.g. the pill's fixedHeight) are never wrong.
        if (surface.RebuildIfScaleChanged(dc)) g_layoutDirty = true;

        // Hover state (geometric). Use the VISIBLE panel rect, not the window rect: since the window
        // is held at the (larger) stable size during a morph, keying hover off the window would make
        // the empty area where the expanded panel *would* be trigger the expand. g_pillRect is the
        // actual drawn panel from the last frame (screen coords).
        POINT cur; GetCursorPos(&cur);
        bool cursorInside = PtInRect(&g_pillRect, cur) != FALSE;
        surface.UpdateAmbientState(cursorInside, now);

        // Flush any pending slider drag with correct scale.
        surface.FlushPendingDrag(dc);

        // Apply queued state transitions (rebuild root) / in-place data refresh.
        bool stateChanged = surface.ApplyPendingState(dc);
        if (stateChanged) g_layoutDirty = true;
        bool dataChanged = surface.RebuildIfDataChanged(dc);  // e.g. notif list changed while Open
        if (dataChanged) g_layoutDirty = true;

        // Target-LOCK + content-fade clock. Re-measure the content size only on a real change
        // (state transition / data refresh) or when SETTLED, then FREEZE it for the whole morph —
        // per-frame re-measure (dynamic Expanded height, Open's MaxOpenHeight clamp) is what
        // re-aimed the spring mid-flight and made the size non-monotonic/choppy. On a state
        // transition, record the start height so content fades in as the panel grows (otherwise the
        // new state's big content would flash clipped inside the still-small pill — the "hitch").
        bool settledNow = wSpring.AtRest() && hSpring.AtRest();
        if (stateChanged || dataChanged || settledNow) {
            D2D1_SIZE_F m = surface.MeasureContent(dc);
            if (stateChanged)      morphStartH = hSpring.value;  // fade content in over the grow
            else if (dataChanged)  morphStartH = m.height;       // resize only, no content fade
            surface.SetLocked(m);
        }
        wSpring.target = surface.LockedW();
        hSpring.target = surface.LockedH();

        auto frame = std::chrono::steady_clock::now();
        float dt = Clamp(std::chrono::duration<float>(frame - prevFrame).count(), 0.001f, 0.033f);
        prevFrame = frame;
        // A morph begins right after an idle sleep, so this first dt spans the whole idle gap
        // (~100ms). Feeding that to a stiff spring makes it lurch most of the way in ONE step — an
        // instant, hitchy morph. Begin every transition with a normal 60fps step instead.
        if (stateChanged) dt = 1.0f / 60.0f;
        // Slightly over-damped (zeta ~1.03): a smooth, strictly monotonic glide, no bounce.
        wSpring.Step(dt, 320.0f, 37.0f);
        hSpring.Step(dt, 320.0f, 37.0f);

        // Content-fade progress: 0 at the panel's morph-start size -> 1 at its locked target. Kept
        // low early so the incoming content isn't drawn (clipped) while the panel is still small.
        float span = surface.LockedH() - morphStartH;
        float t01 = (std::fabs(span) < 1.0f) ? 1.0f
                    : Clamp((hSpring.value - morphStartH) / span, 0.0f, 1.0f);
        float tb = Clamp((t01 - 0.28f) / 0.72f, 0.0f, 1.0f);
        float contentAlpha = tb * tb * (3.0f - 2.0f * tb);  // smoothstep, delayed

        // Ease the displayed volume toward the real level so the popup bar animates (not static).
        static float volDisplay = -1.0f;
        float volTarget = g_volScalar.load();
        if (volDisplay < 0.0f) volDisplay = volTarget;
        volDisplay += (volTarget - volDisplay) * std::min(1.0f, dt * 14.0f);
        if (std::fabs(volTarget - volDisplay) < 0.002f) volDisplay = volTarget;
        g_volDisplayScalar = volDisplay;
        bool volAnimating = (volDisplay != volTarget);

        bool widgetAnimating = surface.Tick(dt);
        bool sizeAnimating = !wSpring.AtRest() || !hSpring.AtRest();

        int cw = std::max(1, (int)std::ceil(wSpring.value));
        int ch = std::max(1, (int)std::ceil(hSpring.value));

        MediaState mediaNow = MediaSnapshot();
        bool mediaLive = surface.state == SurfaceState::Open && mediaNow.active && mediaNow.playing;
        g_waveWanted = mediaLive;  // let the audio-meter thread idle when the waveform is hidden
        bool privacyLive = g_micActive.load() || g_camActive.load();
        bool inputActive = (now - g_lastInputSec.load()) < 0.5;  // hover/drag responsiveness window

        // A frame costs a full UpdateLayeredWindow re-composite of the (possibly large) panel
        // through DWM. Do it only when something actually changed — resting the cursor over the
        // Open panel must NOT drive continuous 60fps compositing (that spikes the whole desktop).
        static bool lastCursorInside = false;
        static int  lastClockKey = -1;
        static bool firstFrame = true;
        const int clockKey = ClockMinuteKey();
        bool active = widgetAnimating || sizeAnimating || surface.IsCapturing() ||
                      mediaLive || privacyLive || inputActive || volAnimating;
        bool needsRender = firstFrame || active || g_layoutDirty.load() ||
                           clockKey != lastClockKey || cursorInside != lastCursorInside;
        lastCursorInside = cursorInside;
        lastClockKey = clockKey;

        if (needsRender) {
            D2D1_RECT_F inner;
            int tgtW = (int)std::ceil(wSpring.target), tgtH = (int)std::ceil(hSpring.target);
            bool settled = wSpring.AtRest() && hSpring.AtRest();
            if (renderer.BeginFrame(s, cw, ch, tgtW, tgtH, settled, scale, inner)) {
                DrawContext pdc = renderer.MakeContext(scale, now);  // brushes valid post-BindDC
                pdc.artBmp = renderer.ArtBitmap(mediaNow.art.gen, mediaNow.art.w, mediaNow.art.h, mediaNow.art.bgra);
                BindGlyphFormats(surface, renderer.GlyphFormat());
                // The gliding panel: animating size, TOP-anchored and horizontally centered inside
                // the fixed window (`inner`). Only this rounded rect (and the content reveal) move;
                // the window and the content's layout origin stay put -> no clock jitter.
                float aw = std::min((float)cw, W(inner)), ah = std::min((float)ch, H(inner));
                float panelLeft = std::floor((inner.left + inner.right - aw) * 0.5f + 0.5f);
                D2D1_RECT_F panelR = D2D1::RectF(panelLeft, inner.top, panelLeft + aw, inner.top + ah);
                // Publish the visible panel's screen rect for next frame's hover test.
                { RECT wr; GetWindowRect(hwnd, &wr);
                  g_pillRect = { wr.left + (LONG)panelR.left, wr.top + (LONG)panelR.top,
                                 wr.left + (LONG)panelR.right, wr.top + (LONG)panelR.bottom }; }
                {
                    float pr = std::min(H(panelR) * 0.5f, 22.0f * scale);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(panelR, pr, pr);
                    pdc.dc->FillRoundedRectangle(rr, pdc.panel);
                    pdc.dc->DrawRoundedRectangle(rr, pdc.border, 1.0f);
                }
                surface.Layout(pdc, inner);
                // Content is clipped to the animating panel and (during a morph) faded in through an
                // opacity LAYER, so the incoming content never shows at full alpha while clipped in
                // the small pill; at rest a plain clip is used (cheaper).
                ID2D1Layer* clyr = contentAlpha < 0.995f ? renderer.EnsureContentLayer() : nullptr;
                if (clyr) {
                    pdc.dc->PushLayer(D2D1::LayerParameters(panelR, nullptr,
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(),
                        contentAlpha), clyr);
                    surface.Paint(pdc);
                    pdc.dc->PopLayer();
                } else {
                    pdc.dc->PushAxisAlignedClip(panelR, D2D1_ANTIALIAS_MODE_ALIASED);
                    surface.Paint(pdc);
                    pdc.dc->PopAxisAlignedClip();
                }
                // Privacy dots (camera = green, mic = orange), pulsing, centered at the top of the panel.
                {
                    bool cam = g_camActive.load(), mic = g_micActive.load();
                    int count = (cam ? 1 : 0) + (mic ? 1 : 0);
                    if (count > 0) {
                        float pr = 2.5f * scale, gap = 11.0f * scale;
                        float cxD = (panelR.left + panelR.right) * 0.5f;
                        float py = panelR.top + 5.5f * scale;
                        float px = cxD - (count - 1) * gap * 0.5f;
                        float pulse = 0.55f + 0.45f * sinf((float)now * 4.0f);
                        if (cam) {
                            ComPtr<ID2D1SolidColorBrush> bb;
                            pdc.dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.85f, 0.35f, pulse), &bb);
                            if (bb) pdc.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), pr, pr), bb.Get());
                            px += gap;
                        }
                        if (mic) {
                            ComPtr<ID2D1SolidColorBrush> bb;
                            pdc.dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.6f, 0.1f, pulse), &bb);
                            if (bb) pdc.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), pr, pr), bb.Get());
                        }
                    }
                }
                renderer.EndFrame(s);
                firstFrame = false;
            }
        }

        // Click-through policy.
        bool wantInteractive = (surface.state == SurfaceState::Open) || cursorInside || surface.IsCapturing();
        SetClickThrough(hwnd, !wantInteractive);

        // Frame pacing. While active, hold true 60fps with a 1ms timer resolution and an ABSOLUTE
        // deadline (paint time is absorbed into the budget, not added on top) — this removes the
        // ~15.6ms quantization / duplicate frames that made the morph steppy. Idle -> drop the
        // resolution (power) and poll slowly for hover/clock.
        if (active) {
            if (!hiRes) { timeBeginPeriod(1); hiRes = true; }
            nextDeadline += std::chrono::microseconds(16667);
            auto nowc = std::chrono::steady_clock::now();
            double waitMs = std::chrono::duration<double, std::milli>(nextDeadline - nowc).count();
            if (waitMs < 0.0) { nextDeadline = nowc; waitMs = 0.0; }  // fell behind -> rebase, no spiral
            WaitForSingleObject(g_stopEvent, (DWORD)(waitMs + 0.5));
        } else {
            if (hiRes) { timeEndPeriod(1); hiRes = false; }
            nextDeadline = std::chrono::steady_clock::now();
            WaitForSingleObject(g_stopEvent, 100);
        }
    }

    if (hiRes) timeEndPeriod(1);
    g_surface = nullptr;
    RemoveDismissHook();
    renderer.Shutdown();
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

}  // namespace

// (Free helpers referenced above need to see Surface/Slider — defined after.)
namespace {
void SetClickThrough(HWND hwnd, bool clickThrough) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR want = clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (want != ex) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);
}
void BindGlyphFormats(Surface& surface, IDWriteTextFormat* glyph) {
    surface.ForEachSlider([glyph](Slider* sl) { sl->glyphFmt = glyph; });
}
}  // namespace

// ---------------------------------------------------------------------------
// Thread lifecycle + tool-mod harness
// ---------------------------------------------------------------------------
namespace {
bool StartThreads() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) return false;
    g_brightEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);     // auto-reset
    g_calRefreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    g_renderThread = CreateThread(nullptr, 0, RenderThreadProc, nullptr, 0, nullptr);
    g_brightThread = CreateThread(nullptr, 0, BrightnessThreadProc, nullptr, 0, nullptr);
    g_calThread = CreateThread(nullptr, 0, CalendarThreadProc, nullptr, 0, nullptr);
    g_mediaThread = CreateThread(nullptr, 0, MediaThreadProc, nullptr, 0, nullptr);
    g_audioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    g_privacyThread = CreateThread(nullptr, 0, PrivacyThreadProc, nullptr, 0, nullptr);
    g_weatherThread = CreateThread(nullptr, 0, WeatherThreadProc, nullptr, 0, nullptr);
    g_inputHookThread = CreateThread(nullptr, 0, InputHookThreadProc, nullptr, 0, nullptr);
#if ICC_HAS_NOTIFICATION_LISTENER
    g_notifThread = CreateThread(nullptr, 0, NotificationThreadProc, nullptr, 0, nullptr);
#endif
    return g_renderThread != nullptr;
}
void StopThreads() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_brightEvent) SetEvent(g_brightEvent);
    if (g_calRefreshEvent) SetEvent(g_calRefreshEvent);
    if (g_inputHookTid) PostThreadMessageW(g_inputHookTid, WM_QUIT, 0, 0);  // unblock GetMessage
    HANDLE handles[] = {g_renderThread, g_notifThread, g_brightThread, g_calThread, g_mediaThread, g_audioThread, g_privacyThread, g_weatherThread, g_inputHookThread};
    for (HANDLE h : handles) if (h) WaitForSingleObject(h, 4000);
    for (HANDLE* h : {&g_renderThread, &g_notifThread, &g_brightThread, &g_calThread, &g_mediaThread, &g_audioThread, &g_privacyThread, &g_weatherThread, &g_inputHookThread})
        if (*h) { CloseHandle(*h); *h = nullptr; }
    g_inputHookTid = 0;
    if (g_brightEvent) { CloseHandle(g_brightEvent); g_brightEvent = nullptr; }
    if (g_calRefreshEvent) { CloseHandle(g_calRefreshEvent); g_calRefreshEvent = nullptr; }
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
}
}  // namespace

// Standard Windhawk mod entry points. The mod is injected (by @include) into
// the dedicated island-host.exe process, where it runs the overlay directly.
BOOL Wh_ModInit() {
    LoadSettings();
    if (!StartThreads()) { StopThreads(); return FALSE; }
    g_layoutDirty = true;
    Wh_Log(L"Island Command Center initialized.");
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    g_layoutDirty = true;
    g_brightRebind = true;
    if (g_brightEvent) SetEvent(g_brightEvent);
    if (g_calRefreshEvent) SetEvent(g_calRefreshEvent);  // re-fetch with any new ICS URL
}

void Wh_ModUninit() { StopThreads(); Wh_Log(L"Island Command Center unloaded."); }
