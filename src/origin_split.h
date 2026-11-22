// Per-object origin split for the far-view render origin fix (plat_origin.cpp).
//
// While the player is more than 70 km from the PLAT camera, ILARTS's views at the carrier (PLAT, extra
// cameras) are drawn against their own origin next to the carrier, and DCS's views against the frame
// origin next to the player. DCS hands each scene object ONE call with every collection (view pass) that
// sees it, and GraphicsVista's GraphSceneObject::collectRenderables (export +0xC8C0) computes the object's
// position once, relative to the FIRST collection's origin (CollectionInfo +0x50C), for all of them. When an
// object is seen by both kinds of view, one kind gets it displaced by the distance between the two origins:
// - ILARTS's collection first: DCS's views draw the object next to the player's camera, at the level of
//   detail of its distance from there (LOD 0 for the carrier) - the "ghost carrier" of 2026-11-10 and the
//   distorted "airport with runway lights" in the sky of 2026-11-19;
// - DCS's collection first: ILARTS's views draw it ~70+ km away (the hull lost from the PLAT, 2026-11-13).
// The wrapper below sits in Visualizer's GraphSceneObject vtable (slot 1 of its ISceneParsable part, an
// aligned pointer swapped atomically). A call whose collections all carry the same origin (always, in stock
// DCS and with the far fix off) goes straight to DCS. A call with several origins is split: one DCS call per
// origin, each with its own collections in DCS's order, so every view gets the object relative to its own
// origin. The group holding the call's last collection runs last.
// enlight's SplineRopeParsable (the arresting cable while it is engaged in a trap; at rest the wires are part of the
// carrier model) has the same bug in its own collectRenderables (+0x63B50: the cable's points minus the origin of the
// first collection that sees it, once per call); slot 1 of its vtable (+0x7FE10) gets the same wrapper. Each class
// installs on its own: one whose code does not match is left alone.
#pragma once

#include <string>

namespace lsodiag {

std::string OriginSplitInstall();   // "" on success, otherwise why not (verifies build and code bytes)
void OriginSplitRemove();
// false: mixed calls are still counted and logged, but passed to DCS unsplit (A/B test). Default true.
void OriginSplitEnable(bool on);
bool OriginSplitEnabled();
// From plat_origin.cpp's buildRenderGraph wrapper, once per build: whether the views at the carrier got
// their own origin this frame (`carrierOwn`; that origin in `carrier`, float3) and the main camera's distance
// to the PLAT camera (m, < 0 unknown). Writes the periodic log line.
void OriginSplitFrame(bool carrierOwn, const float carrier[3], double mainToPlatM);
std::string OriginSplitStats();

#ifdef ORIGIN_SPLIT_TEST
// tools/origin_split_test.cpp: drives the wrappers with a stand-in for DCS's collectRenderables.
// target 0 = GraphSceneObject (models), 1 = SplineRopeParsable (arresting cables).
void OriginSplitTestSet(void* original, int target = 0);
void* OriginSplitTestWrap(int target = 0);
void OriginSplitTestStatsNow();
#endif

}  // namespace lsodiag
