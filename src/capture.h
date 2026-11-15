#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace lsodiag {

struct Renderer;

// Copies the named DCS render target to captures\<name>-<time>.dds/.bmp next to the log.
// Returns a one-line summary, or "" with the reason in `why`. Caller must hold the log open.
std::string CaptureRenderTarget(const char* name, std::string& why);

// Looks up a DCS render target and returns its validated ID3D11Texture2D with one COM reference
// owned by the caller (the DCS-side reference is released before returning). nullptr + why on
// failure. Validation: plausible COM object at texture+0x88, QI(ID3D11Texture2D), same device.
ID3D11Texture2D* OpenDcsTexture(const Renderer& r, const char* name, bool verbose, std::string& why);

// DCS's device and immediate context (borrowed pointers, no reference taken).
bool RendererDevice(const Renderer& r, ID3D11Device** dev, ID3D11DeviceContext** ctx);

}  // namespace lsodiag
