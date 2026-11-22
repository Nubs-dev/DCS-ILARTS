#include "rain.h"

#include "common.h"
#include "compat.h"
#include "engine.h"
#include "extra_cams.h"
#include "night_sky.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace lsodiag {
namespace {

// Visualizer.dll 2.9.31.29434 (same file in bin and bin-mt). See rain.h and docs/FINDINGS.md.
constexpr uintptr_t kVtblRva = 0x23BBD0;      // the precipitation renderable's vtable (built in the provider's
                                              // collectRenderables +0xC9900, at +0xC9B11 / +0xC9B7F)
constexpr uintptr_t kSlotRva = 0x23BBD8;      // its slot 1: the per-view call
constexpr uintptr_t kSlot0Fn = 0xA3C60;       // slot 0 (destructor), checked as a sanity test
constexpr uintptr_t kDrawRva = 0xC9C60;       // the per-view call (this, SceneRenderContext*)
constexpr uintptr_t kIatRainDraw = 0x22E0B8;  // Visualizer's import of Rain::draw (the call at +0xC9D39)
constexpr const char* kRainDrawName = "?draw@Rain@Effects@@QEAAXMMMAEBVVec3d@osg@@AEBVVec3f@4@AEBVMatrixf@4@2_N@Z";
// The whole per-view call (+0xC9C60 .. +0xC9D5B, no relocations): rbx = this, rdi = the context;
// Rain::collectInstances([this+0x10], ctx+0x308 camera, ctx+0x32C origin, ctx+0x184 V*P), then
// Rain::draw([this+0x10], [this+0x18] power, [this+0x1C] / [this+0x20] rain cloud altitudes, ctx+0x308, ctx+0x32C,
// ctx+0x84 V, ctx+0x184, [this+0x24] the update mark).
constexpr unsigned char kDrawCode[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8D, 0xAA, 0x84, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xFA, 0x4C, 0x8D, 0xB2, 0x2C,
    0x03, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x10, 0x4C, 0x8D, 0xBA, 0x08, 0x03, 0x00, 0x00, 0x4C, 0x8B,
    0xCD, 0x4D, 0x8B, 0xC6, 0x49, 0x8B, 0xD7, 0xFF, 0x15, 0x0A, 0x44, 0x16, 0x00, 0xF3, 0x0F, 0x10, 0x4B, 0x18, 0x0F,
    0x57, 0xC0, 0x0F, 0x2F, 0xC8, 0x0F, 0xB6, 0xF0, 0x77, 0x18, 0x4C, 0x8D, 0x05, 0x23, 0x1F, 0x17, 0x00, 0xBA, 0x08,
    0x00, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0x47, 0xE2, 0x16, 0x00, 0xE8, 0x82, 0x6E, 0xFC, 0xFF, 0x40, 0x84, 0xF6, 0x75,
    0x30, 0x4C, 0x8D, 0x05, 0x26, 0x1F, 0x17, 0x00, 0xBA, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0x2A, 0xE2, 0x16,
    0x00, 0xE8, 0x65, 0x6E, 0xFC, 0xFF, 0x4C, 0x8D, 0x05, 0x56, 0x1F, 0x17, 0x00, 0xBA, 0x08, 0x00, 0x00, 0x00, 0x48,
    0x8D, 0x0D, 0x12, 0xE2, 0x16, 0x00, 0xE8, 0x4D, 0x6E, 0xFC, 0xFF, 0x0F, 0xB6, 0x43, 0x24, 0x48, 0x8D, 0x8F, 0x84,
    0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x5B, 0x20, 0xF3, 0x0F, 0x10, 0x53, 0x1C, 0xF3, 0x0F, 0x10, 0x4B, 0x18, 0x88,
    0x44, 0x24, 0x40, 0x48, 0x89, 0x6C, 0x24, 0x38, 0x48, 0x89, 0x4C, 0x24, 0x30, 0x48, 0x8B, 0x4B, 0x10, 0x4C, 0x89,
    0x74, 0x24, 0x28, 0x4C, 0x89, 0x7C, 0x24, 0x20, 0xFF, 0x15, 0x79, 0x43, 0x16, 0x00, 0x48, 0x8B, 0x5C, 0x24, 0x70,
    0x48, 0x8B, 0x6C, 0x24, 0x78, 0x48, 0x8B, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x50, 0x41, 0x5F,
    0x41, 0x5E, 0x5F, 0xC3};
static_assert(sizeof(kDrawCode) == 0xFB, "the whole per-view call");

// Effects2.dll 2.9.31.29434: what the wrapper relies on in Rain::draw / Rain::update.
struct CodeCheck {
    uintptr_t rva;
    const unsigned char* bytes;
    size_t size;
    const char* what;
};
// Rain::draw (+0x52E0, exported): update(+0x75A0) with the camera, origin, first matrix and the mark, then the
// private draw (+0x50E0).
constexpr unsigned char kEffDraw[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xB6, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00,
                                      0x48, 0x8B, 0xD9, 0x88, 0x44, 0x24, 0x38, 0x48, 0x8B, 0x84, 0x24, 0x80, 0x00, 0x00,
                                      0x00, 0x48, 0x89, 0x44, 0x24, 0x30, 0x48, 0x8B, 0x44, 0x24, 0x78, 0x48, 0x89, 0x44,
                                      0x24, 0x28, 0x48, 0x8B, 0x44, 0x24, 0x70, 0x48, 0x89, 0x44, 0x24, 0x20, 0xE8, 0x85,
                                      0x22, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0x48, 0x83, 0xC4, 0x40, 0x5B, 0xE9, 0xB8, 0xFD,
                                      0xFF, 0xFF};
// update +0x75EC: test eax,eax / jne skip / cmp [rsp+0D8h],al / je skip: without the mark, no time step, no
// cameraVel and no simulation.
constexpr unsigned char kEffMark[] = {0x85, 0xC0, 0x0F, 0x85, 0x2A, 0x04, 0x00, 0x00, 0x38, 0x84, 0x24,
                                      0xD8, 0x00, 0x00, 0x00, 0x0F, 0x84, 0x1D, 0x04, 0x00, 0x00};
// update +0x79E5: cameraVel = (camera - stored) / dT into +0x150 / +0x158, the camera stored at +0x110..+0x120.
constexpr unsigned char kEffVel[] = {0xF2, 0x0F, 0x11, 0xA3, 0x50, 0x01, 0x00, 0x00, 0x66, 0x0F, 0x5A, 0xD1, 0xF3, 0x0F, 0x11,
                                     0x93, 0x58, 0x01, 0x00, 0x00, 0xF2, 0x0F, 0x11, 0xBB, 0x10, 0x01, 0x00, 0x00, 0xF2, 0x0F,
                                     0x11, 0xB3, 0x18, 0x01, 0x00, 0x00, 0xF2, 0x0F, 0x11, 0xAB, 0x20, 0x01, 0x00, 0x00};
// update +0x7A1E: the constant buffer is this+0x130 (lea rdx,[rbx+130h]) ...
constexpr unsigned char kEffCb[] = {0x48, 0x8B, 0x8C, 0x24, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8D,
                                    0x93, 0x30, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x43, 0x18};
// ... +0x7A98: uploaded through [this+0x328] vt[2] with 0xA0 bytes, every draw.
constexpr unsigned char kEffCbUpload[] = {0x48, 0x8B, 0x8B, 0x28, 0x03, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x0C,
                                          0x48, 0x8B, 0x01, 0x41, 0xB8, 0xA0, 0x00, 0x00, 0x00, 0xFF, 0x50, 0x10};
constexpr uintptr_t kEffDrawRva = 0x52E0;
const CodeCheck kEffChecks[] = {
    {kEffDrawRva, kEffDraw, sizeof(kEffDraw), "Rain::draw"},
    {0x75EC, kEffMark, sizeof(kEffMark), "Rain::update's update mark test"},
    {0x79E5, kEffVel, sizeof(kEffVel), "Rain::update's cameraVel store"},
    {0x7A1E, kEffCb, sizeof(kEffCb), "Rain::update's constant buffer"},
    {0x7A98, kEffCbUpload, sizeof(kEffCbUpload), "Rain::update's constant buffer upload"},
};

// SceneRenderContext: +0x308 camera (Vec3d, world).
constexpr size_t kCtxCam = 0x308;
// The renderable (0x28 bytes, frame memory): +0x10 the Rain object, +0x18 power, +0x1C / +0x20 rain cloud altitudes,
// +0x24 the update mark (the collection's +0x690 == 0).
constexpr size_t kRendSize = 0x28, kRendRain = 0x10, kRendPower = 0x18, kRendMark = 0x24;
// Effects::Rain (0x540 bytes): +0x18 the particle cells' centre (float3, world: the camera of the last
// collectInstances snapped to the cell grid); +0x94 the precipitation type (Rain::init: 0 rain, 1 snow; Visualizer
// +0xC98B0 inits the provider's +0x08 with 0 and +0x10 with 1); +0xA8 the effect time of the last update (double);
// +0x150 cameraVel (float3, in the constant buffer at +0x130).
constexpr size_t kRainCentre = 0x18, kRainType = 0x94, kRainTime = 0xA8, kRainCamVel = 0x150;
// Rain::init +0x5740: mov [rcx+94h],ebx (the type argument).
constexpr unsigned char kEffInitType[] = {0x89, 0x99, 0x94, 0x00, 0x00, 0x00};
// Visualizer +0xC98B0: the provider's rain object (+0x08) gets type 0, its snow object (+0x10) type 1.
constexpr unsigned char kVisInitTypes[] = {0x48, 0x8B, 0xD9, 0x33, 0xD2, 0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x49, 0x08,
                                           0xFF, 0x15, 0xDF, 0x47, 0x16, 0x00, 0x48, 0x8B, 0x4B, 0x08, 0xBA, 0x01,
                                           0x00, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x10};

// ---- the carrier's rain while DCS draws none (docs/FINDINGS.md "rain in ILARTS's views while you are in snow") ----
// The provider's vtable (+0x23BB88): slot 0 +0xC97C0, slot 1 collectRenderables +0xC9900 (this = the provider, its
// +0x08 the rain object, +0x10 the snow object). collectRenderables gives a collection a rain renderable only when
// power x share is above a threshold (and the cells are in view), so with your camera in pure snow (share 0) no view
// gets one: DCS neither draws nor simulates its rain object.
constexpr uintptr_t kProvVtblRva = 0x23BB88, kProvSlotRva = 0x23BB90, kProvSlot0Fn = 0xC97C0, kCollectRva = 0xC9900;
constexpr size_t kProvRain = 0x08;
constexpr size_t kRainStored = 0x110;   // Rain::update's stored camera (3 doubles), for the next cameraVel
// collectRenderables' entry up to mov r15,rcx (the provider), and its two Rain::collectInstances calls with the
// provider's rain (+0x08) and snow (+0x10) objects.
constexpr unsigned char kCollectEntry[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xE0, 0x00,
                                           0x00, 0x00, 0x48, 0x8B, 0x05, 0xAB, 0xAA, 0x1D, 0x00, 0x48, 0x33, 0xC4,
                                           0x48, 0x89, 0x44, 0x24, 0x68, 0x48, 0x8B, 0xB4, 0x24, 0x30, 0x01, 0x00,
                                           0x00, 0x49, 0x8B, 0xE8, 0x8B, 0xFA, 0x4C, 0x8B, 0xF9};
constexpr unsigned char kCollectRain[] = {0x49, 0x8B, 0x4F, 0x08, 0x4C, 0x8B, 0xCB, 0x4C, 0x8B, 0xC5,
                                          0x49, 0x8B, 0xD6, 0xFF, 0x15, 0xF8, 0x45, 0x16, 0x00};
constexpr unsigned char kCollectSnow[] = {0x49, 0x8B, 0x4F, 0x10, 0x4C, 0x8B, 0xCB, 0x4C, 0x8B, 0xC5,
                                          0x49, 0x8B, 0xD6, 0xFF, 0x15, 0xD3, 0x45, 0x16, 0x00};
constexpr double kProviderFreshSec = 0.5;   // the rain object seen in a collectRenderables this recently
constexpr double kRainGoneSec = 0.25;       // no view drew DCS's rain object for this long: DCS draws none
constexpr double kMarkerStaleSec = 0.3;     // the view that runs the rain simulation, given up after this

// ---- far views: the cloud volume lookup (docs/FINDINGS.md "Rain and cloud shadows in the far views") ----
// GraphicsCore.dll 2.9.31.29434. ContextBase keeps a stack of 0xBD8-byte blocks (array pointer at +0x11D578, current
// index at +0x11D518). The per-view constant buffer cPerView (0x970 bytes, register b7) is block +0x268, so
// gCloudVolumeScale / gCloudVolumeOffset (cPerView +0x6B0 / +0x6C0) are block +0x918 / +0x928. The exported
// setCloudVolumeTransform writes them (its only caller: SceneRenderer's pass binder +0x1EE46, with enlight's per-frame
// values, right before it binds the pass's view); the exported updateBuffers fills the rest of the block from the
// bound context (gOrigin = ContextBase +0x2C) and uploads it - the only upload of cPerView in DCS, called only through
// GraphicsVista's and enlight's imports.
constexpr const char* kGcUpdateName = "?updateBuffers@ContextBase@GraphicsCore@@QEAAXH@Z";
constexpr const char* kGcSetCloudName = "?setCloudVolumeTransform@ContextBase@GraphicsCore@@QEAAXAEBVVec3f@osg@@0@Z";
constexpr uintptr_t kGcUpdateRva = 0x18BD0, kGcSetCloudRva = 0x189D0;
constexpr uintptr_t kGcIndexRva = 0x11D518, kGcArrayRva = 0x11D578;
constexpr size_t kBlockStride = 0xBD8, kBlockScale = 0x918, kBlockOffset = 0x928;
constexpr unsigned kMaxStack = 64;              // sanity limit for the stack index
constexpr size_t kCbCam = 0x08, kCbOrigin = 0x2C, kCbRead = 0x38;   // ContextBase: bound camera (Vec3d), origin
// setCloudVolumeTransform (whole function): offset -> block +0x928, scale -> block +0x918 at [+0x11D578] +
// [+0x11D518] * 0xBD8.
constexpr unsigned char kGcSetCloud[] = {
    0xF2, 0x41, 0x0F, 0x10, 0x00, 0x8B, 0x05, 0x3D, 0x4B, 0x10, 0x00, 0x4C, 0x69, 0xC8, 0xD8, 0x0B, 0x00, 0x00, 0x4C,
    0x03, 0x0D, 0x8F, 0x4B, 0x10, 0x00, 0xF2, 0x41, 0x0F, 0x11, 0x81, 0x28, 0x09, 0x00, 0x00, 0x41, 0x8B, 0x40, 0x08,
    0x41, 0x89, 0x81, 0x30, 0x09, 0x00, 0x00, 0xF2, 0x0F, 0x10, 0x02, 0xF2, 0x41, 0x0F, 0x11, 0x81, 0x18, 0x09, 0x00,
    0x00, 0x8B, 0x42, 0x08, 0x41, 0x89, 0x81, 0x20, 0x09, 0x00, 0x00, 0xC3};
// updateBuffers +0x18BE9: the same block (array +0x11D578, index +0x11D518, stride 0xBD8), cPerView at +0x268.
constexpr unsigned char kGcUpdEntry[] = {
    0x48, 0x8B, 0x2D, 0x88, 0x49, 0x10, 0x00, 0x44, 0x8B, 0xF2, 0x0F, 0x10, 0x81, 0xB4, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0xF1, 0x0F, 0x29, 0x70, 0xB8, 0x0F, 0x29, 0x78, 0xA8, 0x48, 0x8D, 0xBD, 0x68, 0x02, 0x00, 0x00, 0x44, 0x0F, 0x29,
    0x40, 0x98, 0x44, 0x0F, 0x29, 0x48, 0x88, 0x44, 0x0F, 0x29, 0x90, 0x78, 0xFF, 0xFF, 0xFF, 0x44, 0x0F, 0x29, 0x98,
    0x68, 0xFF, 0xFF, 0xFF, 0x44, 0x0F, 0x29, 0xA0, 0x58, 0xFF, 0xFF, 0xFF, 0x44, 0x0F, 0x29, 0xA8, 0x48, 0xFF, 0xFF,
    0xFF, 0x8B, 0x05, 0xDC, 0x48, 0x10, 0x00, 0x48, 0x69, 0xD8, 0xD8, 0x0B, 0x00, 0x00, 0x44, 0x0F, 0x29, 0x74, 0x24,
    0x70, 0x44, 0x0F, 0x29, 0x7C, 0x24, 0x60, 0x48, 0x03, 0xFB};
// updateBuffers +0x1909C: gOrigin (cPerView +0x610) = ContextBase +0x2C, gCameraPos (+0x620) = +0x20.
constexpr unsigned char kGcUpdOrigin[] = {0xF2, 0x0F, 0x10, 0x46, 0x2C, 0xF2, 0x0F, 0x11, 0x87, 0x10, 0x06, 0x00, 0x00,
                                          0x8B, 0x46, 0x34, 0x89, 0x87, 0x18, 0x06, 0x00, 0x00, 0xF2, 0x0F, 0x10, 0x46,
                                          0x20, 0xF2, 0x0F, 0x11, 0x87, 0x20, 0x06, 0x00, 0x00, 0x8B, 0x46, 0x28, 0x89,
                                          0x87, 0x28, 0x06, 0x00, 0x00};
// updateBuffers +0x19508: uploads 0x970 bytes from block +0x268 into one of its two buffers and binds it (slot 7).
constexpr unsigned char kGcUpdUpload[] = {
    0x8B, 0x05, 0x12, 0x40, 0x10, 0x00, 0x48, 0x8D, 0x1D, 0x43, 0x40, 0x10, 0x00, 0xFF, 0xC8, 0x83, 0xE0, 0x01, 0x44,
    0x8B, 0xC0, 0x48, 0x03, 0xC0, 0x44, 0x89, 0x05, 0xF9, 0x3F, 0x10, 0x00, 0x48, 0x8B, 0x0C, 0xC3, 0x48, 0x85, 0xC9,
    0x74, 0x16, 0x48, 0x8B, 0x01, 0x41, 0xB8, 0x70, 0x09, 0x00, 0x00, 0x48, 0x8B, 0xD7, 0xFF, 0x50, 0x10, 0x44, 0x8B,
    0x05, 0xDA, 0x3F, 0x10, 0x00, 0x45, 0x85, 0xF6, 0xBA, 0x07, 0x00, 0x00, 0x00, 0x41, 0x0F, 0x49, 0xD6, 0x85, 0xD2,
    0x78, 0x1A, 0x48, 0x8B, 0x05, 0xD3, 0x83, 0x0B, 0x00, 0x4D, 0x03, 0xC0, 0x48, 0x8B, 0x08, 0x4E, 0x8B, 0x04, 0xC3,
    0x48, 0x8B, 0x01, 0xFF, 0x90, 0xC8, 0x00, 0x00, 0x00};
// GraphicsVista +0x1F224 (Graphics::Context::bindViewportContextToGPU): the viewport's camera - origin (+0x320),
// origin (+0x32C) and camera (+0x308) into the context (+0x20, +0x2C, +0x08), before it calls updateBuffers.
constexpr unsigned char kGvBindCopy[] = {
    0xF2, 0x41, 0x0F, 0x10, 0x80, 0x20, 0x03, 0x00, 0x00, 0xF2, 0x0F, 0x11, 0x41, 0x20, 0x41, 0x8B, 0x80, 0x28, 0x03,
    0x00, 0x00, 0x89, 0x41, 0x28, 0xF2, 0x41, 0x0F, 0x10, 0x80, 0x2C, 0x03, 0x00, 0x00, 0xF2, 0x0F, 0x11, 0x41, 0x2C,
    0x41, 0x8B, 0x80, 0x34, 0x03, 0x00, 0x00, 0x89, 0x41, 0x34, 0x41, 0x0F, 0x10, 0x80, 0x08, 0x03, 0x00, 0x00, 0x0F,
    0x11, 0x41, 0x08, 0xF2, 0x41, 0x0F, 0x10, 0x88, 0x18, 0x03, 0x00, 0x00, 0xF2, 0x0F, 0x11, 0x49, 0x18};
constexpr uintptr_t kGvBindCopyRva = 0x1F224;
// enlight.dll: the cloud implementation (pointer at +0xA08D8): +0x1740 the origin its volume offset is for (float3),
// +0x1770 the offset, +0x1780 the scale. The per-frame update writes all three together (+0x2869D .. +0x28713, the
// origin from the same float3 the offset is computed with); the exported CloudsModule::getCloudVolumeOffset / Scale
// (the binder's source) call the facade ?clouds@enlight's vtable slots 12 / 13 (+0x24420 / +0x24440), which return
// +0x1770 / +0x1780.
constexpr const char* kEnCloudsName = "?clouds@enlight@@3AEAVClouds@1@EA";
constexpr const char* kEnGetOffsetName = "?getCloudVolumeOffset@CloudsModule@render@@QEBA?AVVec3f@osg@@XZ";
constexpr const char* kEnGetScaleName = "?getCloudVolumeScale@CloudsModule@render@@QEBA?AVVec3f@osg@@XZ";
constexpr uintptr_t kEnImplRva = 0xA08D8, kEnFacadeVtblRva = 0x7C850, kEnGetOffRva = 0x24420, kEnGetScaleRva = 0x24440;
constexpr uintptr_t kEnCloudsRefRva = 0x9A178;   // ?clouds@enlight: the reference to the facade object
constexpr uintptr_t kEnExpOffRva = 0x2FD70, kEnExpScaleRva = 0x2FD90, kEnUpdStoresRva = 0x2869D;
constexpr size_t kEnOrigin = 0x1740, kEnOffset = 0x1770, kEnScale = 0x1780;
// +0x24420 / +0x24440 (facade slots 12, 13): rax = [+0xA08D8]; return [rax+1770h] / [rax+1780h].
constexpr unsigned char kEnGetters[] = {
    0x48, 0x8B, 0x05, 0xB1, 0xC4, 0x07, 0x00, 0xF2, 0x0F, 0x10, 0x80, 0x70, 0x17, 0x00, 0x00, 0x8B, 0x80, 0x78, 0x17,
    0x00, 0x00, 0xF2, 0x0F, 0x11, 0x02, 0x89, 0x42, 0x08, 0x48, 0x8B, 0xC2, 0xC3, 0x48, 0x8B, 0x05, 0x91, 0xC4, 0x07,
    0x00, 0xF2, 0x0F, 0x10, 0x80, 0x80, 0x17, 0x00, 0x00, 0x8B, 0x80, 0x88, 0x17, 0x00, 0x00, 0xF2, 0x0F, 0x11, 0x02,
    0x89, 0x42, 0x08, 0x48, 0x8B, 0xC2, 0xC3};
// +0x2869D: offset -> +0x1770, scale -> +0x1780, origin [r14] -> +0x1740.
constexpr unsigned char kEnUpdStores[] = {
    0xF2, 0x0F, 0x11, 0xAF, 0x70, 0x17, 0x00, 0x00, 0xF3, 0x0F, 0x5C, 0xD1, 0xF3, 0x44, 0x0F, 0x11, 0x87, 0xCC, 0x17,
    0x00, 0x00, 0x89, 0x87, 0x7C, 0x17, 0x00, 0x00, 0xF3, 0x0F, 0x58, 0xDA, 0xF3, 0x0F, 0x5E, 0xDF, 0xF3, 0x0F, 0x11,
    0x9F, 0x78, 0x17, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x1D, 0xBC, 0x25, 0x05, 0x00, 0x0F, 0x28, 0xD3, 0x0F, 0x28, 0xCB,
    0xF3, 0x41, 0x0F, 0x5E, 0xDE, 0xF3, 0x0F, 0x5E, 0xD7, 0xF3, 0x0F, 0x5E, 0xCE, 0x0F, 0x28, 0xC2, 0x0F, 0x14, 0xC1,
    0xF2, 0x0F, 0x11, 0x87, 0x80, 0x17, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x97, 0x88, 0x17, 0x00, 0x00, 0xF2, 0x41, 0x0F,
    0x10, 0x06, 0xF2, 0x0F, 0x11, 0x87, 0x40, 0x17, 0x00, 0x00, 0x0F, 0x28, 0xC3, 0x41, 0x8B, 0x46, 0x08, 0x89, 0x87,
    0x48, 0x17, 0x00, 0x00};
// The exported getters (+0x2FD70 / +0x2FD90): rcx = [?clouds@enlight]; call [vtbl+60h] / [vtbl+68h].
constexpr unsigned char kEnExpGetter[2][0x1F] = {
    {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x0D, 0xFB, 0xA3, 0x06, 0x00, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x01,
     0xFF, 0x50, 0x60, 0x48, 0x8B, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3},
    {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x0D, 0xDB, 0xA3, 0x06, 0x00, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x01,
     0xFF, 0x50, 0x68, 0x48, 0x8B, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3}};

using DrawFn = void (*)(void* self, const void* ctx, void* a3, void* a4);
using UploadFn = void (*)(void* context, int slot);
using CollectFn = uintptr_t (*)(void* self, uintptr_t count, void* a3, void* a4, void* a5);

bool g_installed = false;
volatile long g_mode = static_cast<long>(RainMode::Auto);
DrawFn g_draw = nullptr;
void** g_slot = nullptr;

// The carrier's rain while DCS draws none: collectRenderables wrapped (the provider's rain object, when last seen).
bool g_borrowInstalled = false;
CollectFn g_collect = nullptr;
void** g_provSlot = nullptr;
void* volatile g_rainObj = nullptr;
volatile long long g_rainObjAt = 0;    // bits of a double (clock)
volatile long long g_rainSeenAt = 0;   // bits of a double: a view last drew DCS's rain object with DCS's renderable

// The far views' cloud lookup: GraphicsCore's updateBuffers and the import slots that lead to it (GraphicsVista's,
// enlight's), GraphicsCore's block stack and enlight's cloud implementation pointer.
bool g_farInstalled = false;
UploadFn g_upload = nullptr;
void** g_upSlots[2] = {};
const uint32_t* g_gcIndex = nullptr;
unsigned char* const* g_gcArray = nullptr;
void* const* g_enImpl = nullptr;

// The carrier's temperature at sea level (deg C, from the hook), as the bits of a double; kNoTemp = not known.
constexpr long long kNoTemp = 0x7FF8DEAD00000000LL;
volatile long long g_tempBits = kNoTemp;

double QpcClock() {
    static const double f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return static_cast<double>(x.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / f;
}
double (*g_clock)() = &QpcClock;

// The per-draw reads and writes of DCS's objects (render thread, several per view and frame): under SEH rather
// than ReadProcessMemory, which would cost a system call each. Install checks use SafeRead.
bool SehRead(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehWrite(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

double Len(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

void StoreTime(volatile long long* at, double t) {
    long long b = 0;
    memcpy(&b, &t, sizeof(b));
    InterlockedExchange64(at, b);
}
double LoadTime(const volatile long long* at) {
    const long long b = *at;
    double t = 0;
    memcpy(&t, &b, sizeof(t));
    return t;
}

// ---- per-view velocity (auto) ----
constexpr int kTracks = 16, kSamples = 64;   // 64 x 1/60 s > kRainWindowSec
constexpr double kSampleStep = 1.0 / 60, kStaleSec = 2.0, kForgetSec = 10.0;

struct Track {
    bool used = false;
    const void* ctx = nullptr;
    double last[3] = {}, lastT = 0;              // position at the last draw
    int n = 0, head = 0;                         // kept positions (ring, oldest at head)
    double st[kSamples] = {}, sp[kSamples][3] = {};
    double vel[3] = {};
    bool haveVel = false;
    bool own = false;                            // vel from this view's own positions (else another view's)
    double span = 0;                             // time the last estimate covered
};
SRWLOCK g_trackLock = SRWLOCK_INIT;
Track g_tracks[kTracks];
int g_lastTrack = -1;   // the track updated last (its velocity is a new view's first guess)

struct Motion {
    double v[3] = {};
    bool known = false;   // from this view's own positions
    bool guessed = false; // from another view's (a new view)
    double span = 0;      // seconds the estimate covers
    bool newView = false;
};

void ClearSamples(Track& k) {
    k.n = 0;
    k.head = 0;
}

void Push(Track& k, const double* p, double t) {
    if (k.n > 0) {
        const int newest = (k.head + k.n - 1) % kSamples;
        if (t - k.st[newest] < kSampleStep) return;
    }
    int at;
    if (k.n < kSamples) {
        at = (k.head + k.n) % kSamples;
        ++k.n;
    } else {
        at = k.head;
        k.head = (k.head + 1) % kSamples;
    }
    k.st[at] = t;
    for (int i = 0; i < 3; ++i) k.sp[at][i] = p[i];
}

Motion TrackMotion(const void* ctx, const double* cam, double now) {
    Motion m;
    AcquireSRWLockExclusive(&g_trackLock);
    int best = -1;
    double bestD2 = 0;
    bool bestCtx = false;
    for (int i = 0; i < kTracks; ++i) {
        const Track& k = g_tracks[i];
        if (!k.used) continue;
        const double dt = std::fmax(0.0, std::fmin(now - k.lastT, 1.0));
        const double lim = kRainJumpMetres + kRainMaxSpeed * dt;
        const double d[3] = {cam[0] - k.last[0], cam[1] - k.last[1], cam[2] - k.last[2]};
        const double d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (d2 > lim * lim) continue;
        const bool sameCtx = k.ctx == ctx;
        if (best < 0 || (sameCtx && !bestCtx) || (sameCtx == bestCtx && d2 < bestD2)) {
            best = i;
            bestD2 = d2;
            bestCtx = sameCtx;
        }
    }
    if (best < 0) {
        // a new view (or a camera moved to another spot): the slot of a forgotten or the least recently seen view
        int slot = -1;
        double oldest = 0;
        for (int i = 0; i < kTracks; ++i) {
            const Track& k = g_tracks[i];
            if (!k.used || now - k.lastT > kForgetSec) {
                slot = i;
                break;
            }
            if (slot < 0 || k.lastT < oldest) {
                slot = i;
                oldest = k.lastT;
            }
        }
        Track fresh;
        fresh.used = true;
        if (g_lastTrack >= 0 && g_lastTrack != slot && g_tracks[g_lastTrack].used && g_tracks[g_lastTrack].own &&
            now - g_tracks[g_lastTrack].lastT < 1.0) {
            for (int i = 0; i < 3; ++i) fresh.vel[i] = g_tracks[g_lastTrack].vel[i];
            fresh.haveVel = true;
        }
        g_tracks[slot] = fresh;
        if (g_lastTrack == slot) g_lastTrack = -1;
        best = slot;
        m.newView = true;
    }
    Track& k = g_tracks[best];
    if (now - k.lastT > kStaleSec) ClearSamples(k);   // not drawn for a while: start the window afresh
    k.ctx = ctx;
    for (int i = 0; i < 3; ++i) k.last[i] = cam[i];
    k.lastT = now;
    Push(k, cam, now);
    // the oldest kept position within the window
    for (int j = 0; j < k.n; ++j) {
        const int at = (k.head + j) % kSamples;
        const double span = now - k.st[at];
        if (span > kRainWindowSec) continue;
        if (span >= kRainMinSpanSec) {
            for (int i = 0; i < 3; ++i) k.vel[i] = (cam[i] - k.sp[at][i]) / span;
            k.haveVel = true;
            k.own = true;
            k.span = span;
        }
        break;
    }
    for (int i = 0; i < 3; ++i) m.v[i] = k.vel[i];
    m.known = k.haveVel && k.own;
    m.guessed = k.haveVel && !k.own;
    m.span = k.span;
    if (k.own) g_lastTrack = best;
    ReleaseSRWLockExclusive(&g_trackLock);
    const double s = Len(m.v);
    if (s > kRainMaxSpeed)
        for (double& c : m.v) c *= kRainMaxSpeed / s;
    return m;
}

int TrackCount() {
    int n = 0;
    const double now = g_clock();
    AcquireSRWLockShared(&g_trackLock);
    for (const Track& k : g_tracks) n += k.used && now - k.lastT < kStaleSec ? 1 : 0;
    ReleaseSRWLockShared(&g_trackLock);
    return n;
}

// ---- diagnostics ----
enum Kind { kPlat = 0, kCamera = 1, kDcsUpdating = 2, kDcsOther = 3, kKinds = 4 };
const char* const kKindName[kKinds] = {"PLAT (ILARTS)", "camera (ILARTS)", "DCS updating", "DCS other"};

volatile long long g_draws[kKinds] = {};
volatile long long g_auto = 0, g_still = 0, g_hidden = 0, g_stock = 0, g_markCleared = 0, g_conflicts = 0, g_refused = 0;
volatile long long g_nearMiss = 0, g_newViews = 0, g_simIdle = 0;
volatile long g_wantSample[kKinds] = {1, 1, 1, 1};
constexpr int kMaxAnomalyLines = 8;
volatile long g_anomalyLines = 0;
volatile long g_firstMarkLine = 0;

// Since the last stats line: speeds (m/s) and cell centre distances (m).
SRWLOCK g_statLock = SRWLOCK_INIT;
struct Stat {
    double lastOwn[2] = {};   // PLAT, camera: the velocity used, last draw
    double maxOwn = 0;
    double lastDcs = 0, maxDcs = 0;   // DCS's cameraVel after an updating view's draw
    double maxCentreOurs = -1, maxCentreDcs = -1;
    double dcsCam[3] = {};   // the updating view's camera, last draw
    double dcsVel[3] = {};
    bool haveDcs = false;
    double dcsPow[2] = {}, dcsPowT[2] = {-1e9, -1e9};   // rain / snow renderable power in DCS's views, when seen
    double maxShift = 0;      // far views: the largest shift of the cloud lookup corrected (m)
    double lastShare = -1;    // the rain share used in ILARTS's views (last draw)
} g_stat;

// ---- precipitation type in ILARTS's views ----
// DCS splits the precipitation into rain and snow by the temperature at YOUR camera (Weather.dll +0x17900, fed with
// the main camera by Visualizer +0x199752; rain share = clamp((T + 1) / 3, 0, 1): all snow at -1 C and below, all
// rain at +2 C and above), and every view draws both objects with that split. ILARTS's views use the temperature at
// their own camera instead: the carrier's sea-level temperature from the hook, less the ISA lapse rate for the
// camera's height.
constexpr double kLapse = 0.0065;   // C per metre
double RainShareAt(double celsius) { return std::fmin(1.0, std::fmax(0.0, (celsius + 1.0) / 3.0)); }
bool CarrierTemp(double* t) {
    const long long b = g_tempBits;
    if (b == kNoTemp) return false;
    memcpy(t, &b, sizeof(*t));
    return true;
}
volatile long long g_snowHidden = 0, g_rainHidden = 0;
// The carrier's rain while DCS draws none: draws of DCS's rain object through the view's snow renderable, those that
// ran DCS's simulation (the update mark), and those wanted but without a rain object to draw.
volatile long long g_borrowed = 0, g_borrowSim = 0, g_borrowNone = 0;
// Draws skipped in a camera at a hangar deck position, and how near its camera a view must be to be that camera.
volatile long long g_hangarSkipped = 0;
constexpr double kHangarMatchMetres = 0.5;
volatile long g_firstBorrowLine = 0;
SRWLOCK g_markerLock = SRWLOCK_INIT;
double g_markerPos[3] = {}, g_markerT = -1e9;   // the view whose draw runs the simulation, when it last drew

// DCS's own rain share as its views draw it (the last rain and snow renderable powers, each if seen in the last
// second): -1 when neither was seen.
double DcsRainShare(double now) {
    AcquireSRWLockShared(&g_statLock);
    const double r = now - g_stat.dcsPowT[0] < 1.0 ? g_stat.dcsPow[0] : 0.0;
    const double s = now - g_stat.dcsPowT[1] < 1.0 ? g_stat.dcsPow[1] : 0.0;
    ReleaseSRWLockShared(&g_statLock);
    return r + s > 0 ? r / (r + s) : -1.0;
}

// ---- far views: diagnostics of the cloud lookup ----
enum UpState { kUpFixed = 0, kUpStock, kUpSame, kUpUnmatched, kUpUnreadable, kUpConflict, kUpStates };
const char* const kUpStateName[kUpStates] = {
    "corrected to this view's origin", "as DCS has it (stock)", "as DCS has it (no shift: the frame's origin)",
    "as DCS has it (DCS's volume values did not match enlight's)", "as DCS has it (not readable)",
    "corrected, but changed during the upload (left as found)"};
enum UpKind { kUpPlat = 0, kUpCamera = 1, kUpOther = 2 };
const char* const kUpKindName[3] = {"PLAT", "camera", "other pass"};
volatile long long g_upOwn = 0, g_upKind[3] = {}, g_upState[kUpStates] = {}, g_upOutside = 0;
volatile long g_wantFarSample = 1;
// The last per-view upload on the carrier's origin (the rain sample lines say what the draw got).
struct UpSeen {
    bool have = false;
    int state = kUpStock, kind = kUpOther;
    double cam[3] = {};
    float origin[3] = {}, frame[3] = {}, dcsOff[3] = {}, used[3] = {}, scale[3] = {};
};
UpSeen g_upLast;   // under g_statLock

double Shift(const UpSeen& u) {
    const double d[3] = {double(u.origin[0]) - u.frame[0], double(u.origin[1]) - u.frame[1], double(u.origin[2]) - u.frame[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
// Where in DCS's cloud volume (uvw, 0..1 inside) the view's camera is looked up with the offset it got.
void CamUvw(const UpSeen& u, const float* off, double uvw[3]) {
    for (int i = 0; i < 3; ++i) uvw[i] = (u.cam[i] - u.origin[i]) * u.scale[i] + off[i];
}
// How far (m) from the view's camera its cloud lookup lands with that offset (0 when corrected).
double LookupMiss(const UpSeen& u, const float* off) {
    double d2 = 0;
    for (int i = 0; i < 3; ++i) {
        if (u.scale[i] == 0) return -1;
        const double want = double(u.dcsOff[i]) + (double(u.origin[i]) - u.frame[i]) * u.scale[i];
        const double m = (off[i] - want) / u.scale[i];
        d2 += m * m;
    }
    return std::sqrt(d2);
}

// The effect time (Rain +0xA8) seen per Rain object, and when it last changed: DCS's simulation idle?
struct SimSeen {
    const void* rain = nullptr;
    double time = 0, changedAt = 0;
};
SimSeen g_sim[4];

bool SimIdle(const void* rain, double now) {
    double t = 0;
    if (!SehRead(static_cast<const unsigned char*>(rain) + kRainTime, &t, sizeof(t))) return false;
    bool idle = false;
    AcquireSRWLockExclusive(&g_statLock);
    SimSeen* s = nullptr;
    for (SimSeen& e : g_sim)
        if (e.rain == rain) s = &e;
    if (!s) {
        for (SimSeen& e : g_sim)
            if (!e.rain) {
                s = &e;
                break;
            }
        if (!s) s = &g_sim[0];
        s->rain = rain;
        s->time = t;
        s->changedAt = now;
    } else if (t != s->time) {
        s->time = t;
        s->changedAt = now;
    } else {
        idle = now - s->changedAt > 0.5;
    }
    ReleaseSRWLockExclusive(&g_statLock);
    return idle;
}

double CentreDistance(const void* rain, const double* cam) {
    float c[3];
    if (!SehRead(static_cast<const unsigned char*>(rain) + kRainCentre, c, sizeof(c))) return -1;
    const double d[3] = {c[0] - cam[0], c[1] - cam[1], c[2] - cam[2]};
    return Len(d);
}

void Anomaly(const char* what) {
    if (InterlockedIncrement(&g_anomalyLines) <= kMaxAnomalyLines) Log("RAIN anomaly: %s", what);
}

std::string Vec(const double* v) {
    char b[96];
    _snprintf_s(b, _countof(b), _TRUNCATE, "(%.1f, %.1f, %.1f) = %.1f m/s", v[0], v[1], v[2], Len(v));
    return b;
}

std::string FarText(const double* cam, const void* ctx);
std::string TypeText(int type, double camY, double now);

void SampleOurs(int kind, const double* cam, const void* ctx, double centre, int mark, const float* dcsVel, const Motion* own,
                const char* drew, int type, double now) {
    char ownText[200] = "-";
    if (own) {
        char how[64];
        if (own->known)
            _snprintf_s(how, _countof(how), _TRUNCATE, "its positions over %.2f s", own->span);
        else
            _snprintf_s(how, _countof(how), _TRUNCATE, "%s", own->guessed ? "new view: the last tracked view's" : "new view: not known yet");
        _snprintf_s(ownText, _countof(ownText), _TRUNCATE, "%s (%s)", Vec(own->v).c_str(), how);
    }
    const double dv[3] = {dcsVel[0], dcsVel[1], dcsVel[2]};
    char centreText[64] = "not drawn";
    if (centre >= 0) _snprintf_s(centreText, _countof(centreText), _TRUNCATE, "%.1f m from it", centre);
    char mainText[64] = "not seen yet";
    AcquireSRWLockShared(&g_statLock);
    if (g_stat.haveDcs) {
        const double d[3] = {cam[0] - g_stat.dcsCam[0], cam[1] - g_stat.dcsCam[1], cam[2] - g_stat.dcsCam[2]};
        _snprintf_s(mainText, _countof(mainText), _TRUNCATE, "%.3f km away", Len(d) / 1000);
    }
    ReleaseSRWLockShared(&g_statLock);
    Log("RAIN %s view: camera (%.3f, %.3f, %.3f) km, particle cells centred %s, DCS's update mark %d, DCS's streak "
        "velocity (the updating view's camera motion) %s, this view's own motion %s, drawn %s; DCS's updating view %s; "
        "%s; %s",
        kKindName[kind], cam[0] / 1000, cam[1] / 1000, cam[2] / 1000, centreText, mark, Vec(dv).c_str(), ownText, drew,
        mainText, TypeText(type, cam[1], now).c_str(), FarText(cam, ctx).c_str());
}

void SampleDcs(int kind, const double* cam, double centre, int mark, const float* vel) {
    const double v[3] = {vel[0], vel[1], vel[2]};
    Log("RAIN %s view: camera (%.3f, %.3f, %.3f) km, particle cells centred %.1f m from it, update mark %d, streak "
        "velocity %s%s",
        kKindName[kind], cam[0] / 1000, cam[1] / 1000, cam[2] / 1000, centre, mark, Vec(v).c_str(),
        kind == kDcsUpdating ? " (its camera's motion since its last update)" : " (left by the updating view)");
}

// ---- far views: the per-view constant buffer upload ----
// GraphicsCore's current block (the CPU copy of cPerView that updateBuffers fills and uploads).
unsigned char* BlockNow() {
    uint32_t idx = 0;
    unsigned char* arr = nullptr;
    if (!g_gcIndex || !g_gcArray || !SehRead(g_gcIndex, &idx, sizeof(idx)) || !SehRead(g_gcArray, &arr, sizeof(arr)) ||
        !arr || idx >= kMaxStack)
        return nullptr;
    return arr + static_cast<size_t>(idx) * kBlockStride;
}

// enlight's cloud volume state: the origin its offset is for, the offset and the scale.
bool EnState(float origin[3], float off[3], float scale[3]) {
    const unsigned char* impl = nullptr;
    return g_enImpl && SehRead(g_enImpl, &impl, sizeof(impl)) && impl && SehRead(impl + kEnOrigin, origin, 12) &&
           SehRead(impl + kEnOffset, off, 12) && SehRead(impl + kEnScale, scale, 12);
}

void LogFarSample(const UpSeen& u) {
    double uvw[3];
    CamUvw(u, u.used, uvw);
    const bool inside = uvw[0] >= 0 && uvw[0] <= 1 && uvw[2] >= 0 && uvw[2] <= 1;
    Log("RAIN far view upload (%s): camera (%.3f, %.3f, %.3f) km, this view's origin (%.3f, %.3f, %.3f) km, the frame "
        "origin DCS's cloud volume offset is for (%.3f, %.3f, %.3f) km (shift %.1f km); cloud volume offset (%.6f, %.6f, "
        "%.6f) scale (%.3g, %.3g, %.3g) -> (%.6f, %.6f, %.6f); the camera looked up at volume uvw (%.3f, %.3f, %.3f)%s, "
        "%.0f m from where it is (DCS's offset: %.1f km); %s",
        kUpKindName[u.kind], u.cam[0] / 1000, u.cam[1] / 1000, u.cam[2] / 1000, u.origin[0] / 1000, u.origin[1] / 1000,
        u.origin[2] / 1000, u.frame[0] / 1000, u.frame[1] / 1000, u.frame[2] / 1000, Shift(u) / 1000, u.dcsOff[0],
        u.dcsOff[1], u.dcsOff[2], u.scale[0], u.scale[1], u.scale[2], u.used[0], u.used[1], u.used[2], uvw[0], uvw[1],
        uvw[2], inside ? "" : " (outside DCS's cloud volume: its edge values)", LookupMiss(u, u.used),
        LookupMiss(u, u.dcsOff) / 1000, kUpStateName[u.state]);
}

void Record(const UpSeen& u) {
    InterlockedIncrement64(&g_upState[u.state]);
    AcquireSRWLockExclusive(&g_statLock);
    g_upLast = u;
    if (u.state == kUpFixed) {
        const double s = Shift(u);
        if (s > g_stat.maxShift) g_stat.maxShift = s;
    }
    ReleaseSRWLockExclusive(&g_statLock);
    if (g_wantFarSample && InterlockedExchange(&g_wantFarSample, 0)) LogFarSample(u);
}

// In place of ContextBase::updateBuffers (render thread, every cPerView upload). A context on the carrier views' own
// origin (the far view fix) gets the cloud volume offset for that origin: DCS's offset + (view origin - the origin
// DCS's offset is for) x scale, written into the block just before DCS's upload and put back right after. Every
// other upload, and every upload in 'stock', goes through untouched.
void UploadWrap(void* context, int slot) {
    unsigned char cb[kCbRead];
    float origin[3];
    if (!context || !SehRead(context, cb, sizeof(cb))) return g_upload(context, slot);
    memcpy(origin, cb + kCbOrigin, sizeof(origin));
    if (!NightSkyCarrierOrigin(origin)) return g_upload(context, slot);   // DCS's views: as DCS does
    UpSeen u;
    u.have = true;
    memcpy(u.cam, cb + kCbCam, sizeof(u.cam));
    memcpy(u.origin, origin, sizeof(origin));
    bool plat = false, nearMiss = false;
    u.kind = NightSkyOurView(u.cam, &plat, &nearMiss) ? (plat ? kUpPlat : kUpCamera) : kUpOther;
    InterlockedIncrement64(&g_upOwn);
    InterlockedIncrement64(&g_upKind[u.kind]);
    unsigned char* block = BlockNow();
    float enOff[3], enScale[3];
    if (!block || !SehRead(block + kBlockScale, u.scale, 12) || !SehRead(block + kBlockOffset, u.dcsOff, 12) ||
        !EnState(u.frame, enOff, enScale)) {
        u.state = kUpUnreadable;
        g_upload(context, slot);
        return Record(u);
    }
    memcpy(u.used, u.dcsOff, sizeof(u.used));
    if (static_cast<RainMode>(g_mode) == RainMode::Stock) {
        u.state = kUpStock;
        g_upload(context, slot);
        return Record(u);
    }
    // DCS's values are enlight's of this moment: then enlight's origin is the one they are for
    if (memcmp(u.dcsOff, enOff, sizeof(enOff)) != 0 || memcmp(u.scale, enScale, sizeof(enScale)) != 0) {
        u.state = kUpUnmatched;
        g_upload(context, slot);
        return Record(u);
    }
    if (Shift(u) < 1.0) {
        u.state = kUpSame;
        g_upload(context, slot);
        return Record(u);
    }
    for (int i = 0; i < 3; ++i)
        u.used[i] = static_cast<float>(double(u.dcsOff[i]) + (double(u.origin[i]) - double(u.frame[i])) * double(u.scale[i]));
    if (!SehWrite(block + kBlockOffset, u.used, sizeof(u.used))) {
        memcpy(u.used, u.dcsOff, sizeof(u.used));
        u.state = kUpUnreadable;
        g_upload(context, slot);
        return Record(u);
    }
    g_upload(context, slot);
    float after[3];
    if (SehRead(block + kBlockOffset, after, sizeof(after)) && memcmp(after, u.used, sizeof(after)) == 0) {
        SehWrite(block + kBlockOffset, u.dcsOff, sizeof(u.dcsOff));
        u.state = kUpFixed;
    } else {
        u.state = kUpConflict;
        Anomaly("the cloud volume offset changed during a per-view upload (another thread?): left as found");
    }
    double uvw[3];
    CamUvw(u, u.used, uvw);
    if (uvw[0] < 0 || uvw[0] > 1 || uvw[2] < 0 || uvw[2] > 1) InterlockedIncrement64(&g_upOutside);
    Record(u);
}

// For the rain sample lines: what the per-view constants of this draw carry (the last upload on the carrier's origin,
// if it was for this camera).
std::string FarText(const double* cam, const void* ctx) {
    if (!g_farInstalled) return "far-view cloud lookup not installed";
    float origin[3];
    if (!ctx || !SehRead(static_cast<const unsigned char*>(ctx) + 0x32C, origin, sizeof(origin)) ||
        !NightSkyCarrierOrigin(origin))
        return "the view is on the frame's origin (you within 70 km): DCS's cloud lookup is right for it";
    UpSeen u;
    AcquireSRWLockShared(&g_statLock);
    u = g_upLast;
    ReleaseSRWLockShared(&g_statLock);
    if (!u.have || memcmp(u.cam, cam, sizeof(u.cam)) != 0)
        return "the view is on the carrier's origin, but the last upload on it was for another camera";
    char b[300];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "its per-view constants: %s, own origin %.1f km from the frame's, cloud density read %.0f m from the "
                "camera (DCS's offset: %.1f km)",
                kUpStateName[u.state], Shift(u) / 1000, LookupMiss(u, u.used), LookupMiss(u, u.dcsOff) / 1000);
    return b;
}

std::string TypeText(int type, double camY, double now) {
    char b[200];
    double t = 0;
    const double dcs = DcsRainShare(now);
    char dcsText[48] = "not seen";
    if (dcs >= 0) _snprintf_s(dcsText, _countof(dcsText), _TRUNCATE, "%.2f", dcs);
    if (!CarrierTemp(&t)) {
        _snprintf_s(b, _countof(b), _TRUNCATE, "%s object, carrier temperature not known (DCS's split; its rain share at "
                    "your camera %s)", type == 1 ? "snow" : "rain", dcsText);
        return b;
    }
    const double here = t - kLapse * std::fmax(0.0, camY);
    _snprintf_s(b, _countof(b), _TRUNCATE, "%s object, %.1f C at the camera (rain share %.2f; DCS's at your camera %s)",
                type == 1 ? "snow" : "rain", here, RainShareAt(here), dcsText);
    return b;
}

// The provider's collectRenderables (vtable +0x23BB90): DCS's call, then note the provider's rain object. It is
// read again at every frame's collection, so a draw only ever uses the rain object of a live provider.
uintptr_t CollectWrap(void* self, uintptr_t count, void* a3, void* a4, void* a5) {
    const uintptr_t r = g_collect(self, count, a3, a4, a5);
    void* rainObj = nullptr;
    if (self && SehRead(static_cast<const unsigned char*>(self) + kProvRain, &rainObj, sizeof(rainObj)) && rainObj) {
        InterlockedExchangePointer(&g_rainObj, rainObj);
        StoreTime(&g_rainObjAt, g_clock());
    }
    return r;
}

// One of ILARTS's views, given only DCS's snow renderable while DCS draws no rain anywhere (your camera in pure snow:
// DCS's rain share 0) although this view's own temperature calls for rain: DCS's per-view call draws DCS's rain object
// with this renderable for that one call - the renderable's Rain object, power (the snow renderable's x this view's
// rain share) and update mark set just before and put back right after. One view (the marker: the first to draw, kept
// while it keeps drawing) carries the update mark, so that DCS's Rain::update runs the rain simulation, particle counts
// and wind for it (at the carrier); the others draw the rain as it is, with their own motion in cameraVel, as for DCS's
// rain. Nothing is copied and DCS's code runs unchanged. Returns false when it did not draw.
bool BorrowRain(void* self, const void* ctx, void* a3, void* a4, void* rainObj, const unsigned char* rend, double share,
                const double* cam, const float* use, bool still, int kind, const Motion& own, double now) {
    unsigned char* r = static_cast<unsigned char*>(self);
    unsigned char* ro = static_cast<unsigned char*>(rainObj);
    float power = 0;
    memcpy(&power, rend + kRendPower, sizeof(power));
    const float usePower = static_cast<float>(power * share);
    bool marker = false, newMarker = false;
    AcquireSRWLockExclusive(&g_markerLock);
    const double since = now - g_markerT;
    if (since > kMarkerStaleSec) {
        marker = newMarker = true;
    } else {
        const double d[3] = {cam[0] - g_markerPos[0], cam[1] - g_markerPos[1], cam[2] - g_markerPos[2]};
        marker = Len(d) <= 1.0 + kRainMaxSpeed * std::fmax(0.0, since);
    }
    if (marker) {
        for (int i = 0; i < 3; ++i) g_markerPos[i] = cam[i];
        g_markerT = now;
    }
    ReleaseSRWLockExclusive(&g_markerLock);
    float before[3] = {};
    if (!SehRead(ro + kRainCamVel, before, sizeof(before)) || !SehWrite(ro + kRainCamVel, use, 3 * sizeof(float))) {
        InterlockedIncrement64(&g_refused);
        Anomaly("DCS's rain object could not be read or written: the carrier's rain not drawn");
        return false;
    }
    // A new marker (or 'still'): DCS's next cameraVel = (this camera - stored camera) / dT, so the stored camera is this
    // one - no streak from wherever the last update was (your camera, another view).
    if (marker && (newMarker || still)) SehWrite(ro + kRainStored, cam, 3 * sizeof(double));
    void* snowObj = nullptr;
    memcpy(&snowObj, rend + kRendRain, sizeof(snowObj));
    const unsigned char markByte = marker ? 1 : 0, dcsMark = rend[kRendMark];
    if (!SehWrite(r + kRendRain, &rainObj, sizeof(rainObj))) {
        SehWrite(ro + kRainCamVel, before, sizeof(before));
        InterlockedIncrement64(&g_refused);
        Anomaly("DCS's snow renderable could not be written: the carrier's rain not drawn");
        return false;
    }
    SehWrite(r + kRendPower, &usePower, sizeof(usePower));
    SehWrite(r + kRendMark, &markByte, 1);
    g_draw(self, ctx, a3, a4);
    SehWrite(r + kRendRain, &snowObj, sizeof(snowObj));
    SehWrite(r + kRendPower, &power, sizeof(power));
    SehWrite(r + kRendMark, &dcsMark, 1);
    if (!marker) {   // the marker's cameraVel is DCS's own (Rain::update worked it out); the others' is put back
        float after[3] = {};
        if (SehRead(ro + kRainCamVel, after, sizeof(after)) && memcmp(after, use, sizeof(after)) == 0)
            SehWrite(ro + kRainCamVel, before, sizeof(before));
        else
            InterlockedIncrement64(&g_conflicts);
    }
    InterlockedIncrement64(&g_borrowed);
    if (marker) InterlockedIncrement64(&g_borrowSim);
    if (InterlockedExchange(&g_firstBorrowLine, 1) == 0)
        Log("RAIN DCS draws no rain (your camera in snow, DCS's rain share 0): ILARTS's views draw DCS's rain object for "
            "the carrier (rain share %.2f at the first view), the simulation run by one of them", share);
    const double centre = CentreDistance(rainObj, cam);
    AcquireSRWLockExclusive(&g_statLock);
    const double u[3] = {use[0], use[1], use[2]};
    g_stat.lastOwn[kind] = Len(u);
    if (g_stat.lastOwn[kind] > g_stat.maxOwn) g_stat.maxOwn = g_stat.lastOwn[kind];
    if (centre > g_stat.maxCentreOurs) g_stat.maxCentreOurs = centre;
    ReleaseSRWLockExclusive(&g_statLock);
    if (g_wantSample[kind] && InterlockedExchange(&g_wantSample[kind], 0))
        SampleOurs(kind, cam, ctx, centre, markByte, before, &own,
                   marker ? "with DCS's rain object (DCS draws no rain: your camera in snow), running its simulation"
                          : "with DCS's rain object (DCS draws no rain: your camera in snow)",
                   0, now);
    return true;
}

void Wrap(void* self, const void* ctx, void* a3, void* a4) {
    const double now = g_clock();
    double cam[3] = {};
    const bool haveCam = ctx && SehRead(static_cast<const unsigned char*>(ctx) + kCtxCam, cam, sizeof(cam));
    bool plat = false, nearMiss = false;
    const bool ours = haveCam && NightSkyOurView(cam, &plat, &nearMiss) != 0;
    if (nearMiss) InterlockedIncrement64(&g_nearMiss);
    alignas(16) unsigned char rend[kRendSize];
    const bool haveRend = SehRead(self, rend, sizeof(rend));
    void* rain = nullptr;
    if (haveRend) memcpy(&rain, rend + kRendRain, sizeof(rain));
    const int mark = haveRend ? rend[kRendMark] : 0;
    if (rain && rain == g_rainObj) StoreTime(&g_rainSeenAt, now);   // DCS gives some view its rain renderable
    if (!ours || !rain) {
        g_draw(self, ctx, a3, a4);
        if (!haveRend || !rain || !haveCam) return;
        const int kind = mark ? kDcsUpdating : kDcsOther;
        InterlockedIncrement64(&g_draws[kind]);
        const bool want = g_wantSample[kind] != 0;
        if (kind != kDcsUpdating && !want) return;
        float vel[3] = {};
        if (!SehRead(static_cast<unsigned char*>(rain) + kRainCamVel, vel, sizeof(vel))) return;
        // the cells' centre: for the sample lines, and every 64th draw for the stats line
        const double centre = want || (g_draws[kind] & 63) == 1 ? CentreDistance(rain, cam) : -1;
        int type = -1;
        if (kind == kDcsUpdating && !SehRead(static_cast<unsigned char*>(rain) + kRainType, &type, sizeof(type))) type = -1;
        float power = 0;
        memcpy(&power, rend + kRendPower, sizeof(power));
        AcquireSRWLockExclusive(&g_statLock);
        if (centre > g_stat.maxCentreDcs) g_stat.maxCentreDcs = centre;
        if (type == 0 || type == 1) {   // DCS's split as your view draws it
            g_stat.dcsPow[type] = power;
            g_stat.dcsPowT[type] = now;
        }
        if (kind == kDcsUpdating) {
            const double v[3] = {vel[0], vel[1], vel[2]};
            g_stat.lastDcs = Len(v);
            if (g_stat.lastDcs > g_stat.maxDcs) g_stat.maxDcs = g_stat.lastDcs;
            for (int i = 0; i < 3; ++i) {
                g_stat.dcsCam[i] = cam[i];
                g_stat.dcsVel[i] = v[i];
            }
            g_stat.haveDcs = true;
        }
        ReleaseSRWLockExclusive(&g_statLock);
        if (want && InterlockedExchange(&g_wantSample[kind], 0)) SampleDcs(kind, cam, centre, mark, vel);
        return;
    }
    const int kind = plat ? kPlat : kCamera;
    InterlockedIncrement64(&g_draws[kind]);
    // A camera at a hangar deck position (the hook's view flag): inside the ship, never any rain or snow.
    if (!plat && ExtraCamsHangarNear(cam, kHangarMatchMetres)) {
        InterlockedIncrement64(&g_hangarSkipped);
        return;
    }
    if (SimIdle(rain, now)) InterlockedIncrement64(&g_simIdle);
    unsigned char* camVel = static_cast<unsigned char*>(rain) + kRainCamVel;
    float dcsVel[3] = {};
    const bool haveVel = SehRead(camVel, dcsVel, sizeof(dcsVel));
    const bool want = g_wantSample[kind] != 0;
    const auto mode = static_cast<RainMode>(g_mode);
    int type = -1;
    if (!SehRead(static_cast<unsigned char*>(rain) + kRainType, &type, sizeof(type))) type = -1;
    // tracked in every mode, so that a switch (CAMERA_RAIN = 'ab') starts with a full window
    const Motion own = TrackMotion(ctx, cam, now);
    if (own.newView) InterlockedIncrement64(&g_newViews);
    if (mode == RainMode::Hide) {
        InterlockedIncrement64(&g_hidden);
        if (want && InterlockedExchange(&g_wantSample[kind], 0))
            SampleOurs(kind, cam, ctx, -1, mark, dcsVel, &own, "not (hide)", type, now);
        return;
    }
    // auto / still, a view without DCS's mark: the precipitation type of this view's own temperature. DCS's split comes
    // from your camera's (snow in the PLAT of a 20 C mission while you fly in the -1..+2 C band, about 3 km up).
    double tempSea = 0;
    // DCS draws no rain at all (your camera in pure snow) but this view's temperature calls for rain: the carrier's
    // rain from DCS's rain object, through this view's snow renderable. Then the snow below, as for any other view.
    if (g_borrowInstalled && (mode == RainMode::Auto || mode == RainMode::Still) && !mark && type == 1 &&
        CarrierTemp(&tempSea)) {
        const double share = RainShareAt(tempSea - kLapse * std::fmax(0.0, cam[1]));
        if (share > 0 && now - LoadTime(&g_rainSeenAt) > kRainGoneSec) {
            void* rainObj = g_rainObj;
            int rainType = -1;
            if (rainObj && rainObj != rain && now - LoadTime(&g_rainObjAt) < kProviderFreshSec &&
                SehRead(static_cast<unsigned char*>(rainObj) + kRainType, &rainType, sizeof(rainType)) && rainType == 0) {
                float use[3] = {};
                if (mode == RainMode::Auto)
                    for (int i = 0; i < 3; ++i) use[i] = static_cast<float>(own.v[i]);
                BorrowRain(self, ctx, a3, a4, rainObj, rend, share, cam, use, mode == RainMode::Still, kind, own, now);
            } else {
                InterlockedIncrement64(&g_borrowNone);
            }
        }
    }
    if (mode != RainMode::Stock && !mark && (type == 0 || type == 1) && CarrierTemp(&tempSea)) {
        const double share = RainShareAt(tempSea - kLapse * std::fmax(0.0, cam[1]));
        AcquireSRWLockExclusive(&g_statLock);
        g_stat.lastShare = share;
        ReleaseSRWLockExclusive(&g_statLock);
        if ((type == 1 && share >= 1.0) || (type == 0 && share <= 0.0)) {
            InterlockedIncrement64(type == 1 ? &g_snowHidden : &g_rainHidden);
            if (want && InterlockedExchange(&g_wantSample[kind], 0))
                SampleOurs(kind, cam, ctx, -1, mark, dcsVel, &own,
                           type == 1 ? "not (snow: too warm at this view)" : "not (rain: freezing at this view)", type, now);
            return;
        }
    }
    if (mode == RainMode::Stock || !haveVel) {
        if (mode == RainMode::Stock) {
            InterlockedIncrement64(&g_stock);
        } else {
            InterlockedIncrement64(&g_refused);
            Anomaly("the Rain object's cameraVel could not be read: drawn as DCS does");
        }
        g_draw(self, ctx, a3, a4);
        const double centre = CentreDistance(rain, cam);
        const double used[3] = {dcsVel[0], dcsVel[1], dcsVel[2]};
        AcquireSRWLockExclusive(&g_statLock);
        g_stat.lastOwn[kind] = Len(used);
        if (g_stat.lastOwn[kind] > g_stat.maxOwn) g_stat.maxOwn = g_stat.lastOwn[kind];
        if (centre > g_stat.maxCentreOurs) g_stat.maxCentreOurs = centre;
        ReleaseSRWLockExclusive(&g_statLock);
        if (want && InterlockedExchange(&g_wantSample[kind], 0))
            SampleOurs(kind, cam, ctx, centre, mark, dcsVel, &own, "as DCS does (stock: the updating view's motion)", type, now);
        return;
    }
    double v[3] = {};
    if (mode == RainMode::Auto) {
        for (int i = 0; i < 3; ++i) v[i] = own.v[i];
        InterlockedIncrement64(&g_auto);
    } else {
        InterlockedIncrement64(&g_still);
    }
    const float use[3] = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
    // A view DCS marked as its updating view gets DCS's own motion for it (DCS computes it from that view's camera):
    // drawn exactly as DCS does. No copy of DCS's renderable is ever made (a stack copy of a DCS object is the kind
    // of stand-in that crashed DCS in 1.1.0.1).
    if (mark) {
        InterlockedIncrement64(&g_markCleared);
        if (InterlockedExchange(&g_firstMarkLine, 1) == 0)
            Log("RAIN one of ILARTS's views carries DCS's update mark: drawn as DCS does (its own camera's motion)");
        g_draw(self, ctx, a3, a4);
        return;
    }
    if (!SehWrite(camVel, use, sizeof(use))) {
        InterlockedIncrement64(&g_refused);
        Anomaly("the Rain object's cameraVel could not be written: drawn as DCS does");
        g_draw(self, ctx, a3, a4);
        return;
    }
    g_draw(self, ctx, a3, a4);
    float after[3] = {};
    if (SehRead(camVel, after, sizeof(after)) && memcmp(after, use, sizeof(use)) == 0) {
        SehWrite(camVel, dcsVel, sizeof(dcsVel));
    } else {
        InterlockedIncrement64(&g_conflicts);
        Anomaly("cameraVel changed during one of ILARTS's draws (another thread?): left as found");
    }
    const double centre = CentreDistance(rain, cam);
    const double used[3] = {use[0], use[1], use[2]};
    AcquireSRWLockExclusive(&g_statLock);
    g_stat.lastOwn[kind] = Len(used);
    if (g_stat.lastOwn[kind] > g_stat.maxOwn) g_stat.maxOwn = g_stat.lastOwn[kind];
    if (centre > g_stat.maxCentreOurs) g_stat.maxCentreOurs = centre;
    ReleaseSRWLockExclusive(&g_statLock);
    if (want && InterlockedExchange(&g_wantSample[kind], 0))
        SampleOurs(kind, cam, ctx, centre, mark, dcsVel, &own,
                   mode == RainMode::Auto ? "with this view's own motion (auto)" : "as for a camera at rest (still)", type,
                   now);
}

bool WritePtr(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

// Stats line pacing: at most one a minute for the first 10, then one per 5 minutes; only when something drew.
double g_lastLine = -1e9;
int g_lines = 0;

// The import address table slot of dll!func in mod (by name), or null.
void** ImportSlot(HMODULE mod, const char* dll, const char* func) {
    auto base = reinterpret_cast<unsigned char*>(mod);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), func) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

bool CodeIs(const unsigned char* at, const unsigned char* bytes, size_t n) {
    unsigned char b[0x80];
    return n <= sizeof(b) && SafeRead(at, b, n) && memcmp(b, bytes, n) == 0;
}

// The far views' cloud lookup: GraphicsVista's and enlight's imports of ContextBase::updateBuffers -> UploadWrap.
// "" or why not (then the rest of the rain wrapper still works).
std::string FarInstall() {
    if (g_farInstalled) return "";
    HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll"), gv = GetModuleHandleW(L"GraphicsVista.dll"),
            en = GetModuleHandleW(L"enlight.dll");
    if (!gc || !gv || !en) return "GraphicsCore.dll, GraphicsVista.dll or enlight.dll not loaded";
    auto gb = reinterpret_cast<unsigned char*>(gc), vb = reinterpret_cast<unsigned char*>(gv),
         eb = reinterpret_cast<unsigned char*>(en);
    if (reinterpret_cast<unsigned char*>(GetProcAddress(gc, kGcUpdateName)) != gb + kGcUpdateRva)
        return "updateBuffers is not at GraphicsCore+0x18BD0";
    if (reinterpret_cast<unsigned char*>(GetProcAddress(gc, kGcSetCloudName)) != gb + kGcSetCloudRva)
        return "setCloudVolumeTransform is not at GraphicsCore+0x189D0";
    if (reinterpret_cast<unsigned char*>(GetProcAddress(en, kEnGetOffsetName)) != eb + kEnExpOffRva ||
        reinterpret_cast<unsigned char*>(GetProcAddress(en, kEnGetScaleName)) != eb + kEnExpScaleRva)
        return "CloudsModule::getCloudVolumeOffset / Scale are not at enlight+0x2FD70 / +0x2FD90";
    if (reinterpret_cast<unsigned char*>(GetProcAddress(en, kEnCloudsName)) != eb + kEnCloudsRefRva)
        return "enlight's clouds facade is not at +0x9A178";
    const struct {
        const unsigned char* at;
        const unsigned char* bytes;
        size_t n;
        const char* what;
    } checks[] = {
        {gb + kGcSetCloudRva, kGcSetCloud, sizeof(kGcSetCloud), "GraphicsCore: setCloudVolumeTransform"},
        {gb + 0x18BE9, kGcUpdEntry, sizeof(kGcUpdEntry), "GraphicsCore: updateBuffers' block"},
        {gb + 0x1909C, kGcUpdOrigin, sizeof(kGcUpdOrigin), "GraphicsCore: updateBuffers' gOrigin"},
        {gb + 0x19508, kGcUpdUpload, sizeof(kGcUpdUpload), "GraphicsCore: updateBuffers' upload"},
        {vb + kGvBindCopyRva, kGvBindCopy, sizeof(kGvBindCopy), "GraphicsVista: bindViewportContextToGPU"},
        {eb + kEnGetOffRva, kEnGetters, sizeof(kEnGetters), "enlight: the volume offset / scale getters"},
        {eb + kEnUpdStoresRva, kEnUpdStores, sizeof(kEnUpdStores), "enlight: the volume update"},
        {eb + kEnExpOffRva, kEnExpGetter[0], sizeof(kEnExpGetter[0]), "enlight: CloudsModule::getCloudVolumeOffset"},
        {eb + kEnExpScaleRva, kEnExpGetter[1], sizeof(kEnExpGetter[1]), "enlight: CloudsModule::getCloudVolumeScale"},
    };
    for (const auto& c : checks)
        if (!CodeIs(c.at, c.bytes, c.n)) return std::string(c.what) + " is not the expected code";
    // the facade object the exported getters call uses the vtable whose slots 12 / 13 are the checked getters
    const unsigned char* facade = nullptr;
    const unsigned char* vptr = nullptr;
    const unsigned char* slots[2] = {};
    if (!SafeRead(eb + kEnCloudsRefRva, &facade, sizeof(facade)) || !facade || !SafeRead(facade, &vptr, sizeof(vptr)) ||
        vptr != eb + kEnFacadeVtblRva || !SafeRead(vptr + 12 * sizeof(void*), slots, sizeof(slots)) ||
        slots[0] != eb + kEnGetOffRva || slots[1] != eb + kEnGetScaleRva)
        return "enlight's clouds facade does not use the expected vtable";
    void** s[2] = {ImportSlot(gv, "GraphicsCore.dll", kGcUpdateName), ImportSlot(en, "GraphicsCore.dll", kGcUpdateName)};
    for (void** slot : s) {
        void* cur = nullptr;
        if (!slot || !SafeRead(slot, &cur, sizeof(cur)) || cur != gb + kGcUpdateRva)
            return "an import of updateBuffers (GraphicsVista, enlight) does not lead to GraphicsCore's";
    }
    g_gcIndex = reinterpret_cast<const uint32_t*>(gb + kGcIndexRva);
    g_gcArray = reinterpret_cast<unsigned char* const*>(gb + kGcArrayRva);
    g_enImpl = reinterpret_cast<void* const*>(eb + kEnImplRva);
    g_upload = reinterpret_cast<UploadFn>(gb + kGcUpdateRva);
    if (!WritePtr(s[0], reinterpret_cast<void*>(&UploadWrap))) return "could not patch GraphicsVista's import";
    if (!WritePtr(s[1], reinterpret_cast<void*>(&UploadWrap))) {
        WritePtr(s[0], reinterpret_cast<void*>(g_upload));
        return "could not patch enlight's import";
    }
    g_upSlots[0] = s[0];
    g_upSlots[1] = s[1];
    g_farInstalled = true;
    return "";
}

void ResetBorrow() {
    InterlockedExchangePointer(&g_rainObj, nullptr);
    g_rainObjAt = 0;
    g_rainSeenAt = 0;
    g_firstBorrowLine = 0;
    g_borrowed = g_borrowSim = g_borrowNone = 0;
    AcquireSRWLockExclusive(&g_markerLock);
    g_markerT = -1e9;
    ReleaseSRWLockExclusive(&g_markerLock);
}

// The carrier's rain while DCS draws none: the provider's collectRenderables slot -> CollectWrap. "" or why not.
std::string BorrowInstall(unsigned char* vb) {
    if (g_borrowInstalled) return "";
    void* vt[2] = {};
    if (!SafeRead(vb + kProvVtblRva, vt, sizeof(vt)) || vt[0] != vb + kProvSlot0Fn || vt[1] != vb + kCollectRva)
        return "the precipitation provider's vtable is not as expected";
    if (!CodeIs(vb + kCollectRva, kCollectEntry, sizeof(kCollectEntry)) ||
        !CodeIs(vb + 0xC9AA5, kCollectRain, sizeof(kCollectRain)) || !CodeIs(vb + 0xC9ACA, kCollectSnow, sizeof(kCollectSnow)))
        return "collectRenderables is not the expected code";
    ResetBorrow();
    g_collect = reinterpret_cast<CollectFn>(vb + kCollectRva);
    void** slot = reinterpret_cast<void**>(vb + kProvSlotRva);
    if (!WritePtr(slot, reinterpret_cast<void*>(&CollectWrap))) return "could not patch the provider's vtable";
    g_provSlot = slot;
    g_borrowInstalled = true;
    return "";
}

void BorrowRemove() {
    if (!g_borrowInstalled) return;
    WritePtr(g_provSlot, reinterpret_cast<void*>(g_collect));   // a call still inside finishes normally
    g_provSlot = nullptr;
    g_borrowInstalled = false;
    InterlockedExchangePointer(&g_rainObj, nullptr);
}

void FarRemove() {
    if (!g_farInstalled) return;
    // An upload still inside the wrapper finishes normally: the code stays loaded.
    for (void** slot : g_upSlots)
        if (slot) WritePtr(slot, reinterpret_cast<void*>(g_upload));
    g_upSlots[0] = g_upSlots[1] = nullptr;
    g_farInstalled = false;
}

}  // namespace

std::string RainInstall() {
    if (EngineDisabled()) return "engine calls disabled";
    if (g_installed) return "";
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    HMODULE eff = GetModuleHandleW(L"Effects2.dll");
    if (!eff) return "Effects2.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    auto vb = reinterpret_cast<unsigned char*>(vis);
    auto eb = reinterpret_cast<unsigned char*>(eff);
    unsigned char code[sizeof(kDrawCode)];
    if (!SafeRead(vb + kDrawRva, code, sizeof(code)) || memcmp(code, kDrawCode, sizeof(code)) != 0)
        return "Visualizer+0xC9C60 is not the expected code";
    void* vt[2] = {};
    if (!SafeRead(vb + kVtblRva, vt, sizeof(vt)) || vt[0] != vb + kSlot0Fn || vt[1] != vb + kDrawRva)
        return "the precipitation renderable's vtable is not as expected";
    for (const CodeCheck& c : kEffChecks) {
        unsigned char b[128];
        if (c.size > sizeof(b) || !SafeRead(eb + c.rva, b, c.size) || memcmp(b, c.bytes, c.size) != 0)
            return std::string("Effects2: ") + c.what + " is not the expected code";
    }
    if (!CodeIs(eb + 0x5740, kEffInitType, sizeof(kEffInitType))) return "Effects2: Rain::init's type store is not the expected code";
    if (!CodeIs(vb + 0xC98B6, kVisInitTypes, sizeof(kVisInitTypes)))
        return "Visualizer: the precipitation provider's Rain::init calls are not the expected code";
    void* exp = reinterpret_cast<void*>(GetProcAddress(eff, kRainDrawName));
    if (exp != eb + kEffDrawRva) return "Rain::draw is not at Effects2+0x52E0";
    void* imp = nullptr;
    if (!SafeRead(vb + kIatRainDraw, &imp, sizeof(imp)) || imp != exp)
        return "Visualizer's import of Rain::draw does not lead to Effects2's";
    void** slot = reinterpret_cast<void**>(vb + kSlotRva);
    g_draw = reinterpret_cast<DrawFn>(vb + kDrawRva);
    if (!WritePtr(slot, reinterpret_cast<void*>(&Wrap))) return "could not patch the renderable vtable";
    g_slot = slot;
    g_installed = true;
    for (auto& w : g_wantSample) w = 1;
    g_anomalyLines = 0;
    g_firstMarkLine = 0;
    g_lines = 0;
    g_lastLine = -1e9;
    AcquireSRWLockExclusive(&g_trackLock);
    for (Track& k : g_tracks) k = Track{};
    g_lastTrack = -1;
    ReleaseSRWLockExclusive(&g_trackLock);
    AcquireSRWLockExclusive(&g_statLock);
    g_stat = Stat{};
    for (SimSeen& s : g_sim) s = SimSeen{};
    ReleaseSRWLockExclusive(&g_statLock);
    for (auto& k : g_upKind) k = 0;
    for (auto& k : g_upState) k = 0;
    g_upOwn = g_upOutside = 0;
    g_wantFarSample = 1;
    Log("RAIN installed: Visualizer precipitation renderable (vtable +0x23BBD8) -> wrapper; ILARTS's views draw rain and "
        "snow %s",
        g_mode == static_cast<long>(RainMode::Auto)    ? "with their own camera's motion"
        : g_mode == static_cast<long>(RainMode::Still) ? "as for a camera at rest"
        : g_mode == static_cast<long>(RainMode::Hide)  ? "not at all"
                                                       : "as DCS does");
    const std::string borrowWhy = BorrowInstall(vb);
    if (borrowWhy.empty())
        Log("RAIN while you are in snow: the precipitation provider's collectRenderables (vtable +0x23BB90) -> wrapper "
            "(ILARTS's views draw DCS's rain object for the carrier when DCS draws none)");
    else
        Log("RAIN while you are in snow: NOT installed (%s); ILARTS's views show no rain while DCS draws none",
            borrowWhy.c_str());
    const std::string farWhy = FarInstall();
    if (farWhy.empty())
        Log("RAIN far views: GraphicsVista's and enlight's imports of ContextBase::updateBuffers -> wrapper (the views on "
            "the carrier's own origin get the cloud volume offset for it: rain density, cloud shadows)");
    else
        Log("RAIN far views: cloud lookup NOT installed (%s); rain and snow in ILARTS's views still corrected", farWhy.c_str());
    return "";
}

void RainRemove() {
    if (!g_installed) return;
    FarRemove();
    BorrowRemove();
    // A render worker still inside the wrapper finishes normally: the code stays loaded.
    WritePtr(g_slot, reinterpret_cast<void*>(g_draw));
    g_installed = false;
    Log("RAIN removed");
}

void RainSetCarrierTemperature(bool known, double celsius) {
    long long bits = kNoTemp;
    if (known && std::isfinite(celsius) && celsius > -90 && celsius < 70) memcpy(&bits, &celsius, sizeof(bits));
    if (InterlockedExchange64(&g_tempBits, bits) != bits) {
        if (bits == kNoTemp)
            Log("RAIN carrier temperature not known: ILARTS's views draw DCS's rain / snow split (from your camera)");
        else
            Log("RAIN carrier temperature %.1f C at sea level: ILARTS's views draw the precipitation type of their own "
                "height (rain share %.2f at sea level)", celsius, RainShareAt(celsius));
    }
}

void RainSetMode(RainMode m) {
    const long v = static_cast<long>(m);
    if (InterlockedExchange(&g_mode, v) != v)
        Log("RAIN rain and snow in ILARTS's views: %s",
            m == RainMode::Auto    ? "streaks from each view's own camera motion (auto)"
            : m == RainMode::Still ? "streaks as for a camera at rest (still)"
            : m == RainMode::Hide  ? "not drawn (hide)"
                                   : "as DCS draws them: your own camera's motion (stock)");
}

RainMode RainGetMode() { return static_cast<RainMode>(g_mode); }

std::string RainStats() {
    const double now = g_clock();
    long long drawn = g_upOwn;   // the far views' uploads count too (cloud shadows without rain)
    for (const auto& d : g_draws) drawn += d;
    if (drawn == 0 && g_lines > 0) return "";
    if (now - g_lastLine < (g_lines < 10 ? 60.0 : 300.0)) return "";
    const double since = g_lines ? now - g_lastLine : 0;
    g_lastLine = now;
    ++g_lines;
    Stat s;
    AcquireSRWLockExclusive(&g_statLock);
    s = g_stat;
    g_stat.maxOwn = 0;
    g_stat.maxDcs = 0;
    g_stat.maxCentreOurs = g_stat.maxCentreDcs = -1;
    g_stat.maxShift = 0;
    ReleaseSRWLockExclusive(&g_statLock);
    const auto mode = static_cast<RainMode>(g_mode);
    char b[2400];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "installed=%d mode=%s (%.0f s:) ILARTS views: PLAT draws=%lld camera draws=%lld auto=%lld still=%lld "
                "hidden=%lld hangar=%lld stock=%lld marked=%lld conflicts=%lld refused=%lld near_misses=%lld new_views=%lld "
                "views_tracked=%d sim_idle=%lld | speed used: PLAT %.1f, camera %.1f, max %.1f m/s | DCS views: updating "
                "draws=%lld other draws=%lld, streak velocity %.1f (max %.1f) m/s | cells centred within %.1f m (ILARTS) / "
                "%.1f m (DCS) of the camera",
                g_installed ? 1 : 0,
                mode == RainMode::Auto    ? "auto"
                : mode == RainMode::Still ? "still"
                : mode == RainMode::Hide  ? "hide"
                                          : "stock",
                since, InterlockedExchange64(&g_draws[kPlat], 0), InterlockedExchange64(&g_draws[kCamera], 0),
                InterlockedExchange64(&g_auto, 0), InterlockedExchange64(&g_still, 0), InterlockedExchange64(&g_hidden, 0),
                InterlockedExchange64(&g_hangarSkipped, 0), InterlockedExchange64(&g_stock, 0), InterlockedExchange64(&g_markCleared, 0),
                InterlockedExchange64(&g_conflicts, 0), InterlockedExchange64(&g_refused, 0),
                InterlockedExchange64(&g_nearMiss, 0), InterlockedExchange64(&g_newViews, 0), TrackCount(),
                InterlockedExchange64(&g_simIdle, 0), s.lastOwn[0], s.lastOwn[1], s.maxOwn,
                InterlockedExchange64(&g_draws[kDcsUpdating], 0), InterlockedExchange64(&g_draws[kDcsOther], 0), s.lastDcs,
                s.maxDcs, s.maxCentreOurs, s.maxCentreDcs);
    std::string out = b;
    double t = 0;
    char tText[32] = "not known";
    if (CarrierTemp(&t)) _snprintf_s(tText, _countof(tText), _TRUNCATE, "%.1f C at sea level", t);
    _snprintf_s(b, _countof(b), _TRUNCATE,
                " | type: carrier %s, rain share used %.2f, DCS's at your camera %.2f; ILARTS views: snow not drawn=%lld "
                "rain not drawn=%lld; DCS's rain drawn while DCS drew none=%lld (simulated=%lld, no rain object=%lld%s)",
                tText, s.lastShare, DcsRainShare(now), InterlockedExchange64(&g_snowHidden, 0),
                InterlockedExchange64(&g_rainHidden, 0), InterlockedExchange64(&g_borrowed, 0),
                InterlockedExchange64(&g_borrowSim, 0), InterlockedExchange64(&g_borrowNone, 0),
                g_borrowInstalled ? "" : ", not installed");
    out += b;
    if (!g_farInstalled) {
        out += " | far views: cloud lookup not installed";
    } else {
        _snprintf_s(b, _countof(b), _TRUNCATE,
                    " | far views: uploads on the carrier's origin=%lld (PLAT %lld, camera %lld, other %lld) corrected=%lld "
                    "stock=%lld no_shift=%lld unmatched=%lld unreadable=%lld conflicts=%lld camera_outside_volume=%lld, "
                    "max shift %.1f km",
                    InterlockedExchange64(&g_upOwn, 0), InterlockedExchange64(&g_upKind[kUpPlat], 0),
                    InterlockedExchange64(&g_upKind[kUpCamera], 0), InterlockedExchange64(&g_upKind[kUpOther], 0),
                    InterlockedExchange64(&g_upState[kUpFixed], 0), InterlockedExchange64(&g_upState[kUpStock], 0),
                    InterlockedExchange64(&g_upState[kUpSame], 0), InterlockedExchange64(&g_upState[kUpUnmatched], 0),
                    InterlockedExchange64(&g_upState[kUpUnreadable], 0), InterlockedExchange64(&g_upState[kUpConflict], 0),
                    InterlockedExchange64(&g_upOutside, 0), s.maxShift / 1000);
        out += b;
    }
    for (auto& w : g_wantSample) w = 1;
    g_wantFarSample = 1;
    return out;
}

#ifdef RAIN_TEST
void RainTestSet(RainDrawFn draw, double (*clock)()) {
    g_draw = draw;
    g_clock = clock ? clock : &QpcClock;
}
void* RainTestWrap() { return reinterpret_cast<void*>(&Wrap); }
void RainTestInstalled(bool on) {
    g_installed = on;
    if (on) {
        for (auto& w : g_wantSample) w = 1;
        AcquireSRWLockExclusive(&g_trackLock);
        for (Track& k : g_tracks) k = Track{};
        g_lastTrack = -1;
        ReleaseSRWLockExclusive(&g_trackLock);
        AcquireSRWLockExclusive(&g_statLock);
        g_stat = Stat{};
        for (SimSeen& s : g_sim) s = SimSeen{};
        ReleaseSRWLockExclusive(&g_statLock);
    }
}
void RainTestResetStatsClock() {
    g_lastLine = -1e9;
    g_lines = 0;
}
void RainTestSetFar(RainUploadFn upload, const uint32_t* stackIndex, unsigned char* const* stackArray,
                    void* const* cloudImpl, bool installed) {
    g_upload = upload;
    g_gcIndex = stackIndex;
    g_gcArray = stackArray;
    g_enImpl = cloudImpl;
    g_farInstalled = installed;
    g_wantFarSample = 1;
}
void* RainTestUploadWrap() { return reinterpret_cast<void*>(&UploadWrap); }
void RainTestSetBorrow(RainCollectFn collect, bool installed) {
    g_collect = collect;
    g_borrowInstalled = installed;
    ResetBorrow();
}
void* RainTestCollectWrap() { return reinterpret_cast<void*>(&CollectWrap); }
bool RainTestBorrowInstalled() { return g_borrowInstalled; }
bool RainTestFarInstalled() { return g_farInstalled; }
#endif

}  // namespace lsodiag
