// Spout senders for the DCS ILARTS viewer: a picture the viewer draws, shared on the graphics card with OBS (Spout2
// plugin), Resolume, TouchDesigner ... without a copy through the CPU, an encode or a delay. A thin wrapper over the
// Spout2 SDK's SpoutDX (third_party/spout, BSD licence), built on its own (spout_out.cpp) so its headers stay out
// of the viewer.
#pragma once

struct ID3D11Device;
struct ID3D11Texture2D;

namespace spoutout {

struct Sender;
// A sender named `name` (as receivers list it) on the viewer's device; null if Spout could not start.
Sender* Create(ID3D11Device* dev, const char* name);
// The texture's current picture to the receivers (copied into Spout's shared texture on the GPU).
bool Send(Sender* s, ID3D11Texture2D* tex);
void Destroy(Sender* s);

}  // namespace spoutout
