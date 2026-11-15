// Deck jitter investigation (2026-11-12): find the pose DCS actually draws the carrier with.
// Viewer captures show that away from the LSO station the drawn hull turns ~10 % slower than the ship
// and snaps back every ~4 s (dead-reckoning-like), while the smooth ship pose (Unit:getPosition) and the
// matrix Visualizer hands to the scene do not. This module searches writable memory for every copy of
// the carrier's orientation (forward and up axis next to each other: float or double, 4x4 or 3x3, rows
// or columns), then samples all copies every simulation frame against the smooth ship heading. The copy
// with the sawtooth is the drawn pose; its address leads to the code that writes it.
// Observation only: reads memory, writes nothing in DCS.
#pragma once

#include <string>

namespace lsodiag {

// Ship axes of this frame, "xx xy xz yx yy yz zx zy zz" (Unit:getPosition x, y, z). Feeds the running
// scan and samples the candidates found by the last finished scan.
void HullScanShip(const char* axes);
// Starts a background scan (needs ship axes first). "" when started, otherwise why not.
std::string HullScanStart(const char* label);
// Writes the per-candidate statistics since the previous call to hullscan.log (one buffered write) and
// returns a one-line summary for the diagnostic log.
std::string HullScanStats();

}  // namespace lsodiag
