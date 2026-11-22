// Rain and snow in ILARTS's pictures (PLAT, extra cameras).
//
// Cause (static analysis, docs/FINDINGS.md "Rain streaks in ILARTS's views"): DCS draws precipitation with
// Effects2.dll's `Effects::Rain` (one object for rain, one for snow, owned by Visualizer's
// `Effects::PrecipitationProvider`). Each drop is a point stretched into a streak by the geometry shader
// (Bazar/shaders/precipitation/rain.hlsl, snow.hlsl) along rainVel - cameraVel, both from the effect's constant
// buffer cEffectParams (Rain +0x130, 0xA0 bytes; cameraVel at +0x150). The particle cells are placed per view
// (the renderable's per-view call re-collects them around the view's camera), but cameraVel is not:
// `Rain::update` (+0x75A0) works it out from the camera position passed in and the one it stored the frame
// before, and only for a view whose collection marks it as the updating view (renderable +0x24, from
// CollectionInfo +0x690 == 0). Every other view - the PLAT and the extra cameras - draws with the value the
// updating view left behind: your own camera's motion. In flight that is your aircraft's speed, so the drops
// in the PLAT were long streaks pointing at the vanishing point of your flight path ("warp speed"), and
// their brightness against the sky alternated with your manoeuvres.
//
// Fix: slot 1 of the precipitation renderable's vtable (Visualizer +0x23BBD8, its per-view call +0xC9C60 with
// the view's SceneRenderContext) is swapped for a wrapper. DCS's own views go straight through. For one of
// ILARTS's views (the list night_sky.h keeps: context camera +0x308 within 5 cm of a listed view camera), per
// mode:
//   auto  - the view's own camera motion: velocity tracked per view from its camera positions (the move over
//           the last kRainWindowSec, at most kRainMaxSpeed); a deck camera or the PLAT moves with the ship, so
//           the drops fall as seen from a ship at its speed, plus the wind DCS already puts into rainVel;
//   still - a camera at rest (cameraVel 0): the drops fall with rainVel only;
//   hide  - no rain or snow drawn in ILARTS's views;
//   stock - DCS's draw unchanged (your own camera's motion).
// In auto and still the wrapper writes the velocity into the Rain object's cameraVel just before DCS's call
// (which uploads the constant buffer) and puts DCS's value back right after, so DCS's views and DCS's next
// update see exactly what they would have. A view DCS marked as its updating view is drawn exactly as DCS does
// (DCS computes that view's own motion); no copy of DCS's renderable is made. DCS's code and every other
// field of the Rain object are left alone; the draw itself is DCS's, on the render thread.
//
// Precipitation type (auto / still): DCS splits the precipitation into its rain and its snow object by the
// temperature at YOUR camera (Weather.dll: rain share = clamp((T + 1) / 3, 0, 1)) and every view draws both with that
// split, so the PLAT of a 20 C mission showed snowflakes - drifting dots - while you flew in the -1..+2 C band about
// 3 km up. In ILARTS's views the object of the wrong type for the view's own temperature (the carrier's sea-level
// temperature from the hook, less 6.5 C per km of camera height) is not drawn: snow at +2 C and warmer, rain at -1 C
// and colder; in between both, as DCS draws them. Without a temperature from the hook: DCS's split.
//
// With your camera in pure snow (DCS's rain share 0, flying high in a warm mission) DCS gives no view a rain
// renderable, so it neither draws nor simulates its rain object, and ILARTS's views would show nothing. Then, in auto /
// still, a view whose own temperature calls for rain draws DCS's rain object through its snow renderable: the
// renderable's Rain object, power and update mark set for that one call and put back right after. One of ILARTS's
// views carries the mark so that DCS runs the rain simulation for it. The rain object comes from the precipitation
// provider (its collectRenderables, vtable +0x23BB90, wrapped to note it every frame).
//
// Far views (auto / still / hide): the precipitation shaders read the rain density - and every shader its cloud
// shadows - from enlight's cloud volume at pos x gCloudVolumeScale + gCloudVolumeOffset (cPerView), with pos relative
// to the view's origin. DCS sets the offset at every pass bind from enlight's per-frame value, which is for the frame
// origin; ILARTS's views on the carrier's own origin (you more than 70 km away, plat_origin.h) therefore read the
// clouds (frame origin - own origin) away: over you, not over the carrier. GraphicsVista's and enlight's imports of
// ContextBase::updateBuffers (the only upload of cPerView) go to a wrapper: for a context on the carrier's own origin
// the offset in GraphicsCore's CPU copy is set to DCS's + (own origin - enlight's origin) x scale just before DCS's
// upload and put back right after. Every other upload, and 'stock', goes through untouched.
#pragma once

#include <cstdint>
#include <string>

namespace lsodiag {

enum class RainMode { Stock = 0, Auto = 1, Still = 2, Hide = 3 };

// Velocity tracking of ILARTS's views (auto). A view's camera positions are kept (one per 1/60 s at most, the
// last kRainWindowSec); its velocity is the move from the oldest kept position to the current one over that
// time, once that spans at least kRainMinSpanSec (before that: the velocity of the view tracked last, i.e. the
// ship's, or 0). A camera that moves more than kRainJumpMetres + kRainMaxSpeed x the time since its last draw
// has jumped (a camera moved to another spot): it is a new view. Speeds above kRainMaxSpeed are cut to it.
constexpr double kRainWindowSec = 1.0;
constexpr double kRainMinSpanSec = 0.05;
constexpr double kRainMaxSpeed = 60.0;    // m/s (a carrier makes about 15)
constexpr double kRainJumpMetres = 5.0;

std::string RainInstall();   // "" or why not
void RainRemove();
void RainSetMode(RainMode m);
RainMode RainGetMode();
// The carrier's air temperature at sea level (deg C, from the hook), for the precipitation type in ILARTS's views;
// known = false: not known (DCS's split, from your camera).
void RainSetCarrierTemperature(bool known, double celsius);
// A stats line (counters since the last returned line), or "" when nothing is due: nothing was drawn, or the
// last line was less than 60 s ago (the first 10 lines) / 300 s ago (after that). Each returned line also asks
// for one sample line per kind of view at its next draw (and one far view upload line).
std::string RainStats();

#ifdef RAIN_TEST
using RainDrawFn = void (*)(void* self, const void* ctx, void* a3, void* a4);
void RainTestSet(RainDrawFn draw, double (*clock)());
void* RainTestWrap();
void RainTestInstalled(bool on);
void RainTestResetStatsClock();   // the next RainStats() returns a line whenever something was counted
// The far views' cloud lookup with stand-ins: the upload it wraps, GraphicsCore's block stack (index, array) and
// enlight's cloud implementation pointer.
using RainUploadFn = void (*)(void* context, int slot);
void RainTestSetFar(RainUploadFn upload, const uint32_t* stackIndex, unsigned char* const* stackArray,
                    void* const* cloudImpl, bool installed);
void* RainTestUploadWrap();
bool RainTestFarInstalled();
// The carrier's rain while DCS draws none: the provider's collectRenderables it wraps (a stand-in), installed or not.
using RainCollectFn = uintptr_t (*)(void* self, uintptr_t count, void* a3, void* a4, void* a5);
void RainTestSetBorrow(RainCollectFn collect, bool installed);
void* RainTestCollectWrap();
bool RainTestBorrowInstalled();
#endif

}  // namespace lsodiag
