// Render-to-texture ("MFD") viewport resolution control via DCS's own resize path.
//
// Verified by disassembly for DCS 2.9.25 (docs/FINDINGS.md, "Q1 resolution"), same code in 2.9.31:
//   Inter.dll!visualizer -> SceneManagerRenderGraph (vtable Visualizer+0x242128)
//   SceneManagerRenderGraph::enforceResolutionOfMFD(index,w,h) (vtable slot 43) queues, via
//   ed::RenderingThread::callFunction, a lambda that runs on the render thread:
//       DCSSceneRenderer::resizeMFDViewport(index, Vec2i{w,h})   with DCSSceneRenderer = [obj+0x6D8]
//   resizeMFDViewport: bounds-checks index against the viewport vector (this+0xC9F8..0xCA00,
//   0x3F90-byte elements), stores the size in the 20-byte entry at [this+0x7C8]+index*20 (+8 w,
//   +0xC h; <= 0 keeps the current value) and flags viewport[index] (byte 0) for rebuild.
// We perform the same render-thread call directly from our EndFrame callback (skipping the
// unlocked enforced-size map that enforceResolutionOfMFD also writes from its caller's thread).
#pragma once

#include <string>

namespace lsodiag {

// Read-only: logs the MFD viewport table (count, per-index size). Returns "" or the reason.
std::string MfdLogViewports();

// Calls DCS's own SceneManagerRenderGraph::enforceResolutionOfMFD(index, w, h) on the calling
// thread (DCS uses it from non-render threads). It records the size in the map read by
// wTVPictureSourceProvider::get_target_size (used by CockpitBase to size what it draws into the
// target - without it the PLAT symbology is laid out for 512) and queues the render-thread resize.
std::string MfdEnforceResolution(int index, int w, int h);

// Current size of viewport `index` (0 x 0 = never set). Returns "" or why it cannot be read
// (e.g. the viewport does not exist yet).
std::string MfdViewportSize(int index, int* w, int* h);

// Requests that viewport `index` be kept at w x h. Applied on the render thread (retried until the
// viewport exists, re-applied if DCS resets it). w = h = 0 clears the request.
std::string MfdRequestResolution(int index, int w, int h);

// Called from the render-thread EndFrame callback.
void MfdOnRenderThread();

}  // namespace lsodiag
