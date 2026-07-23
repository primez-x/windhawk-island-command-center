# Island Command Center

> **This repo has moved.** Active development now happens on the
> [`island-command-center`](https://github.com/primez-x/windhawk-mods/tree/island-command-center),
> [`island-host-watchdog`](https://github.com/primez-x/windhawk-mods/tree/island-host-watchdog), and
> [`island-taskbar`](https://github.com/primez-x/windhawk-mods/tree/island-taskbar) branches of
> [primez-x/windhawk-mods](https://github.com/primez-x/windhawk-mods), a fork of the official Windhawk
> mods collection. This repo is kept for history and will be archived.

A from-scratch, **interactive** Windhawk overlay for Windows 11 — an island-styled,
unified **control + notification center** inspired by the Dynamic Island design
language, but with genuinely functional widgets (not just a pretty display) and
a **real, variable-strength blur-behind** material (not a tint-only fake).

It draws a layered pill at the top of your monitor that morphs between three states:

- **Collapsed pill** — large centered clock + live weather (☀ 67°), a mini
  **volume element** (a slim vertical fill track that doubles as the divider:
  drag to set, click to mute — the fill dims while muted — scroll-wheel to
  step ±2%), and — whenever something's playing — circular album art on the
  left edge and a live waveform on the right, plus transient pills for brightness changes,
  clipboard, USB/Bluetooth devices, and caps/num lock, and pulsing **mic/camera
  privacy dots** when in use. **Middle-click anywhere on the island to hide it**
  when it's in the way — it fades out smoothly (panel, blur, and content all
  together) and fades back in the instant your cursor leaves the area it would
  normally occupy, or after 15 seconds, whichever comes first.
- **Hover peek** — big clock, date, weather detail, a live volume readout, a
  compact now-playing row (circular art + title/artist) when media is active,
  and a mini "Up next" agenda with a live "in X hr X min" countdown on the next event. Scroll-wheel here adjusts volume too.
- **Control center** (click) — a pinned panel with:
  - **Now playing** — circular album art, friendly source name (Spotify/Edge/
    etc.), centered transport buttons with hover highlights, draggable scrub
    bar with elapsed/remaining time, and a live audio waveform beside the
    title (five rounded bars mirrored around center, smoothly eased).
  - **Volume** and **Brightness** sliders (instant; brightness via DDC/CI on the
    island's monitor, auto-hidden if the monitor doesn't support it).
  - **Functional calendar** — month navigation, today, day selection, and a
    selected‑day **agenda from your Outlook published ICS feed** (real events,
    timezones + recurrence handled).
  - **Notification center** — full list, per‑item dismiss, clear‑all, click to
    launch the source app.

Media (album art, transport, the "now playing" glyph in every state) is
sourced from Windows' System Media Transport Controls, so it works with any
GSMTC-integrated app — Spotify, Edge/Chrome media, the native Media Player,
etc. — with no per-app code; when more than one app has a session (e.g. a
paused browser tab alongside Spotify), the island always shows/controls
whichever one is actually **playing**.

## Real blur-behind

Every backdrop style except `None` genuinely blurs whatever is behind the
island — desktop, other windows, video — through a companion
`Windows.UI.Composition` window kept glued directly beneath the overlay in
z-order (see [Architecture](#architecture)). `BackdropStyle` is an **inline
style string**, using the same `$Name` + `Key=Value` convention as the
[Windows 11 Taskbar Styler](https://windhawk.net) mod:

```
$Acrylic                                    # default: accent-tinted acrylic
$Glass TintColor=#3388CC TintOpacity=0.5    # custom blue glass
$Frosted TintColor=accent FallbackColor=#1A1A1A
BlurAmount=22 TintSaturation=1.1            # fully custom, no named base
None                                        # no backdrop at all (solid panel)
```

`$Name` (leading `$` optional) selects the blur radius + saturation profile —
`Translucent` (15px), `Glass` (5px), `Frosted` (20px), or `Acrylic` (30px +
extra saturation) — matching the Taskbar Styler's WindowGlass theme radii.
Tint defaults to `TintColor=theme`/`FallbackColor=theme` — a neutral shade
that follows the `Theme` setting below (see [Theming](#theming)) — rather
than a fixed hue; `accent` (the live Windows accent color) and hex colors
remain available as explicit overrides. `FallbackColor`/`FallbackOpacity`
control the denser solid tint used when composition blur isn't available
(transparency effects off, or init failed) — the panel degrades gracefully
rather than disappearing. `FocusOpacity` sets how much denser the tint gets
while hovered/open (default: `TintOpacity` + 0.25, capped at 0.92) — see
[Theming](#theming). The default (`$Frosted`) matches the
[Windows 11 Taskbar Styler](https://windhawk.net) mod's own WindowGlass blur
amount, so the two read as one cohesive material rather than two different
surfaces stacked on the same desktop.

## Theming

`Theme` picks how the foreground palette (text/divider/border/track) and the
`theme` tint keyword resolve, independent of what's actually behind the
island:

- **`auto`** (default) — follows Windows' own light/dark "choose your mode"
  setting.
- **`dark`** / **`light`** — force a side regardless of Windows or the
  background.
- **`adaptive`** — the original behavior: samples the desktop behind the
  panel (thin strips just *outside* it, never the panel itself, which would
  feed back its own text) and flips to match. A candidate flip must hold for
  **~3 continuous seconds** before it's accepted, so briefly sliding a window
  across the pill can no longer flicker the text — and every mode (not just
  adaptive) now fades over a deliberately slow **~1.2s**, not the old ~250ms
  snap.

Detection (`auto`) and the `theme` tint keyword both go through
`Windows.UI.ViewManagement.UISettings` — the *exact same* mechanism the
Taskbar Styler mod uses internally for its own `{ThemeResource ...}` colors —
rather than a registry poll, and the `theme` tint's live-side color is the
actual system chrome/background color (not a made-up neutral), so the two
mods land on matching colors rather than each guessing independently.

Independently, the pill is **more transparent at rest and denser while
hovered/open** (`TintOpacity` vs `FocusOpacity` in `BackdropStyle`, eased over
~150ms) — so it stays unobtrusive sitting on the desktop but easy to read
once you're actually looking at it.

## Architecture

Direct2D layered window (per‑pixel alpha via `UpdateLayeredWindow`), a retained
widget tree (Panel / ScrollContainer / Slider / Button / NotifRow / MediaCard /
CalendarView / AgendaList) with absolute hit‑rects, click‑through‑as‑policy (the
collapsed pill never steals focus; the panel goes interactive on hover/open),
`SetCapture` slider dragging, a `WH_MOUSE_LL` click‑outside dismiss, and a
spring‑based size morph (target-locked, content-fade-in, frame-paced) so the
collapse/expand glide is smooth with no clock-text jitter.

Real blur-behind is a **second window** — `WS_EX_NOREDIRECTIONBITMAP`,
input-transparent — kept exactly beneath the overlay in z-order and resized to
match it every frame. It hosts a `Windows.UI.Composition` `SpriteVisual` whose
`CompositionBackdropBrush` (the live desktop content behind it) is run through
a hand-built `IGraphicsEffectD2D1Interop` Gaussian-blur graph at the style's
radius, clipped to the animating rounded panel rect so the blur hugs the pill
through the morph. (The documented `HostBackdropBrush` +
`DWMWA_USE_HOSTBACKDROPBRUSH` route was tried first and empirically only
delivers a wallpaper-layer snapshot on build 26200 — app windows behind never
appear — so the plain backdrop brush is the correct source here.) The tint
itself stays in the Direct2D panel fill, drawn on top of the blur.

Worker threads: render (owns both windows above), media (GSMTC), audio meter
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

## Companion mod: Island Taskbar Hider

`island-taskbar.wh.cpp` is a deliberately minimal, separate mod (`@include
explorer.exe`) that hides the native taskbar elements the island duplicates —
clock/date (via the `ShowClock` registry setting), the notification/Control
Center bell, and the battery/volume/network tray icons — so the island can own
those surfaces without visual duplication. It contains no Direct2D, worker
threads, or networking; everything happens on the taskbar's own UI thread, which
keeps it safe to inject directly into `explorer.exe`. Don't run it alongside the
*Taskbar tray system icon tweaks* mod — both hook the same `SystemTray.IconView`
constructor.

## Build

Uses Windhawk's bundled Clang (mingw‑w64, C++23). With `%LOCALAPPDATA%\Windhawk`
as `$WH`:

```sh
# The overlay mod DLL
clang++ --config "$WH/Compiler/bin/x86_64-w64-windows-gnu.cfg" @flags.rsp \
  -I"$WH/Compiler/include" -include whmodid.h -include windhawk_api.h \
  island-command-center.wh.cpp "$WH/Engine/1.7.3/64/windhawk.lib" \
  -lole32 -loleaut32 -lshcore -ld2d1 -ldwrite -ldwmapi -lgdi32 -luser32 -lshell32 \
  -lruntimeobject -lwindowscodecs -lavrt -lsetupapi -lwinhttp -lpdh -ldxva2 -ladvapi32 \
  -lwinmm -lcoremessaging -luuid \
  -shared -Wl,--export-all-symbols -o island-command-center_1.0.0_100000.dll

# The host exe
clang++ --config "$WH/Compiler/bin/x86_64-w64-windows-gnu.cfg" -std=c++23 \
  -target x86_64-w64-mingw32 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -municode -mwindows -static island-host.cpp -o island-host.exe

# The taskbar-hider companion mod DLL (optional)
clang++ --config "$WH/Compiler/bin/x86_64-w64-windows-gnu.cfg" @flags.rsp \
  -I"$WH/Compiler/include" -include whmodid-taskbar.h -include windhawk_api.h \
  island-taskbar.wh.cpp "$WH/Engine/1.7.3/64/windhawk.lib" \
  -lole32 -loleaut32 -lruntimeobject -lversion \
  -shared -Wl,--export-all-symbols -o island-taskbar_1.0.0_100000.dll
```

`flags.rsp` = Windhawk's `compile_flags.txt` minus `-x c++` / `-DWH_EDITING`, plus `-DWH_MOD`.

## Install (headless)

**Overlay mod:**

1. Place `island-host.exe` somewhere persistent (e.g.
   `%LOCALAPPDATA%\IslandCommandCenter\`) and add a Startup‑folder shortcut to it.
2. Drop the compiled DLL in `%LOCALAPPDATA%\Windhawk\AppData\Engine\Mods\64\`.
3. Write `…\Engine\Mods\island-command-center.ini` (UTF‑16LE) with `[Mod]`
   (`LibraryFileName`, `Include=island-host.exe`, `Architecture=x86-64`,
   `Version`, `SettingsChangeTime`) + `[Settings]` (defaults required — a
   missing key reads as 0/empty).
4. Add the mod id to `…\AppData\userprofile.json` `mods`.
5. `windhawk.exe -restart -tray-only`, then launch `island-host.exe`.

To pick up a new build without dropping the taskbar (a `-restart` re-injects
*every* installed mod, which can transiently crash Explorer): kill
`island-host.exe`, overwrite the deployed DLL's bytes **in place** (keep the
filename the engine already knows), then relaunch `island-host.exe` — no
`-restart` needed.

**Taskbar-hider companion (optional):** same `[Mod]`/`[Settings]` ini pattern
with `Include=explorer.exe`, then `windhawk.exe -restart -tray-only` (this one
*does* need a restart, since it's a new mod the engine has to learn about) and
restart Explorer if it drops during that restart.

## Settings

- `Position` — `top-center` / `top-left` / `top-right` / `bottom-center`
- `TargetMonitor` — 0 = primary, 1-8 = that monitor, -1 = follow the mouse
- `OffsetX` / `OffsetY` — pixel offset from the anchor position
- `SizeScale` — overall scale of the island (e.g. 0.9, 1.0, 1.25)
- `AutoDpiScale` — scale with the monitor's DPI in addition to `SizeScale`
- `AlwaysOnTop`
- `PillOpacity` — 0.35–1.0, fades the whole island; independent of `BackdropStyle`
- `Theme` — `auto` (default) / `dark` / `light` / `adaptive`; see [Theming](#theming)
- `BackdropStyle` — inline style string; see [Real blur-behind](#real-blur-behind) and [Theming](#theming)
- `CalendarIcsUrl` — your Outlook published `.ics` feed URL (kept out of source/repo)
- `CalendarRefreshMinutes`
- `BrightnessEnabled`
- `WeatherCity` — blank to auto-detect by IP
- `WeatherFahrenheit`
- `CaptureVolumeKeys` — intercept hardware volume keys so the island shows its
  own popup instead of the native Windows OSD

## License

MIT — see `LICENSE`.
