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

The title bar shows the target, its size, the received frame rate and the image settings.

**Console interface.** The window is the ILARTS station: program monitor, preview bank, control desk.

- **Page.** It is the web component `ui/ilarts-console.js`, which a web page can host as well (API: `ui/README.md`). `ui/index.html` runs it in a WebView2 composed over the Direct3D pictures with DirectComposition (`viewer/console.inc`).
- **Pictures.** The page is cut open at its monitor screens and reports where they are. The viewer draws the program picture, the previews and the replay monitor at those rectangles. Previews are unadjusted, and camera previews go through the camera curve.
- **State and commands.** The state goes to the page as JSON. The page's commands call the same functions as the keys.
- **Input.** Mouse messages are forwarded with `SendMouseInput` (visual hosting). The page's title bar is the window caption (`WM_NCHITTEST`). Keys arrive through the window, WebView2's accelerator keys, or the page.
- **Since 2026-11-14:**
  - The monitor wall (`[viewer] console_layout` 1 / 2 / q, `console_second`): monitors A (program) and B, each with a channel strip (source, REC, levels, PTZ) acting on the picture it shows; a rack of shared modules. A layout solver in the page places everything to fill the window, scaling the controls down to 55 % in small windows. The host draws monitor B (rect `second`) with its picture's own levels and analyses it for auto-levels.
  - Cameras (protocol v7: 6 cameras, 16 shared targets): ISLAND (picture source 30, tracking) and the deck camera's render slots DECK .. DECK 5 (pool 31, 29, 28, 27, 26), each able to show any deck position (`Header::views`). The viewer's sources are logical: ISLAND and each deck position; a position gets a free slot while a monitor on screen shows it (`AssignSlots`, `CameraReconcile`) and gives it back when none does. The 1 + 4 previews are assignable (`[viewer] console_previews`).
  - Keycap lamps and rocker switches. In layout 1 + 4 the previews show only the sources not on monitor A.
  - Per-source REC switches (`[video] record_<source>`): every recording writes one file per armed source; the main channel (as shown) feeds only the replay ring, black box and stream.
  - Per-picture levels: `[LSOFRAME]` for the PLAT, `[camera<i>_picture]` for each camera; `app.set` is the program's group (`UseGroup`).
  - PTZ joystick (protocol v5 `Header::ptz` / `ptzState`, integrated by the DLL each frame).
  - No FLIR (removed: too costly); `ptz[i].pictureType` stays in the protocol and the viewer always writes 0.
  - No ADJ switch: the console keeps every picture's adjustments on (L works in the classic window only).
  - Presets hold every picture's levels (the PLAT's under the plain keys of `[preset<n>]`, camera i's under `camera<i>_<key>`).
  - Settings dialog (`kSettings` in console.inc), with restart through `--wait-pid`.
  - The symbology switch is gone; it is always on.
- **Interface.** The console is the only interface users get (2026-11-15: F2 and `[viewer] ui` removed; the classic window lacks most functions). Without the WebView2 runtime ILARTS offers Microsoft's download page and exits; if WebView2 fails while running, it says so and closes. The classic window remains for the development tests (`--ui classic`, `--ui-selftest`, `--test-presets`, `--snapshot`). The process is per-monitor DPI aware in console mode.
- **Experimental features** (`[experimental]`, SETTINGS > Experimental, marked ⚠): `flir` shows a FLIR key on each camera's PTZ panel; the viewer then writes `ptz[slot].pictureType = 4` for that camera's render slot (`g_flirOn`, not saved: every session starts normal). The packages always ship the island and deck cameras (`EXTRA_CAMERAS = true` is enforced by `deploy\package.ps1`).
- **Build.** The SDK is in `third_party/webview2` (NuGet Microsoft.Web.WebView2 1.0.2903.40; the loader is linked statically). CMake copies `ui/index.html`, `ui/ilarts-console.js` and `ui/fonts` next to the exe.
- **Tests** (`--test-run` and `--test-log` as usual):
  - `--test-offscreen` renders the window off the screen;
  - `--test-script file` takes lines of `seconds<TAB>` followed by one of: JavaScript, `!shot`, `@vk` (a key), `^x,y` (a click) or `?x,y` (a hit test);
  - `--test-commands` logs the page's commands;
  - `--ui console|classic` chooses the interface.

**Classic window.** It has a video pane (aspect ratio locked to the selected feed) and a sidebar with the
controls: feed and mode selectors, switches (adjustments, auto-levels) and sliders
(exposure, brightness, contrast, gamma). Drag a slider, double-click it to reset it, or use the
mouse wheel over it to step it. Every control also has a key. Settings are saved in `build\Release\ILARTS.ini` (an old lso_viewer.ini is copied over once).

| Key | Control |
|---|---|
| 1 / 2 / 3 | feed: PLAT camera / LSO 1 (station screen) / LSO 2 (cue panel) |
| 4 / 5 | feed: extra camera 1 / 2 (island, catapult station) when DCS offers them; selecting one switches it on |
| C | CCTV look on/off (scan lines, grain / static, lens distortion, vignette; strength slider in the sidebar) |
| B | save the black box now |
| L | all adjustments of the picture on program on/off (classic window only; off = DCS image untouched; mode and values are kept) |
| + / - | exposure, 1/3 EV (pipeline mode) |
| Up / Down, Right / Left, PgUp / PgDn | brightness, contrast, gamma |
| A | auto-levels: darkest 0.5% .. brightest 99% stretched, symbology excluded, max gain 25x |
| R | reset adjustments |
| F5 | the built-in Night preset (fixed: +0.91 EV, brightness +0.13, contrast x1.5, gamma 1.2, auto-levels) |
| F6-F8 / Ctrl+F6-F8 | load / save presets 1-3: each holds the levels of every picture (the PLAT and each camera) (sidebar: click = load, Shift+click = save, right-click = save / rename / clear) |
| F1 | hide / show the sidebar (classic window) |
| V | start / stop a manual recording (MP4) |
| M | recording mode: manual / automatic (every approach the PLAT sees, one file each, overlapping ones at the same time) |
| Space | instant replay of the last approach (else the last 30 s); in the replay: Space pause, Left/Right 2 s or one frame, `,` `.` one frame, Up/Down speed, Home start, S save, Esc live |
| Ctrl+Shift+F9 / F10 / F11 | record / instant replay / save the black box while DCS has the keyboard |

The sidebar's sections fold (click a heading; remembered in the ini) and the sidebar scrolls with the mouse
wheel when it is taller than the window (minimum window height 600).

Recording (viewer/video.h): the picture as shown is drawn again into a 1024x768 canvas at 30 fps, read
back, JPEG-encoded into a 45 s replay ring and, while recording, encoded to H.264 MP4 with Media
Foundation (the GPU's hardware encoder when there is one). Automatic mode follows the hook's pass
reports (`Header::pass`): a pass starts when an airplane closing on the deck enters the PLAT camera's
picture within 2 nm and ends at the trap, bolter or wave-off; the file starts 3 s before and ends 4 s
after. Passes that never get below 150 m within 1200 m of the camera (overhead break arrivals) are
dropped. Files go to `Videos\DCS ILARTS`; the `[video]` section of ILARTS.ini changes folder, size, fps,
replay length, pre/post-roll, bitrate, encoder and the file name.

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
picture. Every source but `a` has its own canvas and encoder.

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

PLAT processing applies to PLAT and to the camera window of LSO 1: the LSO station composites
LSOFRAME into x 192..831, y 150..629 of its 1024x768 frame with tint (0, 1.005, 0.752); the viewer
replaces that window with the tinted processed image and leaves the station symbology untouched.

The processing mode is always Pipeline (Levels by itself when DCS does not share hdrBufferMFD32;
`[viewer] mode=levels|off` in ILARTS.ini overrides it for troubleshooting). Processing works on the R11G11B10_FLOAT source, so shadows lift without 8-bit banding. Note: in
night scenes DCS renders the sky as exactly 0 in LSOFRAME, so only rendered light (aircraft lights,
lit surfaces, clouds) can be brought out; there is no hidden detail in pure black.

Testing without DCS:

```powershell
build\Release\share_selftest.exe                                   # sharing self-test, both GPUs
build\Release\share_selftest.exe --serve <LSOFRAME-*.dds> 60       # publish a captured frame
build\Release\ILARTS.exe --auto --snapshot out.bmp --frames 120 # save what the viewer shows
```

The viewer's hidden test switches (`--test-record S`, `--test-replay`, `--test-auto`, `--test-run S`,
`--test-keys "t:key,..."`, `--test-cam mask`, `--test-stream`, `--test-log file`) drive it against a
stand-in producer.

## DCS compatibility check (after DCS updates)

Every address, offset and expected byte in src/ is for one DCS build (`engine.h: kVerifiedBuild`). At
mission start the DLL runs `compat.cpp`: on the verified build it runs; on another build it compares 43
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

## Distribution package

```powershell
powershell -ExecutionPolicy Bypass -File deploy\package.ps1 -Version 1.0.26
```

builds, from one staged Saved Games layout (`Mods\tech\ILARTS\bin\...`, `Mods\tech\ILARTS\README.txt`,
`Scripts\Hooks\ILARTS-hook.lua`; nothing at the root, which a mod manager would copy into Saved Games):
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
fed through `Global\ILARTSCam<i>` - see `vcam/vcam_shared.h`). It refuses to package while a development switch in the hook (`VERBOSE`, test captures, traces,
A/B modes) is on, and it switches the extra cameras off in the packaged hook unless `-Experimental` is
given (they have not been confirmed in DCS yet). The viewer goes into the mod's `bin` folder, where the DLL starts it from when there is
no `viewer_path.txt` (that file is only written by `install.ps1` for development).

## Output

The DLL writes:

```text
%LOCALAPPDATA%\ILARTS\ILARTS.log
```

The log is UTF-8, one timestamped line per entry, mirrored to `OutputDebugString` (viewable live with
Sysinternals DebugView). A log over 5 MB is renamed to `ILARTS.old.log` when it is next opened. With
the hook's `VERBOSE = false` (the default) it holds installs, errors, state changes, compat results and
passes; `VERBOSE = true` adds the per-second statistics, dumps and the original read-only pass, which
contains:

- DCS process and loaded module paths
- `renderer.dll` exported global symbol addresses and pointer values for
  `RenderAPI::pTexMan`, `pRenderApi`, and `pGpuLowAccess`
- Matching export names/addresses from `dx11backend.dll`, `dxgui.dll`, and
  `edSupercarrier.dll`
- Addresses of selected DX11 methods, explicitly without calling them

Please send back `ILARTS.log` if the DLL runs. If it reports a missing module,
that is useful too; it may indicate the module names or load timing differ in your build.
