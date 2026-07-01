# Island Command Center

A from-scratch, **interactive** Windhawk overlay for Windows 11 — an island-styled,
unified **control + notification center** inspired by the Dynamic Island design
language, but with genuinely functional widgets (not just a pretty display).

It draws a layered pill at the top of your monitor that morphs between three states:

- **Collapsed pill** — large centered clock + live weather (☀ 67°), with transient
  pills for volume/brightness changes, clipboard, USB/Bluetooth devices, and
  caps/num lock, plus pulsing **mic/camera privacy dots** when in use.
- **Hover peek** — big clock, date, weather.
- **Control center** (click) — a pinned panel with:
  - **Now playing** — album art, transport, draggable scrub bar, live audio waveform.
  - **Volume** and **Brightness** sliders (instant; brightness via DDC/CI on the
    island's monitor, auto-hidden if the monitor doesn't support it).
  - **Functional calendar** — month navigation, today, day selection, and a
    selected‑day **agenda from your Outlook published ICS feed** (real events,
    timezones + recurrence handled).
  - **Notification center** — full list, per‑item dismiss, clear‑all, click to
    launch the source app.

## Architecture

Direct2D layered window (per‑pixel alpha via `UpdateLayeredWindow`), a retained
widget tree (Panel / ScrollContainer / Slider / Button / NotifRow / MediaCard /
CalendarView / AgendaList) with absolute hit‑rects, click‑through‑as‑policy (the
collapsed pill never steals focus; the panel goes interactive on hover/open),
`SetCapture` slider dragging, a `WH_MOUSE_LL` click‑outside dismiss, and a
spring‑based size morph. Worker threads: render, media (GSMTC), audio meter
(`IAudioMeterInformation`), notifications (`UserNotificationListener`), brightness
(DDC/CI), calendar (ICS over WinHTTP + a native iCalendar parser), weather
(wttr.in), and mic/cam privacy (registry probe).

### Dedicated host process

Windhawk refuses to inject a hand‑installed mod into `windhawk.exe` itself, so
instead of the usual dedicated‑process ("tool mod", `@include windhawk.exe`)
pattern, this mod targets its **own** host: **`island-host.exe`** (see
`island-host.cpp`) via `@include island-host.exe`. Windhawk injects the mod into
the host exactly as it injects Explorer‑targeted mods, giving dedicated‑process
isolation (out of Explorer) while remaining installable without the Windhawk GUI.

## Build

Uses Windhawk's bundled Clang (mingw‑w64, C++23). With `%LOCALAPPDATA%\Windhawk`
as `$WH`:

```sh
# The mod DLL
clang++ --config "$WH/Compiler/bin/x86_64-w64-windows-gnu.cfg" @flags.rsp \
  -I"$WH/Compiler/include" -include whmodid.h -include windhawk_api.h \
  island-command-center.wh.cpp "$WH/Engine/1.7.3/64/windhawk.lib" \
  -lole32 -loleaut32 -lshcore -ld2d1 -ldwrite -ldwmapi -lgdi32 -luser32 -lshell32 \
  -lruntimeobject -lwindowscodecs -lavrt -lsetupapi -lwinhttp -lpdh -ldxva2 -ladvapi32 \
  -shared -Wl,--export-all-symbols -o island-command-center_1.0.0_100000.dll

# The host exe
clang++ --config "$WH/Compiler/bin/x86_64-w64-windows-gnu.cfg" -std=c++23 \
  -target x86_64-w64-mingw32 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -municode -mwindows -static island-host.cpp -o island-host.exe
```

`flags.rsp` = Windhawk's `compile_flags.txt` minus `-x c++` / `-DWH_EDITING`, plus `-DWH_MOD`.

## Install (headless)

1. Place `island-host.exe` somewhere persistent (e.g.
   `%LOCALAPPDATA%\IslandCommandCenter\`) and add a Startup‑folder shortcut to it.
2. Drop the compiled DLL in `%LOCALAPPDATA%\Windhawk\AppData\Engine\Mods\64\`.
3. Write `…\Engine\Mods\island-command-center.ini` (UTF‑16LE) with `[Mod]`
   (`LibraryFileName`, `Include=island-host.exe`, `Architecture=x86-64`,
   `Version`, `SettingsChangeTime`) + `[Settings]`.
4. Add the mod id to `…\AppData\userprofile.json` `mods`.
5. `windhawk.exe -restart -tray-only`, then launch `island-host.exe`.

## Settings

`CalendarIcsUrl` (your Outlook published `.ics` — kept out of source/repo),
`WeatherCity`, `WeatherFahrenheit`, `TargetMonitor`, `SizeScale`, `OffsetX/Y`,
`Position`, `PillOpacity`, `BrightnessEnabled`, `CalendarRefreshMinutes`.

## License

MIT — see `LICENSE`.
