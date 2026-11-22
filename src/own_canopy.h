// Your own aircraft's canopy in ILARTS's pictures (PLAT, extra cameras).
//
// Cause (static analysis, docs/FINDINGS.md "Own aircraft drawn without its canopy"): DCS hides the parts of
// your aircraft's external model that the cockpit model replaces (canopy, canopy frame, pilot, cockpit
// interior) with draw argument 114: the models' visibility nodes show those parts only while argument 114 is
// below 1, and DCS sets it to 1 on your aircraft while you sit in the cockpit. A draw argument belongs to the
// object, not to a view, so every view of the frame - the PLAT and the extra cameras included - drew your
// aircraft without its canopy. AI aircraft keep 0.
//
// Fix: NGModel.dll parses a model for all views of the frame in one call (NGModelLodsMT::ParseMT2,
// vtable slot 31). For an object whose argument 114 is set and which is seen by one of ILARTS's views, the
// wrapper parses it twice: once for DCS's own views exactly as before, once for ILARTS's views with a copy of
// the arguments in which 114 is 0. The model's per-LOD instance (transforms and visibility, evaluated from
// the arguments; NGModel caches one per LOD in 16 slots and re-evaluates a slot when the arguments it is
// handed differ) is taken from an unused slot for the second parse (slot + number of LODs, through the call
// at NGModel+0x3DBE4), so DCS's own views keep their instances and see argument 114 unchanged. DCS's own
// argument values are never written. The number of LODs is the model's LOD entry count (+0x5A0) minus its first
// drawn entry (+0x5B0). Only objects with argument 114 set are split (your aircraft); every other parse and
// every fetch of DCS's own is passed on unchanged (the fetch wrapper also checks DCS's slot index and binding
// and logs anything unexpected; it never changes them).
//
// ILARTS's views are the extra views DCS sets up for picture sources (SceneRenderer view setup returning to
// +0x24FC3) whose camera is the PLAT camera or an extra camera; plat_origin.cpp reports them per render graph
// build. A collection belongs to one of them when its camera (CollectionInfo +0x318) is that camera.
#pragma once

#include <string>

namespace lsodiag {

std::string OwnCanopyInstall();  // "" or why not (needs plat_origin installed for the view list)
void OwnCanopyRemove();
void OwnCanopyEnable(bool on);
std::string OwnCanopyStats();    // counters since the last call

// From plat_origin.cpp's view setup hook (render graph build): one of ILARTS's views and its camera.
void OwnCanopyNoteView(const double cam[3]);
// From plat_origin.cpp's buildRenderGraph wrapper, after DCS's build: this build's views become the ones the
// scene's object collection is matched against. graph = DCSSceneRenderer + 0x8A8 (or null).
void OwnCanopyBuildEnd(const void* graph);

#ifdef OWN_CANOPY_TEST
void OwnCanopyTestSet(void* parse, void* fetch);
void* OwnCanopyTestParseWrap();
void* OwnCanopyTestFetchWrap();
void OwnCanopyTestStatsNow();
void OwnCanopyTestSampleNow();   // the next near-camera parses write their OWNCANOPY near lines
void OwnCanopyTestReset();       // what install does at the start of each mission
void OwnCanopyTestInstalled(bool on);   // stats lines without patching NGModel
#endif

}  // namespace lsodiag
