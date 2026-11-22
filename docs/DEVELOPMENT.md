# DCS ILARTS - development notes

Build, internals and tests. The user documentation is [README.md](../README.md) and `deploy/README.txt`.

## The original diagnostic DLL (read-only first pass)

This is a small x64 Windows DLL intended for a controlled diagnostic session in DCS.
It does **not** hook Direct3D, patch DCS, invoke undocumented C++ methods, copy textures,
or modify renderer state. It records loaded module paths, selected exported symbols,
and the current values of three exported renderer globals.

## Why this first pass is conservative

The DCS exports we have identified include C++ instance methods such as
`DX11Renderer::getRenderTarget(const char*)`. Their presence in the PE export table
is not enough to call them safely: they require a valid renderer object (`this`),
correct C++ ABI details, and an understanding of the engine's lifetime/thread rules.
This DLL deliberately records their addresses but does not invoke them.

## Build requirements

VS Code is an editor; it does not include Microsoft's C++ compiler or Windows SDK.
Install **Visual Studio 2022 Build Tools** and select:

- Desktop development with C++
- MSVC v143 x64/x86 build tools (x64 is required here)
- Windows 10 or Windows 11 SDK
- CMake tools for Windows (optional; CMake is also available separately)

Then install the VS Code extensions **C/C++** and **CMake Tools** (optional but convenient).

## Build

Open this folder in VS Code. Open a terminal configured with the *x64 Native Tools*
environment, or use the Developer PowerShell for VS 2022 Build Tools.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output:

```text
build\Release\ILARTS.dll
```

ILARTS.exe's icon: `viewer/ILARTS.rc` embeds `assets/ILARTS-light.ico` (id 1: the file's icon in Explorer, the
shortcuts and the installer) and `assets/ILARTS-dark.ico` (id 2). At run time the window and taskbar icon contrast
with the Windows theme: light artwork on a dark taskbar, dark on a light one. The `.ico` files are committed;
after changing the artwork in `assets/` (PNGs at 24-256 px, SVG source), rebuild them with
`python tools/make_icon.py` (needs Inkscape for the 16 and 48 px images).

**Version.** A release build is configured with its version, numbers and dots (quoted in PowerShell, which
otherwise splits the argument at the first dot):

```powershell
cmake -S . -B build -A x64 "-DDCS_ILARTS_VERSION=1.0.27"
```

Without it the version is 0.0.0, a development build, and SETTINGS > Installation > Version shows DEVELOPMENT.
The option is not called `ILARTS_VERSION` because `project(ILARTS)` sets that variable itself (empty). CMake writes
`<build>/generated/ilarts_version.h` from `src/version.h.in` (`ILARTS_VERSION`, `ILARTS_VERSION_W`,
`ILARTS_VERSION_BIN`). It is used by:
- the DLL's version string (`diag.version`, which the hook writes to dcs.log): `ILARTS 1.0.27 (built <date>, verified against DCS ...)`;
- the viewer: SETTINGS > Installation > Version, a read-only `info` field (`kSettings` in `viewer/console.inc`, shown as an LCD by the page);
- the VERSIONINFO resource of ILARTS.dll, ILARTS.exe and ILARTSCam.dll (`src/version.rc`, included by `src/ILARTS.rc`, `viewer/ILARTS.rc`, `vcam/ilarts_cam.rc`): Explorer's Properties > Details.

`deploy/package.ps1` reads the three files' product version and refuses to package when it isn't its `-Version`,
so a stale or development build can't be shipped by mistake. The Build workflow passes the tag's version.

## Run / loading

The DLL exports two entry points (x64, undecorated names):

- `RunLSODiagnostic(void*)` - runs one pass synchronously on the calling thread.
- `StartLSODiagnostic(void*)` - runs one pass on a worker thread and returns immediately.
  The worker holds its own module reference, so the host may `FreeLibrary` at any time.

Both return `ERROR_SUCCESS`, `ERROR_BUSY` (a pass is already running) or `ERROR_OPEN_FAILED`.
`DllMain` does nothing; loading the DLL alone runs no code.

### Loading inside DCS (Lua C module via a GUI hook)

The DLL also exports `luaopen_ILARTS`, so DCS can load it with `require`, the same way
Tacview loads its DLL. Install / uninstall (DCS 2.9.31.29434 only; single player, multiplayer host
or multiplayer client):

```powershell
powershell -ExecutionPolicy Bypass -File deploy\install.ps1
powershell -ExecutionPolicy Bypass -File deploy\install.ps1 -Uninstall
```

The hook reads the carrier, aircraft and wind from the mission scripting state with
`net.dostring_in`, which DCS only allows for the states listed in
`Saved Games\DCS\Config\autoexec.cfg`. That file needs (merged with any lines other mods added):

```lua
net.allow_unsafe_api = {"userhooks"}
net.allow_dostring_in = {"mission", "server"}
```

The installer warns when these are missing; it does not edit the file. As a multiplayer client
there is no mission scripting state; the hook then uses the export interface
(`LoGetWorldObjects`), so the server must allow world object export, and takes the deck wind from
the mission's ground wind.

The PLAT starts with the mission without opening the LSO view (a synthetic copy of DCS's PLAT
camera, `src/plat_cam.h`), on the carrier nearest your camera. Choose another carrier in the
viewer's CARRIER list or with N / Shift+N; visiting an LSO station selects that station's carrier.
Until DCS's own PLAT overlay exists (first station visit) the viewer draws the overlay text itself, and
it draws the LSO station screen (feed LSO 1) itself as well, from DCS's own layout, atlas and fonts.

This copies the DLL to `Saved Games\DCS\Mods\tech\ILARTS\bin\` and the hook to
`Saved Games\DCS\Scripts\Hooks\ILARTS-hook.lua` (and removes an install under the old name, LSODiagnostic). In a single-player mission the hook
runs the read-only pass, then probes `LSOFRAME`, `LSOFINALTARGET`, `LSOSCREEN2`, `TACTICALMAP`,
`mfd0` every 15 s and saves a texture-manager listing (`resources-*.txt`) whenever the result
changes. These are the only engine calls (`isRenderTargetExists`, `getResourcesList`,
`freeStringMem`); see `src/probe.cpp` for the guards. `getResourcesList` also makes DCS write
`shader_dump.txt` into Saved Games.

Do not rename this DLL to `dxgi.dll`, `dinput8.dll` or another system/DCS DLL.

### Offline test host

```powershell
build\Release\lsodiag_host.exe build\Release\ILARTS.dll
build\Release\lsodiag_host.exe build\Release\ILARTS.dll --map "C:\Program Files\Eagle Dynamics\DCS World\bin"
```

`--map` loads the DCS DLLs with `DONT_RESOLVE_DLL_REFERENCES` (no DllMain, no dependencies,
no DCS code runs) so export resolution can be checked against the real binaries.

### Live viewer (Phase 5)

While a single-player mission runs, the hook shares `LSOFRAME`, `LSOFINALTARGET` and `LSOSCREEN2`
as D3D11 shared textures. Start the viewer at any time (before or after DCS):

```powershell
build\Release\ILARTS.exe        # keys 1/2/3 select LSOFRAME / LSOFINALTARGET / LSOSCREEN2
```

The console's title bar shows the program's source, its size and the received frame rate. Its **?** key
(`{cmd:'guide'}`, `OpenGuide` in console.inc) opens the user guide with the default PDF viewer: `..\DCS-ILARTS-Guide.pdf`
from the exe (the packages: `Mods\tech\ILARTS`, one above `bin`), else `..\..\docs\DCS-ILARTS-Guide.pdf` (a
development build in `build\Release`, which `install.ps1`'s `viewer_path.txt` also starts), else one beside the exe,
else the PDF on GitHub (`kGuideUrl`). The test log gets `guide: <path or URL>`; with `--test-offscreen` nothing is
opened. Left of it, only while there is a benchmark, is the benchmark lamp (`state.bench`, see Benchmark below).
Strings in the state keep their line breaks (`JsonEsc` writes `\n`; other control characters become spaces), so
tooltips and the monitors' messages can have several lines.

**Console interface.** The window is the ILARTS station: program monitor, preview bank, control desk.

- **Page.** It is the web component `ui/ilarts-console.js`, which a web page can host as well (API: `ui/README.md`). `ui/index.html` runs it in a WebView2 composed over the Direct3D pictures with DirectComposition (`viewer/console.inc`).
- **Pictures.** The page is cut open at its monitor screens and reports where they are. The viewer draws the program picture, the previews and the replay monitor at those rectangles. Previews are unadjusted, and camera previews go through the camera curve.
- **State and commands.** The state goes to the page as JSON. The page's commands call the same functions as the keys.
- **Input.** Mouse messages are forwarded with `SendMouseInput` (visual hosting). The page's title bar is the window caption (`WM_NCHITTEST`). Keys arrive through the window, WebView2's accelerator keys, or the page.
- **Since 2026-11-14:**
  - The monitor wall (`[viewer] console_layout` 1 / 2 / q, `console_second`): monitors A (program) and B, each with a channel strip (source, REC, levels, PTZ) acting on the picture it shows; a rack of shared modules. A layout solver in the page places everything to fill the window, scaling the controls down to 55 % in small windows. The host draws monitor B (rect `second`) with its picture's own levels and analyses it for auto-levels.
  - Cameras (protocol v7: 6 cameras, 16 shared targets): ISLAND (picture source 30, tracking) and the deck camera's render slots DECK .. DECK 5 (pool 31, 29, 28, 27, 26), each able to show any deck position (`Header::views`). The viewer's sources are logical: ISLAND and each deck position; a position gets a free slot while a monitor on screen shows it (`AssignSlots`, `CameraReconcile`) and gives it back when none does. With the DECK REC switch on, the last position shown (`console_deck`, else the first) keeps a slot too (`DeckRecSource`), so it records off screen; the island camera renders while its REC switch is on. The 1 + 4 previews are assignable (`[viewer] console_previews`).
  - Deck cycle (console.inc `DeckCycle`, `CycleTick`; page: CYCLE and ▾ in each monitor's SOURCE header, `_cycleList`): monitor A or B steps through the ticked deck positions every `seconds` (2-3600; `[viewer] cycle_a` / `cycle_b`, `_views` = source keys, `_seconds`). Hand-over: the next position becomes wanted first (`CycleWants` in `CameraNeeded`, so `AssignSlots` gives it a free slot and asks for its view), the monitor switches once `ViewFor` has it (slot enabled, `views[p].current` = the position) and the target's frame counter has moved `kCycleFrames` (3) past the first frame seen there, then `CameraReconcile` releases the old position. No free slot, the slot lost, or no picture within `kCycleWait` (4 s): it switches in place. It runs only in the console, with the monitor on the wall showing a deck position, not during a replay; a manual position change restarts the interval, the PTZ stick (`CycleHold`) holds it, and `CycleNext` skips positions shown elsewhere or being brought up by the other monitor. The test log gets `cycle A: X -> Y: slot ... asked for it` and `cycle A: Y shown (ready after t s, slot ...); X slot after release: ...`.
  - Hangar positions (a view with `hangar = true` in the hook; sent as `NAME\tH`, kept by the DLL as `kViewHangar` in `views[p].names[k][23]`; remembered in `[cameras] known` as `NAME\tH`; viewer `IsHangar`): the page lists them under HANGAR DECK and the others under FLIGHT DECK (`monitors[].hangar`, `sources[].hangar`, `deckGroups` in the page); AUTO is never armed for them (`TrackOf`, `ptzAuto` / `ptzTarget` refused, `trackMode` 1), the PTZ panel hides AUTO / TARGET (`ptz.track` false), and the hook gives a slot showing one no target.
  - Keycap lamps and rocker switches. In layout 1 + 4 the previews show only the sources not on monitor A.
  - Per-source REC switches (`[video] record_<source>`): every recording writes one file per armed source; the main channel (as shown) feeds only the replay ring, black box and stream.
  - Composite recording (`[video] composite`, the VTR's COMP key): `TakeChannels` gives recordings and stills the channel `g_rec.comp` instead of the per-source channels. It draws the armed sources that have a picture (`compTiles`: by source number, the first four) with `DrawTiles` into 1920 x 1080, at the recording bitrate scaled by pixel count (2.2 Mbit/s at 1024 x 768 gives 5.8 Mbit/s). It runs while the mode is on, so a pass's pre-roll is in its ring, and while a recording or still uses it; `compTiles` is fixed during that time. The per-source channels keep running (they feed the black box and tell which sources have a picture). A mode change applies from the next recording. The test summary (`--test-run`) logs a `composite` line with its size, bitrate and tiles.
  - Stills (`StillTake`, key P / in DCS Ctrl+Shift+F8, console STILL): the next frame each recording channel reads back after the press goes to a PNG (WIC, on a worker thread, written as `.part` then renamed) in `[video] stills_folder` (default `<mission folder>\Stills`), named `ILARTS <date> <time> <SOURCE> DCS <mission clock>.png`; a channel with no picture for 3 s is reported and skipped.
  - Replay on every monitor (`ReplayFeed`, console.inc): monitor B's picture and each preview's (unadjusted, half size) feed replay rings of their own (`replay_seconds`), dropped once off screen for a whole ring. A replay takes each ring's footage of A's time span and draws it at A's position; a monitor whose current source has no footage shows live. Saving writes one extra clip per source not on A (`Replay ... <SOURCE>.mp4`).
  - Per-picture levels: `[LSOFRAME]` for the PLAT, `[camera<i>_picture]` for each camera; `app.set` is the program's group (`UseGroup`).
  - PTZ joystick (protocol v5 `Header::ptz` / `ptzState`, integrated by the DLL each frame).
  - No FLIR (removed: too costly); `ptz[i].pictureType` stays in the protocol and the viewer always writes 0.
  - No ADJ switch: the console keeps every picture's adjustments on (`ConsoleActivate`).
  - Presets hold every picture's levels (the PLAT's under the plain keys of `[preset<n>]`, camera i's under `camera<i>_<key>`).
  - Settings dialog (`kSettings` in console.inc), with restart through `--wait-pid`.
  - The symbology switch is gone; it is always on.
- **Interface.** The console is the only interface. The classic window (a video pane with a Win32 sidebar) is gone: users lost it on 2026-11-15 (F2 and `[viewer] ui` removed), and its code, its keys (L, F1), `--ui`, its `[window]` and `[sidebar]` ini keys and its tests went on 2026-11-18.
  - Without the WebView2 runtime (`ConsoleAvailable`), ILARTS says it can't run on this PC, offers Microsoft's download page (`WebView2Notice`, Yes opens it) and exits with 1. A missing `ui` folder gets its own message (reinstall).
  - If DirectComposition or WebView2 cannot start (`ConsoleActivate` false), or WebView2 fails while running (`ConsoleFail`: the browser process ended, environment or controller errors), ILARTS says so, offers the download page and closes. The test log gets `console failed: notice shown`. A page (render) process that fails is reloaded instead.
  - The process is per-monitor DPI aware.
- **Experimental features** (`[experimental]`, SETTINGS > Experimental, marked ⚠): `flir` shows a FLIR key on each camera's PTZ panel; the viewer then writes `ptz[slot].pictureType = 4` for that camera's render slot (`g_flirOn`, not saved: every session starts normal). The packages always ship the island and deck cameras (`EXTRA_CAMERAS = true` is enforced by `deploy\package.ps1`).
- **Camera tuning** (`[developer] tuning`, SETTINGS > Developer > Camera tuning; `viewer/tuning.inc`): for placing cameras by eye while DCS runs. Development builds only (`DevBuild()`: version 0.0.0, built without `-DDCS_ILARTS_VERSION`): a release build shows no Developer panel and ignores `[developer] tuning` in the ini; not in the user docs until it gets a proper interface. Each camera's PTZ panel gets a TUNE key that opens a panel for the camera on that monitor (ISLAND or the deck position shown): POS X / Y / Z and LOOK X / Y / Z (the look-at point) in metres along the carrier's axes (x forward, y up, z starboard; shown in feet as well), each with − / + (they repeat while held), FOV − / + (1 deg), STEP 0.1 / 0.5 / 1 m (`[developer] tuning_step`), RESET and COPY (the entry's Lua line to the clipboard).
  - Values: from the hook (protocol v9 `Header::camPose[slot]`: the rest pose last sent with `extra_cam('set')`, under the name of the view shown), so they include the user's own entries in the tuning file. The panel uses them only while `camPose.name` is the camera's name; otherwise it shows dashes and its keys are off.
  - Writing: ILARTS owns one section of `Saved Games\DCS\Config\ILARTS-cameras.lua`, the last thing in the returned table, so it wins over the user's entries for the same name:
    ```lua
        -- ILARTS developer tuning: begin
        { name = 'HANGAR FWD', pos = { 54.7, 18.7, 4.0 }, look = { -60.1, 10.5, 0.0 }, fov = 51 },
        -- ILARTS developer tuning: end
    ```
    Everything else is kept byte for byte; without a section one is added before the table's closing brace (with a comma after the last field if it has none), and without a file a new one is written. A small Lua scanner (comments, quoted and long-bracket strings) checks that the result is still `return { ... }` with balanced braces before it is written. UTF-8 without a BOM (a BOM is dropped), the file's own line endings, written to `ILARTS-cameras.lua.ilarts-tmp` and moved over the old file (`MoveFileEx`; if the hook has it open, the next tick tries again), at most every 0.3 s. A file the section can't be added to (not `return { ... }`, half a section) is left alone and the panel shows an alert. The file is read again when its write time changes (an edit by hand).
  - RESET takes the entry out of the section; the hook then puts back its own values (`C.orig`, docs/FINDINGS.md). Until the slot's `camPose.seq` moves (or 2.5 s pass) the panel waits, so a nudge never starts from the old values.
  - PTZ: a new rest pose takes the camera off the joystick in the DLL (`Aim::rest`, fov included), so the picture shows the new rest view.
  - Saved Games: `SavedGamesDcs()` (repair.inc). `--test-saved-games <dir>` replaces it, for tests (repair and the permissions check use it too).
- **Build.** The SDK is in `third_party/webview2` (NuGet Microsoft.Web.WebView2 1.0.2903.40; the loader is linked statically). CMake copies `ui/index.html`, `ui/ilarts-console.js` and `ui/fonts` next to the exe.
- **Tests** (`--test-run` and `--test-log` as usual):
  - `--test-offscreen` renders the window off the screen;
  - `--test-script file` takes lines of `seconds<TAB>` followed by one of: JavaScript, `!shot`, `@vk` (a key), `^x,y` (a click) or `?x,y` (a hit test);
  - `--test-commands` logs the page's commands;
  - `--test-no-webview2` behaves as if the WebView2 runtime were missing: the notice appears and ILARTS exits.
  - Presets, keys and sources are checked through the page: a script calls `_cmd('preset', {index, save})`, `_cmd('rename', ...)`, presses keys (`@38` Up, `@82` R, `@116` F5, `@49`..`@51` sources) and reads `_state` back (`presets`, `flash`); a second start checks that the presets come back from the ini. These replace the classic window's `--test-presets`, `--ui-selftest` and `--snapshot` (2026-11-18); `!shot` and `--test-keys "t:!"` save the pictures and the window.
  - To test the failure notice, end the WebView2 browser process (`msedgewebview2.exe` whose parent is the test ILARTS). In a scratch copy, give the WebView a user data folder of its own first (`ConsoleStart`: `%LOCALAPPDATA%\ILARTS\WebView2`), or the browser process is shared with a running viewer.

**Keys.** The defaults below; `[keys]` in the ini (SETTINGS > Keyboard) rebinds them (`kKeyActions`). The source keys,
the numeric keypad's + / -, Ctrl + a preset's key and the replay's keys are fixed. Settings are saved in
`build\Release\ILARTS.ini` (an old lso_viewer.ini is copied over once).

| Key | Control |
|---|---|
| 1-5 | monitor A's source in the console's order: PLAT, LSO 1, LSO 2, ISLAND, DECK (5 picks a deck position as the DECK key does: the one monitor A shows, else the last one shown if no other monitor has it, else the first free one); the lit one again turns monitor A off |
| C | CCTV look on/off (scan lines, grain / static, lens distortion, vignette; strength in SETTINGS > CCTV look) |
| B | save the black box now |
| P | still of every armed source |
| N / Shift+N | next / previous carrier for the PLAT |
| + / - | exposure, 1/3 EV (pipeline mode) |
| Up / Down, Right / Left, PgUp / PgDn | brightness, contrast, gamma |
| A | auto-levels: darkest 0.5% .. brightest 99% stretched (pipeline: of the camera picture rebuilt from the HDR image, which has no symbology; Levels: DCS's white text left out), max gain 25x |
| R | reset adjustments |
| F5 | the built-in Night preset (fixed: +0.91 EV, brightness +0.13, contrast x1.5, gamma 1.2, auto-levels) |
| F6-F8 / Ctrl+F6-F8 | load / save presets 1-3: each holds the levels of every picture (the PLAT and each camera) (console: click = load, Ctrl or Shift+click = save, right-click = save / rename / clear) |
| V | start / stop a manual recording (MP4) |
| M | recording mode: manual / automatic (every approach the PLAT sees, one file each, overlapping ones at the same time) |
| Space | instant replay of the last approach (else the last 30 s); in the replay: Space pause, Left/Right 2 s or one frame, `,` `.` one frame, Up/Down speed (1/8x to 8x), Home start, S save, Esc live |
| Esc | close ILARTS |
| Ctrl+Shift+F9 / F10 / F11 / F8 | record / instant replay / save the black box / still while DCS has the keyboard |

Recording (viewer/video.h): the picture as shown is drawn again into a 1024x768 canvas at 30 fps, read
back, JPEG-encoded into a 45 s replay ring and, while recording, encoded to H.264 MP4 with Media
Foundation (the GPU's hardware encoder when there is one). Automatic mode follows the hook's pass
reports (`Header::pass`): a pass starts when an airplane closing on the deck enters the PLAT camera's
picture within 2 nm and ends at the trap, bolter or wave-off; the file starts 3 s before and ends 4 s
after. Passes that never get below 150 m within 1200 m of the camera (overhead break arrivals) are
dropped. Files go to `Videos\DCS ILARTS`; the `[video]` section of ILARTS.ini changes folder, size, fps,
replay length, pre/post-roll, bitrate, encoder and the file name.

Whose passes automatic mode records (protocol v8; console ACFT key, `[video] auto_filter =
own|section|flight|all`, default all). The hook decides whose each pass is, so the viewer matches no names: per pass
`Header::passFlags[i]` (bits `kPassOwn` 1, `kPassSection` 2, `kPassFlight` 4; your own aircraft sets all three) and
`Header::ownAircraft` (0 not reported, 1 you have an aircraft or had one this mission, 2 none). Your aircraft is
`world.getPlayer()` in single player and the slot's unit (`myUnitName()`) in multiplayer, read in FEED_QUERY (host /
single player: group from `Unit:getGroup()`, number from `Unit:getNumber()`) or, as a client, from the slot's runtime id
(`DCS.getUnitProperty(slot, UNIT_RUNTIME_ID)` = the export object id that client candidates are keyed by) with group and
number from the mission file (`missionUnits()`; a candidate's number by name, else by matching its group's units'
runtime ids). Section = same group and `ceil(n / 2)` equal (units 1-2, 3-4). The hook keeps the last aircraft you had
this mission (`state.meLast`, cleared with the per-mission state) and matches against it while you have none (landed and
went to the LSO station, released the slot), so `ownAircraft` is 2 only when you have flown nothing yet this mission;
the console then shows NO ACFT (amber) while AUTO is on and the filter is not ALL ACFT. A hook older than the DLL leaves
`ownAircraft` 0, and the viewer then records every pass. Viewer and DLL ship together and the viewer accepts only its
own protocol version, so an older producer is not read at all. The filter decides only whether a new pass starts an
automatic recording (`AutoWantsPass`); passes, grades, the black box and manual recording are unchanged. Hook log:
`PASS your aircraft: ...` when it changes, and `group <name> #<n>, whose <flags>` on each `PASS ... start` line.

Mission folders and the storage limit (viewer/recfiles.inc): every file (`UniqueVideoPath`, stills) goes into
`<recordings>\<date> <HHMM> <mission>`, one folder per mission = DCS process + `Header::passEpoch` (the hook
makes a new epoch at every mission start or restart), named after protocol v8's `Header::missionName` (hook:
`DCS.getMissionName()`, else the .miz name; written with the symbology lines as `mission_name=`) and the time the
viewer first saw the epoch (a name already taken, e.g. a restart in the same minute, gets " (2)"). The folder is
created with its first file (`SessionFolder`); a recording keeps the
folder it started in when it is renamed or the mission changes. Before any mission the name is
`<viewer start> No mission`. A set `[video] stills_folder` is used as is (no per-mission folder). The storage limit
(`[video] storage_limit`, `storage_limit_gb`; ini values below 1 GB down to 0.01 work, for tests) runs on a worker
thread (`store::Worker`): before a recording, still, black box save or replay save, after every saved file and
every 30 s while recording. It counts every file under the recordings folder (and a stills folder elsewhere),
adds what the files being written may still need (a pass: bit rate x (pre-roll + 240 s + post-roll) minus its
size so far; a manual recording: 5 more minutes; a still: w x h x 3; a black box save: the rings' bytes) and
deletes the oldest (last write) files on the ledger `<recordings>\.ilarts-files` (hidden, UTF-8, one path per
line, relative to the recordings folder; every path ILARTS hands out is appended; entries whose file is gone are
dropped when it is loaded) or their `.part` leftovers. A file is opened with `DELETE` and no sharing first, so an
open file is never deleted. Then it removes empty folders below the recordings folder that it emptied, or that
are named like a mission folder or `Stills`, except the current mission's.
Mixed versions: the viewer reads only a header of its own `kVersion`; a v8 viewer cannot map an older DLL's smaller
mapping and tries again every second (closing the handle each time), and an older viewer ignores a v8 header.

File names: `[video] filename`, default `{date} {time} {callsign} Case {case} {grade}`, e.g.
`2026-11-13 21-04-10 Viper Case III (OK) wire 3.mp4`. The hook publishes per pass (protocol v4) the
callsign (player name, else the unit's callsign), the case (III at night, for a straight-in approach or
in weather below 1000 ft / 5 nm; else I, or II with a ceiling under 3000 ft; `RECOVERY_CASE` in the hook
forces one) and the LSO's grade and wire. The grade comes from `S_EVENT_LANDING_QUALITY_MARK`, caught by an
event handler the hook installs in the mission scripting state (host / single player; Supercarrier grades
human pilots only). It usually arrives after the file was saved: the viewer renames the pass's files when
it does (for two minutes). Without a grade, `{grade}` is the result (TRAP / BOLTER / WAVE-OFF).

Black box and stream (viewer/live.h, viewer/rtmp.h): a second, continuous H.264 encoder (a Media
Foundation encoder transform: the GPU's, else Microsoft's; 2.5 Mbit/s, a key frame every second) runs
only while one of the two is on. The black box keeps its access units in memory (5 minutes, about 90 MB at
most) and writes them to MP4 without re-encoding: by itself when the hook reports a mishap
(`Header::mishap`: S_EVENT_CRASH / EJECTION / PILOT_DEAD of an aircraft within 5 nm of the PLAT carrier, or
an aircraft that vanishes close in on its pass) - 60 s before to 15 s after - or on B. The stream is an
RTMP publisher written for this (handshake, AMF0 connect / createStream / publish, FLV AVC + a silent AAC
track, server chunk sizes, pings, acknowledgements, reconnect; no TLS), configured in `[stream] url / key`.
Up to four streams (`[stream]`, `[stream2]` .. `[stream4]`), each with a `source`: monitor A (`a`, the
main encoder's units), monitor B, a fixed source, or `composite` - up to four `tiles` in one 1920 x 1080
picture. Every source but `a` has its own canvas and encoder. A `label` (up to 6 characters) replaces the
target's number on its console key and in the status LCD.

Extra cameras (src/extra_cams.cpp): fixed
cameras on the carrier, rendered by DCS as additional picture sources next to the PLAT's. The hook holds
their positions (`CAMERAS`; `Saved Games\DCS\Config\ILARTS-cameras.lua` overrides them live), the viewer
asks for them (`Header::control.cameraMask`), and nothing in DCS is patched until one is first asked for.
Each enabled camera is a feed of its own and is recorded in the background on its own channel (canvas,
engine, black box), so a pass yields `<name>.mp4` and `<name> ISLAND.mp4`. A camera with `track = true` (the
island camera) follows the landing aircraft from the round down until, trapped, it starts taxiing out of the wires
(it stopped on deck and has since moved 3 m forward or sideways; the wire's pull-back does not count), it leaves
the landing area without stopping, or it climbs 50 ft above the deck. Far from the carrier (main camera beyond 70 km) every view at the carrier - PLAT and extra
cameras - is rendered against one shared origin next to the PLAT camera (plat_origin.cpp): DCS shares an
object's placement between the views of a frame, so they must all use the same one.

Objects that both those views and DCS's own views see (src/origin_split.cpp, hook `PLAT_ORIGIN_SPLIT`, installed with
the PLAT origin hooks): GraphicsVista's `GraphSceneObject::collectRenderables` places an object once per frame,
relative to the origin (CollectionInfo +0x50C) of the first collection Scene.dll hands it, for every view. With the
far fix on, your own view then drew the carrier at full detail next to your camera whenever you looked at it from
70-90 km (the camera's object range, ~90 km observed), or the PLAT lost the hull. enlight's
`SplineRopeParsable::collectRenderables` (+0x63B50) does the same for the arresting cable while it is engaged in a
trap (the wires floated in the sky during traps). The DLL swaps slot 1 of Visualizer's GraphSceneObject vtable
(+0x23BF00) and of enlight's SplineRopeParsable vtable (+0x7FE10) for a wrapper each (own original, each installed on
its own after its own checks): a call whose collections share one origin (every call in stock DCS) goes straight
through; a call with several origins becomes one DCS call per origin, each with its own collections, flags and
outputs in DCS's order, the group holding the last collection last. `PLAT_ORIGIN_SPLIT = 'ab'`
alternates split / no split every 15 s for an in-game comparison (`diag.plat_origin('split_on' | 'split_off')`). It
always logs (no VERBOSE): `PLATORIGIN split: model '...'` / `PLATORIGIN split: arresting cable (...)` for the first 6
objects per far-view episode (what DCS would have done unsplit: which view would have drawn it how far from where it
is), and a `PLATORIGIN split 30s:` line every 30 s while the far fix was on or anything was counted (no line limit),
with the counts per class (`models c/s, arresting cables c/s`).

Your own aircraft in those views (src/own_canopy.cpp, hook `OWN_CANOPY`): DCS hides the canopy, frame and pilot of
your external model with draw argument 114 while you sit in the cockpit, for every view of the frame. The DLL
wraps NGModel's per-object parse (`NGModelLodsMT::ParseMT2`, vtable slot 31): when an object with argument 114
set is seen by one of ILARTS's views, those views' collections are parsed separately with a copy of the arguments
in which 114 is 0, and NGModel's per-LOD instances for that parse come from the cookie's spare slots (slot +
number of LODs, through the redirected call at NGModel+0x3DBE4), so DCS's own views keep theirs. The number of
LODs is the model's LOD entry count (+0x5A0) minus its first drawn entry (+0x5B0; 1 when the .lods file has a
`collision_shell`, which sorts to entry 0: the F/A-18C has 7 entries from 1); more than 8 LODs, or a spare slot
already bound to another LOD, leaves that parse or fetch to DCS. Only objects with 114 set are split; DCS's own
parses and fetches are passed on unchanged, and the fetch wrapper checks each of DCS's fetches (slot index 0..15,
slot unused or bound to the fetched LOD) and logs anything else without changing it. ILARTS's views
are reported by plat_origin's view setup hook (picture-source views at the PLAT or an extra camera) and matched
by camera position (CollectionInfo +0x318). Needs the PLAT origin hooks; details in docs/FINDINGS.md. It always
logs (no VERBOSE), for the whole session: `OWNCANOPY stats 15s` lines for the first 15 minutes, then
`OWNCANOPY stats 300s` every 5 minutes (counts over that time); the first object with argument 114 set; up to
6 objects within 15 m of your camera with their model name and non-zero arguments 0..150; `OWNCANOPY near:` lines
(8 per 15 s for the first 7.5 minutes, then 2 per 5 minutes for your aircraft only: instance, graphics instance,
collections by view, what was done, the LOD slots used and the 114 each holds); `OWNCANOPY split target:` when
the object the split runs for changes (16 lines); `OWNCANOPY anomaly:` for anything unexpected (faults, slot
conflicts, a fetch not redirected, another instance on your aircraft's graphics instance, DCS fetching outside the
16 slots or into a slot bound to another LOD; 8 lines per kind); and `OWNCANOPY not split` when a model's LOD table
rules out the split. Each mission starts its lines and budgets afresh.

Stars in those views (src/night_sky.cpp, hook `CAMERA_STARS`): DCS draws its stars 100 km out (when the celestial
provider has no instance of its own; with one, at half the view's far plane), and ILARTS's views end at 80 km, so
in a wide camera picture only the stars more than 36.9 deg off the axis were drawn: a ring of points toward the
corners, which the CCTV look turns into dashes. The DLL swaps slot 1 of SpaceRenderer's celestial bodies renderable
(vtable +0xC4F8, the per-view call +0x32A0 with the view's SceneRenderContext) for a wrapper and redirects the two
`StarMaterial::draw` calls in `SpaceRendererInstance::draw` (+0x6931 stars, +0x69A5 second star layer) to a filter.
While the wrapper runs for one of ILARTS's views (context camera +0x308 within 5 cm of a camera plat_origin's view
setup hook listed in one of the last 4 render graph builds; the view setup hook flags the PLAT's view, i.e. any
listed view that is not at an extra camera), the filter hides the stars (`'hide'`), draws them with the sphere
scaled to half the far plane (`'full'`, no ring), passes them on (`'stock'`), or (`'auto'`, the default) draws them
as `'full'` with a limiting magnitude and a gain tied to the horizontal field of view (from the context's P00):
none at `kStarsStartFovDeg` (24 deg) or wider, the limit rising linearly in log(field of view) from
`kStarsStartMag` (-1.5) to `kStarsFullMag` (8, the whole catalogue) at `kStarsFullFovDeg` (6 deg); the gain makes a
star that has just come in as bright as `kStarsAppearBrightness` (DCS's magnitude 4.8 star) and reaches DCS's own
brightness once the limit passes that magnitude, and the first `kStarsRampMag` (1.5) magnitudes fade in from zero.
The PLAT gets none in auto. Mechanism (all in copies on the filter's stack, so DCS's material, star object, vertex
list and effect are never written): `StarMaterial::draw` draws `(end - begin) / 20` vertices of the material's list,
which is Space.dll's catalogue brightest first, so a 0x138-byte material copy with a shorter `end` is the limit (the
filter reads each list once, checks the order and keeps the normalized magnitudes per material; an unsorted list is
drawn whole and only dimmed); the star object copy's +0x5C (P11, uScale = pow(1 / P11, 0.8), Star.fx divides alpha by
it) is multiplied by gain^1.25 and its +0x58 lowers the milky way's opacity by the same gain. In `'full'` and
`'auto'` the material copy's effect (+0x20) is a proxy: a 32-slot vtable of thunks that swap in the real effect and
jump to its slot, with slot +0xA8 (set a vector parameter) rewriting the pixelSize handle's value (+0x128) to
(y x P00 / P11, y), so a sprite is square on screen in a 4:3 picture rendered into a square target. DCS's own views
and every other caller always get DCS's call. `'off'` does not install. Needs the PLAT origin hooks. It always logs
(no VERBOSE): `NIGHTSKY install`, `NIGHTSKY ILARTS view:` / `NIGHTSKY DCS view:` when the star geometry of that kind
of view changes (far plane, star radius measured from the star object and vertices, own or global instance, the
angle inside which stars are beyond the far plane and the picture corner's angle; 12 lines per mission),
`NIGHTSKY star list` the first time a list is read (count, sorted or not; 8 lines), and a `NIGHTSKY` stats line every
60 s (renders and star draws per kind, hidden / scaled / faded / points, near misses = renders within 50 m of a
listed camera but not on it, and the last ILARTS draw: field of view, limiting magnitude, gain, stars drawn of the
list). `diag.night_sky('auto' | 'hide' | 'full' | 'stock')` switches live. `night_sky_test` covers the curve (none at
24 deg and wider, monotonic and continuous down to 1 deg, no star coming in brighter than the appear level, the whole
catalogue at DCS's brightness at 6 deg and less), Space.dll's real catalogue (41475 stars, sorted, with a table of
stars per picture by zoom), the copies through a stand-in material / star object / effect (count, uScale, milky way,
pixelSize through the proxy's machine code, the other effect calls forwarded unchanged), the PLAT (none in auto at
any zoom), an unsorted list, DCS's views and data unchanged, and install / remove on the real SpaceRenderer.dll.

Rain and snow in those views (src/rain.cpp, hook `CAMERA_RAIN`): Effects2.dll's `Effects::Rain` stretches each drop
along rainVel - cameraVel (constant buffer cEffectParams at Rain +0x130, cameraVel +0x150), and `Rain::update` works
out cameraVel only for the view whose renderable carries the update mark (renderable +0x24); every other view, the
PLAT and the extra cameras included, drew with the value your own view left, i.e. your aircraft's speed ("warp
speed" streaks). The DLL swaps slot 1 of Visualizer's precipitation renderable (vtable +0x23BBD0, the per-view call
+0xC9C60 with the view's SceneRenderContext) for a wrapper. DCS's views go straight through. For one of ILARTS's
views (night_sky's list: context camera +0x308 within 5 cm of a listed camera, via `NightSkyOurView`) it writes the
velocity into the Rain object's cameraVel just before DCS's call (which uploads the constant buffer) and puts DCS's
value back right after (left alone, and counted, if something else changed it meanwhile); a view DCS marked as the
updating one is drawn exactly as DCS does (no copy of DCS's renderable is made). `'auto'` (the default) uses the
view's own motion:
per view (matched by camera position, the context pointer as tie-break) the camera positions of the last
`kRainWindowSec` (1 s, one per 1/60 s) are kept and the velocity is the move from the oldest to the current one
over that time (QueryPerformanceCounter time; under time acceleration the ship's apparent speed); a move of more
than 5 m + 60 m/s x the time since the last draw is a new view, which starts with the velocity of the view tracked
last (the ship's); speeds are cut at 60 m/s. `'still'` uses 0, `'hide'` skips ILARTS's precipitation draws, `'stock'`
passes them on; `'ab'` (hook only) alternates auto / stock every 15 s with on-screen text. Install refuses unless the
compat check passes, the whole per-view call (0xFB bytes) matches, the vtable holds +0xA3C60 / +0xC9C60, Effects2's
Rain::draw, the update mark test, the cameraVel store, the constant buffer upload, Rain::init's type store (+0x94)
and the provider's init calls (Visualizer +0xC98B6: rain 0, snow 1) match, the
export resolves to Effects2+0x52E0 and Visualizer's import slot (+0x22E0B8) leads to it. It always logs (no
VERBOSE): `RAIN install`, `RAIN <kind> view:` sample lines (PLAT / camera / DCS updating / DCS other: camera, how far
the particle cells' centre is from it, the update mark, the velocity DCS left in cameraVel, the view's own tracked
motion, what was drawn) once per kind at the first rain and after each stats line, a `RAIN` stats line at most
every 60 s for the first 10 and every 300 s after that, only while something drew (draws per kind and mode, marks
cleared, conflicts, refusals, near misses, new views, views tracked, ILARTS draws while DCS's simulation time stood
still, the speeds used and DCS's), `RAIN anomaly:` (8 lines), and `RAIN test:` in 'ab'. `diag.rain('auto' | 'still'
| 'hide' | 'stock')` switches live. `rain_test` drives the wrapper with stand-ins that do what Rain::update does
with cameraVel (a 150 m/s flight with the PLAT and an island camera on a 15 m/s ship: stock gives them 150, auto 15,
your view and a DCS view drawn after them keep DCS's value; draw orders, a mark on an ILARTS view, 10 Hz position
steps, a camera moved 200 m, the 60 m/s cut, near misses, an unreadable Rain object, a value changed underneath, the
lines) and, on the mapped Visualizer.dll / Effects2.dll, scans every reference to the renderable's vtable and to the
Rain::draw import (only the wrapped call) and every rbx-relative write to cameraVel in Rain::update and the private
draw (only the two stores behind the mark), then installs / removes twice and checks the refusals.

Precipitation type in those views (same wrapper, 'auto' / 'still'): DCS splits its precipitation into the rain and
the snow object (Effects2 Rain +0x94: 0 / 1) by the temperature at your camera (Weather.dll +0x17900, rain share
clamp((T + 1) / 3, 0, 1)), and every view draws both with that split. The hook passes the carrier's sea-level
temperature once a minute (`diag.rain('temperature', t)`: DCS's own at the carrier from
`atmosphere.getTemperatureAndPressure` in the server state, host / single player, else the mission's season
temperature); a draw in one of ILARTS's views (without DCS's mark) of the snow object is skipped at +2 C and warmer
at the view's height (6.5 C per km), of the rain object at -1 C and colder. No temperature: DCS's split.

Hangar cameras: the hook passes a hangar view's flag as the 14th argument of `diag.extra_cam('set', ...)`
(`ExtraCamsSetHangar`); in every mode the wrapper skips the precipitation draws of a camera view within 0.5 m of a
rendering extra camera that shows a hangar position (`ExtraCamsHangarNear`; never the PLAT), counted as `hangar=` on
the stats line. `rain_test` stubs `ExtraCamsHangarNear` (auto, still and stock; the PLAT at the same spot still draws).

Rain while your camera is in pure snow (same wrapper, 'auto' / 'still'; optional: when its checks fail, the rest still
installs): collectRenderables (+0xC9900) gives a collection a rain renderable only when power x share is above a
threshold, so with DCS's share 0 (you high up in a warm mission) no view gets one and DCS neither draws nor simulates
the rain object. Slot 1 of the provider's vtable (+0x23BB88; slot 1 +0x23BB90) goes to `CollectWrap`, which notes the
provider's rain object (+0x08) at every collection. When no view has drawn the rain object with DCS's own renderable
for 0.25 s and one of ILARTS's views gets only its snow renderable while its own temperature calls for rain,
`BorrowRain` sets that renderable's Rain object (+0x10), power (+0x18, x the view's rain share) and mark (+0x24) for
one call of DCS's per-view call and puts them back right after; then the snow is handled as above. One view, the
marker (the first to draw; kept while it draws again within 0.3 s at about the same place), carries the mark so that
Rain::update steps the simulation, counts and wind for it; a new marker (and every marker in 'still') first gets its
own camera as Rain +0x110, the stored camera DCS's next cameraVel is worked out from. The others write their own
motion into cameraVel and put DCS's back, as for any draw. Install checks the vtable (+0xC97C0 / +0xC9900), the
entry of collectRenderables and its two collectInstances calls with +0x08 / +0x10. Lines: `RAIN while you are in
snow:` at install, `RAIN DCS draws no rain ...` once, sample lines `drawn with DCS's rain object ...`, and the stats
`type:` part (`DCS's rain drawn while DCS drew none`, `simulated`, `no rain object`). `rain_test` covers it with a
fake provider (you 5.4 km up: the PLAT runs the simulation with the ship's speed, the island its own motion, the
renderables put back, the PLAT closed and the island taking over without a streak, DCS's rain back, a stale provider,
still / stock / hide, the band, freezing, no temperature, not installed) and on the mapped DLL the pointer to
collectRenderables, install / remove and the refusal on changed code.

The far views' cloud lookup (src/rain.cpp, installed with the rain wrapper; optional: when its checks fail, the rest
still installs): the precipitation shaders and every cloud shadow read enlight's cloud volume at pos x
gCloudVolumeScale + gCloudVolumeOffset (cPerView, register b7, +0x6B0 / +0x6C0), pos relative to gOrigin, the view's
origin. GraphicsCore's ContextBase keeps a stack of 0xBD8-byte blocks (array pointer +0x11D578, index +0x11D518;
cPerView at block +0x268, so the two fields at +0x918 / +0x928); SceneRenderer's pass binder sets them from enlight's
per-frame values (for the frame origin) right before it binds a view, and `ContextBase::updateBuffers` (the only
upload of cPerView, imported by GraphicsVista and enlight only) uploads the block. GraphicsVista's and enlight's
import slots of updateBuffers go to `UploadWrap`: when the context's origin (+0x2C) is the carrier views' own origin
(`NightSkyCarrierOrigin`, noted by plat_origin.cpp's build wrapper for the last 4 builds), the block's offset becomes
DCS's + (own origin - enlight's origin +0x1740) x scale for DCS's upload and is put back right after (only when
DCS's offset / scale equal enlight's +0x1770 / +0x1780 of that moment). 'stock' and every other upload pass through.
Install checks setCloudVolumeTransform, updateBuffers' block / gOrigin / upload, GraphicsVista's viewport copy,
enlight's getters, update stores and exported getters, the facade's vtable (+0x7C850, slots 12 / 13) and that both
import slots lead to GraphicsCore+0x18BD0; remove puts the slots back. Lines: `RAIN far views:` at install, one
`RAIN far view upload (PLAT | camera | other pass):` line per stats period (camera, view origin, the frame origin
DCS's offset is for, the shift, DCS's and the corrected offset, the camera's uvw in the volume, how far from the
camera the lookup lands with each), the rain sample lines' cloud lookup and precipitation type parts, and a
`far views:` / `type:` part on the stats line (uploads on the carrier's origin per kind and outcome, max shift;
carrier temperature, rain share used, DCS's at your camera, snow / rain draws skipped). `rain_test` models the
binder, the block, enlight's state and the upload (stock: the PLAT reads the clouds 115 km away; auto / still /
hide: ILARTS's views and a pass with its own camera within a centimetre, DCS's view bit-identical, three orders,
the block put back; unmatched, unreadable, changed underneath, frame mode, within 70 km), the type filter (20 C,
-5 C, the mixed band, camera height, stock, a marked view, the lines), and on the real DLLs: the imports of
updateBuffers / setCloudVolumeTransform / setShadowMatrix / getPerViewCBuffer across bin, the calls through those
slots, the 0x970-byte uploads in GraphicsCore, updateBuffers' writes to the cloud volume fields, enlight's stores to
+0x1740 / +0x1770 / +0x1780, install refusals and install / remove twice.

What the DLL changes on DCS's main thread (audit 2026-11-19, docs/FINDINGS.md):
- `keep_plat` (src/lso_camera.cpp) swaps the globalView object's vtable pointer for a private copy in which only
  slot 31 differs. Its hook gives Visualizer's PLAT view setup the synthetic camera (plat_cam.cpp) and gives the
  last LSO station camera to callers inside Visualizer.dll only; every other caller (Supercarrier, DCS.exe,
  aircraft modules such as FA18C.dll, which imports globalView) gets DCS's own answer. The slot 12/13 answer hooks
  and the call counting thunks (`VIEWCALLS`, `VIEWCALLERS` lines) are added only when `VERBOSE` was on when the
  mission started; `sc_override(true)` adds the slot 12/13 hooks on demand.
- `plat_ui` (src/plat_ui.cpp) records the main thread of every Lua state that makes a dxgui widget call (a
  coroutine is folded into its main thread, using lua.dll's layout only when its `lua_status` / `lua_pushthread`
  code matches) with the time of its latest call. An unknown state is probed with `lua_cpcall` at most once per
  sighting (a state that comes back after 5 s without calls is a new sighting) and only within 2 s of a call;
  the found PLAT UI state is kept while idle. The recorded states are forgotten at install and reset, and the
  hook resets on mission stop and when its step fails.
- Viewport sizes (src/mfd.cpp): a cockpit picture source (0-31) that DCS itself filled within the last 10 s
  (`ExtraCamsCockpitSource`, sampled each frame in the cameras' redirect before the cameras add theirs) is never
  resized: `enforceResolutionOfMFD` is not called for it and the render-thread re-apply skips it (`MFD viewport N
  is in use by the cockpit`). Up to 8 requests; `share_stop` clears them, so none carries into the next mission.

PLAT processing applies to PLAT and to the camera window of LSO 1: the LSO station composites
LSOFRAME into x 192..831, y 150..629 of its 1024x768 frame with tint (0, 1.005, 0.752); the viewer
replaces that window with the tinted processed image and leaves the station symbology untouched.

The processing mode is always Pipeline (Levels by itself when DCS does not share hdrBufferMFD32;
`[viewer] mode=levels|off` in ILARTS.ini overrides it for troubleshooting). Processing works on the R11G11B10_FLOAT source, so shadows lift without 8-bit banding. Note: in
night scenes DCS renders the sky as exactly 0 in LSOFRAME, so only rendered light (aircraft lights,
lit surfaces, clouds) can be brought out; there is no hidden detail in pure black.

How the pipeline reads DCS's PLAT (viewer shader `csK`, `symCover`, `csCam`; CPU `ConsumeKHistogram`, `Smooth`):

- **DCS's exposure `kDcs`.** DCS tone-maps the PLAT as `lso = 1 - exp(-k * hdr)`, one k per frame. `csK` takes
  `log2(-ln(1 - lso) / hdr)` of every mid-tone pixel (0.08..0.7, after undoing the in-DCS LSO_PLAT adjustment) whose
  four neighbours 3 px away agree with it: within 0.6 stop where they are mid-tones themselves, and dark / saturated
  where that k says they should be. The symbology (text, its dark halo, anti-aliased edges) disagrees with its
  neighbours and is left out; a bright picture (sunrise) otherwise left mostly halo pixels in the mid-tones and k read
  6x too low. The median of what is left is the target, followed with a 0.4 s time constant. With fewer than 0.5% of
  the picture's pixels (night, a picture of only highlights and shadows) the last value stays. Extra cameras without
  an LDR picture skip the neighbour test and expose the lit part of their HDR image for a middle grey (200 pixels
  suffice).
- **Symbology mask.** `symCover` compares LSOFRAME with DCS's predicted camera value, each HDR texel tone-mapped and
  then interpolated like LSOFRAME (`predAt`; tone-mapping the interpolated HDR value overshoots at high-contrast
  edges and drew black outlines). A pixel is black ink only when it is darker than the prediction with exposure
  `kDcs / 1.3` (minus a 0.05 dead zone), bright ink only when it is brighter than with `kDcs * 1.3` (plus 0.08) and
  bright in absolute terms: DCS's curve departs from the model by about +-15% in k across tones.
- **AUTO levels.** In the pipeline the histogram covers every pixel of the rebuilt camera picture (it has no
  symbology). With nothing to measure (Levels mode, a picture all white text) the targets go back to 0..1 instead
  of keeping the last ones.
- With `[viewer] debug=1`, a still (P) also writes the PLAT's two source pictures, `LSOFRAME-<time>.dds` and
  `hdrBufferMFD32-<time>.dds`, and `PLAT-<time>.txt` (exposure, levels, in-DCS adjustment) to the stills folder.
  `share_selftest --serve <s> LSOFRAME-<time>.dds hdrBufferMFD32-<time>.dds` plays the pair back.

Synthetic PLAT frames for these tests: `python -I tools/plat_scene.py all <dir> [--adjust 1,1.4]` writes
`LSOFRAME-<scene>.dds` / `hdrBufferMFD32-<scene>.dds` for day, sunrise, glare, silhouette and night (and a mask per
scene for scoring). With `--test-log`, the viewer logs `analysis PLAT k <kDcs> target <k> measured <0|1> levels
<black>..<white> target <black>..<white>` once a second (`--test-analysis 0.1`: every 0.1 s). `LSO_DCS_ROOT=<folder>`
points the viewer at a stand-in DCS folder for the in-DCS adjustment (`Bazar\shaders\PostEffects\BWTV.fx` mentioning
LSO_PLAT plus `LSO_PLAT.hlsl`; an empty folder = not installed).

Rain (the 2026-11-20 report, docs/FINDINGS.md): `plat_scene.py rainnight|rainlit|rainday <dir> --rain both|ldr|hdr
--streaks bright|dark [--frames 16] [--steady]` draws short dashed streaks radiating from a point into both pictures,
LSOFRAME only or the HDR image only (where DCS draws its rain is not known yet). `rainnight` has an overcast sky
barely lit (below csK's 0.08 mid-tone floor, DCS's usual night), `rainlit` a lit one, `rainday` a grey day. With
`--frames N` it writes a looping sequence, `LSOFRAME-<tag>-00.dds` ...: the streaks move outward each frame and the
rain swells from 2% of the streaks to all of them and back (`--steady`: all of them every frame).
`share_selftest --serve <s> --fps 8 <LSOFRAME-<tag>-*.dds> <hdrBufferMFD32-<tag>-*.dds>` plays a sequence (files
with the same target name are its frames). What to look for in the `analysis PLAT` lines (`--test-analysis 0.1`,
AUTO on): kDcs stays within one histogram bin (~1%) through a whole loop in every case, measured from the scene or
held when too little qualifies (a dark night with the streaks in one picture only); AUTO levels may follow the
amount of bright rain slowly (the brightest 1% of the picture are streaks once they cover more than 1% of it);
`--steady` keeps the amount fixed to tell the two apart. The viewer before the washout fix (e45063f) fails the dark night: kDcs jumps between the
right value and 20-40x too high.

Testing without DCS:

```powershell
build\Release\share_selftest.exe                                   # sharing self-test, both GPUs
build\Release\share_selftest.exe --serve 60 <LSOFRAME-*.dds>       # publish a captured frame (seconds, then the frame)
build\Release\share_selftest.exe --serve 60 --fps 8 <LSOFRAME-rain-*.dds> <hdrBufferMFD32-rain-*.dds>  # a sequence: same-named files are frames
build\Release\share_selftest.exe --serve 42 --passes 1:7,6,4,0 <LSOFRAME-*.dds>  # also fake passes: ownAircraft, then each pass's flags
build\Release\ILARTS.exe --test-run 10 --test-keys "6:!" --test-log out.log  # save what the viewer shows (out.log.6.bmp)
build\Release\night_sky_test.exe "C:\Program Files\Eagle Dynamics\DCS World"  # star ring geometry, night-sky wrappers and modes; patches on the mapped SpaceRenderer.dll (installed twice, refused on changed code)
build\Release\rain_test.exe "C:\Program Files\Eagle Dynamics\DCS World"       # rain / snow per ILARTS view (cameraVel, type, rain while you are in snow, the far views' cloud lookup), modes, lines; scans and patches on the mapped DLLs
build\Release\own_canopy_test.exe "C:\Program Files\Eagle Dynamics\DCS World"  # own-canopy wrappers, fetch probe, log lines; patches on the mapped NGModel.dll (installed twice)
build\Release\plat_ui_test.exe "C:\Program Files\Eagle Dynamics\DCS World"     # PLAT UI state search, real Lua states from DCS's lua.dll
build\Release\origin_split_test.exe "C:\Program Files\Eagle Dynamics\DCS World"  # per-object origin split (models, arresting cables); vtable patches on the mapped Visualizer.dll and enlight.dll
```

`plat_ui_test` loads DCS's `lua.dll` (plain Lua 5.1, read from the install, nothing written) and drives
src/plat_ui.cpp's recording and search with real states: an unknown state probed once per sighting and only while
in use, quiet / closed / other-thread states left alone, coroutines folded into their main thread (a suspended
one resumes intact), the PLAT UI state found, called through the whitelist, kept while idle and found again after
a new sighting.

`share_selftest.exe --serve <seconds> --cams <n> <LSOFRAME.dds>` also stands in for the extra cameras: ISLAND and
the deck slots DECK .. DECK 5, each with n positions (the hook's 15 names, its 6 hangar ones marked as hangar, then
HANGAR 7 ...). A camera starts
rendering 250 ms after it is asked for (frames counted only while it renders), a slot reports a requested position
as current after 120 ms and its picture follows 2 frames later; each picture is grey with one white bar per position
number (the island: a diagonal). It prints each change with its time (`cam DECK 2 on`, `cam DECK view 2 CROTCH`,
`cam DECK 2 off`), and what each camera asked for should follow, with its position (`cam DECK 3 track 1 at HANGAR
FWD`: 1 = nothing), so a test log shows the order of slot changes and that no hangar position gets a track mode.
Each camera's pose (`Header::camPose`, v9) is the hook's CAMERAS value for what it shows; with `--tuning <file>` (an
`ILARTS-cameras.lua`, before the .dds files) the producer reads that file every 0.5 s like the hook and applies its
entries in order, printing `tuning file read: n entries` and each pose change (`cam DECK 2 pose HANGAR FWD pos 54.7
18.7 4 look -60.1 10.5 0 fov 51`).

Camera tuning end to end (scratch copy, its own mapping and `LOCALAPPDATA`): a scratch folder with
`Config\ILARTS-cameras.lua` stands in for Saved Games; `share_selftest --serve 28 --cams 15 --tuning
<dir>\Config\ILARTS-cameras.lua <LSOFRAME.dds>`, then `ILARTS.exe --test-saved-games <dir> --test-commands
--test-script ...` with `[developer] tuning=1` in the scratch ini. A script selects a position
(`_cmd('source', {id: 'hangar_fwd', mon: 0})`), clicks TUNE, presses the panel's keys (pointerdown / pointerup on
`[data-tn=pos][data-ax="1"][data-dir="1"]` ...) and reports the panel's text with a made-up command
(`_cmd('probe', {text})`, logged by `--test-commands`); the log has `tuning: wrote <file> (n entries)` and `tuning:
copied <line>`.

With `--passes`, pass n is under way from 6 + 8 (n - 1) s for 4 s. With `--test-log`, the viewer logs each new pass as
`auto pass <id> flags <f> own <o> filter <filter>: record|skip`.

The viewer's hidden test switches (`--test-record S`, `--test-replay`, `--test-auto`, `--test-run S`,
`--test-keys "t:key,..."`, `--test-cam mask`, `--test-stream`, `--test-log file`) drive it against a
stand-in producer. The summary at the end of a test run names the mission folder and what the storage limit did.
`share_selftest.exe --serve <seconds> [--mission NAME ...] [--mission-every S] <LSOFRAME.dds>` also stands in for
the hook's mission: `passEpoch` and `missionName`, with the next mission (a new epoch) every S seconds (10).
`ILARTS.exe --test-storage <folder> [--test-log file]` builds a recordings folder in `<folder>`, runs the storage
limit on it (ILARTS's and other files, a `.part` leftover, an open file, a stills folder elsewhere, empty mission
and user folders) and checks what is left; the exit code is the number of failures.

### Benchmark

SETTINGS > Benchmark (`[benchmark] next_mission`) arms one run for the next mission start; it measures DCS's own frame
times stage by stage and writes a report. Three parts share a block of their own, `<kMappingName>_Bench`
(`src/bench_protocol.h`): it has to exist before the share does, since stage 1 runs with nothing shared.

- **DLL** (`src/bench.cpp`, Lua `diag.bench(action, ...)`): `armed` reads `[benchmark] next_mission` from the
  `ILARTS.ini` beside the ILARTS.exe it starts (`ViewerExePath`: `bin\viewer_path.txt` or beside the DLL; `launch_viewer`
  uses the same and starts the viewer for an armed run even with `[viewer] auto_start=0`). `begin` makes the block
  (once per process, never unmapped, because the render thread may be writing to it), starts a session (`session` + 1,
  `hookState` holding, the hook's `info` lines plus `dcs=`), and installs the renderer callback with no frame function
  (`EnsureRenderCallback(nullptr)` keeps the share's). From then on `BenchFrame` (render_callback.cpp, event 6 =
  `Renderer::EndFrame`, render thread) writes the QPC time since the previous EndFrame into `frameMs[frames % 16384]`
  and then increments `frames`: no lock, one writer. It stops at `ended`. `tick` (paused flag, `hookTick`), `who`
  (the player changed: new info, `whoSeq` + 1), `release` / `ended`, `poll` (the viewer's command, seconds since it
  last looked, its stage text) and `disarm`.
- **Hook** (`BENCHMARK`, `benchStep`): when armed at `onSimulationStart`, the clouds probe is not installed there and
  `step()` returns before anything starts (sharing, keep PLAT, synthetic PLAT, PLAT UI, map objects, PLAT origin, own
  canopy, cameras) until the viewer sends `release`; `START_DELAY` runs meanwhile. What runs in stage 1: the DLL
  loaded, its renderer callback (counters and the frame timer), the compat check, the hook's per-frame callback (it
  returns at once) and its once-a-second step (compat gate, `benchStep`: pause flag, player check, poll), and the
  viewer process itself. No viewer within `BENCH_VIEWER_WAIT` (60 s), or none for 20 s while holding: it disarms the
  switch and starts as usual. `onSimulationStop` and the error path report `ended`; ILARTS.log gets `BENCH` lines for
  every state and stage change.
- **Viewer** (`viewer/bench.inc`, `BenchTick` every loop): opens the block, stamps `viewerTick`, and starts a run when
  it sees a session holding that it has not run (`doneSession`); a session already released that it did not start
  (the viewer was restarted during it) is closed with `done`. Starting writes `next_mission=0`. The stages
  (`bench::Plan`): hook only, PLAT / LSO 1 / LSO 2 (sends `release`), then n = 1..5 views, each with recording off
  and on. While a run is active, `CameraNeeded` returns `BenchWantsCamera` (the stage's views: the fixed cameras
  first, then flight deck positions, then hangar ones, at most as many positions as the deck camera has render slots),
  `RecArmed` returns `BenchRecArmed` (the PLAT and each view, in recording stages only), composite mode is off, and
  manual and pass recordings are refused. Nothing is written to the ini, so the user's cameras, REC switches and
  monitors come back by themselves (`CameraReconcile`) when the run ends. **Start:** a run first waits
  (`BenchWait`, stage text `starting`) until the mission has run `kBenchStartS` (10 s) without a break: not paused and
  the player in place (the info's `player=` line says "in an aircraft", or 20 s without one in single player, 60 s in
  multiplayer: spectators, slot choice). A player change before or during stage 1 goes back to that wait. (2026-11-19:
  the first run measured stage 1 at mission start, partly paused on the briefing screen and while DCS was still
  loading, so later stages had better lows than stage 1.) **Pause:** `BenchHalted` = the hook's `paused` flag
  (`DCS.getPause`, once a second) or no hook step for `kBenchHookStaleMs` (2.5 s). While halted, and
  `kBenchResumeS` (3 s) after, every clock stops (settling, measuring, the wait) and no frame time is taken. When a
  pause is seen during a measurement, its last `kBenchRollbackS` (4 s) of frames are taken back with the clock
  (`BenchRollback`, from `marks`: the hook reports up to a second late); a stage is marked PAUSED only when the pause
  may have begun before that (the viewer was stalled). The stage text gets " (paused)", the status `· PAUSED`, the
  hook logs `BENCH DCS paused` / `BENCH DCS running`. Settling before the 20 s measurement: 10 s
  for stage 1 (after the start wait); the PLAT's picture + 8 s for stage 2; every view's picture + 6 s (no picture after 30 s: measured
  anyway, noted); a recording stage starts the recording once every recorded source's channel has pictures, then 5 s.
  Recording stages call `Engine::beginRecording(kBenchRecKey, video::kNullSink, now)` on those channels: the same
  Media Foundation sink writer and encoder as a file, into `video::NullStream` (an `IMFByteStream` that keeps only
  its position and length; asynchronous calls complete at once), so the encoding costs what a recording costs and
  nothing reaches the disk; the engine reports the frames and bytes encoded (`Discarded` event, key `kBenchRecKey`).
  It stops (and reports what it measured) when the session changes, the hook reports `ended` (mission end, no viewer),
  the player changes after stage 1 (during stage 1 the stage starts again), no frame arrives for 5 s, the share goes
  away, or ILARTS closes.
- **Mission** (`deploy\Missions\ILARTS Benchmark.miz`, Caucasus, CVN-71, the player in an F/A-18C parked hot on deck):
  the scene the how-to names. `package.ps1` puts it at `Missions\ILARTS Benchmark.miz` in all three packages (the
  installer's uninstall removes it); `install.ps1` copies it when missing or different and `-Uninstall` removes it
  while it is still identical. Not under `reference/`, which the public export leaves out.
- **Lamp** (`BenchLamp`, `state.bench = {state, tip}` or null): `armed` (`next_mission=1`, no run), `running`,
  `complete`, `error` (stopped, with the reason in the tooltip). The page shows it in the title bar only while there
  is a state (`hidden`, no space taken): armed and complete steady green, running flashing amber (`.blink`), error
  red. A click while armed or running opens SETTINGS scrolled to the Benchmark panel (its frame lit for a moment);
  after a run it sends `{cmd:'benchLamp'}`: the run's report opens (a stopped run's too, if it measured anything) and
  the lamp goes out. The result is kept in memory only: it lasts until clicked or ILARTS closes; arming again
  puts it away. The Benchmark panel's **i** (`SectionTip`, a section's `tip`) is the how-to.
- **Statistics** (`bench::Compute`): average FPS = frames / their summed frame time; 1% and 0.1% low = 1000 / the
  99th and 99.9th percentile frame time (nearest rank). Deltas against stage 1: fps, ms per frame and percent. ILARTS
  CPU = this process's CPU time / wall time / logical processors.
- **Report** (`bench::WriteReport`): `%LOCALAPPDATA%\ILARTS\benchmarks\ILARTS benchmark <date> <time>.html` (one
  self-contained page) and `.json` (every stage's numbers). The header: ILARTS and DCS versions, graphics card (the
  share's adapter LUID), processor, DCS's window (its largest visible top-level window's client area), mission,
  carrier, aircraft and role (the hook's info), recording settings and encoder, and what else ran (black box, streams,
  Spout, virtual cameras, CCTV look).

Tests:

```powershell
ILARTS.exe --test-bench-report <folder> --test-log <file>   # statistics unit tests + a sample report in <folder>; exit code = failures
share_selftest.exe --serve 200 --cams 15 --bench 8 [--bench-end S] [--bench-who S] [--bench-board S] [--bench-pause S:D] [--bench-stall S:D] <LSOFRAME.dds>
ILARTS.exe --test-run 180 --test-offscreen --test-log <file> --test-bench-seconds 4   # with [benchmark] next_mission=1 in its ini
```

`--bench <ms>` makes share_selftest stand in for the hook: the bench block holding, the share hidden (magic 0) until
the viewer releases it, and a thread writing made-up frame times (<ms> + 0.9 once released + 1.1 per camera
rendering, jitter and hitches); `--bench-end S` ends the mission at S s, `--bench-who S` changes the player,
`--bench-board S` keeps the player out of an aircraft until S s, `--bench-pause S:D` sets the paused flag from S s for
D s (the hook keeps stepping, as in DCS), `--bench-stall S:D` stops `hookTick` from S s for D s.
`--test-bench-seconds S` measures S s per stage and shortens the settling in proportion. The test log gets `bench:`
lines: each stage, `measuring`, its numbers, `recording n sources into the null sink`, `null sink: <frames> frames,
<bytes> bytes`, `waiting until the mission has run ...`, `DCS paused` / `DCS running again`, `measurement taken back
to ... s`, and `run complete; report <path>` or `run stopped: <why>`. Each null-sink count arrives from its encoder
after the recording ends, so after the last stage the report waits for them (`BenchReport`, at most 3 s; closing
ILARTS writes it at once): without that the last stage showed 0 encoded frames. A click on the lamp after a run logs
`benchmark report: <path>`. The lamp is checked through the page (`state.bench`, the `[data-b=benchLamp]` button's
`hidden`, its `.lamp`'s computed opacity over a second for the flashing). Run it in the scratch copy (renamed
`kMappingName`, `LOCALAPPDATA` pointed at a scratch folder, which also takes the report).

## DCS compatibility check (after DCS updates)

Every address, offset and expected byte in src/ is for one DCS build (`engine.h: kVerifiedBuild`). At
mission start the DLL runs `compat.cpp`: on the verified build it runs; on another build it compares 69
code fingerprints (`src/compat_sigs.inc`: every patched site, every called function, the code that holds
the offsets it uses, the instructions through which DCS reads the globals it reads; relocated bytes
masked) plus the DCS.exe globalView vtable and slot 31 with the running DCS. All identical: it runs as
usual ("matched"). Any difference: nothing is installed, the hook only publishes the compat block and the
viewer shows "DCS version not compatible" with the version to roll back to. Every install path checks
`CompatOk()`.

After a DCS update that fails the check: find the changed addresses again (the fingerprints in
`src/compat_sigs.inc` show the code around each), update src/, set `kVerifiedBuild`, and regenerate the fingerprints from the new install:

```powershell
python tools\gen_compat.py "C:\Program Files\Eagle Dynamics\DCS World"   # needs pip install pefile capstone
```

## DCS crash dumps

A DCS crash leaves a bundle with `dcs.<date>-<time>.dmp` (a small minidump), `.crash`, `.log` and `.trk`. To see
whether ILARTS was involved:

```powershell
python -I tools\crash_triage.py <dump.dmp> --ilarts-log <ILARTS.log> --build build\Release   # capstone optional
```

- It prints the exception (read / write, target), the registers, and the disassembled code DCS kept around the
  faulting instruction. Then come the faulting thread's stack from RSP, every thread's current instruction, and
  every reference to ILARTS memory.
- ILARTS memory means ILARTS.dll's image plus the stubs, trampolines, private vtable, synthetic camera and patched
  globalView that the log's session for the dump's process id names.
- A reference on the faulting thread's stack is marked LIVE (at or above the crash RSP, in an active frame) or dead
  (below it, left over from a call that had returned).
- `--build` names ILARTS offsets (function, file:line) through dbghelp. Use a build of the same source: the same
  source gives a byte-identical .text, so compare that section with the installed DLL first.
- DCS writes these dumps with MiniDumpScanMemory. The module list keeps only modules referenced from some stack,
  and memory is limited to stacks plus small windows around referenced addresses. ILARTS.dll being listed proves
  nothing by itself.
- Aircraft DLLs such as FA18C.dll are packed on disk, so only the dump's 256 bytes around RIP can be disassembled.
  The 2026-11-19 analysis in docs/FINDINGS.md shows the method.

## Distribution package

```powershell
powershell -ExecutionPolicy Bypass -File deploy\package.ps1 -Version 1.0.27
```

builds, from one staged Saved Games layout (`Mods\tech\ILARTS\bin\...`, `Mods\tech\ILARTS\README.txt`,
`Scripts\Hooks\ILARTS-hook.lua`, `Missions\ILARTS Benchmark.miz` from `deploy\Missions`; nothing at the root, which
a mod manager would copy into Saved Games):
- `dist\DCS-ILARTS_v<version>.zip` for Open Mod Manager: one folder named like the zip, and `modpack.xml` at the
  root (`<Open_Mod_Manager_Package>`: `<install>` = the folder, `<category>`, `<description>` from
  `deploy\mod-description.md`, `<thumbnail>` = `deploy\thumbnail.png` as a one-line PNG data URI). OMM reads the
  version from `_v<version>` in the file name, so versions are numbers and dots;
- `dist\ovgme\DCS-ILARTS.zip` for OvGME / JSGME / by hand: one folder `DCS-ILARTS` (OvGME accepts a zip only with a
  folder entry named like the zip), `README.txt` (`deploy\mod-description.txt`, ASCII: OvGME reads it in the ANSI
  code page) and `VERSION.txt` at the root; OvGME does not install root files;
- the zips are written with "/" in entry names (Windows PowerShell 5.1's Compress-Archive writes "\");
- `dist\DCS-ILARTS-Setup-<version>.exe` with Inno Setup 6 (`deploy\ILARTS.iss`): per user, into the Saved Games
  folder it finds; runs `ILARTS.exe --repair` afterwards; Start menu "DCS ILARTS" and "DCS ILARTS - Repair"
  (`--repair --report`); optional virtual cameras (`--install-cameras`, elevated for that step); uninstall
  removes them (`--uninstall-cameras`). The uninstaller lives in `Mods\tech\ILARTS\uninstall`.

Repair (`viewer/repair.inc`; SETTINGS > Installation > REPAIR, `--repair`): restores the hook from the copy in
`bin`, removes `LSODiagnostic-hook.lua`, refreshes a development DLL / `viewer_path.txt`, appends a block to
`autoexec.cfg` that adds "userhooks" / "server" to whatever is allowed (checked in DCS's Lua 5.1: other entries
kept, nothing doubled), reinstalls a missing camera component, and reports DCS.exe's version against the
DLL's last compatibility result (`[dcs]` in the ini, from `Header::compat`).

Outputs (`viewer/outputs.inc`): Spout senders (Spout2 SDK's SpoutDX in `third_party/spout`, BSD) and virtual
cameras (`vcam/`: ILARTSCam.dll, a Media Foundation media source per source for Windows 11's Frame Server,
fed through `Global\ILARTSCam<i>` - see `vcam/vcam_shared.h`). Both offer the same seven sources, the last
`composite`: up to four `[outputs] tiles` in one picture, laid out like a composite stream (1920 x 1080 for
Spout; the size the application asks for on the camera). A camera component installed before the composite
camera existed lacks its class, and the viewer asks for a reinstall. It refuses to package while a development switch in the hook (`VERBOSE`, test captures, traces,
A/B modes) is on, and it switches the extra cameras off in the packaged hook unless `-Experimental` is
given (they have not been confirmed in DCS yet). The viewer goes into the mod's `bin` folder, where the DLL starts it from when there is
no `viewer_path.txt` (that file is only written by `install.ps1` for development).

### Release build on GitHub Actions and signing

`.github/workflows/build.yml` runs in the public repo only (`Nubs-dev/DCS-ILARTS`; it is skipped in the private
one). A pushed tag `v<version>`, or a manual run with a version, builds `ILARTS`, `lso_viewer` and `ilarts_cam` with
CMake on `windows-latest`, installs Inno Setup if the runner lacks it, runs `package.ps1`, and uploads the three
packages as the artifact `DCS-ILARTS-<version>`.

Signing is SignPath Foundation's free certificate for open-source projects (`docs/CODE-SIGNING.md`, the policy
page they require; the README links it). It runs only when the repo variable `SIGNPATH_ORGANIZATION_ID` is set,
together with `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_POLICY_SLUG` and the secret `SIGNPATH_API_TOKEN`. Two signing
requests, each waiting for an approver in SignPath (up to two hours):
1. `ILARTS.exe`, `ILARTS.dll`, `ILARTSCam.dll` (artifact configuration `binaries`, `deploy/signpath/binaries.xml`),
   copied back into `build\Release` so every package carries the signed files;
2. the installer built from them (`installer`, `deploy/signpath/installer.xml`).

The Inno Setup uninstaller is written at install time and is not signed. To check the public tree builds on its
own, export it (`python -I tools/export_public.py <scratch folder>`) and run the same CMake and `package.ps1`
there.

### Updates (viewer/update.inc)

The update feed is the public repo's GitHub Releases; no server of our own. `kUpdateRepo` (`Nubs-dev/DCS-ILARTS`)
is the only place the repo is named for it.

- **Check.** A worker thread GETs `https://api.github.com/repos/<repo>/releases/latest` with WinHTTP (system proxy),
  unauthenticated, headers `User-Agent: ILARTS-updater/<version>`, `Accept: application/vnd.github+json`,
  `X-GitHub-Api-Version: 2022-11-28`; nothing else about the user. "latest" never returns drafts or pre-releases.
  The answer is read by a small JSON reader (`upd::ParseJson`, depth 64, 2 MB): `tag_name` must be `v<numbers and
  dots>`, `html_url` is used only if it is on `https://github.com/`, and the asset named exactly
  `DCS-ILARTS-Setup-<version>.exe` gives `browser_download_url`, `size` and `digest` (`sha256:<64 hex>`).
- **Schedule** (`[update]` in ILARTS.ini): `next_check` (unix seconds) and `fails`. The first check runs 8 s after start
  when `next_check` has passed; success sets it a day ahead; a failure (offline, HTTP error, unusable answer) waits
  1, 2, 4, 8, 16 h, then a day (`upd::BackoffSeconds`); a 403 / 429 also waits until `x-ratelimit-reset` /
  `retry-after` + 60 s. Silent: only a log line. The release seen last is kept (`latest`, `page`, `asset`, `url`,
  `sha256`, `size`), so the indicator shows at once after a restart. `check` is the setting (SETTINGS > Installation
  > Check for updates, default 1; the installer's task writes it), `skip` the version skipped.
- **Version order** (`upd::CompareVersions`): the first three numbers are the release; more numbers mark a test build of
  it, before the release (`1.0.28 < 1.0.29.1 < 1.0.29.2 < 1.0.29 < 1.0.30`); missing numbers are 0. A development
  build (0.0.0) never checks.
- **Indicator** (`state.update`, ui/README.md): only while a newer, not skipped release is known: a download icon and
  `<version> AVAILABLE`, `<version> · 37%` while downloading, `UPDATE FAILED` (amber) with the reason in the tooltip.
  Its menu comes from the host: INSTALL (`CLOSE DCS TO INSTALL`, disabled, while `DCS.exe` runs; `TRY AGAIN` after a
  failure), RELEASE NOTES, SKIP THIS VERSION; CANCEL while downloading. Without a verifiable installer (no asset, no
  digest) or in a mod-manager installation, RELEASE PAGE replaces INSTALL and RELEASE NOTES.
- **Installation kind.** Installed by the setup program = an Inno uninstaller `Mods\tech\ILARTS\uninstall\unins*.exe`
  beside `bin` (`UninstallFilesDir`). Otherwise (OMM, OvGME, by hand, a development build) the installer would put
  files under the mod manager's feet, so only the release page is offered.
- **Install.** Downloads (https only; WinHTTP follows GitHub's redirect to its storage, never https to http) to
  `%TEMP%\ILARTS-update\<asset>.part`, stops past the announced size, renames, then opens the file without write
  sharing and keeps it open until the installer has started. Checks: size = `size`, SHA-256 (CNG) = the digest,
  Authenticode (`WinVerifyTrust`, no revocation check): an invalid signature is refused; if ILARTS.exe itself is
  validly signed, the installer must be signed by the same subject name (SignPath Foundation, docs/RELEASING.md).
  Then ILARTS closes normally (recordings finished) and, at the very end of `wWinMain`, starts
  `DCS-ILARTS-Setup-<ver>.exe /SILENT /SP- /ILARTSUPDATE=1`: Setup's progress window only (the user chose INSTALL;
  Inno reuses the folder and tasks of the last install; its own "DCS is running" question still appears), and the
  `[Run]` entry with `Check: FromUpdater` starts ILARTS again. The download folder is emptied at the next start.
- **Installer** (`deploy/ILARTS.iss`): task `checkupdates` (checked by default). `CurPageChanged(wpSelectTasks)`, which
  Setup runs in silent installs too, sets the checkbox from the existing `[update] check`; after installing the task
  is written back. `/TASKS=` / `/MERGETASKS=` on the command line win over the ini. Setup's log has a
  `Check for updates:` line.

Tests (in the scratch copy as for the console, with `LOCALAPPDATA` and `TEMP` pointed at scratch folders):

```powershell
ILARTS.exe --test-update-selftest --test-log <file>     # versions, JSON, releases, backoff, URLs; exit code = failures
ILARTS.exe --test-run 9 --test-offscreen --test-log <file> --test-script <file> --test-commands `
           --test-update-feed http://127.0.0.1:8765/api/unsigned --test-update-mode installer --test-update-dcs 0
```

- `--test-update-feed <file | http://127.0.0.1:port/...>` stands in for the API (checked at start, schedule ignored;
  plain http allowed to the loopback address only) and allows the asset on loopback too; nothing downloaded is ever
  started (the log says `update: would start "<path>" /SILENT /SP- /ILARTSUPDATE=1`), and RELEASE NOTES opens nothing.
- `--test-update-version <ver>` the running version; `--test-update-mode installer|zip`; `--test-update-dcs 0|1`
  (DCS taken as running); `--test-update-signer <name>` (ILARTS.exe taken as signed by that name).
- The test log has `update:` lines: `checking <url>`, `latest X, running Y: newer|no update|newer, skipped; installer
  ..., sha256 given|none; install mode installer|release page`, `check failed: <why>; next check in N s`,
  `downloading <url>`, `verify <path>: <bytes>, sha256 <hex>, signature none|valid (<signer>)|INVALID 0x...`,
  `install refused: <why>`, `download cancelled`, `<ver> skipped`.
- A stand-in for GitHub: a small Python `http.server` on 127.0.0.1 answering `/api/<case>` with a release JSON (or 403
  with `x-ratelimit-remaining: 0`, or 500) and `/dl/<case>/<name>` with the bytes. Cases tested on 2026-11-20: newer
  (indicator, menu, notes), install (unsigned, Microsoft-signed, signed with one byte changed = `0x80096010`
  refused), wrong digest, wrong size, no digest / no asset (RELEASE PAGE only), pre-release, same / older version,
  release of the running test build, test build of a newer release, rate limit, 500, offline (closed port), a
  non-loopback http feed (refused), signer mismatch / unsigned setup for a signed ILARTS, cancel, mod-manager mode,
  DCS running, setting off (no request), setting switched off live, skip (and after a restart), cached result after a
  restart, development build (no request).
- The installer's task handling was tested with a copy of `ILARTS.iss` under another `AppId`, without `[Icons]` /
  `[Run]`, installed silently into a scratch folder and uninstalled again.

## Output

The DLL writes:

```text
%LOCALAPPDATA%\ILARTS\ILARTS.log
```

The log is UTF-8, one timestamped line per entry, mirrored to `OutputDebugString` (viewable live with
Sysinternals DebugView). A log over 5 MB is renamed to `ILARTS.old.log` when it is next opened. With
the hook's `VERBOSE = false` (the default) it holds installs, errors, state changes, compat results and
passes; `VERBOSE = true` adds the per-second statistics, dumps, the globalView call counting (`VIEWCALLS`,
installed only when the mission starts with it on) and the original read-only pass, which
contains:

- DCS process and loaded module paths
- `renderer.dll` exported global symbol addresses and pointer values for
  `RenderAPI::pTexMan`, `pRenderApi`, and `pGpuLowAccess`
- Matching export names/addresses from `dx11backend.dll`, `dxgui.dll`, and
  `edSupercarrier.dll`
- Addresses of selected DX11 methods, explicitly without calling them

Please send back `ILARTS.log` if the DLL runs. If it reports a missing module,
that is useful too; it may indicate the module names or load timing differ in your build.
