// Runs our code on the DCS render thread via DCS's own renderer-callback mechanism.
//
// GraphicsCore.dll (verified by disassembly for 2.9.25, same code in 2.9.31):
//   Graphics::registerRendererCallback(RendererCallback*, RendererCallbackEvent, int priority)
//   Graphics::fireRendererCallbacks(event) calls one virtual method per event (no arguments):
//     event 0->slot1 1->slot2 2->slot3 3->slot4 4->slot9 5->slot5 6->slot6 7->slot8 8->slot7
//   RendererCallback layout: vptr only; slot 0 = virtual destructor, slots 1..9 = empty handlers.
//   Registry: 9 unlocked std::vectors {RendererCallback*, int priority}, sorted by priority.
// GraphicsVista.dll Graphics::Renderer::EndFrame() fires event 6 immediately before calling the
// render API's end-of-frame (present). AVIMaker (DCS's video recorder) registers event 6 with
// priority 0x7FFFFFFF; we do the same, so we run after all other end-of-frame work.
//
// Safety: registered once per process and never unregistered (the registry is unlocked, so we
// keep mutations to a single insert); the DLL is pinned so the callback can never outlive it.
#pragma once

#include <string>

namespace lsodiag {

using RenderFrameFn = void (*)(int event);  // called for every renderer event (0..8)

// Installs (once) a callback for all 9 renderer events; `fn(event)` is invoked for each.
// Returns "" on success, otherwise the reason.
std::string EnsureRenderCallback(RenderFrameFn fn);

// Read-only: logs every registered renderer callback per event (owner module of vtable + handler).
void LogRendererCallbackRegistry();

unsigned long RenderThreadId();          // thread of event 6 (EndFrame); 0 until the first
std::string RenderEventStats();           // per-event call count @ first thread
unsigned long long RenderCallbackCount();

}  // namespace lsodiag
