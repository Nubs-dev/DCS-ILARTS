// Stars in ILARTS's pictures (PLAT, extra cameras).
//
// Cause (static analysis, docs/FINDINGS.md "Night speckles: stars beyond the far plane"): SpaceRenderer.dll
// draws the stars as 1-5 px point sprites (Bazar/shaders/Star.fx) on a sphere around the camera. Its scene
// renderable (vtable SpaceRenderer+0xC4F8, slot 1 = +0x32A0, called per view with the view's
// SceneRenderContext) puts that sphere at half the view's far plane when the provider has its own
// SpaceRendererInstance, and otherwise (the global instance, +0x6560) passes radius -1, which leaves the
// stars where the catalogue put them: 100 km (Space.dll unit vectors x 100000, +0x78ED). The star geometry
// shader drops every star whose depth is outside [0, 1], i.e. beyond the far plane. DCS's main view has a
// 150 km far plane; ILARTS's views (the PLAT's view setup) 80 km. A star at 100 km is then beyond the far
// plane while it is within acos(80/100) = 36.9 deg of the view axis (its view depth is 100 km x cos(angle)),
// and drawn further out: in a zoomed-out camera a ring of stars toward the corners around an empty centre,
// turned into short dashes by the viewer's CCTV look. DCS's own cockpit TV pictures behave the same way.
//
// Fix: the renderable's per-view call is wrapped (vtable slot swap); while it runs for one of ILARTS's views,
// the two calls of StarMaterial::draw in SpaceRendererInstance::draw (+0x6931 stars, +0x69A5 the second star
// layer) go to a filter that, per mode:
//   hide  - skips them (no stars, no milky way: as a CCTV deck camera shows the night sky);
//   full  - draws them with the sphere scaled to half the view's far plane, as DCS does for a view with its
//           own instance (a full star field, no ring);
//   stock - draws them unchanged (the ring);
//   auto  - as full, with a limiting magnitude tied to the zoom (a longer focal length shows fainter stars): no
//           star at kStarsStartFovDeg or wider, then the brightest first, fading in, more and fainter ones as the
//           field narrows, the whole catalogue at kStarsFullFovDeg or narrower. A wide-angle CCTV camera shows
//           no stars (DCS draws every star at 1-5 px whatever the field of view: a wide picture would be dense
//           speckle). The PLAT never shows stars in auto, as DCS's own PLAT.
// In full and auto the stars are also drawn as square points on screen: DCS sizes them in render target
// pixels, and the extra cameras render a 4:3 picture into a square target that is shown at 4:3, which made
// every star 1.33x wider than tall (a dash after the CCTV look's horizontal softening).
//
// How (docs/FINDINGS.md "Stars fade in with the zoom"): StarMaterial::draw draws (end - begin) / 20 vertices
// of its vertex list, which is DCS's star catalogue in order, brightest first (Space.dll's table, checked at
// run time), so a material copy with a shorter list is a limiting magnitude. Star.fx divides each star's alpha
// by uScale = pow(1 / StarObject+0x5C, 0.8) (+0x5C = the view's P11), so a copy with a larger +0x5C is a gain.
// pixelSize (the sprite size, 1 / render target size) goes to the effect through vtable +0xA8; the material
// copy points at a forwarding proxy of the effect that scales it. DCS's material, star object and effect are
// never written.
//
// DCS's own views are never changed. ILARTS's views are the picture source views plat_origin.cpp lists (the
// PLAT camera and the extra cameras), matched by the context's camera position (+0x308).
#pragma once

#include <string>

namespace lsodiag {

enum class NightSkyMode { Stock = 0, Hide = 1, Full = 2, Auto = 3 };
// auto: no star in a view whose horizontal field of view is at least kStarsStartFovDeg; the limiting magnitude
// rises from kStarsStartMag there to kStarsFullMag (the whole catalogue) at kStarsFullFovDeg, linear in
// log(field of view).
constexpr double kStarsStartFovDeg = 24.0;
constexpr double kStarsFullFovDeg = 6.0;
constexpr double kStarsStartMag = -1.5;   // Sirius is -1.44
constexpr double kStarsFullMag = 8.0;     // the catalogue's faintest
// A star that has just come in is drawn as bright as DCS draws a star of about magnitude 4.8 (Star.fx
// brightness); the gain rises from there to DCS's own (1) as the limit passes that magnitude.
constexpr double kStarsAppearBrightness = 0.003;
// The stars brighter than the limit fade in over the first kStarsRampMag magnitudes of the limit.
constexpr double kStarsRampMag = 1.5;

struct NightSkyLook {
    double magLimit = -100;   // stars of this magnitude or brighter are drawn
    double gain = 0;          // x DCS's brightness (0 = nothing drawn)
};
// Horizontal field of view (deg) from the projection's x scale (0 = unknown).
double NightSkyHfovDeg(double p00);
// What auto draws in a view with this horizontal field of view (deg; 0 or less = unknown: nothing).
NightSkyLook NightSkyAutoLook(double hfovDeg);
// Star.fx's normalized magnitude (the vertex value DCS builds, SpaceRenderer +0x789F): 1 - (mag + 4.2) / 12.2,
// clamped to 0..1; and the brightness Star.fx gives it (before twinkle, sky and DCS's zoom gain).
double NightSkyNormMag(double mag);
double NightSkyStarBrightness(double mag);

std::string NightSkyInstall();   // "" or why not
void NightSkyRemove();
void NightSkySetMode(NightSkyMode m);
NightSkyMode NightSkyGetMode();
// One line: counters since the last call, and the star geometry seen in ILARTS's views and in DCS's own.
std::string NightSkyStats();

// From plat_origin.cpp's view setup hook (render graph build): one of ILARTS's views and its camera; plat =
// the PLAT's view (no stars in auto).
void NightSkyNoteView(const double cam[3], bool plat);
// From plat_origin.cpp's buildRenderGraph wrapper, after DCS's build and before NightSkyBuildEnd: the origin the
// views at the carrier got in this build when they had their own (the far view fix, you more than 70 km away).
void NightSkyNoteOrigin(const float origin[3]);
// From plat_origin.cpp's buildRenderGraph wrapper, after DCS's build.
void NightSkyBuildEnd();
// For per-upload filters (rain.h, the far views' cloud lookup): true when origin (a render context's float3
// origin) is the own origin the views at the carrier had in one of the last 4 builds.
bool NightSkyCarrierOrigin(const float origin[3]);
// The same list for other per-view filters (rain.h): 1 when cam is within 5 cm of one of ILARTS's views of the last
// 4 builds (*plat: the PLAT's), else 0 (*nearMiss: within 50 m of one).
int NightSkyOurView(const double cam[3], bool* plat, bool* nearMiss);

// The geometry behind the ring, for the diagnostic line and the offline test. farM = the view's far plane
// (m), starRadius = where the stars are (m), p00 / p11 = the projection's x / y scale. Returns the angle off
// the view axis (deg) inside which the stars are beyond the far plane (0 = none), and sets *cornerDeg to the
// angle of the picture's corner.
double NightSkyRingDeg(double farM, double starRadius, double p00, double p11, double* cornerDeg);

// Slots of the stand-in effect vtable (night_sky.cpp); must exceed every effect slot StarMaterial::draw calls.
constexpr int kStarEffectSlots = 128;

#ifdef NIGHT_SKY_TEST
using NightSkyRenderFn = void (*)(void* self, const void* ctx, void* a3, void* a4);
using NightSkyStarDrawFn = void (*)(void* material, const void* starObject);
void NightSkyTestSet(NightSkyRenderFn render, NightSkyStarDrawFn draw);
void* NightSkyTestRenderWrap();
void* NightSkyTestStarWrap();
void NightSkyTestInstalled(bool on);
bool NightSkyTestProxyCode();   // builds the effect proxy code (NightSkyInstall does it in DCS)
#endif

}  // namespace lsodiag
