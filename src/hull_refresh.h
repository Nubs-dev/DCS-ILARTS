// Deck jitter away from the LSO station (cause found 2026-11-12: static analysis of Visualizer 2.9.31 plus
// HULLSCAN data; the same code is in 2.9.25).
//
// How an object's transform reaches the renderer. Each frame the world object search (+0x19F8C0) runs a
// few searches and hands every object found to +0x128950 -> +0x1242F0 ("add or get the scene entry") ->
// +0xFE220. For an entry that already exists (status 1) +0xCEDE0 decides whether this call refreshes the
// transform. Two throttles, both driven by the distance to the player's MAIN camera (the search
// descriptor, a global at +0x2F0CA8, holds the main camera position for every search, extra views too):
//   1. objects whose flags (obj+0x78, vtable +0x28) have bit 0x40 (the carrier has): only every N-th call,
//      N = 1 up to 500 m, rising to 20 at 3 km and beyond (+2 for the second search pass); call counter
//      at entry+0x40, squared distance at entry+0x38.
//   2. every object: +0xD1070 allows the next refresh only after an interval that grows with distance
//      (0.006-0.01 s near, 0.2-0.25 s beyond 20-35 km); last-refresh times at entry+0x28 / +0x30.
// A refresh = +0x125B50 (object matrix) -> +0xFD870 -> a command to the scene. Between refreshes the
// scene keeps the old ROTATION while the position stays current, so the drawn hull is turned about its
// own origin by up to (turn rate x refresh interval).
//
// At the LSO station the main camera is next to the carrier: refreshed on every frame - steady. With the
// player far away the carrier (and the deck objects that hang on it) is refreshed about 5 times a second:
// measured 0.26 deg steps at 1.32 deg/s every ~0.2 s at 73 fps - the deck jitter in the PLAT. Aircraft
// near the carrier are throttled the same way (0.2 s steps). The "force" argument of +0x128950 (ctx[3],
// a byte) makes +0xCEDE0 skip throttle 2 and do a full refresh; throttle 1 is tested before it.
//
// Fix: the flag test at +0xCEE14 (test al,40h ; je refresh) is routed through a stub that first asks
// HullRefreshWanted(ctx, entry). When enabled and the object is within 3 km of the PLAT camera (object
// position from DCS's own +0x125B50, cached per entry), it sets the force byte and goes straight to the
// refresh, once per frame per object - what DCS itself does when the player is at the station.
// Everything else follows DCS's own flow. Main thread only (search and hand-off run there).
#pragma once

#include <string>

namespace lsodiag {

std::string HullRefreshInstall();  // "" on success, otherwise why not (verifies build and code bytes)
void HullRefreshRemove();
void HullRefreshEnable(bool on);
std::string HullRefreshStats();    // counts since the previous call

}  // namespace lsodiag
