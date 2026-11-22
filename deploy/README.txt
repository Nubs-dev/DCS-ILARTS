DCS ILARTS
==========

The carrier's ILARTS (PLAT camera) and the two LSO station screens in their own window, from anywhere
in the mission - you never have to go to the LSO platform. Records every approach to its own video,
named after the pilot, the recovery case and the LSO's grade; has an instant replay, a black box that
keeps the minutes before a mishap, and can stream the picture to an RTMP server.

Needs: DCS World 2.9.31.29434 with the Supercarrier module, Windows 10 or 11, and the Microsoft Edge
WebView2 Runtime, which Windows 11 (and an up-to-date Windows 10) already has. ILARTS's window is built on
it. Without it, ILARTS does not open: it says the runtime is missing and offers Microsoft's download page,
https://developer.microsoft.com/microsoft-edge/webview2/ (install the "Evergreen Bootstrapper").

DCS ILARTS is an independent community mod. It is not made, supported or endorsed by Eagle Dynamics.
DCS World and Supercarrier are trademarks of Eagle Dynamics SA.
AI disclosure: DCS ILARTS was built with the help of AI tools (Claude, by Anthropic). The author designed
it, directed the work and tested it in DCS.

MOD COMPATIBILITY: DCS ILARTS has not been tested for compatibility with any other mod (aircraft, terrain,
shader or sound mods, or tools that hook into DCS's graphics such as ReShade, VR toolkits or overlays). If
DCS or another mod misbehaves with ILARTS installed, disable your other mods to find the conflict, then
report it with the log.


INSTALL
-------
Pick one:
  * DCS-ILARTS-Setup-<version>.exe: installs into Saved Games\DCS (choose DCS.openbeta if that is yours),
    adds the Start menu entries "DCS ILARTS" and "DCS ILARTS - Repair", and sets up DCS's permissions
    (step 2 below) by itself. Optional: the virtual cameras (Windows 11; asks for administrator rights).
  * DCS-ILARTS_v<version>.zip: for Open Mod Manager (OMM) - add it to your DCS channel's library.
  * DCS-ILARTS.zip: for OvGME / JSGME, or by hand: copy the "Mods" and "Scripts" folders
    from its DCS-ILARTS folder into Saved Games\DCS.
    The OvGME / JSGME installation is not tested. If you use it, give feedback and report problems
    in the #ilarts-issues forum on Discord: https://discord.gg/qsjedwKHNr

1. !!! THE ZIPS ONLY (OMM, OvGME, by hand) - DO NOT SKIP: start ILARTS once
   (Mods\tech\ILARTS\bin\ILARTS.exe). It says "DCS ILARTS is not set up yet": click Yes. That does step 2
   for you (SETTINGS > Installation > REPAIR does the same). Without it ILARTS does not work in single
   player or when you host.

2. Only if you fly single player or HOST multiplayer missions, DCS has to allow the hook to read the
   mission: Saved Games\DCS\Config\autoexec.cfg gets a short block that adds "userhooks" to
   net.allow_unsafe_api and "server" to net.allow_dostring_in, keeping whatever other mods allow.
   REPAIR (and the installer) add it, after saving a copy as autoexec.cfg.ILARTS-backup.

3. Start DCS and load a mission with a carrier. ILARTS does NOT open when DCS starts: its window
   opens by itself as soon as the mission has loaded, and the pictures follow about 20 seconds later (DCS keeps the keyboard). To start it yourself instead, switch off
   SETTINGS > Window > Start with DCS.

Multiplayer as a client (someone else hosts): no autoexec.cfg change is needed, but the server has to
allow object export in its settings, or the PLAT has no aircraft or ship data. DCS reports LSO grades,
crashes and ejections to the host only, so as a client file names have no grade and the black box does
not save itself after a crash or ejection (an aircraft lost on approach is still caught). Everything
else works.


USING IT
--------
The window is laid out like the ship's ILARTS station: monitors, each with its own row of controls
under it, and a rack of shared controls. Drag the window by its title bar and resize it from its edges;
it arranges everything to fill the window - a 16:9 screen, an ultrawide or half a screen (in a small
window the controls get smaller rather than the pictures).
  - MONITORS (rack): 1 = one monitor; 2 = monitors A and B side by side, each with its own controls;
    1 + 4 = monitor A with four small previews: pick each preview's source with its name (or none),
    click a preview's picture to swap it with A. A <-> B swaps the two pictures. Monitor A is the
    program: the picture the black box and stream use.
  - Under each monitor, the controls for the picture on it:
      SOURCE   what the monitor shows (keys 1-5 for monitor A); the lit key again turns the monitor off
               (NO SOURCE) until a source key is pressed. REC: the VTR records this source.
               DECK: the deck cameras (bow bubble, catapult 1, crotch, elevators 1-3, corral, waist
               bubble, junkyard) and the hangar bay cameras (HANGAR FWD and AFT at the ends, HANGAR
               EL 1-4 across from each elevator; they have no AUTO). The arrows under the source keys
               step to the next one no other monitor shows; click the name for the list (FLIGHT DECK and
               HANGAR DECK, as in the CYCLE and preview lists). Several can be up at once - one per
               monitor - and each renders only while a monitor on screen shows it (up to five at a time).
               CYCLE (shown with a deck camera): the monitor steps through the deck positions ticked
               under the arrow next to it (the list's check boxes; FLIGHT DECK or HANGAR DECK ticks or
               unticks the whole group), one every Interval seconds (2 to 3600). The next position
               starts before the current one stops, so each step is clean (with all five in use, it
               changes in place). It skips positions another monitor shows, starts the interval again
               when you pick a position, holds while you move the PTZ joystick and pauses during an
               instant replay. Each monitor keeps its own cycle.
      LEVELS   AUTO levels, RESET, and the knobs (drag up / down, or turn the mouse wheel over them;
               double-click one to set it back to its default).
               The PLAT and the LSO station screen share one set of levels; each camera has its own.
      PTZ      for a camera: drag the joystick to pan and tilt, hold the left button on it to zoom in,
               the right button to zoom out (or turn the mouse wheel). REST: back to the camera's own
               view, and it stays there. AUTO: back to its view, then it follows what the selector
               under it says - the recovery (each landing aircraft from the round down until it taxis
               out of the wires), the catapult nearest the camera, the active catapult, or catapult
               1-4 (the aircraft on it, through the launch). Moving the joystick pauses AUTO until
               AUTO or REST is pressed. A hangar camera has no AUTO or target: it stays where you
               put it, and its picture never shows rain or snow. FLIR (only with SETTINGS >
               Experimental > FLIR view on): that camera's picture in DCS's infrared view - see
               EXPERIMENTAL below.
  - VTR REMOTE (rack): recording and replay. Its REC row shows every source the VTR records: each
    recording (automatic or manual) gives one file per source with REC on, named with the source at
    the end ("... TRAP PLAT.mp4", "... ISLAND.mp4"). Switching REC on for a camera switches it on.
    COMP (lit): each recording is one file instead, "... COMPOSITE.mp4", 1920 x 1080, with the sources
    with REC on laid out as a composite stream's (one whole, two side by side, three or four in a
    2 x 2 grid; more than four: the first four in source order). Stills follow it too. A change
    applies from the next recording.
    Its small readout shows how many files the recordings folder holds and their size.
  - SHIP: the carrier every monitor shows (hull number, ship's name under it). Step with the arrows
    (or N), or click the readout for the list.
  - A camera renders while a monitor shows it, and while its REC switch is on (DECK: the last deck
    position shown).
  - SETTINGS (title bar): every other setting - recording, replay, CCTV look, black box, stream, window.
    Marked settings take effect after RESTART NOW. OPEN FILE, EXPORT and IMPORT for backup or sharing.
  - ? (title bar): opens the user guide (PDF).
  - The lamp left of ? (title bar) shows the benchmark, only while there is one: see BENCHMARK.
Esc closes the window.

Sources:      1 PLAT camera   2 LSO 1 (station screen)   3 LSO 2 (cue panel)   4 Island   5 Deck
              (keys for monitor A)
Carrier:      N / Shift+N next / previous carrier (or the SHIP arrows)
Picture:      (of the source on program) A auto-levels, +/- exposure, arrows brightness/contrast,
              PgUp/PgDn gamma, R reset
CCTV look:    On by default. C switches it on and off (or Settings): scan lines, a little noise and
              static, slight lens distortion and darker corners, as on a real deck camera's monitor.
              Settings set how strong it is. It is part of what gets recorded and streamed. REC ONLY ("In
              recordings only") keeps the window's picture clean and puts the look only into the
              recordings, the black box and the stream.
Presets:      F5 Night (built in, fixed), F6-F8 your three presets (Ctrl+F6-F8 saves). Click a preset
              button to load it; right-click it to save the current look there, rename it or clear it.
              A preset holds the levels of every picture - the PLAT and each camera - so one press sets
              them all.
Every shortcut above (except the source keys 1-5) can be changed under SETTINGS > Keyboard: click
the binding, press the new key (with Shift / Ctrl / Alt if wanted); Esc cancels, Delete clears.

RECORDING
  Manual: V starts and stops a recording.
  Auto (M, or the Auto button): every approach is recorded by itself - from just before the aircraft
        comes into the PLAT picture until a few seconds after the trap, bolter or wave-off. Files are
        named after the date and time, the pilot, the recovery case and the LSO's grade, e.g.
        "2026-11-13 21-04-10 Viper Case III (OK) wire 3.mp4".
          - Pilot: the player's name; for AI aircraft their callsign.
          - Case: III at night, for a straight-in approach, or in weather below 1000 ft / 5 nm;
            otherwise I, or II when the mission has a ceiling under 3000 ft.
          - Grade: the grade Supercarrier's LSO gives (human pilots only, and only when you fly single
            player or host the mission). It can arrive a few seconds after the video was saved; the
            file is then renamed. Without a grade the name ends in TRAP, BOLTER or WAVE-OFF.
        Closely spaced aircraft each get their own file, recorded at the same time. Aircraft that turn
        into the groove are recorded from the moment they come into the picture (out to 2 nm); long
        straight-in (Case III) approaches from 1.5 nm. A pass recording is about 1 minute at most and
        stays under 20 MB (fits Discord).
  ACFT (VTR REMOTE, next to AUTO): which approaches Auto records. Each press steps to the next:
        MY ACFT     your own aircraft
        MY SECTION  your 2-ship element of your flight (aircraft 1-2 or 3-4 of your group)
        MY FLIGHT   every aircraft of your group
        ALL ACFT    every approach (the default)
        After you leave your aircraft this mission (you land and go to the LSO station, or release
        the slot in multiplayer), the choice keeps following that aircraft's flight until the mission
        ends. With nothing flown yet this mission (spectator, LSO or observer slot), MY ACFT, MY
        SECTION and MY FLIGHT record nothing; the display then shows NO ACFT. Manual recording, the
        replay and the black box are not affected.
  While recording, the VTR REMOTE's REC lamp is red and its counter runs.
  Videos go to your Videos\DCS ILARTS folder, into a folder of their own for each mission: its date, the
  time it started and its name, e.g. "2026-11-17 1432 Case I recovery". Every recording, black box file,
  saved replay and still of that mission goes there; restarting the mission starts a new folder (within
  the same minute: "... (2)"). A folder is only made once the mission has a file. FOLDER on the VTR REMOTE opens the current mission's
  folder (the recordings folder while it has none).

STORAGE LIMIT
  SETTINGS > Recording > Storage limit (off by default) and Maximum size (GB): when a new recording or
  still would take the recordings folder (with all its subfolders, and the stills folder if it is
  elsewhere) over the size, ILARTS deletes the oldest of its own recordings and stills first, then
  removes mission folders left empty. It deletes only files it wrote itself (it lists them in the hidden
  file .ilarts-files in the recordings folder; files from older versions of ILARTS are not on it),
  never your other files and never a file that is being written or is open in another program. If
  nothing more can go, the status says "Storage limit reached".

INSTANT REPLAY
  Space replays the last approach (or the last 30 seconds if there was none) on every monitor:
  A, B and the previews play together.
  In the replay: Space pause/play, Left/Right jump 2 s (one frame when paused), , and . one frame,
  Up/Down speed (1/8x, 1/4x, 1/2x, 1x, 2x, 4x, 8x; shown on screen), Home start again, S save the
  replay as a video (one file per monitor), Esc back to live.

STILLS
  P (or STILL on the VTR REMOTE) saves one picture of every source with REC on, as recorded, to the
  Stills folder in the mission's folder: "ILARTS <date time> PLAT DCS 09-30-12.png" - the source, then the
  mission's clock in DCS (as on the PLAT picture). Another folder: SETTINGS > Recording > Stills folder
  (all stills then go straight into that folder, without a folder per mission).
  With COMP lit it saves one composite picture instead ("... COMPOSITE DCS 09-30-12.png").

BLACK BOX
  On unless you switch it off (BLACK BOX switch): the viewer keeps the last 5 minutes of the picture in
  memory, all the time. When an aircraft crashes, a pilot ejects or is lost within
  5 nm of the carrier, or an aircraft disappears close in on its approach, the minute before and the
  15 seconds after are saved by themselves as "<date time> MISHAP <what> <who>.mp4". B (or "Save now")
  saves everything the black box holds at that moment. Crashes and ejections are reported by DCS only
  when you fly single player or host the mission; the Save key always works.

STREAM
  Up to four live streams to RTMP servers (MediaMTX, nginx-rtmp, Twitch, YouTube ...). In SETTINGS,
  STREAMS, its keys 1-4 choose the stream to set up: its label, server, stream key and source -
      Monitor A    what monitor A shows (no extra load: it shares the black box's encoder)
      Monitor B    what monitor B shows
      PLAT, LSO 1, LSO 2, Island   that picture, whatever the monitors show
      Composite    up to four of the above in one 1920 x 1080 picture: one whole, two side by side,
                   three or four in a 2 x 2 grid
  A label (up to 6 characters, e.g. TWITCH) replaces the number on its key. CLEAR (after a
  confirmation) empties every field of the stream shown; SAVE keeps it.
  The STREAM switch on the rack starts every stream that has a server; the four keys under
  its display (not the keyboard) start or stop one each.
  A lamp is red while that stream is live and blinks while it connects or retries. The display beside
  the switch shows the state (LIVE 2/2 ...) and the bit rate, or the stream that is not live and why
  (2 NO SERVER, CLOSED = the server hung up, e.g. Twitch with a wrong key); point at it for the full
  messages. Start automatically (SETTINGS, Stream) starts the streams once DCS is connected and a
  source has a picture; streams turned off by hand stay off until DCS starts again. Every stream other
  than Monitor A is one more encode on the graphics card and one more upload (2.5 Mbit/s, composite
  6 Mbit/s). Plain rtmp:// only, not rtmps://. The streams carry a silent audio track because some
  services refuse video without one.

CAMERAS AND FRAME RATE
  The island camera and the deck positions are extra views that DCS draws for ILARTS. Each one on screen
  (or recording, streaming or sent to OBS / Discord) makes DCS draw the scene once more and costs DCS
  frame rate. A camera stops when nothing uses it. If DCS gets slow, show fewer cameras (layout 1, or
  previews set to none). Managing this is up to you: ILARTS does not limit it.

BENCHMARK
  What ILARTS costs DCS on your PC. Switch on SETTINGS > Benchmark > Benchmark next mission and start a
  mission: it runs once at the mission start, for about six minutes, then switches itself off.
  How to run it (also the "i" beside the Benchmark panel's name):
    - load Missions\ILARTS Benchmark.miz (installed with ILARTS), a calm, repeatable scene: parked on
      deck in the cockpit of an F/A-18C on CVN-71; do not fly
    - keep DCS in front; do not touch the controls, change the view or pause
    - it starts 10 seconds after the mission is running (unpaused, you in your aircraft)
    - compare each stage with Hook only; two runs differ by a few percent
  ILARTS changes the cameras by itself meanwhile and the status display shows the stage. The lamp in
  the title bar: steady green = armed, flashing amber = running, steady green = report ready, red =
  stopped (its tooltip says why). Click it after a run to open the report and put the lamp out. While
  DCS is paused the benchmark waits (paused frames do not count). Each stage is measured for 20
  seconds after a few seconds to settle:
    1  Hook only            DCS with the ILARTS hook loaded, nothing else of ILARTS started
    2  PLAT, LSO 1, LSO 2   ILARTS started as usual, no cameras
    3+ 1 view .. 5 views    the island camera, then deck positions; each count with recording off, then
                            with every source shown recorded (the PLAT and each view). These recordings
                            are encoded like real ones, but nothing is saved.
  Leaving the mission, changing aircraft or slot, or closing ILARTS stops the run (what was measured is
  still reported). Cameras, monitors and REC switches are as before afterwards; no pass is recorded while
  it runs. The report: a web page and a JSON file in %LOCALAPPDATA%\ILARTS\benchmarks (SETTINGS >
  Benchmark > Last report > OPEN shows the newest, Reports > OPEN the folder). Per stage: the average
  frame rate and the 1% and 0.1% lows (the frame rates 99% and 99.9% of the frames beat), each with its
  change against stage 1, e.g. "-15 fps . +2.3 ms . -13%". The scene in a running mission keeps
  changing, so a difference of a few percent can be noise.

PERFORMANCE
  Every picture ILARTS shows is one DCS draws: the PLAT and LSO screens are DCS's own exports, and each
  island or deck camera makes DCS draw the scene once more. That is nearly all of ILARTS's cost, on the
  graphics card and DCS's render thread. ILARTS itself (viewer, recording, black box) uses a few percent
  of the processor.
  The benchmark on two PCs (benchmark mission, F/A-18C parked on deck), average / 1% low:
    Laptop:  RTX 5070 Ti Laptop GPU, Core Ultra 9 275HX, DCS at 2560 x 1600
    Desktop: RTX 5090, Ryzen 7 9800X3D, DCS at 9984 x 1744 (three cockpit viewports, five MFD / RWR /
             IFEI exports)
                            Laptop                    Desktop
    Hook only               127 / 109 fps             104 / 80 fps
    PLAT, LSO 1, LSO 2      101 /  65 fps  (-21%)      86 / 59 fps  (-17%)
    1 view                   82 /  60 fps  (-36%)      80 / 57 fps  (-23%)
    2 views                  73 /  57 fps  (-43%)      67 / 53 fps  (-36%)
    3 views                  62 /  49 fps  (-52%)      49 / 40 fps  (-52%)
    4 views                  52 /  38 fps  (-60%)      43 / 35 fps  (-59%)
    5 views                  51 /  39 fps  (-60%)      43 / 36 fps  (-58%)
    5 views, all recorded    49 /  35 fps  (-61%)      37 / 30 fps  (-64%)
  These numbers set expectations; they are not a comparison of the two PCs. Their DCS graphics settings
  differ, and the desktop also draws three cockpit viewports and five cockpit display exports.
  - The PLAT, LSO and camera pictures cost the most. With the exports and three views, both PCs ran at
    about half the frame rate they had with the hook only.
  - Recording costs less than a view: up to 2 fps on the laptop, 5 to 8 fps on the desktop from two
    views on. ILARTS's own processor use rose from about 2% to 11% with six sources recorded.
  - The lows drop more than the average. If DCS feels less smooth, the 1% low is the number to watch.
  To keep DCS fast: a camera runs only while something uses it (a monitor or preview, recording, a
  stream, Spout, a virtual camera). Show only what you watch (layout 1, or previews set to none). Run
  the benchmark on your own PC: your settings, resolution and graphics card decide the numbers.

EXPERIMENTAL
  SETTINGS > Experimental switches on features that are not finished or are costly. They are off by
  default; use them at your own risk.
  FLIR view    a FLIR key on each camera's PTZ panel shows DCS's infrared picture for that camera.
               WARNING: a camera in FLIR costs DCS considerably more frame rate than the normal view,
               and DCS's infrared picture may look wrong at times. Every camera starts each session in
               its normal view.

While DCS has the keyboard: Ctrl+Shift+F9 starts/stops a recording, Ctrl+Shift+F10 instant replay,
Ctrl+Shift+F11 saves the black box, Ctrl+Shift+F8 takes a still.


AFTER A DCS UPDATE
------------------
The mod checks the running DCS every time a mission starts. If DCS changed something the mod relies
on, the mod switches itself off and the window shows "DCS version not compatible". DCS itself keeps
working normally. Roll DCS back to the version above, or wait for an update of the mod.
If ILARTS stops working after an update (or after any change), run REPAIR: SETTINGS > Installation >
REPAIR, Start menu > "DCS ILARTS - Repair", or the installer again. It puts back the hook and the
autoexec.cfg permissions, removes an old hook of the previous name, reinstalls the camera component if
virtual cameras are on and it went missing, and tells you whether your DCS version is compatible (as
found at the last mission; "not checked yet" until you start one).


UPDATES
-------
Once a day ILARTS asks GitHub whether there is a newer release. When there is, a download icon and "1.1.0
AVAILABLE" (the new version) appear in the title bar. Click it:
  INSTALL            downloads the setup program, checks that it is the file published with the release,
                     closes ILARTS, runs the setup with your earlier choices (folder, virtual cameras,
                     shortcuts) and starts ILARTS again. Close DCS first: it holds ILARTS's files while it
                     runs ("CLOSE DCS TO INSTALL").
  RELEASE NOTES      opens the release's page on GitHub.
  SKIP THIS VERSION  hides it until a newer version comes out.
Installed with OMM, OvGME or by hand, the menu offers RELEASE PAGE instead of INSTALL: download the new
zip there and install it the way you installed this one. "UPDATE FAILED": the tooltip says why; TRY
AGAIN, or download it from the release page.
To turn the check off: SETTINGS > Installation > Check for updates, or untick "Check for updates" in the
setup program. Installing a new version keeps your choice.


OBS, DISCORD AND OTHER PROGRAMS
-------------------------------
SETTINGS > Outputs:
  Spout            each source ticked is a Spout sender "ILARTS PLAT", "ILARTS Monitor A" ... It hands
                   the picture over on the graphics card - no delay, no encode. In OBS: install the free
                   "Spout2 Plugin for OBS", add a "Spout2 Capture" source, pick the sender.
  Virtual cameras  each source ticked is a webcam "ILARTS PLAT" ... for Discord, Teams, Zoom, OBS (Video
                   Capture Device). Windows 11 only. Once per PC: Camera component > INSTALL (asks for
                   administrator rights). The camera is drawn only while a program actually shows it.
                   "Reinstall the camera component": it is from an older version - INSTALL again, and
                   ILARTS restarts itself.
  Sources: Monitor A / Monitor B (whatever they show), PLAT, LSO 1, LSO 2, Island, Composite.
  Composite        up to four of them (Composite setting) in one picture, laid out as a composite
                   stream's: 1920 x 1080 for Spout, the size the program asks for on a camera.
Discord's own screen share also works: share the ILARTS window.


SETTINGS
--------
ILARTS.ini next to ILARTS.exe (Mods\tech\ILARTS\bin); SETTINGS > OPEN FILE opens it.
[video]     folder, width / height (recording size, default 1024 x 768), fps (30), replay_seconds (how
            much the instant replay keeps, 45), preroll (3), postroll (4), bitrate_kbps (average,
            default 2200; files are at most about this x their length), encoder (auto / hardware /
            software), auto_filter (which approaches Auto records: own / section / flight / all, the
            ACFT key), and
            filename = {date} {time} {callsign} Case {case} {grade}
            how pass recordings are named. Also available: {result} {wire} {board} {type} {pilot}
            {carrier} {pass}. stills_folder (default: empty = Stills in each mission's folder; a folder
            set here takes every still, without a folder per mission). storage_limit (1 = on, 0 = off,
            the default) and storage_limit_gb (50): see STORAGE LIMIT.
            composite (the VTR's COMP: 1 = one composite file per recording, 0 = one per source);
            a composite recording's bitrate is bitrate_kbps scaled to 1920 x 1080 (about 5800).
[blackbox]  minutes (how much it keeps, 5), mishap_before (60) and mishap_after (15) seconds.
[stream]    label (up to 6 characters, shown on its key), url, key, source (a = monitor A, b = monitor B, plat, lso1, lso2, island, composite),
            tiles (a composite's sources, e.g. a,b), bitrate_kbps (2500; also the black box's),
            composite_kbps (6000), audio (1 = silent track, 0 = none), autostart (1 = start once DCS
            is connected and a source has a picture). [stream2] - [stream4]: label, url,
            key, source, tiles of the other streams.
[cctv]      on (1 = the CCTV look, the default; 0 = off);
            grain, scanlines, distortion, vignette: each effect's share (1 = normal, 0 = none);
            recordings_only (1 = the look in recordings, black box and stream only).
[viewer]    antialias (1 = smooth the camera pictures' jagged edges, 0 = off),
            auto_start (1 = DCS opens ILARTS when a mission starts, 0 = only when you start it).
[outputs]   spout, cameras: the sources (a, b, plat, lso1, lso2, island, composite), comma-separated;
            tiles: the composite's sources (a,b).
[experimental] flir (1 = the FLIR key on the cameras' PTZ panels; costly, see EXPERIMENTAL).
[benchmark] next_mission (1 = run the benchmark at the next mission start; set back to 0 when it starts).
[update]    check (1 = check for updates once a day, the default; 0 = never), skip (a version you
            skipped). ILARTS keeps the newest release it saw and when to check next here too.


PRIVACY
-------
DCS ILARTS collects no data: no telemetry, no account. Everything it makes (recordings, stills,
settings, the log) stays on your PC. It connects to the network for two things only: an RTMP stream you
set up yourself, to the server you enter, and the check for updates (unless you switch it off): once a
day it asks GitHub's public API for the newest release, sending nothing about you or your PC, and it
downloads the setup program from GitHub when you click INSTALL.
The log (%LOCALAPPDATA%\ILARTS\ILARTS.log) can show your Windows user name in file paths; it only
leaves your PC if you attach it to a bug report. The window is drawn by Microsoft's WebView2 runtime,
which follows your Windows diagnostic data settings.


UNINSTALL
---------
Installed with the setup program: Windows Settings > Apps > DCS ILARTS (also removes the autoexec.cfg
permissions and the virtual cameras). Otherwise, first in ILARTS: SETTINGS > Installation > DCS
permissions > REMOVE (takes out only the lines ILARTS added to autoexec.cfg), and SETTINGS > Outputs >
Camera component > REMOVE if you installed the virtual cameras. Then disable it in OMM / OvGME, or delete
Saved Games\DCS\Mods\tech\ILARTS and Saved Games\DCS\Scripts\Hooks\ILARTS-hook.lua.


LIMITATIONS AND KNOWN ISSUES
----------------------------
Some parts of the picture DCS works out only for the main viewport (the view on your screen) at your
player location (where you are in DCS: in an aircraft, at the LSO or Air Boss station, and so on). The
PLAT and ILARTS's cameras are separate views and don't get them. So:
- No sun shadows in the PLAT or the cameras, even with you at the LSO station (the island's shadow on
  the deck, for example). DCS draws sun shadows for the main viewport only; DCS's own PLAT has none
  either.
- The deck's light: DCS works out reflections and sky light at your player location and uses them for
  every view, so when you are far from the carrier the deck in the PLAT and the cameras can look darker
  or flatter than when you are near it, most visibly in overcast and rain.
- DCS keeps cloud data only for 200 km around your player location: with the carrier farther away, the
  PLAT and the cameras can show different clouds and rain than the carrier really has.
- Rain: in a warm mission the cameras' rain is lighter while your player location is at about
  2.5-3.5 km (DCS turns part of it into snow for every view when it is near freezing there); under time
  acceleration the streaks are longer.
- Multiplayer clients: no LSO grades in file names, no automatic black box save after a crash or
  ejection (DCS sends those events to the host only).

PROBLEMS
--------
The user guide: Mods\tech\ILARTS\DCS-ILARTS-Guide.pdf (Start menu > "DCS ILARTS - User Guide"), and
the same as a web page: Mods\tech\ILARTS\DCS-ILARTS-Guide.html.
Report it at https://github.com/Nubs-dev/DCS-ILARTS/issues with your ILARTS version (SETTINGS >
Installation > Version) and attach the log: %LOCALAPPDATA%\ILARTS\ILARTS.log
(SETTINGS > Installation > Logs > OPEN opens its folder). For a frame rate problem, attach a benchmark
report as well (see BENCHMARK).
You can also report bugs in the #ilarts-issues forum on Discord: https://discord.gg/qsjedwKHNr
DCS or another mod misbehaves with ILARTS installed: ILARTS has not been tested with other mods. Disable
the other mods one at a time to find the conflict, and report it.
ILARTS says it "can't run on this PC yet" (Microsoft Edge WebView2 Runtime missing): click Yes, install the
Evergreen Bootstrapper from Microsoft's page, then start ILARTS again.
ILARTS says the console stopped working: start it again. If it keeps happening, reinstall the WebView2
Runtime from https://developer.microsoft.com/microsoft-edge/webview2/

LINKS
-----
Downloads, source and problem reports: https://github.com/Nubs-dev/DCS-ILARTS
Bug reports and help on Discord: https://discord.gg/qsjedwKHNr
Videos: https://www.youtube.com/@Nubs-dev
DCS ILARTS is free. If you want to say thank you for it, please consider a donation to the
Pilot Mental Health Campaign: https://www.pmhc.org/
