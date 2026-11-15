// DCS ILARTS virtual cameras: what the camera component (ILARTSCam.dll, loaded by Windows' camera service, Frame
// Server) and the viewer (ILARTS.exe) share.
//
// Each source the viewer offers as a webcam ("ILARTS PLAT" ...) is a COM class of ILARTSCam.dll (kCamClsid[i]). The
// viewer creates the cameras with MFCreateVirtualCamera (Windows 11) while it runs. When an application opens one,
// Frame Server loads the DLL; its media source creates the shared block "Global\ILARTSCam<i>" (a service may; a
// user's program usually may not - so the camera creates it and the viewer opens it), says which picture size the
// application chose and stamps every frame it delivers (readTick). The viewer draws that source at that size into
// the block while readTick is recent - so a camera nobody watches costs nothing.
#pragma once

#include <windows.h>

#include <cstdint>

namespace ilcam {

constexpr int kCams = 6;
// the sources, as the viewer names them ("a" monitor A, "b" monitor B, then fixed sources)
constexpr const wchar_t* kCamSource[kCams] = {L"a", L"b", L"plat", L"lso1", L"lso2", L"island"};
constexpr const wchar_t* kCamName[kCams] = {L"ILARTS Monitor A", L"ILARTS Monitor B", L"ILARTS PLAT",
                                            L"ILARTS LSO 1",     L"ILARTS LSO 2",     L"ILARTS Island"};
// {6C1A5E70-3B1D-4E8A-9C27-1F0D5A4B7E0x}, x = the camera
constexpr GUID kCamClsid[kCams] = {
    {0x6c1a5e70, 0x3b1d, 0x4e8a, {0x9c, 0x27, 0x1f, 0x0d, 0x5a, 0x4b, 0x7e, 0x00}},
    {0x6c1a5e70, 0x3b1d, 0x4e8a, {0x9c, 0x27, 0x1f, 0x0d, 0x5a, 0x4b, 0x7e, 0x01}},
    {0x6c1a5e70, 0x3b1d, 0x4e8a, {0x9c, 0x27, 0x1f, 0x0d, 0x5a, 0x4b, 0x7e, 0x02}},
    {0x6c1a5e70, 0x3b1d, 0x4e8a, {0x9c, 0x27, 0x1f, 0x0d, 0x5a, 0x4b, 0x7e, 0x03}},
    {0x6c1a5e70, 0x3b1d, 0x4e8a, {0x9c, 0x27, 0x1f, 0x0d, 0x5a, 0x4b, 0x7e, 0x04}},
    {0x6c1a5e70, 0x3b1d, 0x4e8a, {0x9c, 0x27, 0x1f, 0x0d, 0x5a, 0x4b, 0x7e, 0x05}},
};

constexpr uint32_t kMagic = 0x43564C49;  // "ILVC"
constexpr uint32_t kVersion = 1;
constexpr int kMaxW = 1920, kMaxH = 1080;

// The shared block: this header, then the picture (BGRA, top-down, w * 4 bytes a row).
struct Block {
    uint32_t magic, version;
    volatile LONG askW, askH;     // camera: the size the application chose
    volatile LONG64 readTick;     // camera: GetTickCount64() when it last delivered a frame
    volatile LONG seq;            // viewer: frames written; odd while one is being written
    volatile LONG frameW, frameH; // viewer: the size of the picture in the block
    volatile LONG64 writeTick;    // viewer: GetTickCount64() of the last frame
    uint8_t reserved[64];
};
constexpr size_t kBlockBytes = sizeof(Block) + static_cast<size_t>(kMaxW) * kMaxH * 4;

inline void BlockName(int cam, wchar_t* out, size_t n, bool global) {
    swprintf_s(out, n, L"%sILARTSCam%d", global ? L"Global\\" : L"Local\\", cam);
}

}  // namespace ilcam
