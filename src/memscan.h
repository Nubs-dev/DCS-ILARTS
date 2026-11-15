// Memory search for a distinctive number (glide slope experiment): with the database angle set to an
// unusual value (e.g. 3.137 deg), find every copy DCS keeps of it, as float or double, in degrees or
// radians. Runs on a background thread over committed writable memory; results go to the log (MEMSCAN
// lines: address, region, owning module if any, surrounding bytes).
#pragma once

#include <string>

namespace lsodiag {

// Starts a scan for `degrees` (and its radian value). "" when started, otherwise why not.
std::string MemScanStart(double degrees, const char* label);

}  // namespace lsodiag
