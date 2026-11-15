// spout_out.h over the Spout2 SDK's SpoutDX (compiled without UNICODE, as Spout expects).
#include "spout_out.h"

#include <new>

#include "SpoutDX.h"

namespace spoutout {

struct Sender {
    spoutDX dx;
};

Sender* Create(ID3D11Device* dev, const char* name) {
    Sender* s = new (std::nothrow) Sender;
    if (!s) return nullptr;
    if (!s->dx.OpenDirectX11(dev) || !s->dx.SetSenderName(name)) {
        delete s;
        return nullptr;
    }
    return s;
}

bool Send(Sender* s, ID3D11Texture2D* tex) { return s && tex && s->dx.SendTexture(tex); }

void Destroy(Sender* s) {
    if (!s) return;
    s->dx.ReleaseSender();
    s->dx.CloseDirectX11();
    delete s;
}

}  // namespace spoutout
