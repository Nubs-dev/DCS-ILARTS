// LSO station screens drawn by the viewer: "LSO 1" (DCS's LSOFINALTARGET, LSOStation.dlg) and "LSO 2"
// (DCS's LSOSCREEN2, the cue panel LSOStationScreen2.dlg).
//
// Supercarrier creates both (driven by PLATCameraUI.lua) only when the LSO view is opened. Without a
// station visit the viewer draws the same screens itself: widget bounds, fonts, colours, pictures and the
// position formulas below are taken from DCS's own files (Mods/tech/Supercarrier/PLATCameraUI/*.dlg and
// PLATCameraUI.lua, 2.9.31); the pictures (LSO-grids.dds BC7, LSOStationScreen2.tga, cuePanelAircraft*.tga)
// and the font (DejaVuLGCSansCondensed.ttf) are read from the DCS installation at run time, nothing of
// DCS's is copied.
//
// Output: premultiplied BGRA layers, 1024 x 768 (LSO 1) and 1152 x 768 (LSO 2). On LSO 1 the PLAT picture
// goes underneath the layer in the rectangle x 192..831, y 150..629 (PLATCamera widget), tinted like DCS's
// station composite.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace station {

constexpr int kW = 1024, kH = 768;          // LSO 1
constexpr int kCueW = 1152, kCueH = 768;    // LSO 2
constexpr uint32_t kInk = 0xC3FF00;         // 0xc3ff00ff: the screens' yellow-green

// LSO 1 values, already formatted as PLATCameraUI.lua prints them.
struct Values {
    char heading[16] = "";        // MAGNETIC_HEADING "SH %03d DEG"
    char list[24] = "";           // LIST_VALUE
    char trim[24] = "";           // TRIM_VALUE
    char deckWind[4] = "";        // DECK_WIND "%02d"
    char cross[8] = "";           // CROSSWIND_* text ("%2d P" / "%2d S")
    int crossSide = 0;            // 1 port, -1 starboard, 0 hidden
    int foul = 0;                 // setFoulDeck: 0 CLEAR, > 0 FOUL
    char acType[24] = "F/A-18C/D";
    char acHE[24] = "H/E = 16.35 ft";
    char acBA[24] = "B/A = 3.50 deg";
    float htrFt = 14.07f, htdFt = 230.0f;               // setHook(70.11 m, 4.288 m): Supercarrier's value before any trap
    float htrDesiredFt = 14.1f, htdDesiredFt = 230.0f;  // onLSOStationCreate
    int rope = 3;                                        // setDesiredRope(3)
    char case3[3][8] = {};                               // CASE_III_1..3 (board numbers)
};

// LSO 2: one row of setCuePanelInfo(idx, status, unit_type, boardnumber, pilot, x, z, yaw, fuelstate, col3, col5..col8).
struct CueRow {
    int status = -1;          // > 0: case III (aircraft drawn on the approach picture)
    char type[24] = "";       // DCS type ("FA-18C_hornet")
    char cols[8][24] = {};    // grid columns 1..8 (1 board, 2 pilot, 3, 4 fuel "%0.1f/00", 5..8)
    char board[8] = "";
    float x = 0, z = 0, yaw = 0;   // m from the ramp along / across the angled deck axis, rad
    float fuel = -1;               // fuelstate (lb); < 0 = unknown
};
struct CueValues {
    int count = 0;
    CueRow rows[16];
};

class Renderer {
public:
    ~Renderer() {
        for (HFONT& f : fonts_)
            if (f) DeleteObject(f);
        if (dc_) {
            if (oldBmp_) SelectObject(dc_, oldBmp_);
            DeleteDC(dc_);
        }
        if (bmp_) DeleteObject(bmp_);
    }

    // dcsRoot: the DCS installation folder. dev / ctx: any D3D11 device, used once to decode the atlas.
    // Returns "" or what failed (the screens are still drawn, without the missing parts).
    std::string init(const std::wstring& dcsRoot, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
        std::string why;
        const std::wstring font = dcsRoot + L"\\dxgui\\skins\\fonts\\DejaVuLGCSansCondensed.ttf";
        const wchar_t* face = AddFontResourceExW(font.c_str(), FR_PRIVATE, nullptr) > 0 ? L"DejaVu LGC Sans Condensed" : L"Arial Narrow";
        if (face[0] == L'A') why += "font not found; ";
        for (int i = 0; i < kFonts; ++i)
            fonts_[i] = CreateFontW(-kFontSize[i], 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = kSW;
        bi.bmiHeader.biHeight = -kSH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dc_ = CreateCompatibleDC(nullptr);
        void* bits = nullptr;
        bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc_ || !bmp_ || !bits) return why + "GDI surface failed";
        scratch_ = static_cast<uint32_t*>(bits);
        oldBmp_ = SelectObject(dc_, bmp_);
        SetBkMode(dc_, TRANSPARENT);
        SetTextColor(dc_, RGB(255, 255, 255));
        st_.px.assign(static_cast<size_t>(kW) * kH, 0);
        st_.w = kW, st_.h = kH;
        cue_.px.assign(static_cast<size_t>(kCueW) * kCueH, 0);
        cue_.w = kCueW, cue_.h = kCueH;
        const std::wstring dir = dcsRoot + L"\\Mods\\tech\\Supercarrier\\PLATCameraUI\\";
        const std::string a = LoadAtlas(dir + L"LSO-grids.dds", dev, ctx);
        if (!a.empty()) why += "atlas: " + a + "; ";
        if (!LoadTga(dir + L"LSOStationScreen2.tga", cueBkg_)) why += "LSOStationScreen2.tga; ";
        static const wchar_t* icons[4] = {L"cuePanelAircraft.tga", L"cuePanelAircraft_F-14.tga", L"cuePanelAircraft_S-3.tga",
                                          L"cuePanelAircraft_E-2.tga"};
        for (int i = 0; i < 4; ++i)
            if (!LoadTga(dir + icons[i], icon_[i])) why += "icon " + std::to_string(i) + "; ";
        return why;
    }

    const uint32_t* pixels() const { return st_.px.data(); }
    const uint32_t* cuePixels() const { return cue_.px.data(); }

    // ---- LSO 1 (LSOStation.dlg)
    void compose(const Values& v) {
        Canvas& c = st_;
        std::fill(c.px.begin(), c.px.end(), 0u);
        // pictures (atlas rects from LSOStation.dlg), tinted with the skin colour
        Sprite(c, 179, 197, 780, 512, 0, 0, 780, 512);                          // STATIC_GRID
        Sprite(c, 51, 65, 64, 128, 900, 370, 964, 498);                         // DECK_PICT
        // hook markers: PLATCameraUI.lua setHookToRampMarkerPos / setHookTouchdownMarkerPos
        if (v.htrFt <= 45 && v.htrFt >= -10) Sprite(c, 936, HtrY(v.htrFt, 22), 19, 22, 792, 333, 811, 355);           // HTR_MARKER
        if (v.htdFt <= 350 && v.htdFt >= 0) Sprite(c, HtdX(v.htdFt, 22), 644, 22, 19, 791, 445, 813, 464);             // HTD_MARKER
        if (v.htrDesiredFt <= 45 && v.htrDesiredFt >= -10) Fill(c, 869, HtrY(v.htrDesiredFt, 8), 24, 8, kInk);         // HTR_DESIRED
        if (v.htdDesiredFt <= 350 && v.htdDesiredFt >= 0) Fill(c, HtdX(v.htdDesiredFt, 8), 669, 8, 20, kInk);          // HTD_DESIRED
        static const float ropes[4] = {170, 210, 250, 290};
        if (v.rope >= 1 && v.rope <= 4) Sprite(c, HtdX(ropes[v.rope - 1], 14), 653, 14, 14, 795, 482, 809, 497);        // ROPE_DESIRED
        // case III stack (CASE_III_12/22/32: 2 px frame; CASE_III_1..3: board numbers, offset 4)
        const int cy[3] = {11, 47, 83};
        for (int i = 0; i < 3; ++i) {
            Frame(c, 832, cy[i], 155, 38, 2, kInk);
            if (v.case3[i][0]) Text(c, v.case3[i], 832, cy[i], 155, 38, kF24, kInk, Align::Left, 4);
        }
        // text widgets
        Text(c, "KTS CLSG", 55, 211, 154, 22, kF22, kInk);
        Text(c, "NM", 54, 240, 154, 22, kF22, kInk);
        Text(c, "LIST (DEG)", 9, 334, 183, 22, kF22, kInk);
        Text(c, v.list, 9, 361, 183, 22, kF22, kInk);
        Text(c, "TRIM (DEG)", 9, 404, 183, 22, kF22, kInk);
        Text(c, v.trim, 9, 431, 183, 22, kF22, kInk);
        Text(c, v.acType, 5, 522, 183, 22, kF22, kInk);
        Text(c, v.acHE, 5, 550, 183, 22, kF22, kInk);
        Text(c, v.acBA, 5, 578, 183, 22, kF22, kInk);
        Text(c, v.heading, 397, 82, 251, 40, kF45, kInk);
        Text(c, v.deckWind, 77, 121, 24, 22, kF22, kInk);
        if (v.crossSide > 0) Text(c, v.cross, 27, 90, 45, 22, kF22, kInk, Align::Right);
        if (v.crossSide < 0) Text(c, v.cross, 117, 90, 45, 22, kF22, kInk);
        Text(c, "HOOK TOUCHDOWN", 424, 720, 211, 22, kF22, kInk);
        TextWrapped(c, "H O O K    T O    R A M P", 963, 294, 16, 383, kF24, kInk);
        // SIG_FOUL is moved onto SIG_CLEAR's place (onLSOStationCreate)
        if (v.foul > 0) {
            Fill(c, 386, 24, 133, 40, 0xFF4D00);
            Text(c, "FOUL ", 386, 24, 133, 40, kF45, 0xFFFFFF, Align::Center);
        } else {
            Fill(c, 386, 24, 133, 40, kInk);
            Text(c, "CLEAR", 386, 24, 133, 40, kF45, 0x000000);
        }
    }

    // ---- LSO 2 (LSOStationScreen2.dlg + setCuePanelInfo)
    void composeCue(const CueValues& v) {
        Canvas& c = cue_;
        std::fill(c.px.begin(), c.px.end(), 0u);
        Picture(c, cueBkg_, 0, 0, kCueW, kCueH);  // window background, tinted
        static const int columns[9] = {14, 71, 203, 265, 335, 381, 443, 466, 793};
        for (int r = 0; r < v.count && r < 16; ++r) {
            const CueRow& row = v.rows[r];
            for (int k = 0; k < 8; ++k)
                if (row.cols[k][0]) Text(c, row.cols[k], columns[k], 74 + 27 * r, columns[k + 1] - columns[k], 25, kF20, kInk);
            if (row.status <= 0) continue;
            // cue_panel_aircraft_set: 66 px per 2 nm around (1097, 341); x up, z right
            const float pixY = 341 - 66 * row.x / (1852 * 2), pixX = 1097 + 66 * row.z / (1852 * 2);
            if (pixY < 22 || pixY > 748 || pixX < 812) continue;
            const int px = static_cast<int>(std::lround(pixX)) - 70, py = static_cast<int>(std::lround(pixY)) - 40;   // panel
            const Image& ic = icon_[IconFor(row.type)];
            Rotated(c, ic, px + 46, py, 32, 40, row.yaw);   // aircraft (64 x 64, pivot 32,40), setAngle(deg(yaw))
            Text(c, row.board, px + 93, py + 13, 55, 25, kF18, kInk);
            if (row.fuel >= 0) {
                char f[16];
                _snprintf_s(f, _countof(f), _TRUNCATE, "%0.1f", row.fuel * 0.001);
                Text(c, f, px + 93, py + 35, 55, 25, kF18, kInk);
            }
        }
    }

private:
    enum class Align { Left, Right, Center };
    struct Canvas {
        std::vector<uint32_t> px;  // premultiplied BGRA
        int w = 0, h = 0;
    };
    struct Image {
        std::vector<uint32_t> px;  // straight BGRA (A in the top byte)
        int w = 0, h = 0;
    };
    static constexpr int kFonts = 7;
    static constexpr int kFontSize[kFonts] = {16, 18, 20, 22, 24, 45, 45};
    enum { kF16, kF18, kF20, kF22, kF24, kF45 };
    static constexpr int kSW = kCueW, kSH = kCueH;  // scratch surface covers both screens
    HFONT fonts_[kFonts] = {};
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    uint32_t* scratch_ = nullptr;
    Canvas st_, cue_;
    std::vector<uint32_t> atlas_;  // RGBA8 (R in the low byte)
    int aw_ = 0, ah_ = 0;
    Image cueBkg_, icon_[4];

    static int HtrY(float feet, int h) {
        const float y = 464 + feet * (104 - 464) / 30.0f;
        return static_cast<int>(std::lround(y - 0.5f * h + 197));
    }
    static int HtdX(float feet, int w) {
        const float x = 728 + feet * (14 - 728) / 350.0f;
        return static_cast<int>(std::lround(x - 0.5f * w + 179));
    }
    // PLATCameraUI.lua findFace
    static int IconFor(const char* t) {
        if (!strncmp(t, "F-14", 4)) return 1;
        if (!strncmp(t, "S-3B", 4)) return 2;
        if (!strncmp(t, "E-2", 3)) return 3;
        return 0;
    }

    // premultiplied "over" of colour rgb (0xRRGGBB) with alpha a (0..1)
    static void Over(Canvas& c, int x, int y, uint32_t rgb, float a) {
        if (x < 0 || y < 0 || x >= c.w || y >= c.h || a <= 0) return;
        if (a > 1) a = 1;
        uint32_t& d = c.px[static_cast<size_t>(y) * c.w + x];
        const float r = ((rgb >> 16) & 255) * a, g = ((rgb >> 8) & 255) * a, b = (rgb & 255) * a, k = 1 - a;
        const float db = (d & 255) * k + b, dg = ((d >> 8) & 255) * k + g, dr = ((d >> 16) & 255) * k + r,
                    da = ((d >> 24) & 255) * k + 255 * a;
        d = static_cast<uint32_t>(db + 0.5f) | static_cast<uint32_t>(dg + 0.5f) << 8 | static_cast<uint32_t>(dr + 0.5f) << 16 |
            static_cast<uint32_t>(da + 0.5f) << 24;
    }
    // a straight-alpha BGRA texel multiplied by the skin colour
    static void OverTexel(Canvas& c, int x, int y, uint32_t t) {
        const float a = ((t >> 24) & 255) / 255.0f;
        const uint32_t r = (((t >> 16) & 255) * ((kInk >> 16) & 255)) / 255, g = (((t >> 8) & 255) * ((kInk >> 8) & 255)) / 255,
                       b = ((t & 255) * (kInk & 255)) / 255;
        Over(c, x, y, r << 16 | g << 8 | b, a);
    }

    static void Fill(Canvas& c, int x, int y, int w, int h, uint32_t rgb) {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i) Over(c, i, j, rgb, 1);
    }

    static void Frame(Canvas& c, int x, int y, int w, int h, int t, uint32_t rgb) {
        Fill(c, x, y, w, t, rgb);
        Fill(c, x, y + h - t, w, t, rgb);
        Fill(c, x, y + t, t, h - 2 * t, rgb);
        Fill(c, x + w - t, y + t, t, h - 2 * t, rgb);
    }

    // atlas rect (x1,y1)-(x2,y2) stretched to the widget (x,y,w,h), multiplied by the skin colour
    void Sprite(Canvas& c, int x, int y, int w, int h, int x1, int y1, int x2, int y2) {
        if (atlas_.empty()) return;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                const int ax = x1 + (i * (x2 - x1)) / w, ay = y1 + (j * (y2 - y1)) / h;
                if (ax < 0 || ay < 0 || ax >= aw_ || ay >= ah_) continue;
                const uint32_t t = atlas_[static_cast<size_t>(ay) * aw_ + ax];   // RGBA, R low
                OverTexel(c, x + i, y + j, (t & 0xFF00FF00u) | (t & 255) << 16 | ((t >> 16) & 255));
            }
    }

    // whole image stretched to (x,y,w,h)
    static void Picture(Canvas& c, const Image& im, int x, int y, int w, int h) {
        if (im.px.empty()) return;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                OverTexel(c, x + i, y + j, im.px[static_cast<size_t>(j * im.h / h) * im.w + (i * im.w / w)]);
    }

    // image at (x,y) turned by `angle` (rad, clockwise on screen) about its pixel (pivX,pivY), bilinear
    static void Rotated(Canvas& c, const Image& im, int x, int y, float pivX, float pivY, float angle) {
        if (im.px.empty()) return;
        const float ca = std::cos(angle), sa = std::sin(angle);
        const float cx = x + pivX, cy = y + pivY;
        const int r = static_cast<int>(std::ceil(std::hypot(static_cast<float>(im.w), static_cast<float>(im.h))));
        for (int j = static_cast<int>(cy) - r; j <= static_cast<int>(cy) + r; ++j)
            for (int i = static_cast<int>(cx) - r; i <= static_cast<int>(cx) + r; ++i) {
                const float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
                const float u = ca * dx + sa * dy + pivX - 0.5f, v = -sa * dx + ca * dy + pivY - 0.5f;   // inverse rotation
                if (u < -1 || v < -1 || u > im.w || v > im.h) continue;
                const int u0 = static_cast<int>(std::floor(u)), v0 = static_cast<int>(std::floor(v));
                const float fu = u - u0, fv = v - v0;
                float acc[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    const int uu = u0 + (k & 1), vv = v0 + (k >> 1);
                    if (uu < 0 || vv < 0 || uu >= im.w || vv >= im.h) continue;
                    const float wgt = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
                    const uint32_t t = im.px[static_cast<size_t>(vv) * im.w + uu];
                    const float a = ((t >> 24) & 255) / 255.0f;
                    acc[0] += wgt * a * (t & 255), acc[1] += wgt * a * ((t >> 8) & 255), acc[2] += wgt * a * ((t >> 16) & 255);
                    acc[3] += wgt * a;
                }
                if (acc[3] <= 0.001f) continue;
                const uint32_t t = static_cast<uint32_t>(std::min(255.0f, acc[0] / acc[3])) |
                                   static_cast<uint32_t>(std::min(255.0f, acc[1] / acc[3])) << 8 |
                                   static_cast<uint32_t>(std::min(255.0f, acc[2] / acc[3])) << 16 |
                                   static_cast<uint32_t>(std::min(255.0f, acc[3] * 255)) << 24;
                OverTexel(c, i, j, t);
            }
    }

    // GDI text into the scratch surface (white on black), then blended as coverage with the colour,
    // clipped to the widget (dxgui Static: horzAlign default left + offset, vertAlign middle).
    void Text(Canvas& c, const char* s, int x, int y, int w, int h, int font, uint32_t rgb, Align al = Align::Left, int offset = 0) {
        if (!s || !*s || !scratch_) return;
        RECT box{x, y, x + w, y + h};
        Clear(box);
        HGDIOBJ old = SelectObject(dc_, fonts_[font]);
        RECT r{x + offset, y, x + w, y + h};
        const UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                         (al == Align::Right ? DT_RIGHT : (al == Align::Center ? DT_CENTER : DT_LEFT));
        DrawTextA(dc_, s, -1, &r, fmt);
        SelectObject(dc_, old);
        Blend(c, box, rgb);
    }

    // narrow box with word wrapping (HOOK_SIGNAL2: one letter per line), vertically centred
    void TextWrapped(Canvas& c, const char* s, int x, int y, int w, int h, int font, uint32_t rgb) {
        if (!scratch_) return;
        RECT box{x, y, x + w, y + h};
        Clear(box);
        HGDIOBJ old = SelectObject(dc_, fonts_[font]);
        RECT calc{0, 0, w, 0};
        DrawTextA(dc_, s, -1, &calc, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX | DT_CALCRECT);
        const int th = calc.bottom - calc.top;
        RECT r{x, y + (h - th) / 2, x + w, y + (h - th) / 2 + th};
        DrawTextA(dc_, s, -1, &r, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
        SelectObject(dc_, old);
        Blend(c, box, rgb);
    }

    void Clear(const RECT& r) {
        for (int j = std::max(0L, r.top); j < std::min<long>(kSH, r.bottom); ++j)
            for (int i = std::max(0L, r.left); i < std::min<long>(kSW, r.right); ++i) scratch_[static_cast<size_t>(j) * kSW + i] = 0;
    }

    void Blend(Canvas& c, const RECT& r, uint32_t rgb) {
        GdiFlush();
        for (int j = std::max(0L, r.top); j < std::min<long>(kSH, r.bottom); ++j)
            for (int i = std::max(0L, r.left); i < std::min<long>(kSW, r.right); ++i) {
                const uint32_t v = scratch_[static_cast<size_t>(j) * kSW + i];
                const float cov = std::max({(v >> 16) & 255u, (v >> 8) & 255u, v & 255u}) / 255.0f;
                Over(c, i, j, rgb, cov);
            }
    }

    // 32-bit TGA, uncompressed (2) or RLE (10); stored bottom-up unless descriptor bit 5.
    static bool LoadTga(const std::wstring& path, Image& out) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
        std::vector<unsigned char> b;
        fseek(f, 0, SEEK_END);
        b.resize(static_cast<size_t>(ftell(f)));
        fseek(f, 0, SEEK_SET);
        const size_t got = fread(b.data(), 1, b.size(), f);
        fclose(f);
        if (got != b.size() || b.size() < 18) return false;
        const int idLen = b[0], type = b[2], w = b[12] | b[13] << 8, h = b[14] | b[15] << 8, bpp = b[16], desc = b[17];
        if ((type != 2 && type != 10) || bpp != 32 || w <= 0 || h <= 0) return false;
        const size_t n = static_cast<size_t>(w) * h;
        std::vector<uint32_t> px(n);
        size_t o = 18 + idLen, i = 0;
        auto rd = [&](uint32_t& v) {
            if (o + 4 > b.size()) return false;
            memcpy(&v, &b[o], 4);
            o += 4;
            return true;
        };
        while (i < n) {
            if (type == 2) {
                if (!rd(px[i++])) return false;
                continue;
            }
            if (o >= b.size()) return false;
            const int head = b[o++], cnt = (head & 0x7F) + 1;
            if (head & 0x80) {
                uint32_t v;
                if (!rd(v)) return false;
                for (int k = 0; k < cnt && i < n; ++k) px[i++] = v;
            } else {
                for (int k = 0; k < cnt && i < n; ++k)
                    if (!rd(px[i++])) return false;
            }
        }
        out.w = w, out.h = h;
        out.px.resize(n);
        const bool topDown = (desc & 0x20) != 0;
        for (int y = 0; y < h; ++y) memcpy(&out.px[static_cast<size_t>(y) * w], &px[static_cast<size_t>(topDown ? y : h - 1 - y) * w], w * 4);
        return true;
    }

    // LSO-grids.dds is BC7 (DXGI 98, 1024 x 512, one mip): decoded by the GPU once and read back.
    std::string LoadAtlas(const std::wstring& path, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
        if (!dev || !ctx) return "no device";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return "file not found";
        std::vector<unsigned char> b;
        fseek(f, 0, SEEK_END);
        b.resize(static_cast<size_t>(ftell(f)));
        fseek(f, 0, SEEK_SET);
        const size_t got = fread(b.data(), 1, b.size(), f);
        fclose(f);
        if (got != b.size() || b.size() < 148 || memcmp(b.data(), "DDS ", 4) != 0) return "not a DDS file";
        uint32_t h = 0, w = 0, fourcc = 0, fmt = 0;
        memcpy(&h, &b[12], 4);
        memcpy(&w, &b[16], 4);
        memcpy(&fourcc, &b[84], 4);
        memcpy(&fmt, &b[128], 4);
        if (fourcc != 0x30315844u /* 'DX10' */ || fmt != DXGI_FORMAT_BC7_UNORM || w % 4 || h % 4) return "unexpected DDS format";
        const size_t pitch = static_cast<size_t>(w / 4) * 16, size = pitch * (h / 4);
        if (b.size() < 148 + size) return "DDS too short";
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_BC7_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{&b[148], static_cast<UINT>(pitch), 0};
        ID3D11Texture2D* src = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Texture2D* rt = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11Texture2D* staging = nullptr;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        std::string why;
        auto release = [&]() {
            if (src) src->Release();
            if (srv) srv->Release();
            if (rt) rt->Release();
            if (rtv) rtv->Release();
            if (staging) staging->Release();
            if (vs) vs->Release();
            if (ps) ps->Release();
        };
        if (FAILED(dev->CreateTexture2D(&td, &sd, &src)) || FAILED(dev->CreateShaderResourceView(src, nullptr, &srv))) {
            release();
            return "BC7 texture not supported";
        }
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt)) || FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
            release();
            return "render target failed";
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) {
            release();
            return "staging failed";
        }
        static const char kCopy[] =
            "Texture2D t : register(t0);\n"
            "float4 vs(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2);"
            " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
            "float4 ps(float4 p : SV_Position) : SV_Target { return t.Load(int3(p.xy, 0)); }\n";
        ID3DBlob* vb = nullptr;
        ID3DBlob* pb = nullptr;
        if (FAILED(D3DCompile(kCopy, sizeof(kCopy) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, nullptr)) ||
            FAILED(D3DCompile(kCopy, sizeof(kCopy) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, nullptr)) ||
            FAILED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs)) ||
            FAILED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps)))
            why = "shader failed";
        if (vb) vb->Release();
        if (pb) pb->Release();
        if (why.empty()) {
            D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(nullptr);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(0, 1, &none);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->CopyResource(staging, rt);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                atlas_.resize(static_cast<size_t>(w) * h);
                for (uint32_t j = 0; j < h; ++j)
                    memcpy(&atlas_[static_cast<size_t>(j) * w], static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(j) * m.RowPitch, w * 4);
                ctx->Unmap(staging, 0);
                aw_ = static_cast<int>(w);
                ah_ = static_cast<int>(h);
            } else {
                why = "readback failed";
            }
        }
        release();
        return why;
    }
};

}  // namespace station
