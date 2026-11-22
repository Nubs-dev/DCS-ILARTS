-- DCS ILARTS hook (GUI hooks environment; runs on DCS's Main thread, NOT the render thread).
-- Loads Saved Games\DCS\Mods\tech\ILARTS\bin\ILARTS.dll as a Lua C module.
--
-- DCS 2.9.25 renders on a separate render thread. Everything that touches DCS's D3D11 context or
-- the renderer's render-target list therefore runs inside a DCS renderer callback (EndFrame, render
-- thread), installed by share_start(). This script only starts/stops that work and logs status.
--
-- Single player, multiplayer host, and multiplayer client (MP_CLIENT). Host / single player read the
-- carriers, aircraft and wind from the mission scripting state ('server'); that needs
-- Saved Games\DCS\Config\autoexec.cfg with
--   net.allow_unsafe_api = {"userhooks"}   and   net.allow_dostring_in = {"mission", "server"}
-- (DCS only lets hooks use net.dostring_in for the states listed there). A client has no scripting
-- state and reads them from the export interface instead (the server must allow world object export).
--
-- Behaviour:
--   ~10 s after mission start : read-only memory pass (no engine calls); start sharing
--                                LSOFRAME / LSOFINALTARGET / LSOSCREEN2 (see viewer\lso_viewer.cpp);
--                                queue one capture of each (performed on the render thread once the
--                                target exists; results in diagnostic.log and captures\)
--   every 30 s                 : log sharing counters
-- Output: %LOCALAPPDATA%\ILARTS\ILARTS.log, captures\
-- Remove this file to disable completely.

-- hdrBufferMFD32: DCS's scene-linear HDR PLAT camera image before symbology and camera look;
-- the viewer rebuilds the PLAT picture from it (exposure / levels) and lays DCS's symbology on top.
-- Slots 5-8 (share_retarget counts them from 0: 4-7) are the extra cameras' pictures, two per camera.
-- 16 slots: the PLAT and station screens, then two per extra camera (LDR, HDR), pointed at a camera's render targets
-- only while it renders ('off' = nothing to copy)
local NAMES = { 'LSOFRAME', 'LSOFINALTARGET', 'LSOSCREEN2', 'hdrBufferMFD32', 'off', 'off', 'off', 'off', 'off', 'off', 'off', 'off',
                'off', 'off', 'off', 'off' }
local MAX_CAMERAS = 6   -- extra cameras the DLL and the shared memory have room for
local START_DELAY = 8   -- s from the mission's first frame before the mod adds any rendering (not during the load)
local CAMERA_START_DELAY = 5  -- s after the PLAT is live before extra cameras start (not during the mission load)
local FIRST_DELAY, STATUS_PERIOD = 10, 30
local CAPTURE_ONCE = false   -- one capture of each PLAT render target to captures\ (testing)

-- 2.9.25 docs name the hook API table 'Sim'; older hooks use 'DCS'. Use whichever exists.
local API = rawget(_G, 'Sim') or rawget(_G, 'DCS')
if not API then return end

local function dcslog(msg) log.write('ILARTS', log.INFO, msg) end

-- Development diagnostics: per-second statistics, dumps and investigation output in diagnostic.log, plus
-- the one-off memory pass and test captures. Off for normal use (installs, errors and state changes are
-- always logged).
local VERBOSE = false

local ok, diag = pcall(function()
    local lfs = require('lfs')
    package.cpath = package.cpath .. ';' .. lfs.writedir() .. 'Mods\\tech\\ILARTS\\bin\\?.dll;'
    return require('ILARTS')
end)
if not ok or type(diag) ~= 'table' then
    log.write('ILARTS', log.ERROR, 'failed to load ILARTS: ' .. tostring(diag))
    return
end
dcslog('loaded ' .. tostring(diag.version) .. ', log: ' .. tostring(diag.logpath()))
if diag.verbose then diag.verbose(VERBOSE) end
local function vlog(msg) if VERBOSE then diag.log(msg) end end
local function vdcslog(msg) if VERBOSE then dcslog(msg) end end

local state = nil   -- per-mission state; nil when idle/disabled

-- Extra cameras on the carrier (EXPERIMENTAL: written 2026-11-13, not yet run in DCS). DCS renders each one
-- like another PLAT, so each costs frame rate; nothing is installed until one is switched on in the viewer.
-- false = the viewer does not offer them at all.
-- pos / look: where the camera sits and the point it looks at, in the carrier's axes (m from the model
-- origin: x forward, y up, z starboard; the flight deck is at y = 20.15, the PLAT camera at -14, 20.5, -14.9,
-- the ramp at x = -160, the island around x -60..-30, z 22..36). fov = horizontal field of view (deg).
-- size = the square render size (px), shown at `aspect`. source = the unused cockpit TV picture source used
-- (30 / 31). type = 0: black-and-white TV like the PLAT (4 = DCS's infrared picture, untested).
-- track = true: from the moment an approaching aircraft crosses the round down the camera follows it, and goes
-- back to `look` once a trapped aircraft starts taxiing out of the wires (or leaves the landing area without
-- stopping), or climbs 50 ft above the deck (bolter, wave-off).
-- It follows like a PTZ camera: it only pans (the tilt stays that of `look`), at pan_rate deg/s with a ramp up
-- and down of pan_ramp s, and it only starts a move when the aircraft is pan_deadband deg off its aim - so the
-- aircraft is not always centred. pan_only = 'right' / 'left': while tracking it only pans that way in the
-- picture (the island camera never swings left to meet the aircraft at the round down; it waits for it).
-- views = { { name, pos, look, fov, hangar }, ... }: positions the camera can be put at (the viewer picks one; the
-- first at the start). A camera with views takes pos / look / fov from the view shown. hangar = true: a hangar
-- deck position (grouped apart in the viewer; it never tracks).
-- To tune a camera while DCS runs, put a file ILARTS-cameras.lua in Saved Games\DCS\Config that returns a
-- table like CAMERAS (same names - a camera's or a view's; only the fields to change); it is re-read within
-- 0.5 s of a change, and a field taken out of it goes back to this table's value. The viewer's DEVELOPER
-- switch (SETTINGS) writes a section of that file from its camera tuning panel.
local EXTRA_CAMERAS = true
local CAMERAS = {
    -- island: where the user put it on 2026-11-13 (port face of the island, 9 m above the deck), framed on the
    -- landing area as in their reference picture
    { name = 'ISLAND', pos = { -41.6, 29.5, 21.6 }, look = { -103.4, 26.2, -4.3 }, fov = 48, aspect = 4 / 3, size = 768, source = 30, type = 0,
      track = true, pan_rate = 60, pan_ramp = 0.3, pan_deadband = 8, pan_only = 'right' },
    -- deck camera: one camera (picture source 31) the viewer puts at one of these views - the arrows under the
    -- monitor that shows it cycle them. Moving it costs no frame rate: it is still one picture for DCS to render.
    -- The positions are where the user put DCS's camera on 2026-11-14, 2 ft (0.61 m) lower as they asked: 1.25 m
    -- above the deck (the junkyard 0.95 m), level; the heading in each comment is from the bow, clockwise.
    -- fov: the bow bubble's set by the user; the others not measured yet (60).
    -- NB: the junkyard was read 3 m from elevator 3 and the corral 3 m from elevator 2, with the same headings.
    -- pool: the picture sources of the deck camera's render slots - one per deck position shown at the same
    -- time (the viewer hands them out and gives them back). A cockpit that uses one of these sources keeps it:
    -- that slot then does not render (EXTRACAM stats: source_busy).
    { name = 'DECK', aspect = 4 / 3, size = 768, source = 31, pool = { 31, 29, 28, 27, 26 }, type = 0, views = {
        { name = 'BOW BUBBLE', pos = { 71.6, 21.4, 6.1 }, look = { -28.1, 21.4, 13.3 }, fov = 70 },     -- 176 deg: aft, slightly to starboard
        { name = 'CATAPULT 1', pos = { 97.4, 21.4, 21.2 }, look = { -1.3, 21.4, 5.4 }, fov = 60 },      -- 189 deg: aft along catapult 1
        { name = 'CROTCH', pos = { 99.0, 21.4, -21.0 }, look = { 6.5, 21.4, 16.9 }, fov = 60 },         -- 158 deg: aft, toward the crotch
        { name = 'ELEVATOR 1', pos = { 43.0, 21.4, 39.2 }, look = { -9.8, 21.4, -45.7 }, fov = 60 },    -- 238 deg: aft and to port
        { name = 'ELEVATOR 2', pos = { -5.0, 21.4, 39.3 }, look = { 65.3, 21.4, -31.8 }, fov = 60 },    -- 315 deg: forward and to port
        { name = 'CORRAL', pos = { -2.2, 21.4, 38.7 }, look = { 65.4, 21.4, -35.0 }, fov = 60 },        -- 313 deg
        { name = 'WAIST BUBBLE', pos = { -21.8, 21.4, -37.4 }, look = { 48.4, 21.4, 33.8 }, fov = 60 }, -- 45 deg: forward, across the waist
        { name = 'ELEVATOR 3', pos = { -84.1, 21.4, 39.4 }, look = { 12.3, 21.4, 12.8 }, fov = 60 },    -- 345 deg: forward past the island
        { name = 'JUNKYARD', pos = { -81.2, 21.1, 39.0 }, look = { 15.1, 21.1, 12.1 }, fov = 60 },      -- 344 deg
        -- Hangar bay (2026-11-18, from DCS's hangar model: docs/FINDINGS.md "Hangar deck geometry"). Floor y = 8.71,
        -- the beams' undersides ~15.5 and the lamps down to 14.9 in the model. The positions below were set by eye in DCS
        -- with the viewer's tuning panel on 2026-11-18 (about 17 m: the model's figures put the cameras in the walls).
        -- Bays: aft x -130..-64, middle -61..3, forward 6..60 (fire door leaves stowed at z +-12.7..17.8 between
        -- them). Elevator doorways (starboard, z ~+16): 1 x 16.5..38.2, 2 -30.8..-9.1, 3 -109.8..-88.2; elevator 4
        -- (port, z ~-16) -115.5..-94.8. Inner walls at z -18 (port) and +17 (starboard).
        -- hangar = true: listed under HANGAR DECK in the viewer, and never follows anything (no AUTO: nothing to
        -- track from below the flight deck); the joystick still turns it.
        { name = 'HANGAR FWD', hangar = true, pos = { 48.2, 17.1, 0.0 }, look = { -60.0, -7.0, 0.0 }, fov = 50 },     -- forward end, aft down the hangar
        { name = 'HANGAR AFT', hangar = true, pos = { -126.2, 17.0, -5.0 }, look = { -20.0, -14.5, 0.0 }, fov = 50 },  -- aft bulkhead, forward
        { name = 'HANGAR EL 1', hangar = true, pos = { 27.4, 17.1, -17.3 }, look = { 27.4, 12.0, -3.5 }, fov = 65 }, -- port wall
        { name = 'HANGAR EL 2', hangar = true, pos = { -20.0, 16.5, -17.3 }, look = { -20.0, 12.5, -5.0 }, fov = 65 }, -- port wall
        { name = 'HANGAR EL 3', hangar = true, pos = { -99.0, 16.9, -15.3 }, look = { -99.0, 12.5, -3.0 }, fov = 65 }, -- port wall, over elevator 4's door
        { name = 'HANGAR EL 4', hangar = true, pos = { -105.2, 17.0, 15.3 }, look = { -105.2, 12.5, 4.0 }, fov = 65 }, -- over elevator 3's door
    } },
}

-- Recovery case for the recordings' names: nil = worked out per pass (night or a straight-in approach or
-- weather below 1000 ft / 5 nm = III; otherwise I, or II with a ceiling under 3000 ft); 1 / 2 / 3 = always that.
local RECOVERY_CASE = nil
-- A crash, ejection or lost pilot within this distance of the PLAT carrier is reported to the viewer (its
-- black box keeps the footage). Host and single player only (mission events).
local MISHAP_RANGE = 9260
local captured = false   -- captures are queued once per DCS session

-- PLAT camera resolution: DCS renders the LSO camera into render-to-texture viewport 32 at 512x512.
-- The DLL resizes that viewport through DCS's own render-thread resize (DCSSceneRenderer::
-- resizeMFDViewport), re-applied if DCS resets it. Set LSO_RESOLUTION = 0 to leave DCS's default.
-- Disabled (0): resizing breaks the PLAT symbology layout (drawn by protected Supercarrier code for 512).
local LSO_VIEWPORT, LSO_RESOLUTION = 32, 0

-- Q4: log whether the LSO camera is available from DCS's view system (read-only observation).
local LSO_CAMERA_PROBE = true

-- Keep the PLAT camera rendering when you leave the LSO station: once DCS has handed out the LSO
-- camera this mission (visit the LSO platform once), the view system keeps returning it after you
-- leave. Restored at mission stop. Set false to get DCS's stock behaviour.
local KEEP_PLAT = true

-- Start the PLAT without visiting the station: DCS only creates the PLAT camera, its cloud renderer and
-- the symbology the first time the LSO view opens. At mission start the hook switches to the LSO view
-- (DCS command iCommandViewLndgOfficer, LAlt+F9), waits until the camera exists, then switches back to
-- the cockpit (iCommandViewCockpit, F1) if you have an aircraft. You see the LSO view for about a second.
-- Values from Input.dll's command table (same values in 2.9.25.21123 and 2.9.31.29434), which stores value + 1 (iCommandNull is 1 there,
-- 0 in DCS): 132 - 1 / 8 - 1. (Sending 132 / 8 gave the Air Boss / external F2 view, 2026-11-09.)
local AUTO_LSO_VIEW = true

-- Start ILARTS.exe (the viewer) when sharing starts, unless it is already running. The window opens without
-- taking the keyboard focus from DCS. Set false to start the viewer yourself (any time; order is free).
local AUTO_START_VIEWER = true

-- Benchmark (ILARTS: SETTINGS > Benchmark, [benchmark] next_mission in ILARTS.ini). When it is armed at a mission
-- start, the hook starts nothing of ILARTS until the viewer has measured DCS with the hook alone (stage 1); then it
-- starts as usual and the viewer runs the other stages (cameras, recording) and writes the report. No viewer within
-- BENCH_VIEWER_WAIT s: ILARTS starts as usual and the benchmark is switched off.
local BENCHMARK = true
local BENCH_VIEWER_WAIT = 60
local CMD_VIEW_LSO, CMD_VIEW_COCKPIT = 131, 7
local AUTO_LSO_BACK_DELAY, AUTO_LSO_TIMEOUT = 0.5, 8

-- route 1: the hooks environment's Export.LoSetCommand; route 2: LoSetCommand inside the export state
local function sendCommand(c, route)
    local ex = rawget(_G, 'Export')
    if route ~= 2 and ex and ex.LoSetCommand then
        local ok, err = pcall(ex.LoSetCommand, c)
        if ok then return true, 'Export.LoSetCommand' end
    end
    local r = net.dostring_in('export', 'LoSetCommand(' .. c .. ') return "ok"')
    if r == 'ok' then return true, 'export state' end
    return false, tostring(r)
end

local function isMultiplayer() return API.isMultiplayer and API.isMultiplayer() or false end

-- Multiplayer: the name of the unit in your slot (world.getPlayer() only works in single player), or
-- nil as a spectator / in a non-aircraft role. Aircraft slot IDs are the unit's mission ID ('id' or
-- 'id_seat' for multi-seat units).
local function mySlotUnitId()
    local okp, slot = pcall(net.get_player_info, net.get_my_player_id(), 'slot')
    return okp and tonumber(tostring(slot or ''):match('^(%d+)')) or nil
end
local function myUnitName()
    local id = mySlotUnitId()
    if not id or not API.getUnitProperty or not API.UNIT_NAME then return nil end
    local okn, name = pcall(API.getUnitProperty, id, API.UNIT_NAME)
    return okn and type(name) == 'string' and name ~= '' and name or nil
end

-- On-screen text in DCS (test phase labels): single player shows it as before; when hosting it goes to
-- your own aircraft only, not to every player on the server.
local function screenText(msg, secs)
    local code
    if isMultiplayer() and not (API.isServer and API.isServer()) then return end   -- client: no scripting state
    if isMultiplayer() then
        local name = myUnitName()
        if not name then return end
        code = string.format('local u = Unit.getByName(%q) if u then '
            .. 'if trigger.action.outTextForUnit then trigger.action.outTextForUnit(u:getID(), %q, %d, true) '
            .. 'else trigger.action.outTextForGroup(u:getGroup():getID(), %q, %d, true) end end',
            name, msg, secs, msg, secs)
    else
        code = string.format('trigger.action.outText(%q, %d, true)', msg, secs)
    end
    pcall(net.dostring_in, 'server', code)
end

local function hasAircraft()
    -- multiplayer: from the slot (the export API answer depends on the server's export settings)
    if isMultiplayer() then return myUnitName() ~= nil end
    local ex = rawget(_G, 'Export')
    if ex and ex.LoGetSelfData then
        local ok, d = pcall(ex.LoGetSelfData)
        if ok then return d ~= nil end
    end
    return net.dostring_in('export', 'return tostring(LoGetSelfData() ~= nil)') == 'true'
end

-- From the mission file (host and client alike), cached per mission: each plane / helicopter unit's group and its
-- number in that group (its place in the editor's list: 1-1 = 1, 1-2 = 2 ...), and each group's units (mission ids).
local function missionUnits()
    if state.missionUnits then return state.missionUnits end
    local mu = { byName = {}, groups = {} }
    state.missionUnits = mu
    local okm, m = pcall(API.getCurrentMission)
    local mis = okm and type(m) == 'table' and m.mission
    if not (mis and mis.coalition) then return mu end
    for _, side in pairs(mis.coalition) do
        for _, country in ipairs(type(side) == 'table' and side.country or {}) do
            for _, cat in ipairs({ 'plane', 'helicopter' }) do
                for _, g in ipairs(country[cat] and country[cat].group or {}) do
                    local list = {}
                    for i, u in ipairs(g.units or {}) do
                        if u.name then mu.byName[u.name] = { group = tostring(g.name or ''), n = i } end
                        list[#list + 1] = { id = u.unitId, n = i }
                    end
                    if g.name then mu.groups[tostring(g.name)] = list end
                end
            end
        end
    end
    return mu
end

-- Q4 overlay: Supercarrier's PLAT symbology (PLATCameraUI.lua) runs in a private Lua state that only
-- updates while you're at the LSO station. The DLL finds that state through its dxgui widget calls
-- (plat_ui), logs what DCS feeds the overlay, and keeps the clock running while the PLAT camera is
-- being kept alive away from the station.
local PLAT_UI = true

-- Q4 overlay experiment (off): give Supercarrier the view-state answers it gets at the LSO station.
-- Tested 2026-11-08: Supercarrier receives them but still stops feeding the overlay away from the
-- station (it decides that from some other state), so this stays off.
local SC_OVERRIDE = false

-- Q4 overlay feed: while the PLAT camera is kept alive away from the station, the hook computes the
-- PLAT symbology values itself (clock, aircraft distance / speed / vertical speed, deck wind, ship
-- attitude) from the mission scripting state and pushes them into the overlay ~10x a second.
-- While you are at the station it compares its values with Supercarrier's (PLATFEED calib lines)
-- and picks the distance / speed definitions that match Supercarrier best.
local PLAT_FEED = true

-- Synthetic PLAT camera (plat_cam.h): DCS's PLAT camera only exists after the LSO view has been opened
-- once and then stays on that station's carrier. With this on, the PLAT renders from mission start
-- without touching your view, on the carrier chosen in the viewer (default: nearest to your camera;
-- visiting a station selects that station's carrier). The viewer draws the PLAT text itself until DCS's
-- own overlay exists. SYNTHETIC_FALLBACK_S: if LSOFRAME has not appeared that many seconds after the
-- start, open the LSO view once the old way (AUTO_LSO_VIEW); 0 = never.
local SYNTHETIC_PLAT = true
local SYNTHETIC_FALLBACK_S = 20

-- Multiplayer as a client (someone else hosts): carriers, aircraft and ship motion come from the export
-- interface (the server must allow world object export), the deck wind from the mission's ground wind.
-- MISSION_WIND_DIR: how the mission file's wind 'dir' is meant ('to' or 'from'). Confirmed 'to'
-- (2026-11-12, ground wind entered as 060/12 in the editor: DCS's own wind at 11 m was 3.144, 5.446 m/s,
-- i.e. air moving toward 060 at 12.2 kt; the 'to' reading gave 3.09, 5.36). Hosted / single player
-- sessions still log both readings against DCS's wind (WINDCHECK lines).
local MP_CLIENT = true
local MISSION_WIND_DIR = 'to'

-- Cloud jitter investigation: record SceneRenderer's calls into the volumetric cloud renderer
-- (CLOUDS lines every 2 s; one renderer-context dump at the station and one away).
local CLOUDS_PROBE = true
-- Cloud blockiness/jitter fix (needs CLOUDS_PROBE). The cloud shader builds each pixel's ray from a point
-- 4 cm in front of the camera, in floats 16+ km from the origin -> rays off by a few percent. 'on' gives
-- the PLAT's cloud pass a ray matrix rebuilt in double (nothing else changed, so the clouds stay put).
-- 'ab' = alternate fix / no fix every 15 s while away (CLOUDREBASE lines), 'on', 'off'.
-- (The previous approach, moving the origin, is still available as diag.clouds('rebase_full').)
local CLOUD_REBASE = 'on'
local CLOUD_REBASE_PERIOD = 15

-- PLAT deck jitter. DCS has ONE render origin per frame, next to YOUR camera; far from it positions are
-- rounded (1.5 cm steps at 180 km), which is the PLAT deck jitter (confirmed 2026-11-10: moving the whole
-- frame's origin to the PLAT steadies the deck and shakes the cockpit instead).
-- Fix (confirmed 2026-11-10 01:25): only the PLAT view gets an origin next to the PLAT camera - its
-- view setup plus the frame-origin copy in the data the scene uses to place that view's objects; the
-- cloud pass is handed a context put back on the frame origin, so the clouds stay where they are.
-- 'on' = always, 'ab' = alternates on / off every 15 s while away (PLATORIGIN test: fix ON/OFF, with
-- on-screen text), 'observe' = installed but never changes anything, 'off' = not installed.
local PLAT_ORIGIN = 'on'
local PLAT_ORIGIN_AB_PERIOD = 15
-- With the fix active (you more than 70 km from the carrier) an object seen both by the views at the carrier and by
-- your own view was placed for one origin in all of them: your view then drew the carrier, full detail, next to
-- your camera (2026-11-10 ghost carrier; 2026-11-19 "airport with runway lights" in the sky), or the PLAT lost
-- the hull. true = such objects are placed per origin (origin_split.h); 'ab' = alternates split / no split every
-- 15 s (PLATORIGIN test lines, on-screen text) to see the difference; false = DCS's single placement.
local PLAT_ORIGIN_SPLIT = true
-- Your own aircraft's canopy in the PLAT and the extra cameras. While you sit in the cockpit DCS hides your
-- external model's canopy, frame and pilot (draw argument 114) in every view of the frame; true draws them in
-- ILARTS's views as seen from outside (DCS's own views unchanged). Needs PLAT_ORIGIN. false = not installed.
local OWN_CANOPY = true
-- Stars in the PLAT and the extra cameras. DCS puts its stars 100 km out and ILARTS's views end at 80 km, so a
-- zoomed-out camera showed them only toward its corners: a ring of short dashes around an empty centre (2026-11-19,
-- night_sky.h). 'auto' = stars tied to a camera's zoom, as a longer lens shows fainter stars: none at 24 deg or
-- wider, then the brightest first, fading in, the whole star field at 6 deg or narrower; never in the PLAT.
-- 'hide' = no stars in ILARTS's views (a CCTV deck camera does not show them), 'full' = the whole star
-- field (drawn at half the view's far plane, as DCS does for some views), 'stock' = as DCS draws them (the ring).
-- 'auto' and 'full' draw each star as a square point (DCS's sprites came out 1.33x wide in a 4:3 camera picture).
-- DCS's own views are unchanged. Needs PLAT_ORIGIN (it lists the views). 'off' = not installed.
local CAMERA_STARS = 'auto'
-- Rain and snow in the PLAT and the extra cameras. DCS stretches each drop along its motion relative to the camera,
-- but works out the camera's motion only for your own view; the PLAT and the cameras drew with yours, so in flight
-- the drops were long "warp speed" streaks at your aircraft's speed (2026-11-20, rain.h). 'auto' = each of ILARTS's
-- views uses its own camera's motion (the ship's), 'still' = as for a camera at rest, 'hide' = no rain or snow in
-- ILARTS's views, 'stock' = as DCS draws them, 'ab' = alternates auto / stock every 15 s (on-screen text, RAIN test
-- lines) to compare. 'auto' and 'still' also draw the precipitation type of the carrier's temperature (DCS takes rain
-- or snow from your own height: snow in the PLAT of a warm mission while you fly about 3 km up). More than 70 km away,
-- 'auto', 'still' and 'hide' also read the rain density and the cloud shadows over the carrier, not over you.
-- DCS's own views are unchanged. Needs PLAT_ORIGIN (it lists the views). 'off' = not installed.
local CAMERA_RAIN = 'auto'
local CAMERA_RAIN_AB_PERIOD = 15
-- true: while on the F10 map, run DCS's world object search for the PLAT (experiment, see map_objects.h).
-- Set to false if DCS crashes or the map misbehaves.
local MAP_OBJECTS = true
local FEED_PERIOD = 0.1

-- Runs in the mission scripting state ('server'). Prepended: cx, cy, cz (PLAT camera position),
-- doff (Supercarrier's distance reference point, metres forward of the camera along the landing
-- axis), la0, la1, lw (landing area: along-axis range and half-width, metres, relative to the camera),
-- keepShip, startRange, mishapRange, trks (unit names of the aircraft whose passes are being followed),
-- me, mp (the unit in your multiplayer slot or nil; mp = multiplayer: without a slot unit you have no aircraft).
-- Returns 'k=v' tokens: ship values, foul flag, camera in ship frame, the aircraft the overlay follows,
-- near = airplanes within 800 m and 300 m up ("name|al|lat|ht|air|relspeed": the catapults' tracking),
-- cands = airborne airplanes astern within startRange ("name|al|lat|ht|closure|range|type|player|callsign|group|number",
-- ';' separated, names %-encoded; number = the unit's place in its group), me = your aircraft
-- ("name|group|number": world.getPlayer() in single player, the slot's unit in multiplayer; absent = none),
-- trks = the followed ones ("name|al|lat|ht|air|relspeed|range|closure" or
-- "name|gone"), marks = LSO grades since the last call ("unit|comment") and mishaps = crashes / ejections
-- near the carrier since the last call ("kind|who|unit"), both from EVENT_INSTALL's handler.
local FEED_QUERY = [==[
local ok, res = pcall(function()
  local G = _G
  local ship = G.__lsodiag_ship and Unit.getByName(G.__lsodiag_ship)
  if ship and ship:isExist() then
    -- without a chosen carrier (keepShip) the PLAT camera follows the carrier whose LSO station was
    -- visited last: re-pick if it moved away
    local p = ship:getPoint()
    if not keepShip and (p.x - cx) ^ 2 + (p.z - cz) ^ 2 > 500 ^ 2 then ship = nil end
  else
    ship = nil
  end
  if not ship then
    ship = nil
    local bd
    for side = 0, 2 do
      for _, gr in ipairs(coalition.getGroups(side, Group.Category.SHIP) or {}) do
        for _, u in ipairs(gr:getUnits() or {}) do
          local p = u:getPoint()
          local d = (p.x - cx) ^ 2 + (p.z - cz) ^ 2
          if not bd or d < bd then bd, ship = d, u end
        end
      end
    end
    if not ship then return 'noship' end
    G.__lsodiag_ship = ship:getName()
  end
  local P = ship:getPosition()
  local sv = ship:getVelocity()
  local w = atmosphere.getWind({x = P.p.x, y = P.p.y + 30, z = P.p.z})
  local h = math.atan2(P.x.z, P.x.x)
  local ah = h - math.rad(9)                     -- angled deck axis
  local axx, axz = math.cos(ah), math.sin(ah)
  local rtx, rtz = -axz, axx                     -- starboard of the axis
  local rwx, rwz = w.x - sv.x, w.z - sv.z        -- air motion relative to the ship
  local out = {}
  local function put(k, v) out[#out + 1] = k .. '=' .. (type(v) == 'number' and string.format('%.3f', v) or tostring(v)) end
  local function enc(t) return (tostring(t):gsub('[^%w%-_%.]', function(c) return string.format('%%%02X', c:byte()) end)) end
  put('hdg', math.deg(h) % 360)
  put('pitch', math.deg(math.asin(math.max(-1, math.min(1, P.x.y)))))
  put('roll', -math.deg(math.asin(math.max(-1, math.min(1, P.z.y)))))
  put('deck', rwx * axx + rwz * axz)
  put('cross', rwx * rtx + rwz * rtz)
  local ex, ey, ez = cx - P.p.x, cy - P.p.y, cz - P.p.z
  put('camF', ex * P.x.x + ey * P.x.y + ez * P.x.z)
  put('camU', ex * P.y.x + ey * P.y.y + ez * P.y.z)
  put('camS', ex * P.z.x + ey * P.z.y + ez * P.z.z)
  local function axisPos(p)
    local rx, rz = p.x - cx, p.z - cz
    return rx * axx + rz * axz, rx * rtx + rz * rtz, p.y - cy
  end
  local function inLanding(al, lat, ht) return al > la0 and al < la1 and math.abs(lat) < lw and ht < 12 and ht > -15 end
  local foul
  local best, bestAbs
  local approach, near = {}, {}
  for side = 0, 2 do
    for _, cat in ipairs({Group.Category.AIRPLANE, Group.Category.HELICOPTER}) do
      for _, gr in ipairs(coalition.getGroups(side, cat) or {}) do
        for _, u in ipairs(gr:getUnits() or {}) do
          if u:isExist() then
            local p = u:getPoint()
            local al, lat, ht = axisPos(p)
            local air = u:inAir()
            local onDeck = not air and inLanding(al, lat, ht)
            if onDeck and not foul then foul = u:getTypeName() end
            if air and cat == Group.Category.AIRPLANE and al < -20 then
              local dx, dy, dz = p.x - cx, p.y - cy, p.z - cz
              local r = math.sqrt(dx * dx + dy * dy + dz * dz)
              if r < startRange then
                local v = u:getVelocity()
                local cl = -((v.x - sv.x) * dx + (v.y - sv.y) * dy + (v.z - sv.z) * dz) / math.max(r, 1)
                local okp, pn = pcall(u.getPlayerName, u)
                local okc, cs = pcall(u.getCallsign, u)
                local okn, num = pcall(u.getNumber, u)
                approach[#approach + 1] = string.format('%s|%.1f|%.1f|%.1f|%.1f|%.1f|%s|%s|%s|%s|%d', enc(u:getName()), al, lat, ht, cl, r,
                  enc(u:getTypeName()), enc(okp and pn or ''), enc(okc and cs or ''), enc(gr:getName()), okn and tonumber(num) or 0)
              end
            end
            if cat == Group.Category.AIRPLANE and math.abs(al) < 800 and math.abs(lat) < 800 and ht < 300 and ht > -15 then
              local v = u:getVelocity()
              near[#near + 1] = string.format('%s|%.1f|%.1f|%.1f|%d|%.2f', enc(u:getName()), al, lat, ht, air and 1 or 0,
                math.sqrt((v.x - sv.x) ^ 2 + (v.z - sv.z) ^ 2))
            end
            if cat == Group.Category.AIRPLANE then
              local cand = onDeck
              if air and al < 150 and al > -40000 and math.abs(lat) < 0.35 * math.abs(al) + 150
                 and ht < 0.15 * math.abs(al) + 120 and ht > -30 then
                local v = u:getVelocity()
                cand = v.x * axx + v.z * axz > 15
              end
              if cand and (not bestAbs or math.abs(al) < bestAbs) then best, bestAbs = {u, p, al, lat, ht, air}, math.abs(al) end
            end
          end
        end
      end
    end
    if not foul then
      for _, so in ipairs(coalition.getStaticObjects(side) or {}) do
        if so:isExist() then
          local p = so:getPoint()
          if math.abs(p.x - cx) < 400 and math.abs(p.z - cz) < 400 and inLanding(axisPos(p)) then
            foul = 'static ' .. tostring(so:getTypeName())
            break
          end
        end
      end
    end
  end
  put('foul', foul and 1 or 0)
  if foul then put('foulBy', (tostring(foul):gsub('%s', '_'))) end
  if best then
    local u, p, al, lat, ht, air = best[1], best[2], best[3], best[4], best[5], best[6]
    local v = u:getVelocity()
    local dx, dy, dz = p.x - cx, p.y - cy, p.z - cz
    local qx, qz = p.x - (cx + doff * axx), p.z - (cz + doff * axz)
    local aw = atmosphere.getWind(p)
    local r = math.sqrt(dx * dx + dy * dy + dz * dz)
    put('ac', 1)
    put('r', r)
    put('d', math.sqrt(qx * qx + dy * dy + qz * qz))
    put('g', math.sqrt(v.x ^ 2 + v.y ^ 2 + v.z ^ 2))
    put('a', math.sqrt((v.x - aw.x) ^ 2 + (v.y - aw.y) ^ 2 + (v.z - aw.z) ^ 2))
    put('c', -((v.x - sv.x) * dx + (v.y - sv.y) * dy + (v.z - sv.z) * dz) / math.max(r, 1))
    put('vy', v.y)
    put('al', al)
    put('lat', lat)
    put('ht', ht)
    put('air', air and 1 or 0)
    put('type', (tostring(u:getTypeName()):gsub('%s', '_')))
    put('name', enc(u:getName()))
    local okp, pn = pcall(u.getPlayerName, u)
    if okp and pn and pn ~= '' then put('pname', enc(pn)) end
  end
  local followed = {}
  for _, name in ipairs(trks or {}) do
    local u = Unit.getByName(name)
    if u and u:isExist() then
      local p = u:getPoint()
      local al, lat, ht = axisPos(p)
      local v = u:getVelocity()
      local dx, dy, dz = p.x - cx, p.y - cy, p.z - cz
      local r = math.sqrt(dx * dx + dy * dy + dz * dz)
      followed[#followed + 1] = string.format('%s|%.1f|%.1f|%.1f|%d|%.2f|%.1f|%.1f', enc(name), al, lat, ht, u:inAir() and 1 or 0,
        math.sqrt((v.x - sv.x) ^ 2 + (v.z - sv.z) ^ 2), r, -((v.x - sv.x) * dx + (v.y - sv.y) * dy + (v.z - sv.z) * dz) / math.max(r, 1))
    else
      followed[#followed + 1] = enc(name) .. '|gone'
    end
  end
  if #followed > 0 then put('trks', table.concat(followed, ';')) end
  local meU
  if me then meU = Unit.getByName(me) elseif not mp and world.getPlayer then meU = world.getPlayer() end
  if meU and meU:isExist() then
    local d = meU:getDesc() or {}
    if d.category == Unit.Category.AIRPLANE or d.category == Unit.Category.HELICOPTER then
      local okg, gn = pcall(function() return meU:getGroup():getName() end)
      local okn, num = pcall(meU.getNumber, meU)
      put('me', enc(meU:getName()) .. '|' .. enc(okg and gn or '') .. '|' .. string.format('%d', okn and tonumber(num) or 0))
    end
  end
  if #approach > 0 then put('cands', table.concat(approach, ';')) end
  if #near > 0 then put('near', table.concat(near, ';')) end
  local mk = G.__ilarts_marks
  if mk and #mk > 0 then
    local t = {}
    for _, m in ipairs(mk) do t[#t + 1] = enc(m.name) .. '|' .. enc(m.comment) end
    G.__ilarts_marks = {}
    put('marks', table.concat(t, ';'))
  end
  local mh = G.__ilarts_mishaps
  if mh and #mh > 0 then
    local t = {}
    for _, m in ipairs(mh) do
      if (m.x - cx) ^ 2 + (m.z - cz) ^ 2 < mishapRange ^ 2 then t[#t + 1] = enc(m.kind) .. '|' .. enc(m.who) .. '|' .. enc(m.name) end
    end
    G.__ilarts_mishaps = {}
    if #t > 0 then put('mishaps', table.concat(t, ';')) end
  end
  return table.concat(out, ' ')
end)
return ok and tostring(res) or ('error ' .. tostring(res))
]==]

-- Runs once per mission in the mission scripting state: an event handler that queues the LSO's grades
-- (S_EVENT_LANDING_QUALITY_MARK: Supercarrier grades human pilots) and aircraft crashes, ejections and lost
-- pilots for FEED_QUERY to pick up. Everything inside pcall: a handler error must never reach the mission.
local EVENT_INSTALL = [==[
local ok, res = pcall(function()
  local G = _G
  if G.__ilarts_ev then return 'already' end
  G.__ilarts_marks, G.__ilarts_mishaps = {}, {}
  local h = {}
  function h:onEvent(e)
    pcall(function()
      if not e or not e.initiator then return end
      local ev, id, u = world.event, e.id, e.initiator
      if id == ev.S_EVENT_LANDING_QUALITY_MARK then
        local q = G.__ilarts_marks
        if #q < 32 then q[#q + 1] = { name = u:getName(), comment = tostring(e.comment or '') } end
      elseif id == ev.S_EVENT_CRASH or id == ev.S_EVENT_EJECTION or id == ev.S_EVENT_PILOT_DEAD then
        if Object.getCategory(u) ~= Object.Category.UNIT then return end
        local d = u:getDesc()
        if not d or (d.category ~= Unit.Category.AIRPLANE and d.category ~= Unit.Category.HELICOPTER) then return end
        local p = u:getPoint()
        local okp, pn = pcall(u.getPlayerName, u)
        local okc, cs = pcall(u.getCallsign, u)
        local who = (okp and pn and pn ~= '' and pn) or (okc and cs and cs ~= '' and cs) or u:getName()
        local kind = (id == ev.S_EVENT_CRASH and 'crash') or (id == ev.S_EVENT_EJECTION and 'ejection') or 'pilot lost'
        local q = G.__ilarts_mishaps
        if #q < 32 then q[#q + 1] = { kind = kind, who = tostring(who), name = u:getName(), x = p.x, z = p.z } end
      end
    end)
  end
  world.addEventHandler(h)
  G.__ilarts_ev = h
  return 'installed'
end)
return ok and tostring(res) or ('error ' .. tostring(res))
]==]

local SPEED_KEYS = { 'g', 'a', 'c' }
-- m from the PLAT camera: an aircraft that turns into the picture starts a pass as soon as it shows, out to
-- PASS_START_RANGE (2 nm: Case I / II, long turns into the groove); one that has been in the picture all along
-- (a Case III straight-in) starts at PASS_STRAIGHT_RANGE (1.5 nm), so its recording stays about a minute. The
-- 2026-11-13 Case I passes came into the picture at 1.7-2.4 km; Case III passes from 2 nm took 67 s.
local PASS_START_RANGE = 3700
local PASS_STRAIGHT_RANGE = 2780
local PASS_TURNIN_MEMORY = 20       -- s: "turned into the picture" = seen outside it within this time

local function camPos()
    local s = diag.lso_camera()
    local x, y, z = s:match('pos %(([-%d.]+), ([-%d.]+), ([-%d.]+)%)')
    if x then return tonumber(x), tonumber(y), tonumber(z) end
end

-- Returns a table of the query's values (q.ac set when an aircraft is tracked), or nil, error.
local function feedQuery()
    local cx, cy, cz = camPos()
    if not cx then return nil, 'no camera position' end
    local f = state.feed
    local names = {}
    for key in pairs(state.passes and state.passes.cur or {}) do names[#names + 1] = string.format('%q', key) end
    -- the aircraft a camera follows after its pass ended (cameraTrack)
    local tc = state.cams and state.cams.trackC
    if tc and not (state.passes and state.passes.cur[tc.key]) then names[#names + 1] = string.format('%q', tc.key) end
    if not f.eventsInstalled then
        f.eventsInstalled = true
        diag.log('EVENTS handler for LSO grades and mishaps: ' .. tostring(net.dostring_in('server', EVENT_INSTALL)))
    end
    local mp = isMultiplayer()
    local me = mp and myUnitName() or nil
    local code = string.format('local cx, cy, cz = %.3f, %.3f, %.3f\nlocal doff, la0, la1, lw = %.3f, %.1f, %.1f, %.1f\nlocal keepShip = %s\n' ..
                               'local startRange, mishapRange = %.0f, %.0f\nlocal trks = {%s}\nlocal me, mp = %s, %s\n',
                               cx, cy, cz, f.distOff, f.la0, f.la1, f.lw, state.sel and 'true' or 'false', PASS_START_RANGE,
                               MISHAP_RANGE, table.concat(names, ','), me and string.format('%q', me) or 'nil', mp and 'true' or 'false') .. FEED_QUERY
    local res = tostring(net.dostring_in('server', code))
    if not res:match('^hdg=') then return nil, res end
    local q = {}
    for k, v in res:gmatch('(%w+)=(%S+)') do q[k] = tonumber(v) or v end
    if q.ac ~= 1 then q.ac = nil end
    local function dec(t) return t and (tostring(t):gsub('%%(%x%x)', function(h) return string.char(tonumber(h, 16)) end)) end
    q.name, q.pname = dec(q.name), dec(q.pname)
    local function recs(t)
        local out = {}
        for rec in tostring(t or ''):gmatch('[^;]+') do
            local fl = {}
            for x in (rec .. '|'):gmatch('([^|]*)|') do fl[#fl + 1] = x end
            out[#out + 1] = fl
        end
        return out
    end
    q.candList, q.trkMap, q.nearList = {}, {}, {}
    for _, a in ipairs(recs(q.near)) do
        q.nearList[#q.nearList + 1] = { key = dec(a[1]), al = tonumber(a[2]), lat = tonumber(a[3]), ht = tonumber(a[4]), air = tonumber(a[5]),
                                        rs = tonumber(a[6]) }
    end
    for _, a in ipairs(recs(q.cands)) do
        local name = dec(a[1])
        q.candList[#q.candList + 1] = { key = name, name = name, al = tonumber(a[2]), lat = tonumber(a[3]), ht = tonumber(a[4]),
                                        c = tonumber(a[5]), r = tonumber(a[6]), type = dec(a[7]), pname = a[8] ~= '' and dec(a[8]) or nil,
                                        callsign = (a[9] or '') ~= '' and dec(a[9]) or nil, group = dec(a[10] or ''), num = tonumber(a[11]) }
    end
    -- your aircraft; in multiplayer the slot's unit before it is in the world counts too (from the mission file)
    if q.me then
        local m = {}
        for x in (tostring(q.me) .. '|'):gmatch('([^|]*)|') do m[#m + 1] = x end
        q.me = { key = dec(m[1]), name = dec(m[1]), group = dec(m[2] or ''), n = tonumber(m[3]) or 0 }
    elseif me then
        local mu = missionUnits().byName[me]
        q.me = { key = me, name = me, group = mu and mu.group or '', n = mu and mu.n or 0 }
    end
    q.markList, q.mishapList = {}, {}
    for _, m in ipairs(recs(q.marks)) do q.markList[#q.markList + 1] = { name = dec(m[1]), comment = dec(m[2]) or '' } end
    for _, m in ipairs(recs(q.mishaps)) do q.mishapList[#q.mishapList + 1] = { kind = dec(m[1]), who = dec(m[2]), name = dec(m[3]) } end
    for _, t in ipairs(recs(q.trks)) do
        local name = dec(t[1])
        if t[2] == 'gone' then
            q.trkMap[name] = { gone = true }
        else
            q.trkMap[name] = { al = tonumber(t[2]), lat = tonumber(t[3]), ht = tonumber(t[4]), air = tonumber(t[5]), rs = tonumber(t[6]),
                               r = tonumber(t[7]), c = tonumber(t[8]) }
        end
    end
    return q
end

-- Last arguments Supercarrier passed to a setter, from plat_ui('log').
local function scArgs(summary, name)
    local inner = summary:match(name .. '=%d+%(([^)]*)%)')
    if not inner then return nil end
    local t = {}
    for v in (inner .. ','):gmatch('([^,]*),') do t[#t + 1] = tonumber(v) or v end
    return t
end

local function bestKey(errs, keys)
    local best, be
    for _, k in ipairs(keys) do
        local e = errs[k]
        if e and e.n >= 5 and (not be or e.sum / e.n < be) then best, be = k, e.sum / e.n end
    end
    return best
end

local function angleDiff(a, b) return (a - b + 540) % 360 - 180 end

-- At the station: compare our values with Supercarrier's (calibration).
local function feedCalibrate(now)
    local summary = tostring(diag.plat_ui('log'))
    local q, err = feedQuery()
    if not q then
        vlog('PLATFEED calib query failed: ' .. tostring(err))
        return
    end
    local f = state.feed
    local yp = scArgs(summary, 'setShipYawPitchRoll')
    if yp and type(yp[1]) == 'number' then f.magOff, f.magOffStation = angleDiff(yp[1], q.hdg), true end
    local dw = scArgs(summary, 'setDeckWind')
    local ad = scArgs(summary, 'setAircraftData')
    local fd = scArgs(summary, 'setFoulDeck')
    if not f.camLogged then
        f.camLogged = true
        vlog(string.format('PLATFEED PLAT camera in ship frame: fwd=%.1f up=%.1f stbd=%.1f m', q.camF, q.camU, q.camS))
    end
    local line = string.format('PLATFEED calib: ship ours hdg=%.1f deck=%.2f cross=%.2f foul=%d%s | sc yaw=%s deckwind=%s foul=%s',
        q.hdg, q.deck, q.cross, q.foul, q.foulBy and ('(' .. q.foulBy .. ')') or '',
        yp and tostring(yp[1]) or '?', dw and table.concat(dw, ',') or '?', fd and tostring(fd[1]) or '?')
    if ad then line = line .. ' | sc aircraft=' .. table.concat(ad, ',') end
    if q.ac then
        line = line .. string.format(' | ours ac %s air=%d d=%.1f r=%.1f g=%.1f a=%.1f c=%.1f vy=%.2f axis(al=%.1f lat=%.1f ht=%.1f)',
            tostring(q.type), q.air, q.d, q.r, q.g, q.a, q.c, q.vy, q.al, q.lat, q.ht)
        if ad and type(ad[1]) == 'number' and ad[1] > 0 then
            for _, k in ipairs(SPEED_KEYS) do
                local e = f.err[k] or { n = 0, sum = 0 }; f.err[k] = e
                e.n, e.sum = e.n + 1, e.sum + math.abs(q[k] - ad[2])
            end
            f.speedKey = bestKey(f.err, SPEED_KEYS) or f.speedKey
        end
    else
        line = line .. ' | ours ac none'
    end
    line = line .. string.format(' | using speed=%s magOff=%.1f', f.speedKey, f.magOff or 0)
    vlog(line)
end

-- ---- Multiplayer client data source (no mission scripting state on a client) -------------------
-- Carriers, aircraft and the ship's motion come from the export interface (Export.LoGetWorldObjects;
-- the server must allow world object export), the wind from the mission's ground wind.
local function isClient() return isMultiplayer() and not (API.isServer and API.isServer()) end

local function isCarrierType(t) return type(t) == 'string' and (t:match('^CVN_7%d$') ~= nil or t == 'Stennis') end

-- Ship / aircraft axes from export heading / pitch / bank (rad): x forward, y up, z starboard.
local function axesHPB(h, p, b)
    local ch, sh, cp, sp, cb, sb = math.cos(h), math.sin(h), math.cos(p), math.sin(p), math.cos(b or 0), math.sin(b or 0)
    local x = { cp * ch, sp, cp * sh }
    local y0 = { -sp * ch, cp, -sp * sh }
    local z0 = { -sh, 0, ch }
    local y = { y0[1] * cb + z0[1] * sb, y0[2] * cb + z0[2] * sb, y0[3] * cb + z0[3] * sb }
    local z = { z0[1] * cb - y0[1] * sb, z0[2] * cb - y0[2] * sb, z0[3] * cb - y0[3] * sb }
    return x, y, z
end

local function worldObjects()
    local ex = rawget(_G, 'Export')
    if not (ex and ex.LoGetWorldObjects) then return nil, 'Export.LoGetWorldObjects unavailable' end
    local ok, objs = pcall(ex.LoGetWorldObjects)
    if not ok or type(objs) ~= 'table' then return nil, 'LoGetWorldObjects: ' .. tostring(objs) end
    return objs
end

-- Mission ground wind as a vector (m/s, the direction the air moves to). MISSION_WIND_DIR says how the
-- mission file's 'dir' is meant; the host logs both readings against DCS's own wind (WINDCHECK).
local function missionWind()
    local okm, m = pcall(API.getCurrentMission)
    local w = okm and type(m) == 'table' and m.mission and m.mission.weather and m.mission.weather.wind
    local g = w and w.atGround
    if not g then return { x = 0, y = 0, z = 0 } end
    local d = math.rad(tonumber(g.dir) or 0)
    if MISSION_WIND_DIR == 'from' then d = d + math.pi end
    local s = tonumber(g.speed) or 0
    return { x = s * math.cos(d), y = 0, z = s * math.sin(d) }
end

-- Your aircraft as a client: the slot's unit. Its key is the export interface's object id (the unit's runtime id), as the
-- candidates' keys are; group and number come from the mission file, else the group from the export interface.
local function clientMe(objs)
    local id = mySlotUnitId()
    local name = myUnitName()
    if not (id and name) then return nil end
    local rid
    if API.UNIT_RUNTIME_ID then
        local okr, r = pcall(API.getUnitProperty, id, API.UNIT_RUNTIME_ID)
        rid = okr and tonumber(r) or nil
    end
    local o = rid and (objs[rid] or objs[tostring(rid)])
    local mu = missionUnits().byName[name]
    return { key = rid and tostring(rid) or name, name = name, group = mu and mu.group or tostring(o and o.GroupName or ''), n = mu and mu.n or 0 }
end

-- A client's candidate's number in its group (the export interface has none): the mission file's unit of that name in
-- that group, else the group's unit whose runtime id is the object's.
local function missionNumber(group, key, name)
    local mu = missionUnits()
    local byName = name and mu.byName[name]
    if byName and byName.group == group then return byName.n end
    if API.getUnitProperty and API.UNIT_RUNTIME_ID then
        for _, u in ipairs(mu.groups[group] or {}) do
            local okr, r = pcall(API.getUnitProperty, u.id, API.UNIT_RUNTIME_ID)
            if okr and r and tostring(tonumber(r)) == tostring(key) then return u.n end
        end
    end
    return 0
end

-- Same fields as FEED_QUERY, from the export interface. Velocities by differencing successive calls.
local function feedQueryClient(cx, cy, cz)
    local objs, err = worldObjects()
    if not objs then return nil, err end
    local f = state.feed
    local now = API.getModelTime()
    f.prevPos = f.prevPos or {}
    local prev, cur = f.prevPos, {}
    f.seen = f.seen or {}   -- id -> last known position, kept 30 s (clientGameEvent: where a crash happened)
    local function vel(id, p)
        f.seen[id] = { t = now, x = p.x, z = p.z }
        cur[id] = { t = now, x = p.x, y = p.y, z = p.z }
        local o = prev[id]
        local dt = o and now - o.t or 0
        if dt <= 0.01 then return (o and o.v) or { x = 0, y = 0, z = 0 } end
        local v = { x = (p.x - o.x) / dt, y = (p.y - o.y) / dt, z = (p.z - o.z) / dt }
        cur[id].v = v
        return v
    end
    -- the carrier under the PLAT camera
    local ship, shipId, bd
    if state.sel and state.sel.id and objs[state.sel.id] and objs[state.sel.id].Position then
        ship, shipId = objs[state.sel.id], state.sel.id   -- the chosen carrier
    end
    for id, o in pairs(ship and {} or objs) do
        if isCarrierType(o.Name) and o.Position then
            local d = (o.Position.x - cx) ^ 2 + (o.Position.z - cz) ^ 2
            if not bd or d < bd then bd, ship, shipId = d, o, id end
        end
    end
    if not ship then return nil, 'noship' end
    local sp = ship.Position
    local sv = vel(shipId, sp)
    local X, Y, Z = axesHPB(ship.Heading or 0, ship.Pitch or 0, ship.Bank or 0)
    local w = missionWind()
    local h = math.atan2(X[3], X[1])
    local ah = h - math.rad(9)
    local axx, axz = math.cos(ah), math.sin(ah)
    local rtx, rtz = -axz, axx
    local rwx, rwz = w.x - sv.x, w.z - sv.z
    local q = {
        hdg = math.deg(h) % 360,
        pitch = math.deg(math.asin(math.max(-1, math.min(1, X[2])))),
        roll = -math.deg(math.asin(math.max(-1, math.min(1, Z[2])))),
        deck = rwx * axx + rwz * axz,
        cross = rwx * rtx + rwz * rtz,
        foul = 0,
        candList = {},
        trkMap = {},
    }
    local function axisPos(p)
        local rx, rz = p.x - cx, p.z - cz
        return rx * axx + rz * axz, rx * rtx + rz * rtz, p.y - cy
    end
    local function inLanding(al, lat, ht) return al > f.la0 and al < f.la1 and math.abs(lat) < f.lw and ht < 12 and ht > -15 end
    local best, bestAbs
    for id, o in pairs(objs) do
        local lvl1 = o.Type and o.Type.level1
        if lvl1 == 1 and o.Position then   -- air
            local p = o.Position
            local v = vel(id, p)
            local al, lat, ht = axisPos(p)
            local rs = math.sqrt((v.x - sv.x) ^ 2 + (v.z - sv.z) ^ 2)
            local onDeck = inLanding(al, lat, ht) and rs < 40
            local air = not (ht < 12 and ht > -15 and rs < 40)
            if onDeck and q.foul == 0 then q.foul, q.foulBy = 1, tostring(o.Name):gsub('%s', '_') end
            if o.Type.level2 == 1 then     -- airplane
                local cand = onDeck
                if air and al < 150 and al > -40000 and math.abs(lat) < 0.35 * math.abs(al) + 150
                   and ht < 0.15 * math.abs(al) + 120 and ht > -30 then
                    cand = v.x * axx + v.z * axz > 15
                end
                if cand and (not bestAbs or math.abs(al) < bestAbs) then best, bestAbs = { o, p, v, al, lat, ht, air, id }, math.abs(al) end
            end
            local key = tostring(id)
            if lvl1 == 1 and o.Type.level2 == 1 and air and al < -20 then
                local dx, dy, dz = p.x - cx, p.y - cy, p.z - cz
                local r = math.sqrt(dx * dx + dy * dy + dz * dz)
                if r < PASS_START_RANGE then
                    q.candList[#q.candList + 1] = { key = key, name = tostring(o.UnitName or o.Name), al = al, lat = lat, ht = ht, r = r,
                        c = -((v.x - sv.x) * dx + (v.y - sv.y) * dy + (v.z - sv.z) * dz) / math.max(r, 1), type = tostring(o.Name),
                        group = tostring(o.GroupName or '') }
                end
            end
            if o.Type.level2 == 1 and math.abs(al) < 800 and math.abs(lat) < 800 and ht < 300 and ht > -15 then
                q.nearList = q.nearList or {}
                q.nearList[#q.nearList + 1] = { key = key, al = al, lat = lat, ht = ht, air = air and 1 or 0, rs = rs }
            end
            if f.trackIds and f.trackIds[key] then
                -- on deck: at deck height over the deck (the export interface has no in-air flag); 1.5 m
                -- above the PLAT camera's height on deck in the 2026-11-10 logs, 5+ m just before touchdown
                local deck = ht < 3.5 and ht > -5 and al > f.la0 - 20 and al < f.la1 + 150 and math.abs(lat) < 45
                local dx, dy, dz = p.x - cx, p.y - cy, p.z - cz
                local r = math.sqrt(dx * dx + dy * dy + dz * dz)
                q.trkMap[key] = { al = al, lat = lat, ht = ht, air = deck and 0 or 1, rs = rs, r = r,
                                  c = -((v.x - sv.x) * dx + (v.y - sv.y) * dy + (v.z - sv.z) * dz) / math.max(r, 1) }
            end
        end
    end
    for key in pairs(f.trackIds or {}) do
        if not q.trkMap[key] then q.trkMap[key] = { gone = true } end
    end
    for id, s in pairs(f.seen) do
        if now - s.t > 30 then f.seen[id] = nil end
    end
    f.prevPos = cur
    q.me = clientMe(objs)
    if best then
        local o, p, v, al, lat, ht, air, oid = unpack(best)
        local dx, dy, dz = p.x - cx, p.y - cy, p.z - cz
        local qx, qz = p.x - (cx + f.distOff * axx), p.z - (cz + f.distOff * axz)
        local r = math.sqrt(dx * dx + dy * dy + dz * dz)
        q.ac, q.r, q.d = 1, r, math.sqrt(qx * qx + dy * dy + qz * qz)
        q.g = math.sqrt(v.x ^ 2 + v.y ^ 2 + v.z ^ 2)
        q.a = math.sqrt((v.x - w.x) ^ 2 + v.y ^ 2 + (v.z - w.z) ^ 2)
        q.c = -((v.x - sv.x) * dx + (v.y - sv.y) * dy + (v.z - sv.z) * dz) / math.max(r, 1)
        q.vy, q.al, q.lat, q.ht, q.air = v.y, al, lat, ht, air and 1 or 0
        q.type = tostring(o.Name):gsub('%s', '_')
        q.name, q.oid = tostring(o.UnitName or o.Name), oid
    end
    return q
end

-- PLAT overlay text, formatted like PLATCameraUI.lua (setDate / setTime / setFoulDeck / setDeckWind /
-- setAircraftData / setCarrierID), with the field caps of plat_ui.cpp.
local function carrierIdText(t)
    if t == 'Stennis' then return '74' end
    return (tostring(t or ''):match('%d+')) or 'XX'
end

-- Magnetic minus true heading (deg) for the station's "SH xxx DEG". Supercarrier's own value when it was
-- seen at a station (feedCalibrate); otherwise from your aircraft (export: magnetic vs true yaw), refreshed
-- every 10 s; 0 as a spectator without either.
local function magOffset()
    local f = state.feed
    if f.magOffStation then return f.magOff or 0 end
    local now = API.getModelTime()
    if now >= (f.magNext or 0) then
        f.magNext = now + 10
        local ex = rawget(_G, 'Export')
        local ok, m, d = pcall(function() return ex.LoGetMagneticYaw(), ex.LoGetSelfData() end)
        if ok and type(m) == 'number' and type(d) == 'table' and type(d.Heading) == 'number' then
            f.magOff = (math.deg(m - d.Heading) + 540) % 360 - 180
        end
    end
    return f.magOff or 0
end

-- ---- Passes (the viewer's automatic recording) -----------------------------------------------------
-- A pass starts when an airborne airplane closing on the carrier comes into the PLAT camera's picture within
-- PASS_START_RANGE (the viewer's pre-roll makes the recording start a few seconds before). It ends with
--   trap     on deck and slower than PASS_TRAP_SPEED relative to the ship
--   bolter   touched down and airborne again for a second
--   wave-off past the landing area without touching down, or out of the picture / opening for 3 s
--   other    the aircraft is gone, another carrier was chosen, or PASS_MAX_S passed
--   none     not an approach: it never came below PASS_LOW_M within PASS_LOW_RANGE of the camera without
--            touching down (an overhead break arrival flies up the wake at 800 ft); the viewer drops it
-- The PLAT camera looks aft 0.31 deg to starboard of the reversed landing axis (heading + 170.694 deg =
-- axis + 179.694 deg), pitched 3.6 deg up; its 17 deg field of view spans the picture's WIDTH (4:3), so the
-- half angles are 8.5 deg across and 6.4 deg up/down. Measured 2026-11-13 (AI Case I passes, Tacview against
-- the recording): the aircraft appeared at the picture's edge at 8.0-8.1 deg off the camera axis. The first
-- gate (14 / 11 deg) started recordings ~10 s before the aircraft showed.
local PASS_HALF_H, PASS_HALF_V = 8.5, 6.4
local PASS_CAM_YAW = 0.306          -- deg, camera axis vs the reversed landing axis (port approaches read more)
local PASS_CAM_PITCH = 3.6
local PASS_TRAP_SPEED = 8           -- m/s
local PASS_MAX_S = 150
local PASS_LOW_M, PASS_LOW_RANGE = 150, 1200
local PASS_RESULT = { trap = 1, bolter = 2, waveoff = 3, other = 4, none = 5 }
local missionCueInfo   -- defined with the cue panel below (board numbers by unit name)
local cameraTrack      -- defined with the extra cameras below
local cameraViews

local function passInFrame(al, lat, ht, scale)
    local depth = -al
    if depth < 20 then return false end
    local h = math.deg(math.atan2(-lat, depth)) + PASS_CAM_YAW   -- + = right of the picture centre (port side)
    local v = math.deg(math.atan(ht / depth)) - PASS_CAM_PITCH
    h = math.abs(h)
    return h < PASS_HALF_H * scale and math.abs(v) < PASS_HALF_V * scale
end

local PASS_MAX_CONCURRENT = 4      -- the viewer gets this many (Header::passes)

-- Recovery case the mission's weather allows by day: 1 = ceiling 3000 ft and 5 nm visibility or better,
-- 2 = 1000 ft and 5 nm, 3 = worse. A ceiling = a cloud layer that is broken or overcast (density 6+, or a
-- "Broken" / "Overcast" / rain preset: Preset13 and up).
local function weatherCase()
    if state.wxCase then return state.wxCase end
    local okm, m = pcall(API.getCurrentMission)
    local w = okm and type(m) == 'table' and m.mission and m.mission.weather
    local case = 1
    if type(w) == 'table' then
        local cl = type(w.clouds) == 'table' and w.clouds or {}
        local preset = tostring(cl.preset or '')
        local n = tonumber(preset:match('^Preset(%d+)$'))
        local ceiling = (tonumber(cl.density) or 0) >= 6 or (n and n >= 13) or (preset ~= '' and not n and preset:lower():find('rain') ~= nil)
        local base = (tonumber(cl.base) or 99999) * 3.28084
        local vis = type(w.visibility) == 'table' and tonumber(w.visibility.distance) or 80000
        if w.enable_fog and type(w.fog) == 'table' and tonumber(w.fog.visibility) then vis = math.min(vis, tonumber(w.fog.visibility)) end
        if vis < 9260 or (ceiling and base < 1000) then case = 3
        elseif ceiling and base < 3000 then case = 2 end
    end
    state.wxCase = case
    diag.log('PASS recovery case by the mission weather (day): ' .. case)
    return case
end

local function recoveryCase(turnedIn)
    if RECOVERY_CASE then return RECOVERY_CASE end
    local night = state.feed and state.feed.night
    if night == nil and state.missionStart then
        local hour = math.floor(((state.missionStart + API.getModelTime()) % 86400) / 3600)
        night = hour < 6 or hour >= 19
    end
    local wx = weatherCase()
    if night or wx == 3 or not turnedIn then return 3 end
    return wx
end

-- A mishap for the viewer's black box (the same aircraft at most once in 15 s).
local function reportMishap(now, text, key)
    if not diag.share_mishap then return end
    state.mishaps = state.mishaps or {}
    if key and state.mishaps[key] and now - state.mishaps[key] < 15 then return end
    if key then state.mishaps[key] = now end
    diag.share_mishap(text)
    diag.log('MISHAP ' .. text)
end

-- Multiplayer client: crashes, ejections and lost pilots from DCS's multiplayer game events (the ones behind the
-- "... crashed" messages), since the mission's own events reach the host only. They cover players' aircraft only
-- (AI has no player id). Where it happened: the unit's runtime id (DCS.getUnitProperty) is the export interface's
-- object id, whose last position feedQueryClient keeps for 30 s. Inside MISHAP_RANGE of the PLAT camera it goes to
-- the black box like a host's mishap; an unknown position is logged and not reported. Every event is logged
-- (CLIENT EVENT), so a test shows whether a client receives them at all.
local CLIENT_MISHAPS = { crash = 'crash', eject = 'ejection', pilot_death = 'pilot lost' }
local function clientGameEvent(name, playerId, unitId)
    if not (state and state.client) then return end
    local kind = CLIENT_MISHAPS[name]
    if not kind then return end
    local okn, who = pcall(net.get_player_info, playerId, 'name')
    who = (okn and who and who ~= '') and tostring(who) or ('player ' .. tostring(playerId))
    local rid
    if API.getUnitProperty and API.UNIT_RUNTIME_ID and unitId then
        local okr, r = pcall(API.getUnitProperty, unitId, API.UNIT_RUNTIME_ID)
        rid = okr and tonumber(r) or nil
    end
    local seen = state.feed and state.feed.seen
    local pos = rid and seen and (seen[rid] or seen[tostring(rid)])
    local cx, _, cz = camPos()
    local dist = pos and cx and math.sqrt((pos.x - cx) ^ 2 + (pos.z - cz) ^ 2)
    diag.log(string.format('CLIENT EVENT %s player %s (%s) unit %s runtime %s: %s', tostring(name), tostring(playerId), who,
                           tostring(unitId), tostring(rid), dist and string.format('%.0f m from the PLAT camera', dist) or 'position unknown'))
    if dist and dist <= MISHAP_RANGE then reportMishap(API.getModelTime(), kind .. ' ' .. who, rid and tostring(rid) or who) end
end

-- An LSO grade (S_EVENT_LANDING_QUALITY_MARK comment, e.g. "LSO: GRADE:OK  : (LUL)X  WIRE# 3") for the
-- pass of that aircraft: the one under way, else the one that ended most recently.
local function passGrade(now, name, comment)
    local P = state.passes
    local grade = comment:match('GRADE:%s*([^:]-)%s*:') or comment:match('GRADE:%s*(%S+)') or ''
    local wire = comment:match('WIRE#%s*(%d)') or ''
    local c = P and P.cur[name]
    if not c and P then
        for _, e in ipairs(P.ended) do
            if e.key == name and now - (e.tEnd or 0) < 90 then c = e break end
        end
    end
    diag.log(string.format('PASS grade for %s: "%s" wire "%s"%s (%s)', tostring(name), grade, wire,
                           c and (' -> pass ' .. c.id) or ' - no pass of that aircraft', comment))
    if c and grade ~= '' then c.grade, c.wire = grade, wire end
end

-- Camera tracking (EXTRA_CAMERAS): the round down is ~146 m aft of the PLAT camera along the landing axis
-- (unit database: runway centreline from (-43.8, -10.0) to the ramp (-159.7, 9.0); PLAT camera at x -14).
-- The tracking ends when the aircraft climbs TRACK_CLIMB above the deck (deck 20.15, PLAT camera 20.5); when,
-- trapped, it starts taxiing out of the wires: on deck it came to a stop (slower than TRACK_STOP_SPEED relative
-- to the ship) and has since moved TRACK_WIRES_M forward or sideways from the furthest-aft point of that stop
-- (the wire's pull-back rolls it aft, which does not count); when it is on deck, slower than TRACK_TAXI_SPEED and
-- outside the landing area (la0..la1, +-lw) without having stopped; or after TRACK_MAX_S.
-- Catapults (Nimitz class; CoreMods/tech/USS_Nimitz/scripts/USS_Nimitz_RunwaysAndRoutes.lua, GT.RunWays 2-5): the
-- stroke's start (carrier axes x, z), its heading (deg from the bow, clockwise) and length (m).
local CATAPULTS = {
    { 59.954, 18.02, 354.3, 107.8 }, { 58.80, -3.752, 358.0, 108.8 }, { -37.374, -20.162, 355.002, 112.0 }, { -56.176, -32.90, 359.957, 130.0 },
}
-- An airplane is ON a catapult when it is on deck within CAT_ON_ALONG m of the stroke's start along it and
-- CAT_ON_CROSS m across, slower than 3 m/s; it LAUNCHES when it then moves faster than 8 m/s along it. The
-- tracking ends CAT_GONE_ALONG m past the stroke's end or 60 m up, or when it leaves the catapult without launching.
local CAT_ON_ALONG, CAT_ON_CROSS, CAT_GONE_ALONG = { -25, 10 }, 6, 300
-- The viewer's target for each camera (ptz[i].trackMode): 0 = not set (the camera's own `track`), 1 = none (rest),
-- 2 = the recovery (landing area), 3 = the catapult nearest the camera, 4 = the active catapult, 5..8 = catapult 1..4.
local TRACK_TARGETS = { [2] = 'landing', [3] = 'nearest', [4] = 'active', [5] = 1, [6] = 2, [7] = 3, [8] = 4 }

local TRACK_RAMP_AL = -146
local TRACK_CLIMB = 50 * 0.3048
local TRACK_TAXI_SPEED = 15
local TRACK_STOP_SPEED = 1.5        -- m/s
local TRACK_WIRES_M = 3             -- m
local TRACK_MAX_S = 120

local function passEnd(now, c, result, why)
    local P = state.passes
    if not c.touched and (result == 'waveoff' or result == 'other') and (c.minHt or 1e9) > PASS_LOW_M then
        why, result = (why and (why .. ', ') or '') .. string.format('lowest %.0f m near the deck', c.minHt or -1), 'none'
    end
    c.result, c.tEnd = PASS_RESULT[result], now
    P.cur[c.key] = nil
    P.cool[c.key] = true      -- no new pass for this aircraft until it has left the start window
    table.insert(P.ended, 1, c)
    while #P.ended > PASS_MAX_CONCURRENT do table.remove(P.ended) end
    -- a camera that follows this aircraft keeps its position coming until the tracking ends (cameraTrack)
    if state.feed and state.feed.trackIds and not (state.cams and state.cams.trackC == c) then state.feed.trackIds[c.key] = nil end
    diag.log(string.format('PASS %d %s%s: %s %s %s, %.1f s', c.id, result, why and (' (' .. why .. ')') or '', tostring(c.type),
                           c.board ~= '' and c.board or '-', tostring(c.pilot), now - c.t0))
end

-- One followed pass, from its aircraft's state t (q.trkMap).
local function passFollow(now, c, t)
    if not t or t.gone or not t.al then
        -- gone in the groove (close in and low): most likely it hit the ramp or the water
        if (c.range or 1e9) < PASS_LOW_RANGE and (c.minHt or 1e9) < PASS_LOW_M then
            reportMishap(now, 'lost on approach ' .. tostring(c.callsign or c.pilot), c.key)
        end
        return passEnd(now, c, 'other', 'aircraft gone')
    end
    local f = state.feed
    c.range = t.r
    if -t.al < PASS_LOW_RANGE then c.minHt = math.min(c.minHt or 1e9, t.ht) end
    if t.air == 0 then
        if not c.touched then c.touched, c.touchAt = true, now end
        c.airSince = nil
        if t.rs < PASS_TRAP_SPEED then return passEnd(now, c, 'trap') end
    else
        c.airSince = c.airSince or now
        if c.touched and now - c.airSince > 1.0 then return passEnd(now, c, 'bolter') end
        if not c.touched and t.al > f.la1 then return passEnd(now, c, 'waveoff') end
        if not c.touched and t.al < f.la0 then
            if not passInFrame(t.al, t.lat, t.ht, 2) or (t.c or 0) < -5 then
                c.outSince = c.outSince or now
                if now - c.outSince > 3 then return passEnd(now, c, 'waveoff', 'left the approach') end
            else
                c.outSince = nil
            end
        end
    end
    if now - c.t0 > PASS_MAX_S then return passEnd(now, c, 'other', 'timed out') end
end

-- Whose pass it is, for the viewer's automatic recording filter (Header::passFlags): 1 = your aircraft (with 2 and 4),
-- 2 = your section (your 2-ship element: units 1-2, 3-4 ... of your group), 4 = your flight (your group), 0 = none of
-- these, or you have had no aircraft this mission. Against your aircraft now (state.me), so a slot change applies to
-- passes under way; without one (you landed and went to the LSO station, or released the slot) against the last one
-- you had this mission (state.meLast: unit, group name and number, so it works after the group is gone too).
local function passWhose(c)
    local me = state.me or state.meLast
    if not me then return 0 end
    if c.key == me.key then return 7 end
    if me.group == '' or c.group ~= me.group then return 0 end
    if (me.n or 0) > 0 and (c.num or 0) > 0 and math.ceil(me.n / 2) == math.ceil(c.num / 2) then return 6 end
    return 4
end

-- Every feed tick: follow the passes under way, start one for every other airplane that comes into the
-- picture (closely spaced aircraft each get their own; straight-in aircraft only inside PASS_STRAIGHT_RANGE),
-- and let an aircraft whose pass ended start a new one only after it has left the start window.
local function passTick(now, q)
    local P = state.passes
    if not P then
        P = { id = 0, cur = {}, ended = {}, cool = {}, outside = {}, epoch = math.floor(API.getRealTime() * 10) % 1000000000 }
        state.passes = P
    end
    for _, m in ipairs(q.markList or {}) do passGrade(now, m.name, m.comment) end
    for _, m in ipairs(q.mishapList or {}) do reportMishap(now, tostring(m.kind) .. ' ' .. tostring(m.who), m.name) end
    for _, c in pairs(P.cur) do
        if not state.sel or state.sel.name ~= c.carrier then passEnd(now, c, 'other', 'carrier changed')
        else passFollow(now, c, q.trkMap and q.trkMap[c.key]) end
    end
    local n = 0
    for _ in pairs(P.cur) do n = n + 1 end
    local inWindow = {}
    for _, a in ipairs(q.candList or {}) do
        local inFrame = a.al and passInFrame(a.al, a.lat, a.ht, 1)
        if a.al and not inFrame then P.outside[a.key] = now end
        local turnedIn = P.outside[a.key] and now - P.outside[a.key] < PASS_TURNIN_MEMORY
        if inFrame and (a.c or 0) > 5 and a.r < (turnedIn and PASS_START_RANGE or PASS_STRAIGHT_RANGE) then
            inWindow[a.key] = true
            if not P.cur[a.key] and not P.cool[a.key] and n < PASS_MAX_CONCURRENT and state.sel then
                P.id = P.id + 1
                n = n + 1
                local info = missionCueInfo(state.sel.name)
                local c = { id = P.id, key = a.key, name = a.name, type = tostring(a.type), board = info.board[a.name] or '',
                            pilot = a.pname or a.name, carrier = state.sel.name, carrierType = state.sel.type, t0 = now, range = a.r,
                            callsign = a.pname or a.callsign or a.name, case = recoveryCase(turnedIn), group = a.group or '',
                            num = a.num or (state.client and missionNumber(a.group or '', a.key, a.name)) or 0 }
                P.cur[a.key] = c
                state.feed.trackIds = state.feed.trackIds or {}
                state.feed.trackIds[a.key] = true
                diag.log(string.format('PASS %d start: %s %s %s (%s) at %.0f m, %s, case %d, group %s #%d, whose %d', c.id, c.type,
                                       c.board ~= '' and c.board or '-', c.pilot, tostring(c.callsign), a.r,
                                       turnedIn and 'turned in' or 'straight in', c.case, c.group, c.num, passWhose(c)))
            end
        end
    end
    for key in pairs(P.cool) do
        if not inWindow[key] then P.cool[key] = nil end
    end
    for key, t in pairs(P.outside) do
        if now - t > PASS_TURNIN_MEMORY then P.outside[key] = nil end
    end
end

local function passLines(lines)
    if state.meKnown then lines[#lines + 1] = 'own_aircraft=' .. ((state.me or state.meLast) and 1 or 2) end
    local P = state.passes
    if not P then return end
    local function clean(t) return (tostring(t or ''):gsub('[|\r\n]', ' ')) end
    local list = {}
    for _, c in pairs(P.cur) do list[#list + 1] = c end
    table.sort(list, function(a, b) return a.id < b.id end)
    for _, c in ipairs(P.ended) do
        if #list >= PASS_MAX_CONCURRENT then break end
        list[#list + 1] = c
    end
    lines[#lines + 1] = 'pass_epoch=' .. P.epoch
    lines[#lines + 1] = 'passes=' .. #list
    for i, c in ipairs(list) do
        lines[#lines + 1] = string.format('pass%d=%d|%d|%d|%.0f|%s|%s|%s|%s|%d|%s|%s|%s|%d', i, c.id, c.result and 0 or 1, c.result or 0,
                                          c.range or 0, clean(c.type), clean(c.board), clean(c.pilot), clean(c.carrierType),
                                          c.case or 0, clean(c.callsign or c.pilot), clean(c.grade), clean(c.wire), passWhose(c))
    end
end

local function publishSymbology(now, q, missionTime)
    if not diag.share_symbology then return end
    local f = state.feed
    local lines = { 'valid=1', 'dcsOverlay=' .. (state.uiReady and 1 or 0) }
    local okm, m = pcall(API.getCurrentMission)
    local dt = okm and type(m) == 'table' and m.mission and m.mission.date
    if dt then lines[#lines + 1] = string.format('date=%02d-%02d-%02d', dt.Month or 1, dt.Day or 1, (dt.Year or 2000) % 100) end
    local hour = 12
    if missionTime then
        local t = missionTime % 86400
        hour = math.floor(t / 3600)
        lines[#lines + 1] = string.format('time=%02d:%02d:%02d', hour, math.floor(t / 60) % 60, math.floor(t) % 60)
    end
    local night = f.night
    if night == nil then night = hour < 6 or hour >= 19 end
    lines[#lines + 1] = 'night=' .. (night and 1 or 0)
    lines[#lines + 1] = 'id=' .. carrierIdText(state.sel and state.sel.type)
    if q then
        local foul = q.foul == 1
        lines[#lines + 1] = 'status=' .. (foul and 'F' or 'C')
        lines[#lines + 1] = 'statusVisible=' .. ((not foul or math.floor(now / f.foulHalfPeriod) % 2 == 0) and 1 or 0)
        lines[#lines + 1] = string.format('wind=%02d', math.min(99, math.abs(q.deck) * 1.94384))
        local d, s, vy = 0, 0, 0
        if q.ac then d, s, vy = q.d, q[f.speedKey] or 0, q.vy end
        lines[#lines + 1] = string.format('speed=%03d', math.min(999, math.max(0, s * 1.94384)))
        lines[#lines + 1] = string.format('distance=%05d', math.min(99900, math.floor(d * 3.28084 / 100) * 100))
        lines[#lines + 1] = string.format('sink=%02d', math.min(99, math.max(0, -vy * 3.28084)))
        -- LSO station screen (setShipYawPitchRoll / setDeckWind / setFoulDeck in PLATCameraUI.lua)
        lines[#lines + 1] = 'st_valid=1'
        lines[#lines + 1] = string.format('st_heading=SH %03d DEG', (q.hdg + magOffset()) % 360)
        lines[#lines + 1] = q.roll > 0 and string.format('st_list=%0.1f STBD DOWN', q.roll)
                            or string.format('st_list=%0.1f STBD UP', math.abs(q.roll))
        lines[#lines + 1] = q.pitch > 0 and string.format('st_trim=%0.1f BOW UP', q.pitch)
                            or string.format('st_trim=%0.1f BOW DOWN', math.abs(q.pitch))
        local cross = q.cross * 1.94384
        if cross > 1 then
            lines[#lines + 1] = string.format('st_cross=%2d P', cross + 0.5)
            lines[#lines + 1] = 'st_crossSide=1'
        elseif cross < -1 then
            lines[#lines + 1] = string.format('st_cross=%2d S', math.abs(cross) + 0.5)
            lines[#lines + 1] = 'st_crossSide=-1'
        else
            lines[#lines + 1] = 'st_crossSide=0'
        end
        lines[#lines + 1] = 'st_foul=' .. (q.foul == 1 and 1 or 0)
    end
    lines[#lines + 1] = 'waveoff=0'
    -- Supercarrier's gate: adjustGate(17, 291.3, -2.826) in every logged sample
    lines[#lines + 1] = 'gateFov=17'
    lines[#lines + 1] = 'gateDistance=291.3'
    lines[#lines + 1] = 'gateDeltaH=-2.826'
    if state.missionName and state.missionName ~= '' then lines[#lines + 1] = 'mission_name=' .. state.missionName end
    passLines(lines)
    diag.share_symbology(table.concat(lines, '\n'))
end

-- Feed tick (FEED_PERIOD): computes the overlay values; pushes them into DCS's own overlay when it
-- exists and you are away from the station, and always publishes them to the viewer.
local function feedPush(now)
    local f = state.feed
    local pushDcs = state.uiReady and state.camKind == 'subst'
    local function call(name, ...)
        if not pushDcs then return end
        local ok2, err2 = diag.plat_ui('call', name, ...)
        if not ok2 and err2 ~= f.lastErr then
            f.lastErr = err2
            diag.log('PLATFEED ' .. name .. ' failed: ' .. tostring(err2))
        end
        f.calls = f.calls + 1
    end
    local t = state.missionStart and (state.missionStart + API.getModelTime()) or nil
    if t then
        local tt = t % 86400
        call('setTime', math.floor(tt / 3600), math.floor(tt / 60) % 60, math.floor(tt) % 60)
    end
    local q, err
    if state.client then
        local cx, cy, cz = camPos()
        if cx then q, err = feedQueryClient(cx, cy, cz) else err = 'no camera position' end
    else
        q, err = feedQuery()
    end
    if not q then
        if err ~= f.lastQueryErr then
            f.lastQueryErr = err
            diag.log('PLATFEED query failed: ' .. tostring(err))
        end
        publishSymbology(now, nil, t)
        return
    end
    f.lastQueryErr = nil
    -- your aircraft now; the last one stays your affiliation for the rest of the mission (state is per mission)
    state.me, state.meKnown = q.me, true
    if q.me then state.meLast = { key = q.me.key, name = q.me.name, group = q.me.group, n = q.me.n } end
    local meText = q.me and string.format('%s (group %s #%d)', tostring(q.me.name), tostring(q.me.group), q.me.n or 0)
                   or state.meLast and string.format('none, earlier %s (group %s #%d)', tostring(state.meLast.name), tostring(state.meLast.group),
                                                     state.meLast.n or 0) or 'none'
    if meText ~= f.lastMe then
        f.lastMe = meText
        diag.log('PASS your aircraft: ' .. meText)
    end
    if state.sel then
        local okp, errp = pcall(passTick, now, q)
        if not okp and errp ~= f.lastPassErr then
            f.lastPassErr = errp
            diag.log('PASS error: ' .. tostring(errp))
        end
    end
    publishSymbology(now, q, t)
    if state.cams then
        local okv, errv = pcall(cameraViews, now)
        if not okv and errv ~= f.lastViewErr then
            f.lastViewErr = errv
            diag.log('EXTRACAM views error: ' .. tostring(errv))
        end
        local okt, errt = pcall(cameraTrack, now, q)
        if not okt and errt ~= f.lastTrackErr then
            f.lastTrackErr = errt
            diag.log('EXTRACAM tracking error: ' .. tostring(errt))
        end
    end
    -- DCS's overlay shows the number of the carrier whose station was visited; follow the chosen one
    if pushDcs and state.sel and state.sel.type ~= f.lastCarrierId then
        f.lastCarrierId = state.sel.type
        call('setCarrierID', state.sel.type)
    end
    call('setDeckWind', q.deck, q.cross)
    call('setShipYawPitchRoll', (q.hdg + (f.magOff or 0)) % 360, q.pitch, q.roll)
    if q.ac then
        call('setAircraftData', q.d, q[f.speedKey], q.vy, false)
    else
        call('setAircraftData', 0, 0, 0, false)
    end
    -- foul deck: 0 = clear ("C"); 1 / 2 = foul with the "F" hidden / shown (alternated = flashing)
    local foul = 0
    if q.foul == 1 then foul = (math.floor(now / f.foulHalfPeriod) % 2 == 0) and 2 or 1 end
    call('setFoulDeck', foul)
    local acDesc = q.ac and (tostring(q.type) .. (q.air == 1 and '' or ' on deck')) or 'none'
    if acDesc ~= f.lastAc then
        f.lastAc = acDesc
        vlog('PLATFEED aircraft: ' .. acDesc .. (q.ac and string.format(' at %.0f m', q.d) or ''))
    end
    local foulDesc = q.foul == 1 and ('FOUL (' .. tostring(q.foulBy) .. ')') or 'clear'
    if foulDesc ~= f.lastFoul then
        f.lastFoul = foulDesc
        vlog('PLATFEED deck: ' .. foulDesc)
    end
end

-- Glide slope (IFLOLS basic angle). The angle is per ship type in the unit database
-- (CoreMods/tech/USS_Nimitz/Database/USS_CVN_7x.lua: GT.OLS.GlideslopeBasicAngle and the 4th value of
-- GT.ICLS_Glideslope_position, both 3.5). The reliable way to change it is deploy\set-glideslope.ps1
-- (edits those files, needs admin, restart DCS). GLIDESLOPE_ANGLE = number additionally tries to change
-- it at runtime in every Lua state that holds the unit database (whether DCS honours that is unknown;
-- the log shows what was found). nil = only report.
local GLIDESLOPE_ANGLE = nil
local GLIDESLOPE_CARRIERS = { CVN_71 = true, CVN_72 = true, CVN_73 = true, CVN_74 = true, CVN_75 = true }
-- Record the 2D ball overlay (FLOLS.lua update(meatball, datum, cutlights)) with the player aircraft's
-- position in the carrier frame, twice a second while the overlay is active (FLOLS lines), to measure
-- which angle the ball is centred at.
local FLOLS_LOG = false
-- When the database angle is not the stock 3.5 (set e.g. 3.137 with set-glideslope.ps1), search DCS's
-- memory for that number (MEMSCAN lines) 30 s into the mission and when the ball first shows, to find
-- where Supercarrier keeps each carrier's angle (the route to per-carrier, in-mission changes).
local GLIDESLOPE_MEMSCAN = false
-- Deck jitter investigation (needs CLOUDS_PROBE): every 30 s record a 3 s window of the PLAT camera
-- position per rendered PLAT frame (CAMTRACE lines, from the DLL) and the carrier's simulated position
-- per simulation frame (SHIPTRACE lines), on the same clock, at the station and away.
local DECK_TRACE = false
-- Heading step investigation: per-frame camera heading against the carrier's simulated heading.
local HDG_TRACE = false
-- true: the PLAT camera's orientation follows the carrier every frame. DCS itself only turns the LSO
-- camera 3-5 times a second (steps up to 0.17 deg) while the hull turns smoothly, which makes the deck
-- twitch and the sky move in steps. Set to false to get DCS's own camera orientation back.
local CAMERA_SMOOTH = true
-- Far-view deck jitter test (needs CAMERA_SMOOTH). Hypothesis: away from the station the hull is drawn with
-- the ship's state-update pose, whose heading steps (up to 0.17 deg behind while turning, the heading the
-- export interface reports), while the smoothed camera follows the smooth heading. Alternates every
-- SHIP_POSE_PERIOD s, everywhere (station too): phase A = camera follows the smooth ship (normal), phase B
-- = the same ship axes turned onto the stepped export heading. On-screen text names the phase; SHIPPOSE
-- lines log the heading difference. Expected if right: B = deck locked to the crosshair away (sky steps),
-- deck twitching at the station.
local SHIP_POSE_TEST = false   -- run 2026-11-11 19:10: hypothesis refuted (deck jitters in both phases)
local SHIP_POSE_PERIOD = 15
-- Deck jitter: find the pose DCS draws the carrier with (needs CAMERA_SMOOTH, which supplies the ship
-- axes). Scans memory for copies of the carrier's orientation 20 s after start and every 90 s, and logs
-- how each copy lags the smooth heading (HULLSCAN lines; details in hullscan.log next to the log).
local HULL_SCAN = false   -- done: the cause is known (hull refresh throttle, below)
-- Deck jitter fix (hull_refresh.h). Away from the LSO station DCS refreshes the carrier's transform for
-- the renderer only every 20th search call (about 5 times a second), so its drawn rotation lags and
-- snaps while it turns (aircraft near the carrier step the same way). 'on' = objects within 3 km of the
-- PLAT camera are refreshed every frame (what DCS itself does when you are at the station), 'ab' =
-- alternate ON / OFF every HULL_REFRESH_PERIOD s with on-screen text (test), 'off' = not installed.
-- A/B run 2026-11-12 08:10: deck steady in every ON phase, jitter in every OFF phase, no frame cost.
local HULL_REFRESH = 'on'
local HULL_REFRESH_PERIOD = 15
local gsAngle = nil   -- angle found in the database files / GLIDESLOPE_ANGLE, nil when stock

local DB_PROBE = [==[
local out = {}
local ok, err = pcall(function()
  local d = rawget(_G, 'db')
  if type(d) ~= 'table' or type(d.Units) ~= 'table' or type(d.Units.Ships) ~= 'table' then out[#out + 1] = 'no db.Units.Ships'; return end
  local list = d.Units.Ships.Ship
  if type(list) ~= 'table' then out[#out + 1] = 'no Ship list'; return end
  for _, u in pairs(list) do
    if type(u) == 'table' and type(u.OLS) == 'table' then
      local t = tostring(u.type or u.Name)
      local before = u.OLS.GlideslopeBasicAngle
      local s = t .. '=' .. tostring(before)
      if NEW and WANT[t] then
        u.OLS.GlideslopeBasicAngle = NEW
        if type(u.ICLS_Glideslope_position) == 'table' then u.ICLS_Glideslope_position[4] = NEW end
        s = s .. '->' .. tostring(NEW)
      end
      out[#out + 1] = s
    end
  end
end)
if not ok then out[#out + 1] = 'error ' .. tostring(err) end
return table.concat(out, ' ')
]==]

local function glideslopeReport(when)
    -- what the database files say (read-only)
    local files = {}
    for n in pairs(GLIDESLOPE_CARRIERS) do
        -- DCS runs with the install folder as its working directory
        local fh = io.open('./CoreMods/tech/USS_Nimitz/Database/USS_' .. n .. '.lua', 'r')
        if fh then
            local txt = fh:read('*a')
            fh:close()
            files[#files + 1] = n .. '=' .. tostring(txt:match('GlideslopeBasicAngle%s*=%s*([%d%.]+)'))
        end
    end
    table.sort(files)
    vlog('GLIDESLOPE [' .. when .. '] files: ' .. table.concat(files, ' '))
    gsAngle = GLIDESLOPE_ANGLE
    for _, f in ipairs(files) do
        local v = tonumber(f:match('=([%d%.]+)'))
        if v and math.abs(v - 3.5) > 1e-6 then gsAngle = v end
    end
    local want = {}
    for n in pairs(GLIDESLOPE_CARRIERS) do want[#want + 1] = '["' .. n .. '"]=true' end
    local code = 'local NEW = ' .. (GLIDESLOPE_ANGLE and tostring(GLIDESLOPE_ANGLE) or 'nil') ..
                 '\nlocal WANT = {' .. table.concat(want, ',') .. '}\n' .. DB_PROBE
    local f = loadstring(code)
    if f then
        local ok, r = pcall(f)
        vlog('GLIDESLOPE [' .. when .. '] hooks env: ' .. tostring(r))
    end
    for _, st in ipairs({ 'gui', 'mission', 'server', 'config', 'export' }) do
        local ok, r = pcall(net.dostring_in, st, code)
        vlog('GLIDESLOPE [' .. when .. '] state ' .. st .. ': ' .. tostring(ok and r or ('dostring_in failed: ' .. tostring(r))))
    end
end

-- player aircraft in the frame of the nearest ship: 'type fwd up stbd' (metres)
local FLOLS_QUERY = [==[
local ok, res = pcall(function()
  local p = world.getPlayer()
  if not p and ILARTS_PLAYER then p = Unit.getByName(ILARTS_PLAYER) end   -- multiplayer (hosting)
  if not p then return 'noplayer' end
  local pp = p:getPoint()
  local best, bd
  for side = 0, 2 do
    for _, gr in ipairs(coalition.getGroups(side, Group.Category.SHIP) or {}) do
      for _, u in ipairs(gr:getUnits() or {}) do
        local q = u:getPoint()
        local d = (q.x - pp.x) ^ 2 + (q.z - pp.z) ^ 2
        if not bd or d < bd then bd, best = d, u end
      end
    end
  end
  if not best then return 'noship' end
  local P = best:getPosition()
  local ex, ey, ez = pp.x - P.p.x, pp.y - P.p.y, pp.z - P.p.z
  return string.format('%s %.2f %.2f %.2f', best:getTypeName(), ex * P.x.x + ey * P.x.y + ez * P.x.z,
                       ex * P.y.x + ey * P.y.y + ez * P.y.z, ex * P.z.x + ey * P.z.y + ez * P.z.z)
end)
return ok and tostring(res) or ('error ' .. tostring(res))
]==]

local function flolsSample(now)
    local r = tostring(diag.plat_ui('flols'))
    local n, m, d, c = r:match('^update=(%d+)%(([^,]*),([^,]*),([^)]*)%)')
    if not n then
        if r ~= state.flolsMsg then
            state.flolsMsg = r
            diag.log('FLOLS ' .. r)
        end
        return
    end
    n = tonumber(n)
    if n == state.flolsCount then return end   -- overlay not being updated
    state.flolsCount = n
    if GLIDESLOPE_MEMSCAN and gsAngle and not state.memscanBall and tonumber(m) then
        state.memscanBall = true
        diag.log('GLIDESLOPE memscan (ball showing): ' .. tostring(diag.memscan(gsAngle, 'ball')))
    end
    if state.client then return end
    local who = isMultiplayer() and myUnitName() or nil
    local g = tostring(net.dostring_in('server', 'local ILARTS_PLAYER = ' .. (who and string.format('%q', who) or 'nil')
        .. '\n' .. FLOLS_QUERY))
    diag.log(string.format('FLOLS meatball=%s datum=%s cutlights=%s | player in ship frame: %s', m, d, c, g))
end

-- DCS compatibility (DLL compat.h): the verified build, or another build whose relevant code is unchanged,
-- runs normally; anything else installs nothing, and the viewer explains why. False until known.
local function compatGate(now)
    if state.compat then return state.compat ~= 'incompatible' end
    if not diag.compat then
        state.compat = 'verified'
        return true
    end
    local st, ver, detail = diag.compat()
    if st == 'pending' then
        state.compatSince = state.compatSince or now
        if not state.compatWarned and now - state.compatSince > 60 then
            state.compatWarned = true
            diag.log('COMPAT still waiting after 60 s: ' .. tostring(detail))
        end
        return false
    end
    state.compat = st or 'verified'
    if st == 'incompatible' then
        local msg = 'DCS ' .. tostring(ver) .. ' is not compatible with this version of DCS ILARTS (' .. tostring(detail) ..
                    '); the mod has switched itself off for this session. Roll DCS back or wait for an update of the mod.'
        log.write('ILARTS', log.WARNING or log.INFO, msg)
        diag.log(msg)
        local okA, whyA = diag.share_announce()
        if not okA then diag.log('COMPAT announce failed: ' .. tostring(whyA)) end
        return false
    end
    if st == 'matched' then dcslog('DCS ' .. tostring(ver) .. ': not the verified build, but the code the mod depends on is unchanged') end
    return true
end

-- Benchmark (BENCHMARK above; DLL bench.h). The player as the benchmark sees them: slot and whether in an aircraft.
local function benchWho()
    local slot = isMultiplayer() and mySlotUnitId() or nil
    return (slot and ('slot ' .. slot) or 'no slot') .. (hasAircraft() and ', in an aircraft' or ', no aircraft')
end
local function benchInfo()
    local t = { 'mission=' .. tostring(state.missionName or ''):gsub('[\r\n]', ' ') }
    local ex = rawget(_G, 'Export')
    local okS, d = false, nil
    if ex and ex.LoGetSelfData then okS, d = pcall(ex.LoGetSelfData) end
    t[#t + 1] = 'aircraft=' .. (okS and type(d) == 'table' and tostring(d.Name or '') or '')
    t[#t + 1] = 'role=' .. (not isMultiplayer() and 'single player' or state.client and 'multiplayer client' or 'multiplayer host')
    t[#t + 1] = 'player=' .. benchWho()
    return table.concat(t, '\n')
end
-- Once a second, before anything starts. True while ILARTS is held back (stage 1: the hook alone).
local function benchStep(now)
    local b = state.bench
    if b.phase == 'arming' then
        b.who = benchWho()
        local ok, why = diag.bench('begin', benchInfo())
        if not ok then
            diag.log('BENCH not started: ' .. tostring(why))
            state.bench = nil
            return false
        end
        b.phase, b.t0 = 'hold', now
        diag.log('BENCH armed: ILARTS waits until the viewer has measured DCS with the hook alone')
    end
    local paused = API.getPause and API.getPause() or false
    diag.bench('tick', paused)  -- the viewer stops its clocks while DCS is paused
    if paused ~= b.paused then
        b.paused = paused
        diag.log('BENCH ' .. (paused and 'DCS paused' or 'DCS running'))
    end
    local who = benchWho()
    if who ~= b.who then  -- the viewer restarts stage 1 (still holding) or stops the run
        b.who = who
        diag.bench('who', benchInfo())
        diag.log('BENCH player: ' .. who)
    end
    local cmd, age, stage = diag.bench('poll')
    if stage ~= b.stage then
        b.stage = stage
        if stage and stage ~= '' then diag.log('BENCH stage: ' .. stage) end
    end
    if cmd == 'done' then
        diag.bench('ended', 'run over')
        state.bench = nil
        return false
    end
    local seen = tonumber(age)  -- s since the viewer last looked (nil: not yet)
    if b.phase == 'hold' then
        if cmd == 'release' then
            b.phase = 'run'
            diag.bench('release')
            return false
        end
        if (not seen and now - b.t0 > BENCH_VIEWER_WAIT) or (seen and seen > 20) then
            diag.log('BENCH no viewer ' .. (seen and ('for ' .. seen .. ' s') or ('after ' .. BENCH_VIEWER_WAIT .. ' s')) ..
                     ': ILARTS starts as usual; the benchmark is switched off')
            diag.bench('ended', 'no viewer')
            diag.bench('disarm')
            state.bench = nil
            return false
        end
        return true
    end
    if seen and seen > 30 then  -- the viewer went away during the run (it puts its own things back when it returns)
        diag.log('BENCH the viewer stopped looking (' .. seen .. ' s): run over')
        diag.bench('ended', 'no viewer')
        state.bench = nil
    end
    return false
end

-- The air temperature at sea level at the carrier (C), for the precipitation type in ILARTS's views (DCS splits rain
-- and snow by the temperature at your own camera): DCS's own at the carrier (host / single player), else the
-- mission's. nil when neither is known.
local TEMP_QUERY = [==[
local ok, res = pcall(function()
  local t = atmosphere.getTemperatureAndPressure({x = px, y = 0, z = pz})
  return string.format('%.2f', t - 273.15)
end)
return ok and res or ('error ' .. tostring(res))
]==]
local function carrierTemperature()
    if not state.client and state.sel and state.sel.x and state.sel.z then
        local code = string.format('local px, pz = %.1f, %.1f\n', state.sel.x, state.sel.z) .. TEMP_QUERY
        local okq, r = pcall(net.dostring_in, 'server', code)
        local t = okq and tonumber(r)
        if t then return t end
    end
    local okm, m = pcall(API.getCurrentMission)
    local w = okm and type(m) == 'table' and m.mission and m.mission.weather
    return type(w) == 'table' and type(w.season) == 'table' and tonumber(w.season.temperature) or nil
end

local function step(now)
    if not compatGate(now) then return end
    if state.bench then
        state.firstStep = state.firstStep or now  -- START_DELAY runs while the benchmark holds ILARTS back
        if benchStep(now) then return end
    end
    if not state.started then
        -- Nothing on the GPU while DCS is still loading the mission: sharing, the PLAT's second scene render and the
        -- cameras wait START_DELAY s from the first frame. (2026-11-14/15: twice the GPU faulted - NVIDIA "FECS
        -- Exception: UCODE Fatal Error", DCS lost its device - within half a second of a scene render being added
        -- during the mission load: the extra cameras once, the PLAT once.)
        state.firstStep = state.firstStep or now
        if now - state.firstStep < START_DELAY then return end
        state.started = true
        diag.log('mission running')
        if VERBOSE then diag.run() end   -- read-only memory pass (module list, renderer globals)
        local started, why = diag.share_start(unpack(NAMES))
        state.sharing = started and true or false
        dcslog('sharing: ' .. (started and 'started' or ('refused: ' .. tostring(why))))
        diag.log('sharing: ' .. (started and 'started' or ('refused: ' .. tostring(why))))
        state.nextStatus = now + STATUS_PERIOD
        if KEEP_PLAT and diag.keep_plat then
            local r, msg = diag.keep_plat(true)
            state.keepPlat = r and true or false
            dcslog('keep PLAT: ' .. tostring(msg))
            if state.keepPlat and SC_OVERRIDE and diag.sc_override then diag.sc_override(true) end
            diag.log('keep PLAT: ' .. tostring(msg))
            -- synthetic PLAT camera: no LSO view needed (the old view switch stays as a fallback)
            if state.keepPlat and SYNTHETIC_PLAT and diag.plat_cam then
                local r, why = diag.plat_cam('enable')
                state.synth = r and true or false
                diag.log('PLATCAM enable: ' .. tostring(r and 'ok' or why))
                if state.synth then
                    local vp, vwhy = diag.plat_cam('viewport')
                    diag.log('PLATCAM viewport 32 at start: ' .. tostring(vp or vwhy))
                    local w = vp and tonumber(vp:match('^(%d+)'))
                    if (not w or w <= 0) and LSO_RESOLUTION <= 0 then
                        -- the size Supercarrier gives it when the LSO view first opens
                        local r2, e2 = diag.set_mfd_resolution(LSO_VIEWPORT, 512, 512)
                        diag.log('PLATCAM viewport 32 size 512x512: ' .. tostring(r2 and 'requested' or e2))
                    end
                    if SYNTHETIC_FALLBACK_S > 0 then state.synthDeadline = now + SYNTHETIC_FALLBACK_S end
                end
            end
            if state.keepPlat and AUTO_LSO_VIEW and not state.synth then
                -- sent from the frame callback once the sim is running and you are in an aircraft
                state.autoLso = { phase = 'pending', tries = 0 }
            end
        end
        if state.sharing and LSO_RESOLUTION > 0 then
            local r, err = diag.set_mfd_resolution(LSO_VIEWPORT, LSO_RESOLUTION, LSO_RESOLUTION)
            dcslog('PLAT resolution ' .. LSO_RESOLUTION .. ': ' .. tostring(r and 'requested' or err))
        end
        if state.sharing and CAPTURE_ONCE and not captured then
            captured = true
            state.captureAt = now + 20   -- give the user time to open the LSO view
        end
    end
    -- One capture of the PLAT chain (with the BWTV shader probe installed, LSOFRAME's top-left
    -- 18x2 pixels carry the BWTV draw's parameters).
    if state.captureAt and now >= state.captureAt then
        state.captureAt = nil
        for _, n in ipairs({ 'LSOFRAME', 'LSOFINALTARGET', 'ldrBuffer0MFD32' }) do
            local r, err = diag.capture(n)
            dcslog('capture ' .. n .. ': ' .. tostring(r or err))
        end
    end
    if state.sharing and now >= state.nextStatus then
        state.nextStatus = now + STATUS_PERIOD
        local st = diag.share_status()
        vlog('share status: ' .. st)
        vdcslog('share status: ' .. st)
    end
    -- synthetic PLAT camera: did the PLAT start without the LSO view? If not, fall back to it once.
    if state.synthDeadline and now >= state.synthDeadline then
        state.synthDeadline = nil
        local st = state.sharing and diag.share_status() or ''
        if st:match('LSOFRAME gen=%d+ %d+x%d+ copies=%d+ skipped=%d+ live') then
            diag.log('PLATCAM LSOFRAME is live without opening the LSO view')
        else
            diag.log('PLATCAM no LSOFRAME ' .. SYNTHETIC_FALLBACK_S .. ' s after start (' .. tostring(diag.plat_cam('viewport'))
                     .. '); falling back to opening the LSO view once')
            if AUTO_LSO_VIEW and state.keepPlat then state.autoLso = { phase = 'pending', tries = 0 } end
        end
    end
    if state.synth and now >= (state.synthStatsAt or 0) then
        state.synthStatsAt = now + 10
        vlog('PLATCAM ' .. tostring(diag.plat_cam('stats')))
    end
    -- Q4 overlay bridge
    if PLAT_UI and diag.plat_ui then
        if not state.uiInstalled then
            state.uiInstalled = true
            local r, err = diag.plat_ui('install')
            diag.log('PLATUI install: ' .. tostring(r or err))
        end
        local st = diag.plat_ui('status')
        if st ~= state.uiStatus then
            state.uiStatus = st
            diag.log('PLATUI ' .. st)
        end
        if st:match('^PLAT UI state %x') then
            if now >= (state.uiNext or 0) then
                state.uiNext = now + 10
                vlog('PLATUI fed: ' .. tostring(diag.plat_ui('log')))
            end
            state.uiReady = true
            if PLAT_FEED and state.feed and state.camKind == 'live' and not state.client then
                local okc, errc = pcall(feedCalibrate, now)
                if not okc then diag.log('PLATFEED calib error: ' .. tostring(errc)) end
            end
            if state.feed and now >= (state.feed.nextReport or 0) then
                state.feed.nextReport = now + 10
                if state.feed.calls > 0 then vlog('PLATFEED pushed ' .. state.feed.calls .. ' setter calls') end
            end
        end
    end
    -- Q4 observation: is the LSO camera available from the view system right now? (once a second;
    -- the summary is logged when its state changes and every 10 s, full details by the DLL on change)
    if LSO_CAMERA_PROBE and diag.lso_camera then
        local s = diag.lso_camera()
        local kind = s:match('substituting') and 'subst' or s:match('^camera null') and 'null' or (s:match('^camera ') and 'live' or s)
        if kind ~= state.camKind then state.camSince = now end
        if kind ~= state.camKind or now >= (state.camNext or 0) then
            if kind ~= state.camKind then diag.log('LSOCAM ' .. s) else vlog('LSOCAM ' .. s) end
            state.camKind = kind
            state.camNext = now + 10
        end
        if VERBOSE and now >= (state.traceNext or 0) then
            state.traceNext = now + 5
            vlog('POSETRACE [' .. tostring(kind) .. '] ' .. tostring(diag.lso_camera('trace')))
        end
    end
    -- F10 map: keep DCS's world object search running so the PLAT's models (carrier hull) keep moving
    if MAP_OBJECTS and diag.map_objects and state.keepPlat then
        if state.mapObjOk == nil then
            local r, why = diag.map_objects('install')
            state.mapObjOk = r and true or false
            diag.log('MAPOBJECTS install: ' .. tostring(r and 'ok' or why))
            if state.mapObjOk then diag.map_objects('on') end
        end
        if state.mapObjOk and now >= (state.mapObjNext or 0) then
            state.mapObjNext = now + 5
            vlog('MAPOBJECTS ' .. tostring(diag.map_objects('stats')))
        end
    end
    -- PLAT render origin fix
    if PLAT_ORIGIN ~= 'off' and diag.plat_origin and state.keepPlat then
        if state.originOk == nil then
            local r, why = diag.plat_origin('install')
            state.originOk = r and true or false
            diag.log('PLATORIGIN install: ' .. tostring(r and 'ok' or why))
            if state.originOk and PLAT_ORIGIN ~= 'observe' then diag.plat_origin('on') end
        end
        if state.originOk then
            -- objects shared by the views at the carrier and your own view: placed per origin
            local splitWant = PLAT_ORIGIN_SPLIT ~= false
            if PLAT_ORIGIN_SPLIT == 'ab' then splitWant = math.floor(now / PLAT_ORIGIN_AB_PERIOD) % 2 == 0 end
            if splitWant ~= state.splitOn then
                state.splitOn = splitWant
                diag.plat_origin(splitWant and 'split_on' or 'split_off')
                if PLAT_ORIGIN_SPLIT == 'ab' then
                    screenText('PLAT test: object split ' .. (splitWant and 'ON' or 'OFF'), PLAT_ORIGIN_AB_PERIOD - 1)
                    diag.log('PLATORIGIN test: object split ' .. (splitWant and 'ON' or 'OFF'))
                end
            end
            if PLAT_ORIGIN == 'abc' then
                -- far-view deck jitter test: A = current (per-view origin, camera smoothing), B = whole-frame
                -- origin at the PLAT (the main view shakes), C = per-view origin without camera smoothing
                local ph = 0
                if state.camKind == 'subst' then
                    ph = math.floor((now - (state.camSince or now)) / PLAT_ORIGIN_AB_PERIOD) % 3
                end
                if ph ~= state.originPhase then
                    state.originPhase = ph
                    diag.plat_origin(ph == 1 and 'frame' or 'view')
                    if state.smoothOn then diag.lso_camera(ph == 2 and 'smooth_off' or 'smooth_on') end
                    local label = ({ [0] = 'A', [1] = 'B (your own view may shake)', [2] = 'C' })[ph]
                    if state.camKind == 'subst' then
                        screenText('PLAT test phase ' .. label, PLAT_ORIGIN_AB_PERIOD - 1)
                    end
                    diag.log('PLATORIGIN test: phase ' .. label .. ' [' .. tostring(state.camKind) .. ']')
                end
            elseif PLAT_ORIGIN == 'ab' then
                local want = true
                if state.camKind == 'subst' then
                    want = math.floor((now - (state.camSince or now)) / PLAT_ORIGIN_AB_PERIOD) % 2 == 0
                end
                if want ~= state.originOn then
                    state.originOn = want
                    diag.plat_origin(want and 'on' or 'off')
                    if state.camKind == 'subst' then
                        -- on-screen label in DCS so the phases are unambiguous
                        screenText('PLAT origin fix: ' .. (want and 'ON' or 'OFF'), PLAT_ORIGIN_AB_PERIOD - 1)
                    end
                    diag.log('PLATORIGIN test: fix ' .. (want and 'ON' or 'OFF') .. ' [' .. tostring(state.camKind) .. ']')
                end
            end
            if now >= (state.originNext or 0) then
                state.originNext = now + 5
                vlog('PLATORIGIN [' .. tostring(state.camKind) .. '] ' .. tostring(diag.plat_origin('stats')))
            end
        end
    end
    -- your own aircraft's canopy in the PLAT and the extra cameras (needs the PLAT origin hooks: they list the views)
    if OWN_CANOPY and state.originOk and diag.own_canopy then
        if state.canopyOk == nil then
            local r, why = diag.own_canopy('install')
            state.canopyOk = r and true or false
            diag.log('OWNCANOPY install: ' .. tostring(r and 'ok' or why))
            if state.canopyOk then diag.own_canopy('on') end
        end
        if state.canopyOk and now >= (state.canopyNext or 0) then
            state.canopyNext = now + 5
            vlog('OWNCANOPY ' .. tostring(diag.own_canopy('stats')))
        end
    end
    -- stars in the PLAT and the extra cameras (needs the PLAT origin hooks: they list the views)
    if CAMERA_STARS ~= 'off' and state.originOk and diag.night_sky then
        if state.starsOk == nil then
            local r, why = diag.night_sky('install')
            state.starsOk = r and true or false
            diag.log('NIGHTSKY install: ' .. tostring(r and 'ok' or why))
            if state.starsOk then diag.night_sky(CAMERA_STARS) end
        end
        if state.starsOk and now >= (state.starsNext or 0) then
            state.starsNext = now + 60
            diag.log('NIGHTSKY ' .. tostring(diag.night_sky('stats')))
        end
    end
    -- rain and snow in the PLAT and the extra cameras (needs the PLAT origin hooks: they list the views)
    if CAMERA_RAIN ~= 'off' and state.originOk and diag.rain then
        if state.rainOk == nil then
            local r, why = diag.rain('install')
            state.rainOk = r and true or false
            diag.log('RAIN install: ' .. tostring(r and 'ok' or why))
            if state.rainOk and CAMERA_RAIN ~= 'ab' then diag.rain(CAMERA_RAIN) end
        end
        if state.rainOk and CAMERA_RAIN == 'ab' then
            local want = math.floor(now / CAMERA_RAIN_AB_PERIOD) % 2 == 0 and 'auto' or 'stock'
            if want ~= state.rainPhase then
                state.rainPhase = want
                diag.rain(want)
                screenText('ILARTS rain test: ' .. (want == 'auto' and 'camera motion (fix ON)' or 'stock (fix OFF)'),
                    CAMERA_RAIN_AB_PERIOD - 1)
                diag.log('RAIN test: ' .. want)
            end
        end
        if state.rainOk and now >= (state.rainTempNext or 0) then
            state.rainTempNext = now + 60
            local t = carrierTemperature()
            t = t and math.floor(t * 10 + 0.5) / 10   -- whole tenths: the DLL logs each change
            if t ~= state.rainTemp then
                state.rainTemp = t
                diag.rain('temperature', t)
            end
        end
        if state.rainOk and now >= (state.rainNext or 0) then
            state.rainNext = now + 60
            local line = diag.rain('stats')
            if line and line ~= '' then diag.log('RAIN ' .. tostring(line)) end
        end
    end
    if GLIDESLOPE_MEMSCAN and gsAngle and not state.memscanStart and API.getModelTime() > 30 then
        state.memscanStart = true
        diag.log('GLIDESLOPE memscan (mission +30 s): ' .. tostring(diag.memscan(gsAngle, 'mission')))
    end
    -- 2D ball overlay vs aircraft geometry
    if FLOLS_LOG and state.uiInstalled and now >= (state.flolsNext or 0) then
        state.flolsNext = now + 0.5
        local okf, errf = pcall(flolsSample, now)
        if not okf then diag.log('FLOLS error: ' .. tostring(errf)) end
    end
    -- PLAT cloud pass rebase (A/B)
    if CLOUD_REBASE ~= 'off' and state.cloudsOk then
        local want = CLOUD_REBASE == 'on'
        if CLOUD_REBASE == 'ab' then
            want = state.camKind == 'subst' and
                   math.floor((now - (state.camSince or now)) / CLOUD_REBASE_PERIOD) % 2 == 0
        end
        if want ~= state.rebaseOn then
            state.rebaseOn = want
            diag.clouds(want and 'rebase_on' or 'rebase_off')
            diag.log('CLOUDREBASE test: ' .. (want and 'ON' or 'OFF') .. ' [' .. tostring(state.camKind) .. ']')
        end
        if now >= (state.rebaseNext or 0) then
            state.rebaseNext = now + 5
            vlog('CLOUDREBASE [' .. tostring(state.camKind) .. '] ' .. tostring(diag.clouds('rebase_stats')))
        end
    end
    -- cloud renderer calls, at the station vs away
    if CLOUDS_PROBE and diag.clouds and not state.cloudsOk and not state.cloudsRetried then
        state.cloudsRetried = true
        local r, why = diag.clouds('install')
        state.cloudsOk = r and true or false
        state.cloudsDumped = state.cloudsDumped or {}
        diag.log('CLOUDS probe (retry): ' .. tostring(r and 'installed' or why))
    end
    if VERBOSE and CLOUDS_PROBE and diag.clouds and state.cloudsOk and now >= (state.cloudsNext or 0) then
        state.cloudsNext = now + 2
        local k = state.camKind
        local act = 'stats'
        if (k == 'live' or k == 'subst') and not state.cloudsDumped[k] and now - (state.camSince or now) >= 3 then
            state.cloudsDumped[k] = true
            act = 'dump'
            diag.log('CLOUDS requesting context dump [' .. k .. ']')
        end
        diag.log('CLOUDS [' .. tostring(k) .. '] ' .. tostring(diag.clouds(act)))
    end
    -- Q4 overlay: which view-system functions does Supercarrier call, at the station vs away?
    if VERBOSE and diag.view_calls and state.keepPlat and now >= (state.vcNext or 0) then
        state.vcNext = now + 5
        diag.log('VIEWCALLS [' .. tostring(state.camKind) .. '] ' .. diag.view_calls())
    end
end

local callbacks = {}

function callbacks.onMissionLoadBegin()
    if not VERBOSE then return end
    local ok, err = pcall(glideslopeReport, 'mission load')
    if not ok then diag.log('GLIDESLOPE error: ' .. tostring(err)) end
end

-- The mission's name for the viewer (its recordings folder): DCS.getMissionName(), else the mission file's name.
local function missionName()
    local name
    if API.getMissionName then
        local ok, n = pcall(API.getMissionName)
        if ok and type(n) == 'string' then name = n end
    end
    if (not name or name == '') and API.getMissionFilename then
        local ok, f = pcall(API.getMissionFilename)
        if ok and type(f) == 'string' then name = f:match('([^\\/]+)$') end
    end
    name = tostring(name or ''):gsub('%.[mM][iI][zZ]$', ''):gsub('[%c|]', ' ')
    return name
end

function callbacks.onSimulationStart()
    local mp = isMultiplayer()
    local client = mp and not (API.isServer and API.isServer())
    if client and not MP_CLIENT then
        dcslog('multiplayer client - diagnostic inactive (MP_CLIENT = false)')
        diag.log('multiplayer client - inactive (MP_CLIENT = false)')
        state = nil
        return
    end
    if mp then diag.log(client and 'multiplayer session, client (export data source)' or 'multiplayer session, hosting') end
    state = { next = API.getRealTime() + FIRST_DELAY, started = false, sharing = false, client = client }
    if BENCHMARK and diag.bench and diag.bench('armed') then
        state.bench = { phase = 'arming' }  -- step() holds ILARTS back until the viewer has measured stage 1
        diag.log('BENCH armed for this mission')
    end
    -- Open ILARTS straight away: a separate process, nothing added to DCS's rendering. It shows "Waiting for DCS"
    -- until sharing starts (START_DELAY s later) and shows the compatibility notice if the mod switches itself off.
    if AUTO_START_VIEWER and diag.launch_viewer then
        local okv, rv = pcall(diag.launch_viewer)
        dcslog('viewer: ' .. tostring(rv))
    end
    if PLAT_FEED then
        state.feed = { err = {}, speedKey = 'g', calls = 0, nextPush = 0,
                       -- Supercarrier's distance is measured to a point 4.6 m forward of the PLAT camera on the
                       -- landing axis (fits on deck to ~0.3 m; airborne it reads ~2 m more = one frame of closure)
                       distOff = 4.6,
                       -- landing area relative to the PLAT camera (m): the camera is ~150 m forward of the ramp;
                       -- Supercarrier cleared the deck when the trapped aircraft taxied ~13 m off the centreline
                       la0 = -160, la1 = 120, lw = 13,
                       foulHalfPeriod = 0.32 }                     -- Supercarrier flashes the "F" every 19 frames at 60 fps
    end
    if CLOUDS_PROBE and diag.clouds and not state.bench then  -- (benchmark: step() installs it once ILARTS starts)
        local r, why = diag.clouds('install')
        state.cloudsOk = r and true or false
        state.cloudsDumped = {}
        diag.log('CLOUDS probe: ' .. tostring(r and 'installed' or why))
    end
    local okm, m = pcall(API.getCurrentMission)
    state.missionStart = okm and type(m) == 'table' and m.mission and tonumber(m.mission.start_time) or nil
    state.missionName = missionName()
end

-- DCS's multiplayer game events (crash, eject, pilot_death ...): used as a client only (clientGameEvent).
function callbacks.onGameEvent(name, a1, a2)
    local ok, err = pcall(clientGameEvent, name, a1, a2)
    if not ok then diag.log('CLIENT EVENT error: ' .. tostring(err)) end
end

function callbacks.onSimulationStop()
    if state then
        if state.bench then pcall(diag.bench, 'ended', 'mission ended') end
        if state.keepPlat then
            local _, msg = diag.keep_plat(false)
            diag.log('keep PLAT stop: ' .. tostring(msg))
        end
        if state.uiInstalled then pcall(diag.plat_ui, 'reset') end
        if state.cloudsOk then pcall(diag.clouds, 'remove') end
        if state.canopyOk then pcall(diag.own_canopy, 'remove') end
        if state.starsOk then pcall(diag.night_sky, 'remove') end
        if state.rainOk then pcall(diag.rain, 'remove') end
        if state.originOk then pcall(diag.plat_origin, 'remove') end
        if state.mapObjOk then pcall(diag.map_objects, 'remove') end
        if state.hullFixOk then pcall(diag.hull_refresh, 'remove') end
        if state.cams and diag.extra_cam then pcall(diag.extra_cam, 'remove') end
        if state.smoothOn then pcall(diag.lso_camera, 'smooth_off') end
        if state.shipDraw then pcall(diag.lso_camera, 'ship_draw_off') end
        diag.log('mission stopped; share status: ' .. diag.share_status())
        diag.share_stop()
    end
    state = nil
end

local SHIP_TRACE_QUERY = [==[
local ok, res = pcall(function()
  local ship = _G.__lsodiag_ship and Unit.getByName(_G.__lsodiag_ship)
  if not ship then return 'noship' end
  local P = ship:getPosition()
  return string.format('%.4f %.4f %.4f %.6f %.6f', P.p.x, P.p.y, P.p.z, P.x.x, P.x.z)
end)
return ok and res or ('error ' .. tostring(res))
]==]

local SHIP_AXES_QUERY = [==[
local ok, res = pcall(function()
  local ship = _G.__lsodiag_ship and Unit.getByName(_G.__lsodiag_ship)
  if not ship then return 'noship' end
  local P = ship:getPosition()
  return string.format('%.9f %.9f %.9f %.9f %.9f %.9f %.9f %.9f %.9f %.4f %.4f %.4f', P.x.x, P.x.y, P.x.z, P.y.x, P.y.y, P.y.z, P.z.x, P.z.y, P.z.z, P.p.x, P.p.y, P.p.z)
end)
return ok and res or ('error ' .. tostring(res))
]==]

-- The ship as the export interface reports it (what the graphics side works from): "x y z heading pitch bank".
local SHIP_ID_QUERY = [==[
local ok, res = pcall(function()
  local ship = _G.__lsodiag_ship and Unit.getByName(_G.__lsodiag_ship)
  if not ship then return 'noship' end
  return tostring(ship.id_) .. ' ' .. tostring(ship:getName())
end)
return ok and res or ('error ' .. tostring(res))
]==]

local function exportShip()
    local ex = rawget(_G, 'Export')
    if not (ex and ex.LoGetObjectById) then return 'noexport' end
    if state.exportShipId == nil then
        state.exportShipId = false
        local r = tostring(net.dostring_in('server', SHIP_ID_QUERY))
        local id, name = r:match('^(%d+) (.+)$')
        id = tonumber(id)
        local how = 'none'
        if id and ex.LoGetObjectById(id) then
            state.exportShipId, how = id, 'id_'
        elseif ex.LoGetWorldObjects then
            for k, o in pairs(ex.LoGetWorldObjects() or {}) do
                if name and o.UnitName == name then state.exportShipId, how = k, 'UnitName' break end
            end
        end
        diag.log('HDGTRACE export ship: query=' .. r .. ' id=' .. tostring(state.exportShipId) .. ' via ' .. how)
    end
    if not state.exportShipId then return 'noid' end
    local o = ex.LoGetObjectById(state.exportShipId)
    if not (o and o.Position) then return 'gone' end
    return string.format('%.4f %.4f %.4f %.7f %.7f %.7f', o.Position.x, o.Position.y, o.Position.z, o.Heading or 0, o.Pitch or 0, o.Bank or 0)
end

-- ---- Carrier list and selection (synthetic PLAT camera) -----------------------------------------
-- Every carrier the PLAT can sit on: "name|type|x|y|z" lines (Supercarrier types and Stennis).
local CARRIER_QUERY = [==[
local ok, res = pcall(function()
  local out = {}
  for side = 0, 2 do
    for _, gr in ipairs(coalition.getGroups(side, Group.Category.SHIP) or {}) do
      for _, u in ipairs(gr:getUnits() or {}) do
        local t = u:getTypeName()
        if u:isExist() and (t:match('^CVN_7%d$') or t == 'Stennis') then
          local p = u:getPoint()
          out[#out + 1] = string.format('%s|%s|%.1f|%.1f|%.1f', u:getName(), t, p.x, p.y, p.z)
        end
      end
    end
  end
  return table.concat(out, '\n')
end)
return ok and res or ('error ' .. tostring(res))
]==]

-- DCS's wind at a point, for WINDCHECK (host / single player): "x z" (m/s).
local WIND_QUERY = [==[
local ok, res = pcall(function()
  local w = atmosphere.getWind({x = px, y = py, z = pz})
  return string.format('%.3f %.3f', w.x, w.z)
end)
return ok and res or ('error ' .. tostring(res))
]==]

local function listCarriers()
    local out = {}
    if state.client then
        local objs = worldObjects()
        for id, o in pairs(objs or {}) do
            if isCarrierType(o.Name) and o.Position then
                out[#out + 1] = { name = tostring(o.UnitName or o.Name), type = o.Name, x = o.Position.x, y = o.Position.y,
                                  z = o.Position.z, id = id }
            end
        end
    else
        local r = tostring(net.dostring_in('server', CARRIER_QUERY))
        for line in r:gmatch('[^\n]+') do
            local n, t, x, y, z = line:match('^(.-)|([^|]+)|([-%d.]+)|([-%d.]+)|([-%d.]+)$')
            if n then out[#out + 1] = { name = n, type = t, x = tonumber(x), y = tonumber(y), z = tonumber(z) } end
        end
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

local function selectCarrier(c, why)
    state.sel = { name = c.name, type = c.type, x = c.x, y = c.y, z = c.z, id = c.id }
    if not state.client then pcall(net.dostring_in, 'server', string.format('_G.__lsodiag_ship = %q', c.name)) end
    if state.synth then diag.plat_cam('select', c.x, c.y, c.z, c.type) end
    diag.log(string.format('PLATCAM carrier %s (%s) at (%.0f, %.0f): %s', c.name, c.type, c.x, c.z, why))
end

local function nearestCarrier(list, x, z, maxDist)
    local best, bd
    for _, c in ipairs(list) do
        local d = (c.x - x) ^ 2 + (c.z - z) ^ 2
        if (not maxDist or d < maxDist * maxDist) and (not bd or d < bd) then best, bd = c, d end
    end
    return best
end

-- ---- LSO 2 cue panel (setCuePanelInfo) -----------------------------------------------------------
-- Airborne airplanes of the carrier's coalition: "name|type|player|fuel lb|x|z|yaw|group". x / z in
-- Supercarrier's cue panel frame: metres along / across the angled deck axis from a point 148 m aft and
-- 25 m to port of the PLAT camera (fitted to its setCuePanelInfo values, 2026-11-09/10 logs); yaw = minus
-- the track angle in that frame (rad). Prepended: cx, cz (PLAT camera).
local CUE_QUERY = [==[
local ok, res = pcall(function()
  local ship = _G.__lsodiag_ship and Unit.getByName(_G.__lsodiag_ship)
  if not ship then return 'noship' end
  local P = ship:getPosition()
  local ah = math.atan2(P.x.z, P.x.x) - math.rad(9)
  local axx, axz = math.cos(ah), math.sin(ah)
  local rtx, rtz = -axz, axx
  local out = {}
  for _, gr in ipairs(coalition.getGroups(ship:getCoalition(), Group.Category.AIRPLANE) or {}) do
    for _, u in ipairs(gr:getUnits() or {}) do
      if u:isExist() and u:inAir() then
        local p = u:getPoint()
        local rx, rz = p.x - cx, p.z - cz
        local al, lat = rx * axx + rz * axz, rx * rtx + rz * rtz
        if al * al + lat * lat < 120000 ^ 2 then
          local v = u:getVelocity()
          local va, vl = v.x * axx + v.z * axz, v.x * rtx + v.z * rtz
          local d = u:getDesc() or {}
          local fuel = (u:getFuel() or 0) * (d.fuelMassMax or 0) * 2.20462
          out[#out + 1] = string.format('%s|%s|%s|%.0f|%.1f|%.1f|%.4f|%s', u:getName(), u:getTypeName(),
            tostring(u:getPlayerName() or ''), fuel, al + 148, lat - 25, -math.atan2(vl, va), gr:getName())
        end
      end
    end
  end
  return table.concat(out, '\n')
end)
return ok and res or ('error ' .. tostring(res))
]==]

-- From the mission file (host and client alike): board numbers by unit name, and the plane groups whose
-- route lands on the chosen carrier. Cached per carrier.
function missionCueInfo(carrierName)
    local c = state.cueInfo
    if c and c.carrier == carrierName then return c end
    c = { carrier = carrierName, board = {}, recovering = {} }
    state.cueInfo = c
    local okm, m = pcall(API.getCurrentMission)
    local mis = okm and type(m) == 'table' and m.mission
    if not (mis and mis.coalition) then return c end
    local carrierId
    for _, side in pairs(mis.coalition) do
        for _, country in ipairs(type(side) == 'table' and side.country or {}) do
            for _, g in ipairs(country.ship and country.ship.group or {}) do
                for _, u in ipairs(g.units or {}) do
                    if u.name == carrierName then carrierId = u.unitId end
                end
            end
        end
    end
    for _, side in pairs(mis.coalition) do
        for _, country in ipairs(type(side) == 'table' and side.country or {}) do
            for _, cat in ipairs({ 'plane', 'helicopter' }) do
                for _, g in ipairs(country[cat] and country[cat].group or {}) do
                    for _, u in ipairs(g.units or {}) do
                        if u.name and u.onboard_num then c.board[u.name] = tostring(u.onboard_num) end
                    end
                    for _, pt in ipairs(g.route and g.route.points or {}) do
                        if carrierId and (pt.linkUnit == carrierId or pt.helipadId == carrierId) and
                           (pt.type == 'Land' or pt.action == 'Landing') then
                            c.recovering[g.name or ''] = true
                        end
                    end
                end
            end
        end
    end
    return c
end

-- Candidates "as CUE_QUERY" from the export interface (client): no fuel, velocities by differencing.
local function cueCandidatesClient(cx, cz, carrierType)
    local objs = worldObjects()
    if not objs then return {} end
    local now = API.getModelTime()
    local ship, side
    for _, o in pairs(objs) do
        if o.Name == carrierType and o.Position and (not ship or (o.Position.x - cx) ^ 2 + (o.Position.z - cz) ^ 2 <
           (ship.Position.x - cx) ^ 2 + (ship.Position.z - cz) ^ 2) then ship = o end
    end
    if not ship then return {} end
    side = ship.CoalitionID
    local X = axesHPB(ship.Heading or 0, 0, 0)
    local ah = math.atan2(X[3], X[1]) - math.rad(9)
    local axx, axz = math.cos(ah), math.sin(ah)
    local rtx, rtz = -axz, axx
    state.cuePrev = state.cuePrev or {}
    local prev, cur, out = state.cuePrev, {}, {}
    for id, o in pairs(objs) do
        if o.Type and o.Type.level1 == 1 and o.Type.level2 == 1 and o.CoalitionID == side and o.Position then
            local p = o.Position
            cur[id] = { t = now, x = p.x, y = p.y, z = p.z }
            local q = prev[id]
            local vx, vz, vy = 0, 0, 0
            if q and now - q.t > 0.05 then vx, vy, vz = (p.x - q.x) / (now - q.t), (p.y - q.y) / (now - q.t), (p.z - q.z) / (now - q.t) end
            local speed = math.sqrt(vx * vx + vz * vz)
            if speed > 40 or p.y - ship.Position.y > 40 then   -- airborne
                local rx, rz = p.x - cx, p.z - cz
                local al, lat = rx * axx + rz * axz, rx * rtx + rz * rtz
                out[#out + 1] = { name = tostring(o.UnitName), type = tostring(o.Name), player = '', fuel = -1,
                                  x = al + 148, z = lat - 25, yaw = -math.atan2(vx * rtx + vz * rtz, vx * axx + vz * axz),
                                  group = tostring(o.GroupName or '') }
            end
        end
    end
    state.cuePrev = cur
    return out
end

-- Once a second: the cue panel rows (LSO 2) and the case III stack (LSO 1). Which aircraft and status follow
-- what Supercarrier showed in the logs: flights routed to land on the carrier, plus airborne aircraft of its
-- coalition within 10 nm; by day -1, 0 inside ~8 km inbound; at night (case III) 1 for the nearest inbound
-- aircraft and 2 for the others, those drawn on the approach picture.
local function cueTick(now, missionTime)
    if not (diag.share_cue and state.sel) then return end
    local cx, _, cz = camPos()
    if not cx then return end
    local info = missionCueInfo(state.sel.name)
    local cands = {}
    if state.client then
        cands = cueCandidatesClient(cx, cz, state.sel.type)
    else
        local r = tostring(net.dostring_in('server', string.format('local cx, cz = %.3f, %.3f\n', cx, cz) .. CUE_QUERY))
        for line in r:gmatch('[^\n]+') do
            local n, t, pl, fu, x, z, yaw, g = line:match('^(.-)|(.-)|(.-)|([-%d.]+)|([-%d.]+)|([-%d.]+)|([-%d.]+)|(.*)$')
            if n then
                cands[#cands + 1] = { name = n, type = t, player = pl, fuel = tonumber(fu), x = tonumber(x), z = tonumber(z),
                                      yaw = tonumber(yaw), group = g }
            end
        end
    end
    local rows = {}
    for _, a in ipairs(cands) do
        a.dist = math.sqrt(a.x * a.x + a.z * a.z)
        if info.recovering[a.group] or a.dist < 18520 then rows[#rows + 1] = a end
    end
    table.sort(rows, function(a, b) return a.dist < b.dist end)
    local hour = missionTime and math.floor((missionTime % 86400) / 3600) or 12
    local night = state.feed and state.feed.night
    if night == nil then night = hour < 6 or hour >= 19 end
    local lines, case3, active = {}, {}, false
    for i, a in ipairs(rows) do
        if i > 16 then break end
        local inbound = a.x < 0 and math.abs(a.yaw) < math.rad(60)
        local status
        if night then
            if inbound and not active then status, active = 1, true else status = 2 end
        else
            status = (inbound and a.dist < 8000) and 0 or -1
        end
        local board = info.board[a.name] or ''
        if status > 0 and #case3 < 3 then case3[#case3 + 1] = board end
        local pilot = (a.player ~= '' and a.player) or a.name
        lines[#lines + 1] = string.format('%d|%s|%s|%s|%.0f|%.1f|%.1f|%.4f', status, a.type, board, (pilot:gsub('[|\n]', ' ')),
                                          a.fuel or -1, a.x, a.z, a.yaw)
    end
    lines[#lines + 1] = 'case3=' .. table.concat(case3, '|')
    diag.share_cue(table.concat(lines, '\n'))
end

-- ---- Extra cameras ----------------------------------------------------------------------------------
-- Every feed tick: the aircraft that has crossed the round down, in the carrier's axes, to the cameras with
-- track = true, until it taxis out of the wires or the landing area or climbs away (TRACK_* above). Positions come from the
-- passes' tracking (q.trkMap: along / across the landing axis and height, relative to the PLAT camera). Each
-- pass is tracked once.
function cameraTrack(now, q)
    local C = state.cams
    local P = state.passes
    local f = state.feed
    if not (C and f and C.mask ~= 0 and diag.extra_cam) then return end
    P = P or { cur = {} }
    C.trackDone = C.trackDone or {}
    local function stop(why)
        local c = C.trackC
        C.trackDone[c.id] = true
        C.trackC = nil
        if f.trackIds and not P.cur[c.key] then f.trackIds[c.key] = nil end
        diag.log(string.format('EXTRACAM tracking pass %d ended: %s', c.id, why))
    end
    -- an approaching aircraft that has just crossed the round down and has not been tracked yet
    local function newcomer()
        for k, c in pairs(P.cur) do
            local t = q.trkMap and q.trkMap[k]
            if c ~= C.trackC and not C.trackDone[c.id] and t and t.al and not t.gone and t.al > TRACK_RAMP_AL and t.ht - 0.35 < TRACK_CLIMB then
                return c
            end
        end
    end
    if C.trackC then
        local c = C.trackC
        local t = q.trkMap and q.trkMap[c.key]
        if P.cur[c.key] and P.cur[c.key] ~= c then stop('a new pass of the same aircraft')
        elseif not P.cur[c.key] and newcomer() then stop('the next aircraft is at the round down')
        elseif not t or t.gone or not t.al then stop('aircraft gone')
        elseif t.ht - 0.35 > TRACK_CLIMB then stop('climbed 50 ft above the deck')
        elseif t.air == 0 and C.trackStop and (t.al - C.trackStop.al > TRACK_WIRES_M or math.abs(t.lat - C.trackStop.lat) > TRACK_WIRES_M) then
            stop('taxiing out of the wires')
        elseif t.air == 0 and (t.rs or 0) < TRACK_TAXI_SPEED and (math.abs(t.lat) > f.lw or t.al > f.la1 or t.al < f.la0) then
            stop('taxiing out of the landing area')
        elseif now - C.trackSince > TRACK_MAX_S then stop('time limit')
        elseif t.air == 0 and (t.rs or 1e9) < TRACK_STOP_SPEED then
            -- stopped in the wires (or rolled back by them): taxiing out is measured from the furthest-aft stop
            if not C.trackStop then diag.log(string.format('EXTRACAM tracking pass %d: stopped on deck', c.id)) end
            if not C.trackStop or t.al < C.trackStop.al then C.trackStop = { al = t.al, lat = t.lat } end
        end
    end
    if not C.trackC then
        local c = newcomer()
        if c then
            C.trackC, C.trackSince, C.trackStop = c, now, nil
            diag.log(string.format('EXTRACAM tracking pass %d (%s) from the round down', c.id, tostring(c.callsign or c.pilot)))
        end
    end
    -- landing-axis frame -> carrier axes: the axis points 9 deg to port of the bow; PLAT camera at (-14, 20.5, -14.8713)
    local c9, s9 = math.cos(math.rad(9)), math.sin(math.rad(9))
    local function carrier(t) return -14.0 + t.al * c9 + t.lat * s9, 20.5 + t.ht + 1.5, -14.8713 - t.al * s9 + t.lat * c9 end  -- y: a little above
    local key = C.trackC and C.trackC.key
    local t = key and q.trkMap[key]
    local landing = t and { carrier(t) }
    -- the catapults: the airplane on each one, from on the catapult through the launch
    C.cats = C.cats or {}
    local byKey = {}
    for _, a in ipairs(q.nearList or {}) do byKey[a.key] = a end
    for k, cat in ipairs(CATAPULTS) do
        local st = C.cats[k] or {}
        C.cats[k] = st
        local hd = math.rad(cat[3])
        local dx, dz = math.cos(hd), math.sin(hd)
        local function rel(a)
            local x, _, z = carrier(a)
            local rx, rz = x - cat[1], z - cat[2]
            return rx * dx + rz * dz, -rx * dz + rz * dx
        end
        if st.key then
            local a = byKey[st.key]
            local why
            if not a then
                why = 'gone'
            else
                local along, cross = rel(a)
                if not st.launch and a.rs > 8 and along > 0 then
                    st.launch = now
                    diag.log(string.format('EXTRACAM catapult %d: %s launching', k, st.key))
                end
                if st.launch then
                    if along > cat[4] + CAT_GONE_ALONG or a.ht > 60 or now - st.launch > 20 then why = 'launched' end
                elseif math.abs(cross) > CAT_ON_CROSS + 4 or along < CAT_ON_ALONG[1] - 10 or along > CAT_ON_ALONG[2] + 10 then
                    why = 'left without launching'
                end
                st.pos = { carrier(a) }
            end
            if why then
                diag.log(string.format('EXTRACAM catapult %d: %s %s', k, st.key, why))
                st.key, st.launch, st.pos = nil, nil, nil
            end
        end
        if not st.key then
            for _, a in ipairs(q.nearList or {}) do
                local along, cross = rel(a)
                if a.air == 0 and a.rs < 3 and along > CAT_ON_ALONG[1] and along < CAT_ON_ALONG[2] and math.abs(cross) < CAT_ON_CROSS then
                    st.key, st.since, st.launch, st.pos = a.key, now, nil, { carrier(a) }
                    diag.log(string.format('EXTRACAM catapult %d: %s on the catapult', k, a.key))
                    break
                end
            end
        end
    end
    -- the active catapult: the one launching, else the one taken most recently
    local active
    for k, st in ipairs(C.cats) do
        if st.key and (not active or (st.launch and not C.cats[active].launch) or
                       (not st.launch == not C.cats[active].launch and (st.since or 0) > (C.cats[active].since or 0))) then
            active = k
        end
    end
    -- each camera: what it follows
    C.tracking = C.tracking or {}
    for i, c in ipairs(C.list) do
        if C.mask % (2 ^ i) >= 2 ^ (i - 1) then
            local mode = 0
            if diag.share_camera_view then
                local _, _, m = diag.share_camera_view(i - 1)
                mode = tonumber(m) or 0
            end
            local target = TRACK_TARGETS[mode]
            if mode == 0 and c.track then target = 'landing' end
            if c.views and c.views[c.view or 1] and c.views[c.view or 1].hangar then target = nil end  -- a hangar position: never
            if target == 'nearest' then
                local best, bd
                for k, cat in ipairs(CATAPULTS) do
                    local hd = math.rad(cat[3])
                    local mx, mz = cat[1] + math.cos(hd) * cat[4] / 2, cat[2] + math.sin(hd) * cat[4] / 2
                    local d = (mx - c.pos[1]) ^ 2 + (mz - c.pos[3]) ^ 2
                    if not bd or d < bd then best, bd = k, d end
                end
                target = best
            elseif target == 'active' then
                target = active
            end
            local pos
            if target == 'landing' then pos = landing
            elseif type(target) == 'number' and C.cats[target] then pos = C.cats[target].pos end
            if pos then
                diag.extra_cam('track', i - 1, pos[1], pos[2], pos[3], API.getModelTime())
            elseif C.tracking[i] then
                diag.extra_cam('track', i - 1)
            end
            C.tracking[i] = pos and true or nil
        end
    end
end

-- A camera with views takes its position, look-at point and field of view from the view shown.
local function applyView(c)
    if not c.views or #c.views == 0 then return end
    c.view = math.max(1, math.min(#c.views, c.view or 1))
    local v = c.views[c.view]
    c.pos, c.look = v.pos, v.look
    c.baseFov = c.baseFov or c.fov or 60
    c.fov = v.fov or c.baseFov
end

-- The last two arguments: what the camera shows (the view, else the camera; the DLL publishes the pose under that
-- name, Header::camPose, for the viewer's tuning panel), and whether that is a hangar position (no rain or snow).
local function sendCamera(i, c)
    applyView(c)
    local v = c.views and c.views[c.view]
    local shown = v and v.name or c.name
    diag.extra_cam('set', i - 1, c.pos[1], c.pos[2], c.pos[3], c.look[1], c.look[2], c.look[3], c.fov or 60, c.aspect or 4 / 3,
                   c.source or (29 + i), c.type or 0, tostring(shown), (v and v.hangar) and true or false)
end

-- Every camera's settings to the DLL (at the start, and after the tuning file changed).
local function configureCameras(C)
    C.configured = true
    for i, c in ipairs(C.list) do
        sendCamera(i, c)
        -- pan_only: 'right' / 'left' as seen in the picture of a camera that looks aft (right = toward port)
        diag.extra_cam('ptz', i - 1, c.pan_rate or 60, c.pan_ramp or 0.3, c.pan_deadband or 8,
                       c.pan_only == 'right' and 1 or c.pan_only == 'left' and -1 or 0)
    end
end

-- The tuning file, Saved Games\DCS\Config\ILARTS-cameras.lua, checked every 0.5 s (its text: lfs's modification
-- time has a resolution of 1 s, and the viewer's tuning panel writes more often than that). Each read starts from
-- the table as written: every field an earlier read set is put back first (C.orig), so an entry taken out of the
-- file (the panel's RESET) is undone at once. The cameras are then sent again, so a slot already showing a tuned
-- view moves without a view change.
local function cameraTuning(C, now)
    if now < (C.fileAt or 0) then return end
    C.fileAt = now + 0.5
    local okl, lfs = pcall(require, 'lfs')
    if not (okl and lfs and lfs.writedir) then return end
    local path = lfs.writedir() .. 'Config\\ILARTS-cameras.lua'
    local f = io.open(path, 'rb')
    local text = f and f:read('*a')
    if f then f:close() end
    if text == C.fileText then return end
    C.fileText = text
    local t = {}
    if text then
        if text:sub(1, 3) == '\239\187\191' then text = text:sub(4) end  -- a UTF-8 BOM (Lua 5.1 cannot read one)
        local chunk, err = loadstring(text, '=ILARTS-cameras.lua')
        local okf, r = pcall(chunk or function() error(err, 0) end)
        if not (okf and type(r) == 'table') then
            diag.log('EXTRACAM ' .. path .. ' not usable: ' .. tostring(r))
            return  -- keep what the last good read set
        end
        t = r
    end
    C.orig = C.orig or {}  -- [object] = { [field] = { value as written in CAMERAS } }
    for obj, fields in pairs(C.orig) do
        for k, v in pairs(fields) do obj[k] = v[1] end
    end
    local function set(obj, o)
        local keep = C.orig[obj] or {}
        C.orig[obj] = keep
        for k, v in pairs(o) do
            if k ~= 'name' then
                if not keep[k] then keep[k] = { obj[k] } end
                obj[k] = v
            end
        end
    end
    for _, o in ipairs(t) do  -- in the file's order: a later entry for the same name wins
        if type(o) == 'table' and o.name then
            for _, c in ipairs(C.list) do
                if c.name == o.name then set(c, o) end
                for _, v in ipairs(c.views or {}) do  -- a view of a camera, by its name (shared by a pool's slots)
                    if v.name == o.name then set(v, o) end
                end
            end
        end
    end
    configureCameras(C)
    diag.log('EXTRACAM settings ' .. (text and 'read from ' or 'reset: no ') .. path)
end

-- Every feed tick: the viewer's view requests (each a new sequence number; one made before the hook started
-- counts too), applied at once, and each camera's views to the viewer (the names, the one shown).
function cameraViews(now)
    local C = state.cams
    if not (C and C.list and diag.share_camera_view and diag.share_camera_views) then return end
    if C.configured then cameraTuning(C, now) end
    C.viewSeq = C.viewSeq or {}
    for i, c in ipairs(C.list) do
        if c.views and #c.views > 0 then
            local seq, req = diag.share_camera_view(i - 1)
            seq, req = tonumber(seq) or 0, tonumber(req) or -1
            local fresh = C.viewSeq[i] == nil and seq ~= 0 or C.viewSeq[i] ~= nil and seq ~= C.viewSeq[i]
            C.viewSeq[i] = seq
            if fresh and req >= 0 and req < #c.views and req + 1 ~= c.view then
                c.view = req + 1
                if C.configured then sendCamera(i, c) end
                diag.log(string.format('EXTRACAM %s: view %s', c.name, c.views[c.view].name))
            end
            applyView(c)
            local names = {}
            for k, v in ipairs(c.views) do  -- '\t' + flags after the name: 'H' = a hangar position (ShareSetCameraViews)
                names[k] = tostring(v.name):gsub('[|\t]', ' ') .. (v.hangar and '\tH' or '')
            end
            local text = table.concat(names, '|')
            if text ~= c.viewsText or c.view ~= c.viewShown or now >= (c.viewsAt or 0) then
                c.viewsText, c.viewShown, c.viewsAt = text, c.view, now + 2
                diag.share_camera_views(i - 1, c.view - 1, text)
            end
        end
    end
end

-- Once a second: the cameras' settings to the DLL, the viewer's request (which cameras
-- to render), what DCS made of it (which render targets exist), and the frame rate before / after a change.
local function cameraTick(now)
    if not (EXTRA_CAMERAS and diag.extra_cam and diag.share_cameras and state.sharing) then return end
    local C = state.cams
    if not C then
        C = { seq = 0, mask = 0, fps = {}, list = {} }
        for _, c in ipairs(CAMERAS) do
            local views
            if c.views then  -- copies, so a mission restart starts from the table as written; shared by a pool's slots
                views = {}
                for k, v in ipairs(c.views) do
                    local w = {}
                    for kk, vv in pairs(v) do w[kk] = vv end
                    views[k] = w
                end
            end
            for k, src in ipairs(c.pool or { c.source or (29 + #C.list + 1) }) do
                if #C.list >= MAX_CAMERAS then break end
                local t = {}
                for kk, v in pairs(c) do t[kk] = v end
                t.pool, t.views, t.source = nil, views, src
                if k > 1 then t.name = c.name .. ' ' .. k end
                applyView(t)
                C.list[#C.list + 1] = t
            end
        end
        state.cams = C
    end
    -- frame rate (DCS frames per second over the last 10 s)
    local frames = tonumber(diag.frame_count and diag.frame_count() or 0) or 0
    if C.lastFrames and now > C.lastT then
        table.insert(C.fps, (frames - C.lastFrames) / (now - C.lastT))
        if #C.fps > 10 then table.remove(C.fps, 1) end
    end
    C.lastFrames, C.lastT = frames, now
    local function avgFps()
        local s = 0
        for _, v in ipairs(C.fps) do s = s + v end
        return #C.fps > 0 and s / #C.fps or 0
    end
    -- the tuning file (cameraTuning, then every 0.5 s from cameraViews)
    if not C.configured then
        cameraTuning(C, now)
        if not C.configured then configureCameras(C) end
    end
    local avail, why = diag.extra_cam('available')
    if (avail and 'yes' or tostring(why)) ~= C.availText then
        C.availText = avail and 'yes' or tostring(why)
        diag.log('EXTRACAM available: ' .. C.availText)
    end
    -- the viewer's request
    local seq, want = diag.share_camera_control()
    seq, want = tonumber(seq) or 0, tonumber(want) or 0
    -- Not while the mission is still loading: the cameras start CAMERA_START_DELAY s after the PLAT is live.
    -- (2026-11-14: two cameras started 1 s into a mission load; 0.26 s later the GPU faulted - NVIDIA "FECS
    -- Exception: UCODE Fatal Error", TDR - and DCS went down.)
    if not C.readyAt then
        local st = diag.share_status and diag.share_status() or ''
        if st:match('LSOFRAME gen=%d+ %d+x%d+ copies=%d+ skipped=%d+ live') then
            C.readyAt = now + CAMERA_START_DELAY
            diag.log(string.format('EXTRACAM PLAT live: cameras may start in %d s', CAMERA_START_DELAY))
        end
    end
    local ready = C.readyAt and now >= C.readyAt
    -- one more camera per tick (each new one makes DCS build a render pipeline); switching off is at once
    local step = want
    do
        local add = 0
        for i = 1, #C.list do
            local bit = 2 ^ (i - 1)
            if want % (2 * bit) >= bit and not (C.mask % (2 * bit) >= bit) then add = bit break end
        end
        step = 0
        for i = 1, #C.list do
            local bit = 2 ^ (i - 1)
            if C.mask % (2 * bit) >= bit and want % (2 * bit) >= bit then step = step + bit end
        end
        step = step + add
    end
    -- a request for cameras waits while they cannot run yet (the PLAT camera starting with a mission)
    if seq ~= C.seq and (avail and ready or want == 0) then
        if step == want then C.seq = seq end  -- else the next tick starts the next camera
        want = step
        if want ~= C.mask then
            for i, c in ipairs(C.list) do
                if want % (2 ^ i) >= 2 ^ (i - 1) then
                    local r, e = diag.set_mfd_resolution(c.source or (29 + i), c.size or 768, c.size or 768)
                    diag.log(string.format('EXTRACAM %s: picture source %d at %d px: %s', c.name, c.source or (29 + i), c.size or 768,
                                           tostring(r and 'requested' or e)))
                end
            end
            local r, e = diag.extra_cam('enable', want)
            diag.log(string.format('EXTRACAM cameras %d -> %d: %s', C.mask, want, r and 'ok' or tostring(e)))
            if r then
                C.perf = { from = C.mask, to = want, before = avgFps(), at = now + 12 }
                C.mask, C.since = want, now
                C.fps = {}
            end
        end
    end
    if C.perf and now >= C.perf.at then
        diag.log(string.format('EXTRACAM frame rate: cameras %d -> %d: %.1f fps before, %.1f fps after (10 s averages)', C.perf.from,
                               C.perf.to, C.perf.before, avgFps()))
        C.perf = nil
    end
    -- which render targets DCS made for each camera; share the ones that exist
    local lines = {}
    for i, c in ipairs(C.list) do
        local src = c.source or (29 + i)
        local on = C.mask % (2 ^ i) >= 2 ^ (i - 1)
        local ldr, hdr = 'mfd' .. src, 'hdrBufferMFD' .. src
        if on and diag.rt_exists then
            local found = {}
            for _, n in ipairs({ 'mfd' .. src, 'ldrBuffer0MFD' .. src, 'hdrBufferMFD' .. src }) do found[n] = diag.rt_exists(n) and true or false end
            if not found[ldr] and found['ldrBuffer0MFD' .. src] then ldr = 'ldrBuffer0MFD' .. src end
            local text = string.format('mfd%d=%s ldrBuffer0MFD%d=%s hdrBufferMFD%d=%s', src, tostring(found['mfd' .. src]), src,
                                       tostring(found['ldrBuffer0MFD' .. src]), src, tostring(found[hdr]))
            if text ~= c.targetsText then
                c.targetsText = text
                diag.log('EXTRACAM ' .. c.name .. ' render targets: ' .. text .. ' (sharing ' .. ldr .. ' and ' .. hdr .. ')')
            end
            if diag.share_retarget then
                diag.share_retarget(4 + (i - 1) * 2, ldr)
                diag.share_retarget(5 + (i - 1) * 2, hdr)
                c.shared = true
            end
        elseif c.shared and diag.share_retarget then
            -- switched off: stop copying its render targets (DCS no longer draws into them; the copy would only
            -- hand the viewer the last picture, frozen, every frame)
            diag.share_retarget(4 + (i - 1) * 2, 'off')
            diag.share_retarget(5 + (i - 1) * 2, 'off')
            c.shared, c.targetsText = false, nil
        end
        lines[#lines + 1] = string.format('%s|%s|%s|%d|%d|%.4f', c.name, ldr, hdr, avail and 1 or 0, on and 1 or 0, c.aspect or 4 / 3)
    end
    diag.share_cameras(table.concat(lines, '\n'))
    if C.mask ~= 0 and now >= (C.statsAt or 0) then
        C.statsAt = now + 10
        diag.log('EXTRACAM ' .. tostring(diag.extra_cam('stats')))
        diag.log('EXTRACAM share: ' .. tostring(diag.share_status()))
    end
end

-- Once a second: carrier list to the viewer, the viewer's choice, a station visit, the default.
local function carrierTick(now)
    local list = listCarriers()
    -- the viewer's request (a new sequence number)
    if diag.share_control then
        local seq, name = diag.share_control()
        seq = tonumber(seq) or 0
        -- sharing clears the shared block when a mission starts, so a request already there was made in this
        -- mission (the viewer choosing its carrier again after a restart): act on it too
        local first = state.ctlSeq == nil
        if first and seq == 0 then
            state.ctlSeq = seq
        elseif seq ~= state.ctlSeq then
            state.ctlSeq = seq
            for _, c in ipairs(list) do
                if c.name == name then
                    selectCarrier(c, 'chosen in the viewer')
                    break
                end
            end
        end
    end
    -- arriving at a station (or changing station) selects that station's carrier; the viewer's choice
    -- after that stands
    -- The station is found from your own view (the main camera stands on the station's carrier). DCS's LSO
    -- camera is only the fallback: with two carriers it was seen to report the other carrier on alternate
    -- seconds after each selection, which flipped the selection every second (2026-11-20 log, CVN_71 / 72).
    -- A station change must also hold for two ticks, and a station selection is not repeated within 10 s.
    if state.camKind == 'live' then
        local ex = rawget(_G, 'Export')
        local okc, cam = pcall(function() return ex and ex.LoGetCameraPosition and ex.LoGetCameraPosition() end)
        local c, src
        if okc and cam and cam.p then
            c, src = nearestCarrier(list, cam.p.x, cam.p.z, 2000), 'your camera'
        else
            local cx, _, cz = camPos()
            c, src = cx and nearestCarrier(list, cx, cz, 2000), "DCS's LSO camera"
        end
        if c and c.name ~= state.stationCarrier then
            if state.stationPending ~= c.name then
                state.stationPending = c.name
            elseif not state.sel or state.sel.name ~= c.name then
                if now >= (state.stationHold or 0) then
                    state.stationCarrier, state.stationPending = c.name, nil
                    state.stationHold = now + 10
                    selectCarrier(c, 'LSO station visited (' .. src .. ')')
                end
            else
                state.stationCarrier, state.stationPending = c.name, nil
            end
        elseif c then
            state.stationPending = nil
        end
    else
        state.stationCarrier, state.stationPending = nil, nil
    end
    -- keep the selection current; pick a default when there is none (or it is gone)
    local selIdx = -1
    for i, c in ipairs(list) do
        if state.sel and c.name == state.sel.name then
            selIdx = i - 1
            state.sel.x, state.sel.y, state.sel.z, state.sel.id = c.x, c.y, c.z, c.id
            if state.synth then diag.plat_cam('select', c.x, c.y, c.z, c.type) end
        end
    end
    if selIdx < 0 and #list > 0 then
        local ex = rawget(_G, 'Export')
        local okc, cam = pcall(function() return ex and ex.LoGetCameraPosition and ex.LoGetCameraPosition() end)
        local c = okc and cam and cam.p and nearestCarrier(list, cam.p.x, cam.p.z) or list[1]
        selectCarrier(c, state.sel and 'previous carrier gone' or 'nearest to your camera')
        for i, d in ipairs(list) do
            if d.name == c.name then selIdx = i - 1 end
        end
    end
    if diag.share_carriers then
        local lines = {}
        for _, c in ipairs(list) do
            lines[#lines + 1] = string.format('%s|%s|%.0f|%.0f', c.name:gsub('[|\n]', ' '), c.type, c.x, c.z)
        end
        diag.share_carriers(table.concat(lines, '\n'), selIdx)
    end
    if #list ~= (state.carrierCount or -1) then
        state.carrierCount = #list
        diag.log('PLATCAM carriers in the mission: ' .. #list)
    end
    local okq, errq = pcall(cueTick, now, state.missionStart and (state.missionStart + API.getModelTime()) or nil)
    if not okq and errq ~= state.cueErr then
        state.cueErr = errq
        diag.log('CUEPANEL error: ' .. tostring(errq))
    end
    local okx, errx = pcall(cameraTick, now)
    if not okx and errx ~= state.camErr then
        state.camErr = errx
        diag.log('EXTRACAM error: ' .. tostring(errx))
    end
    -- host / single player: check the mission's ground wind against DCS's own (for client mode)
    if VERBOSE and not state.client and state.sel and now >= (state.windCheckAt or 0) then
        state.windCheckAt = now + 60
        local code = string.format('local px, py, pz = %.1f, %.1f, %.1f\n', state.sel.x, 11, state.sel.z) .. WIND_QUERY
        local r = tostring(net.dostring_in('server', code))
        local wx, wz = r:match('^([-%d.]+) ([-%d.]+)$')
        local save = MISSION_WIND_DIR
        MISSION_WIND_DIR = 'to'
        local a = missionWind()
        MISSION_WIND_DIR = 'from'
        local b = missionWind()
        MISSION_WIND_DIR = save
        diag.log(string.format('WINDCHECK DCS at 11 m: %s | mission dir as "to": %.2f %.2f | as "from": %.2f %.2f (using %s)',
                               wx and (wx .. ' ' .. wz) or r, a.x, a.z, b.x, b.z, MISSION_WIND_DIR))
    end
end

-- The chosen carrier's pose this frame: "xx xy xz yx yy yz zx zy zz px py pz", or nil.
local function selectedShipAxes()
    local sel = state.sel
    if not sel then return nil end
    if state.client then
        local ex = rawget(_G, 'Export')
        if not (ex and ex.LoGetObjectById and sel.id) then return nil end
        local ok, o = pcall(ex.LoGetObjectById, sel.id)
        if not (ok and o and o.Position) then return nil end
        local X, Y, Z = axesHPB(o.Heading or 0, o.Pitch or 0, o.Bank or 0)
        return string.format('%.9f %.9f %.9f %.9f %.9f %.9f %.9f %.9f %.9f %.4f %.4f %.4f', X[1], X[2], X[3], Y[1], Y[2], Y[3],
                             Z[1], Z[2], Z[3], o.Position.x, o.Position.y, o.Position.z)
    end
    local axes = net.dostring_in('server', SHIP_AXES_QUERY)
    if type(axes) == 'string' and axes:match('^[-%d]') then return axes end
    return nil
end

-- SHIP_POSE_TEST: the ship axes "xx xy xz yx yy yz zx zy zz" turned about the vertical onto the export
-- heading (the stepped, state-update heading). Returns the turned axes (or the input when not possible) and
-- the heading difference in radians (export minus smooth; nil when there is no export heading).
local function steppedAxes(axes)
    local ok, es = pcall(exportShip)
    local h = ok and tonumber((tostring(es)):match('^%S+ %S+ %S+ (%S+)'))
    if not h then return axes, nil end
    local v = {}
    for n in axes:gmatch('%S+') do v[#v + 1] = tonumber(n) end
    if #v ~= 9 then return axes, nil end
    local d = h - math.atan2(v[3], v[1])
    d = (d + math.pi) % (2 * math.pi) - math.pi
    if math.abs(d) > math.rad(1) then return axes, d end   -- not a small lag: leave the axes alone
    local c, s = math.cos(d), math.sin(d)
    for i = 0, 2 do
        local x, z = v[3 * i + 1], v[3 * i + 3]
        v[3 * i + 1], v[3 * i + 3] = x * c - z * s, x * s + z * c
    end
    return string.format('%.9f %.9f %.9f %.9f %.9f %.9f %.9f %.9f %.9f', unpack(v)), d
end

local function shipPoseTest(now, axes)
    local t = state.poseTest
    if not t then t = {}; state.poseTest = t end
    local ph = math.floor(now / SHIP_POSE_PERIOD) % 2
    local function flush()
        if t.n and t.n > 0 then
            diag.log(string.format('SHIPPOSE [%s] phase %s frames=%d applied=%d no_heading=%d too_big=%d export-minus-smooth min %+.4f max %+.4f deg',
                tostring(state.camKind), t.ph == 1 and 'B' or 'A', t.n, t.applied, t.nohdg, t.big,
                math.deg(t.dmin or 0), math.deg(t.dmax or 0)))
        end
        t.n, t.applied, t.nohdg, t.big, t.dmin, t.dmax, t.logAt = 0, 0, 0, 0, nil, nil, now + 5
    end
    if ph ~= t.ph then
        flush()
        t.ph = ph
        local label = ph == 1 and 'B' or 'A'
        screenText('PLAT test phase ' .. label, SHIP_POSE_PERIOD - 1)
        diag.log('SHIPPOSE test: phase ' .. label .. ' [' .. tostring(state.camKind) .. ']')
    elseif now >= t.logAt then
        flush()
    end
    local stepped, d = steppedAxes(axes)
    t.n = t.n + 1
    if d then
        t.dmin = math.min(t.dmin or d, d)
        t.dmax = math.max(t.dmax or d, d)
        if math.abs(d) > math.rad(1) then t.big = t.big + 1 end
    else
        t.nohdg = t.nohdg + 1
    end
    if ph == 1 and stepped ~= axes then
        t.applied = t.applied + 1
        return stepped
    end
    return axes
end

local function deckTrace(now)
    local d = state.deckTrace
    if not d then
        d = { next = now + 20 }
        state.deckTrace = d
    end
    if not d.active then
        if now < d.next or not state.cloudsOk or not state.camKind or state.camKind == 'null' then return end
        diag.clouds('trace_on')
        d.active = { t0 = now, kind = tostring(state.camKind), rows = {} }
        diag.log(string.format('DECKTRACE start [%s] qpc=%.6f realtime=%.4f modeltime=%.4f', d.active.kind,
                 tonumber(diag.clouds('qpc')) or 0, now, API.getModelTime()))
        return
    end
    local a = d.active
    local q = tonumber(diag.clouds('qpc')) or 0
    local s = tostring(net.dostring_in('server', SHIP_TRACE_QUERY))
    a.rows[#a.rows + 1] = string.format('%.6f %.4f %s', q, API.getModelTime(), s)
    if now - a.t0 >= 3 then
        local n = diag.clouds('trace_dump', a.kind)
        for _, r in ipairs(a.rows) do diag.log('SHIPTRACE ' .. a.kind .. ' ' .. r) end
        diag.log(string.format('DECKTRACE end [%s] camera samples=%s ship samples=%d', a.kind, tostring(n), #a.rows))
        d.active = nil
        d.next = now + 27
    end
end

function callbacks.onSimulationFrame()
    if not state then return end
    local now = API.getRealTime()
    -- per-frame PLAT camera pose sample (POSETRACE lines: does the heading advance smoothly?)
    if LSO_CAMERA_PROBE and diag.lso_camera and state.keepPlat then
        -- deck jitter fix: refresh what the PLAT view sees on every frame (hull_refresh.h)
        if HULL_REFRESH ~= 'off' and diag.hull_refresh then
            if state.hullFixOk == nil then
                local r, why = diag.hull_refresh('install')
                state.hullFixOk = r and true or false
                diag.log('HULLREFRESH install: ' .. tostring(r and 'ok' or why))
                if state.hullFixOk and HULL_REFRESH == 'on' then diag.hull_refresh('on') end
            end
            if state.hullFixOk then
                if HULL_REFRESH == 'ab' then
                    local on = math.floor(now / HULL_REFRESH_PERIOD) % 2 == 1
                    if on ~= state.hullFixOn then
                        if state.hullFixOn ~= nil then
                            -- close the statistics windows so each one lies inside one phase
                            vlog('HULLREFRESH [' .. tostring(state.camKind) .. '] (phase end) ' .. tostring(diag.hull_refresh('stats')))
                            state.hullFixStatsAt = now + 5
                            if HULL_SCAN and diag.hull_scan then
                                diag.log('HULLSCAN [' .. tostring(state.camKind) .. '] (phase end) ' .. tostring(diag.hull_scan('stats')))
                                state.hullStatsAt = now + 5
                            end
                        end
                        state.hullFixOn = on
                        diag.hull_refresh(on and 'on' or 'off')
                        screenText('PLAT test: hull refresh ' .. (on and 'ON' or 'OFF'), HULL_REFRESH_PERIOD - 1)
                        diag.log('HULLREFRESH test: ' .. (on and 'ON' or 'OFF') .. ' [' .. tostring(state.camKind) .. ']')
                    end
                end
                if now >= (state.hullFixStatsAt or 0) then
                    state.hullFixStatsAt = now + (HULL_REFRESH == 'ab' and 5 or 60)
                    vlog('HULLREFRESH [' .. tostring(state.camKind) .. '] ' .. tostring(diag.hull_refresh('stats')))
                end
            end
        end
        -- carrier list / selection (viewer, station visit, default), once a second
        if now >= (state.carrierAt or 0) then
            state.carrierAt = now + 1
            local okc, errc = pcall(carrierTick, now)
            if not okc and errc ~= state.carrierErr then
                state.carrierErr = errc
                diag.log('PLATCAM carrier tick error: ' .. tostring(errc))
            end
        end
        local axes
        if state.synth then
            -- the chosen carrier's mission pose: the synthetic camera's fallback and cross-check
            axes = selectedShipAxes()
            if axes then diag.plat_cam('ship', axes) end
        end
        if CAMERA_SMOOTH then
            if state.smoothOn == nil then
                local why = diag.lso_camera('smooth_on')
                state.smoothOn = (why == '')
                diag.log('CAMSMOOTH enable: ' .. (state.smoothOn and 'ok' or tostring(why)))
            end
            if state.smoothOn then
                axes = axes or selectedShipAxes()
                if type(axes) == 'string' and axes:match('^[-%d]') then
                    if SHIP_POSE_TEST then axes = shipPoseTest(now, axes) end
                    diag.lso_camera('ship', axes)
                    if HULL_SCAN and diag.hull_scan then
                        state.hullScanAt = state.hullScanAt or (now + 20)
                        if now >= state.hullScanAt then
                            state.hullScanAt = now + 90
                            local ok, why = diag.hull_scan('start', tostring(state.camKind))
                            diag.log('HULLSCAN start [' .. tostring(state.camKind) .. ']: ' .. (ok and 'ok' or tostring(why)))
                        end
                        if now >= (state.hullStatsAt or 0) then
                            state.hullStatsAt = now + 5
                            diag.log('HULLSCAN [' .. tostring(state.camKind) .. '] ' .. tostring(diag.hull_scan('stats')))
                        end
                    end
                end
                if now >= (state.smoothNext or 0) then
                    state.smoothNext = now + 5
                    vlog('CAMSMOOTH ' .. tostring(diag.lso_camera('smooth_stats')))
                end
            end
        end
        local okp, pose = pcall(diag.lso_camera, 'sample')
        -- trace: the pose DCS drew the carrier with since the previous simulation frame
        local draw
        if HDG_TRACE then
            if state.shipDraw == nil then
                local why = diag.lso_camera('ship_draw_on')
                state.shipDraw = (why == '')
                diag.log('HDGTRACE ship draw tap: ' .. (state.shipDraw and 'ok' or tostring(why)))
            end
            if state.shipDraw then draw = diag.lso_camera('ship_draw') end
        end
        -- HDGTRACE: every 20 s, 4 s of per-frame rows "camera: qpc yaw x y z | ship: x y z fwd.x fwd.z"
        -- (ship = Unit:getPosition() in the mission environment) to compare camera and ship heading.
        if HDG_TRACE and okp and type(pose) == 'string' and pose ~= 'none' then
            local h = state.hdgTrace
            if not h then h = { next = now + 15 }; state.hdgTrace = h end
            if not h.rows and now >= h.next then h.rows, h.t0 = {}, now end
            if h.rows then
                local s = tostring(net.dostring_in('server', SHIP_TRACE_QUERY))
                local oke, es = pcall(exportShip)
                h.rows[#h.rows + 1] = string.format('%.4f %s | %s | %s | %s | %s', API.getModelTime(), pose, s, tostring(axes), tostring(es), tostring(draw))
                if now - h.t0 >= 4 then
                    -- one buffered write to hdgtrace.log next to the diagnostic log (the log flushes every
                    -- line: several hundred rows through it froze DCS for seconds)
                    local pre = (os and os.date and os.date('%Y-%m-%d %H:%M:%S') or '0000-00-00 00:00:00') .. '.000 [0] LUA HDGTRACE '
                    local out = { pre .. 'start rows=' .. #h.rows .. ' kind=' .. tostring(state.camKind) }
                    for _, r in ipairs(h.rows) do out[#out + 1] = pre .. r end
                    local path = tostring(diag.logpath()):gsub('[^\\/]+$', 'hdgtrace.log')
                    local f = io and io.open(path, 'a')
                    if f then f:write(table.concat(out, '\n'), '\n'); f:close() end
                    diag.log('HDGTRACE wrote ' .. #h.rows .. ' rows kind=' .. tostring(state.camKind) .. ' to ' .. path .. (f and '' or ' (FAILED)'))
                    h.rows, h.next = nil, now + 16
                end
            end
        end
    end
    if DECK_TRACE then
        local okd, errd = pcall(deckTrace, now)
        if not okd then diag.log('DECKTRACE error: ' .. tostring(errd)) end
    end
    -- automatic first LSO view (see AUTO_LSO_VIEW)
    local a = state.autoLso
    if a and a.phase == 'pending' and now >= (a.tNext or 0) then
        a.tNext = now + 0.5
        local s = diag.lso_camera()
        local paused = API.getPause and API.getPause()
        local mt = API.getModelTime and API.getModelTime() or 0
        if not s:match('^camera null %(last 0+%)') then
            state.autoLso = nil
            diag.log('AUTOLSO not needed: ' .. s)
        elseif not paused and mt > 2 then
            a.wait = a.wait or now
            local back = hasAircraft()
            -- in an aircraft, or nothing to return to after 20 s of running sim (spectator). Multiplayer:
            -- only once you are in an aircraft (the sim runs while you are still choosing a slot).
            if back or (not isMultiplayer() and now - a.wait > 20) then
                a.tries = a.tries + 1
                local ok2, how = sendCommand(CMD_VIEW_LSO)
                diag.log(string.format('AUTOLSO try %d: LSO view (%d) %s (model time %.1f s, return to cockpit after: %s)',
                         a.tries, CMD_VIEW_LSO, ok2 and ('sent via ' .. how) or ('failed: ' .. how), mt, tostring(back)))
                if ok2 then
                    a.phase, a.t0, a.back, a.retried = 'wait', now, back, false
                else
                    state.autoLso = nil
                end
            end
        end
    end
    if a and a.phase ~= 'pending' then
        if a.phase == 'wait' then
            local s = diag.lso_camera()
            if s:match('^camera [0-9A-F]') then
                a.phase, a.tBack = 'back', now + AUTO_LSO_BACK_DELAY
                diag.log(string.format('AUTOLSO camera handed out after %.1f s', now - a.t0))
            elseif not a.retried and now - a.t0 > 3 then
                a.retried = true
                local ok2, how = sendCommand(CMD_VIEW_LSO, 2)
                diag.log('AUTOLSO no camera after 3 s, resent via export state: ' .. (ok2 and how or ('failed: ' .. how)))
            elseif now - a.t0 > AUTO_LSO_TIMEOUT then
                diag.log('AUTOLSO no LSO camera after ' .. AUTO_LSO_TIMEOUT .. ' s (' .. s .. ')')
                if a.tries < 3 then
                    a.phase, a.tNext = 'pending', now + 2   -- try again
                    if a.back then sendCommand(CMD_VIEW_COCKPIT) end
                else
                    a.phase, a.tBack = 'back', now
                end
            end
        end
        if a.phase == 'back' and now >= a.tBack then
            state.autoLso = nil
            if a.back then
                local ok2, how = sendCommand(CMD_VIEW_COCKPIT)
                diag.log('AUTOLSO back to cockpit: ' .. (ok2 and 'sent' or ('failed: ' .. how)))
            end
        end
    end
    -- overlay feed (between the once-a-second steps). Also at the station: DCS draws its own overlay
    -- there (feedPush does not push into it), but the viewer still needs dcsOverlay=1 and fresh values,
    -- or it keeps drawing its own copy over DCS's with the last values from before the visit.
    local f = state.feed
    if f and now >= f.nextPush and (state.camKind == 'subst' or state.camKind == 'live') then
        f.nextPush = now + FEED_PERIOD
        local okf, errf = pcall(feedPush, now)
        if not okf then
            diag.log('PLATFEED error, feed disabled: ' .. tostring(errf))
            state.feed = nil
        end
    end
    if now < state.next then return end
    state.next = now + 1
    local good, err = pcall(step, now)
    if not good then
        log.write('ILARTS', log.ERROR, 'step failed, disabling: ' .. tostring(err))
        if state.keepPlat then pcall(diag.keep_plat, false) end
        if state.uiInstalled then pcall(diag.plat_ui, 'reset') end  -- as at mission stop: no recorded states left behind
        if state.cloudsOk then pcall(diag.clouds, 'remove') end
        if state.canopyOk then pcall(diag.own_canopy, 'remove') end
        if state.starsOk then pcall(diag.night_sky, 'remove') end
        if state.rainOk then pcall(diag.rain, 'remove') end
        if state.originOk then pcall(diag.plat_origin, 'remove') end
        if state.mapObjOk then pcall(diag.map_objects, 'remove') end
        if state.hullFixOk then pcall(diag.hull_refresh, 'remove') end
        if state.cams and diag.extra_cam then pcall(diag.extra_cam, 'remove') end
        if state.smoothOn then pcall(diag.lso_camera, 'smooth_off') end
        if state.shipDraw then pcall(diag.lso_camera, 'ship_draw_off') end
        if state.bench then pcall(diag.bench, 'ended', 'ILARTS stopped (error)') end
        pcall(diag.share_stop)
        state = nil
    end
end

if VERBOSE then
    local ok, err = pcall(glideslopeReport, 'DCS start')
    if not ok then diag.log('GLIDESLOPE error: ' .. tostring(err)) end
end

API.setUserCallbacks(callbacks)
