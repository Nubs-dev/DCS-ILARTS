DCS ILARTS
==========

The carrier's ILARTS (PLAT camera) and the two LSO station screens in their own window, from anywhere
in the mission - you never have to go to the LSO platform. Records every approach to its own video,
named after the pilot, the recovery case and the LSO's grade; has an instant replay, a black box that
keeps the minutes before a mishap, and can stream the picture to an RTMP server.

Needs: DCS World 2.9.31.29434 with the Supercarrier module, Windows 10 or 11. The console interface
uses the Microsoft Edge WebView2 runtime, which Windows 11 (and an up-to-date Windows 10) already has;
without it ILARTS cannot open and offers Microsoft's download page.

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

1. !!! THE ZIPS ONLY (OMM, OvGME, by hand) - DO NOT SKIP: start ILARTS once
   (Mods\tech\ILARTS\bin\ILARTS.exe). It says "DCS ILARTS is not set up yet": click Yes. That does step 2
   for you (SETTINGS > Installation > REPAIR does the same). Without it ILARTS does not work in single
   player or when you host.

2. Only if you fly single player or HOST multiplayer missions, DCS has to allow the hook to read the
   mission: Saved Games\DCS\Config\autoexec.cfg gets a short block that adds "userhooks" to
   net.allow_unsafe_api and "server" to net.allow_dostring_in, keeping whatever other mods allow.
   REPAIR (and the installer) add it, after saving a copy as autoexec.cfg.ILARTS-backup.

3. Start DCS and load a mission with a carrier. ILARTS does NOT open when DCS starts: its window
   opens by itself about 10 seconds after the mission has loaded (DCS keeps the keyboard). To start it yourself instead, switch off
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
    program: the picture the replay, black box and stream use.
  - Under each monitor, the controls for the picture on it:
      SOURCE   what the monitor shows (keys 1-5 for monitor A). REC: the VTR records this source.
               DECK: the deck cameras (bow bubble, catapult 1, crotch, elevators 1-3, corral, waist
               bubble, junkyard). The arrows under the source keys step to the next one no other
               monitor shows; click the name for the list. Several can be up at once - one per monitor -
               and each renders only while a monitor on screen shows it (up to five at a time).
      LEVELS   AUTO levels, RESET, and the knobs (drag up / down, or turn the mouse wheel over them).
               The PLAT and the LSO station screen share one set of levels; each camera has its own.
      PTZ      for a camera: drag the joystick to pan and tilt, hold the left button on it to zoom in,
               the right button to zoom out (or turn the mouse wheel). REST: back to the camera's own
               view, and it stays there. AUTO: back to its view, then it follows what the selector
               under it says - the recovery (each landing aircraft from the round down until it taxis
               out of the wires), the catapult nearest the camera, the active catapult, or catapult
               1-4 (the aircraft on it, through the launch). Moving the joystick pauses AUTO until
               AUTO or REST is pressed. FLIR (only with SETTINGS > Experimental > FLIR view on): that
               camera's picture in DCS's infrared view - see EXPERIMENTAL below.
  - VTR REMOTE (rack): recording and replay. Its REC row shows every source the VTR records: each
    recording (automatic or manual) gives one file per source with REC on, named with the source at
    the end ("... TRAP PLAT.mp4", "... ISLAND.mp4"). Switching REC on for a camera switches it on.
  - SHIP: the carrier every monitor shows. Step with the arrows (or N), or click the readout for the list.
  - A camera renders while a monitor shows it; the island camera also while its REC switch is on.
  - SETTINGS (title bar): every other setting - recording, replay, CCTV look, black box, stream, window.
    Marked settings take effect after RESTART NOW. OPEN FILE, EXPORT and IMPORT for backup or sharing.
Esc closes the window.

Sources:      1 PLAT camera   2 LSO 1 (station screen)   3 LSO 2 (cue panel)   4 Island   5 Deck
              (keys for monitor A)
Carrier:      N / Shift+N next / previous carrier (or the SHIP arrows)
Picture:      (of the source on program) A auto-levels, +/- exposure, arrows brightness/contrast,
              PgUp/PgDn gamma, R reset
CCTV look:    C switches it on and off (or Settings): scan lines, a little noise and static, slight lens
              distortion and darker corners, as on a real deck camera's monitor. Settings set how strong
              it is. It is part of what gets recorded and streamed. REC ONLY ("In
              recordings only") keeps the window's picture clean and puts the look only into the
              recordings, the black box and the stream.
Presets:      F5 Night (built in, fixed), F6-F8 your three presets (Ctrl+F6-F8 saves). Click a preset
              button to load it; right-click it to save the current look there, rename it or clear it.
              A preset holds the levels of every picture - the PLAT and each camera - so one press sets
              them all.
Every shortcut above (except the source keys 1-9) can be changed under SETTINGS > Keyboard: click
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
  While recording, the VTR REMOTE's REC lamp is red and its counter runs.
  Videos go to your Videos\DCS ILARTS folder (FOLDER on the VTR REMOTE).

INSTANT REPLAY
  Space replays the last approach (or the last 30 seconds if there was none).
  In the replay: Space pause/play, Left/Right jump 2 s (one frame when paused), , and . one frame,
  Up/Down speed (full, 1/2, 1/4), Home start again, S save the replay as a video, Esc back to live.

BLACK BOX
  On unless you switch it off (BLACK BOX switch): the viewer keeps the last 5 minutes of the picture in
  memory, all the time. When an aircraft crashes, a pilot ejects or is lost within
  5 nm of the carrier, or an aircraft disappears close in on its approach, the minute before and the
  15 seconds after are saved by themselves as "<date time> MISHAP <what> <who>.mp4". B (or "Save now")
  saves everything the black box holds at that moment. Crashes and ejections are reported by DCS only
  when you fly single player or host the mission; the Save key always works.

STREAM
  Up to four live streams to RTMP servers (MediaMTX, nginx-rtmp, Twitch, YouTube ...). In SETTINGS,
  STREAMS, keys 1-4 choose the stream to set up: its server, stream key and source -
      Monitor A    what monitor A shows (no extra load: it shares the black box's encoder)
      Monitor B    what monitor B shows
      PLAT, LSO 1, LSO 2, Island   that picture, whatever the monitors show
      Composite    up to four of the above in one 1920 x 1080 picture: one whole, two side by side,
                   three or four in a 2 x 2 grid
  The STREAM switch on the rack starts every stream that has a server; keys 1-4 start or stop one.
  A lamp is red while that stream is live and blinks while it connects or retries. Every stream other
  than Monitor A is one more encode on the graphics card and one more upload (2.5 Mbit/s, composite
  6 Mbit/s). Plain rtmp:// only, not rtmps://. The streams carry a silent audio track because some
  services refuse video without one.

CAMERAS AND FRAME RATE
  The island camera and the deck positions are extra views that DCS draws for ILARTS. Each one on screen
  (or recording, streaming or sent to OBS / Discord) makes DCS draw the scene once more and costs DCS
  frame rate. A camera stops when nothing uses it. If DCS gets slow, show fewer cameras (layout 1, or
  previews set to none). Managing this is up to you: ILARTS does not limit it.

EXPERIMENTAL
  SETTINGS > Experimental switches on features that are not finished or are costly. They are off by
  default; use them at your own risk.
  FLIR view    a FLIR key on each camera's PTZ panel shows DCS's infrared picture for that camera.
               WARNING: a camera in FLIR costs DCS considerably more frame rate than the normal view,
               and DCS's infrared picture may look wrong at times. Every camera starts each session in
               its normal view.

While DCS has the keyboard: Ctrl+Shift+F9 starts/stops a recording, Ctrl+Shift+F10 instant replay,
Ctrl+Shift+F11 saves the black box.


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


OBS, DISCORD AND OTHER PROGRAMS
-------------------------------
SETTINGS > Outputs:
  Spout            each source ticked is a Spout sender "ILARTS PLAT", "ILARTS Monitor A" ... It hands
                   the picture over on the graphics card - no delay, no encode. In OBS: install the free
                   "Spout2 Plugin for OBS", add a "Spout2 Capture" source, pick the sender.
  Virtual cameras  each source ticked is a webcam "ILARTS PLAT" ... for Discord, Teams, Zoom, OBS (Video
                   Capture Device). Windows 11 only. Once per PC: Camera component > INSTALL (asks for
                   administrator rights). The camera is drawn only while a program actually shows it.
  Sources: Monitor A / Monitor B (whatever they show), PLAT, LSO 1, LSO 2, Island.
Discord's own screen share also works: share the ILARTS window.


SETTINGS
--------
ILARTS.ini next to ILARTS.exe (Mods\tech\ILARTS\bin); SETTINGS > OPEN FILE opens it.
[video]     folder, width / height (recording size, default 1024 x 768), fps (30), replay_seconds (how
            much the instant replay keeps, 45), preroll (3), postroll (4), bitrate_kbps (average,
            default 2200; files are at most about this x their length), encoder (auto / hardware /
            software), and
            filename = {date} {time} {callsign} Case {case} {grade}
            how pass recordings are named. Also available: {result} {wire} {board} {type} {pilot}
            {carrier} {pass}.
[blackbox]  minutes (how much it keeps, 5), mishap_before (60) and mishap_after (15) seconds.
[stream]    url, key, source (a = monitor A, b = monitor B, plat, lso1, lso2, island, composite),
            tiles (a composite's sources, e.g. a,b), bitrate_kbps (2500; also the black box's),
            composite_kbps (6000), audio (1 = silent track, 0 = none). [stream2] - [stream4]: url,
            key, source, tiles of the other streams.
[cctv]      grain, scanlines, distortion, vignette: each effect's share (1 = normal, 0 = none);
            recordings_only (1 = the look in recordings, black box and stream only).
[viewer]    antialias (1 = smooth the camera pictures' jagged edges, 0 = off),
            auto_start (1 = DCS opens ILARTS when a mission starts, 0 = only when you start it).
[outputs]   spout, cameras: the sources (a, b, plat, lso1, lso2, island), comma-separated.
[experimental] flir (1 = the FLIR key on the cameras' PTZ panels; costly, see EXPERIMENTAL).


UNINSTALL
---------
Installed with the setup program: Windows Settings > Apps > DCS ILARTS (also removes the autoexec.cfg
permissions and the virtual cameras). Otherwise, first in ILARTS: SETTINGS > Installation > DCS
permissions > REMOVE (takes out only the lines ILARTS added to autoexec.cfg), and SETTINGS > Outputs >
Camera component > REMOVE if you installed the virtual cameras. Then disable it in OMM / OvGME, or delete
Saved Games\DCS\Mods\tech\ILARTS and Saved Games\DCS\Scripts\Hooks\ILARTS-hook.lua.


PROBLEMS
--------
The user guide: Mods\tech\ILARTS\DCS-ILARTS-Guide.pdf (Start menu > "DCS ILARTS - User Guide").
Report it at https://github.com/Nubs-dev/DCS-ILARTS/issues and attach the log: %LOCALAPPDATA%\ILARTS\ILARTS.log
DCS or another mod misbehaves with ILARTS installed: ILARTS has not been tested with other mods. Disable
the other mods one at a time to find the conflict, and report it.

LINKS
-----
Downloads, source and problem reports: https://github.com/Nubs-dev/DCS-ILARTS
Videos: https://www.youtube.com/channel/UCsvqdzJiNPpHHsO97z9y0KA
DCS ILARTS is free. If you want to say thank you for it, please consider a donation to the
Pilot Mental Health Campaign: https://www.pmhc.org/
