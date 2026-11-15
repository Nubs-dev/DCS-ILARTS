// Guarded access to the DCS DX11Renderer. Every engine call in this project goes through here.
// Behaviour of each function was verified by disassembly for DCS 2.9.25.21123 and checked again for
// 2.9.31.29434 (docs/FINDINGS.md, "Port to DCS 2.9.31.29434").
#pragma once

#include <windows.h>

#include <string>

namespace lsodiag {

// The DCS build that every address, offset and expected byte in this project is for. Every patch
// refuses to install on any other build. Re-verify all of them before changing this.
constexpr const char* kVerifiedBuild = "2.9.31.29434";

// Offsets inside DX11Renderer / DX11 texture objects, established by disassembly (same in 2.9.25 and 2.9.31):
//   DX11Renderer::getDevice        -> *(this + 0x28)
//   DX11Renderer::getDeviceContext -> *(this + 0x30)
//   DX11Renderer::copyTexture uses *(texture + 0x88) as the ID3D11Resource* for CopyResource
//   ed::Ptr<ITexture> ref-count: int32 at texture + 8; on drop to zero vtable slot 15 (+0x78)
constexpr size_t kTextureResourceOffset = 0x88;
constexpr size_t kTextureRefCountOffset = 0x8;
constexpr size_t kTextureReleaseSlot = 15;

using IsRtExistsFn = bool (*)(void* self, const char* name);
using GetResListFn = const char* (*)(void* self);
using FreeStrFn = void (*)(void* self, const char* p);
// Returns ed::Ptr<ITexture> by value: hidden return slot in rdx, name in r8.
using GetRenderTargetFn = void** (*)(void* self, void** ret, const char* name);
using GetPtrFn = void* (*)(void* self);

struct Renderer {
    void* self = nullptr;
    IsRtExistsFn isRtExists = nullptr;
    GetResListFn getResList = nullptr;
    FreeStrFn freeStr = nullptr;
    GetRenderTargetFn getRenderTarget = nullptr;
    GetPtrFn getDevice = nullptr;
    GetPtrFn getDeviceContext = nullptr;
};

// Returns "" on success, otherwise why the renderer must not be used.
std::string ResolveRenderer(Renderer& r);

// Set after any SEH fault in an engine call; all later engine calls are refused.
bool EngineDisabled();
void DisableEngine(const char* what, DWORD code);

// SEH-wrapped calls. On exception they return the failure value and set *code.
int SehIsRtExists(const Renderer& r, const char* name, DWORD* code);         // 1/0, -1 on fault
const char* SehGetResList(const Renderer& r, DWORD* code);
bool SehFreeStr(const Renderer& r, const char* p, DWORD* code);
void* SehGetRenderTarget(const Renderer& r, const char* name, DWORD* code);  // owned reference or null
void* SehGetPtr(GetPtrFn fn, void* self, DWORD* code);
// Drops one reference exactly as ed::Ptr does (lock xadd -1; at zero call vtable slot 15).
bool SehReleaseTexture(void* texture, DWORD* code);

}  // namespace lsodiag
