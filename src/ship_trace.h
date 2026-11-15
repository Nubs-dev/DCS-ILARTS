// Trace only: what pose DCS draws ships with. Transport.dll (unprotected, 2.9.31) woShip::Parse(parser,
// const wPosition3<float>& pos, effectState*) is the ship's draw call; pos is the pose for that draw.
// A detour on its first 15 bytes records the pose of the ship nearest to a reference point (the PLAT
// camera) and passes the call on unchanged.
#pragma once

#include <string>

namespace lsodiag {

std::string ShipTraceInstall();  // "" on success, otherwise why it refused
void ShipTraceRemove();
void ShipTraceSetRef(double x, double z);
// Draws since the previous call: "n=<calls> ships=<objects> t=<time of the pose> t0=<ship state time>
// rate=<yaw rate> diff=<largest difference between the calls' poses> | 12 floats: x, y, z axes, position"
std::string ShipTraceLast();
// Second tap: edObjects viObjectShape::getObjectPosition (the matrix the scene draws a shape with), for
// the object at the ship's own position. "d=<calls> dq=<qpc of the first> dpt=<time of the latest
// woShip::Position call before it> ddiff=<largest difference between the calls> | axes, position".
std::string ShipDrawInstall();
// Third tap: Visualizer +0x125B50, the object matrix handed to the scene. Its calls for the hull are added
// to the same per-call list, with the time argument and whether a parent matrix was passed.
std::string ShipMatInstall();
std::string ShipDrawLast();

}  // namespace lsodiag
