// Carrier hull frozen in the PLAT while the player is on the F10 map (user report 2026-11-10: the PLAT
// camera and the deck crew follow the ship, the hull is drawn where the ship was).
//
// Visualizer.dll 2.9.31 (unprotected), per frame, fn +0x19EFC0 takes one of two paths:
//   3D views  +0x198240: frame function +0x18FB50, render commands, then ALWAYS +0x19F8C0.
//   F10 map   +0x1991F0: frame function (PLAT view still built: option byte +0x104 = 1), render
//                        commands, then +0x19F8C0 ONLY IF [+0x2E1968] != 0. That pointer comes from
//                        +0x1A9850 (globalView slot 72, else [+0x2D2B08]); the object behind it is only
//                        created when +0x13CF60() is true (taken to be VR, where the map sits in a 3D
//                        scene). On a monitor it is null, so +0x19F8C0 never runs on the map.
// +0x19F8C0 is the world object search: +0x171B20 etc. run a viSearch (edObjects) per view - main views,
// the 0x330-byte extra views (MFDs, PLAT), ... - and hand every object found to the scene (+0x128950).
// Without it the PLAT view is drawn from what the last 3D frame submitted, so ordinary models (the hull)
// stay where they were. The deck crew move because they are updated through animator.dll messages
// (CharacterInstanceGraphicsReflection::updateGraphicInstance), not through this search.
//
// Fix (experiment): at +0x199D2F (the null test) jump to a stub. If the pointer is null, this is enabled
// and the view-mode object [this+0x318+5*8] exists, the stub calls +0x19F8C0 the way the map path would,
// except that the camera matrix is the frame's stored main camera (+0x2E1790, the last 3D camera) and
// not +0x2D8750. Otherwise DCS's own flow is untouched (including VR).
#pragma once

#include <string>

namespace lsodiag {

std::string MapObjectsInstall();  // "" on success, otherwise why not (verifies build and code bytes)
void MapObjectsRemove();
void MapObjectsEnable(bool on);
std::string MapObjectsStats();

}  // namespace lsodiag
