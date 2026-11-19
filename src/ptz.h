// Pan motion of a PTZ camera: fixed rate with a short ramp up and down, in moves.
//
// The camera stays where it is until the target is more than `deadband` off its aim. Then it pans toward the
// target: the rate ramps up (rate / ramp per second) to the fixed rate, holds it, and ramps down so that it
// stops where the target was when the ramp down began. It then waits until the target is off by more than
// the deadband again. A target that moves faster than the fixed rate is followed at the fixed rate, lagging;
// a slow one is followed in short moves. So the target is not always centred - as with a real camera on a
// pan head. Pure arithmetic (used by extra_cams.cpp and by the offline test).
//
// A target that will soon move faster than the fixed rate (a landing aircraft passing the island camera
// sweeps at up to ~120 deg/s) would leave the picture before such a move catches up. Given where the target
// is heading (PanAhead: its bearing predicted over the next second), a move starts as soon as one started any
// later would let the target get more than `outrun` off the aim, and keeps going at the fixed rate for as long
// as stopping would mean starting again straight away: the camera gets ahead of the aircraft before the
// sweep and the aircraft crosses the picture during it.
#pragma once

#include <algorithm>
#include <cmath>

namespace lsodiag {

struct PanState {
    double pan = 0;    // rad
    double rate = 0;   // rad/s, signed
    int phase = 0;     // 0 idle, 1 moving at / toward the fixed rate, 2 ramping down onto `goal`
    double goal = 0;   // rad: where the ramp down ends
};

inline double PanWrap(double a) {  // into (-pi, pi]
    const double kPi = 3.14159265358979323846;
    a = std::fmod(a + kPi, 2 * kPi);
    if (a <= 0) a += 2 * kPi;
    return a - kPi;
}

// Where the target is heading: its bearing (rad) at step, 2 step, ... n step seconds from now.
struct PanAhead {
    static constexpr int kMax = 40;
    double bearing[kMax] = {};
    int n = 0;
    double step = 0.05;   // s
    double outrun = 0;    // rad: how far off the target may get before a move must already be under way
};

// How far (rad, toward dir) the target gets past a camera that is at `from` now, stays there for `delay` s
// and then pans toward dir from rest to the fixed rate: the most over the prediction.
inline double PanOutrun(const PanAhead& a, double from, double delay, int dir, double rate, double acc) {
    double most = -1e9;
    for (int k = 0; k < a.n; ++k) {
        const double t = (k + 1) * a.step - delay;
        double c = 0;  // how far the camera has gone
        if (t > 0) {
            const double tr = rate / acc;  // to the fixed rate
            c = t < tr ? 0.5 * acc * t * t : 0.5 * acc * tr * tr + rate * (t - tr);
        }
        most = std::max(most, dir * PanWrap(a.bearing[k] - (from + dir * c)));
    }
    return most;
}

// One step of dt seconds toward `target` (rad). rate (rad/s) and ramp (s) as configured; deadband (rad): how
// far the target may be off before a move starts (0 = go there: used for the return to the rest view).
// only: +1 / -1 = moves start only toward larger / smaller pan angles (a target on the other side is left alone;
// a move under way ramps down), 0 = both ways. ahead: where the target is heading (nullptr = not known).
inline void PanStep(PanState& s, double target, double deadband, double rate, double ramp, double dt, int only = 0,
                    const PanAhead* ahead = nullptr) {
    if (!(dt > 0) || !(rate > 0)) return;
    const double acc = rate / std::max(ramp, 0.02);
    const double eps = 1e-4;  // rad (0.006 deg): there
    double e = PanWrap(target - s.pan);
    // a target about to outrun the camera: start or keep a move toward where it is heading, at the fixed rate
    bool push = false;
    int pushDir = only;
    if (ahead && ahead->n > 0 && s.phase != 2) {
        if (pushDir == 0) pushDir = PanWrap(ahead->bearing[ahead->n - 1] - target) >= 0 ? 1 : -1;
        if (s.rate * pushDir >= 0) {
            // were the camera to stop now: where it would stop, and when
            const double stop = s.rate * s.rate / (2 * acc), tStop = std::fabs(s.rate) / acc;
            push = PanOutrun(*ahead, s.pan + pushDir * stop, tStop, pushDir, rate, acc) > ahead->outrun;
        }
    }
    if (!push && only != 0 && e * only < 0) {  // the target is on the side the camera does not pan to
        if (s.phase == 0) return;
        if (s.phase == 1) {
            const double stop = s.rate * s.rate / (2 * acc);
            s.phase = 2;
            s.goal = s.pan + (s.rate > 0 ? stop : -stop);
        }
        e = 0;
    }
    if (s.phase == 0) {
        if (!push && std::fabs(e) <= std::max(deadband, eps)) return;
        s.phase = 1;
    }
    double want = 0;
    if (s.phase == 1) {
        const double dir = e >= 0 ? 1.0 : -1.0;
        const double stop = s.rate * s.rate / (2 * acc);  // angle needed to ramp down from the current rate
        if (push) {
            want = pushDir * rate;
        } else if (s.rate * dir < 0) {
            // the target is now on the other side: ramp down first, where the current motion ends
            s.phase = 2;
            s.goal = s.pan + (s.rate > 0 ? stop : -stop);
        } else if (std::fabs(e) <= stop + std::fabs(s.rate) * dt) {
            s.phase = 2;
            // where the target is now; a target nearer than the ramp down (a push that just ended): where the
            // ramp down ends, not a swing back
            s.goal = s.pan + (s.rate * dir > 0 && std::fabs(e) < stop ? dir * stop : e);
        } else {
            want = dir * rate;
        }
    }
    if (s.phase == 2) {
        const double eg = PanWrap(s.goal - s.pan);
        const double v = std::sqrt(2 * acc * std::fabs(eg));  // the rate from which a constant ramp down ends on the goal
        if (std::fabs(eg) <= eps || (v * dt >= std::fabs(eg) && std::fabs(s.rate) <= 3 * acc * dt)) {
            s.pan = s.goal;
            s.rate = 0;
            s.phase = 0;
            return;
        }
        want = (eg >= 0 ? 1.0 : -1.0) * std::min(rate, v);
    }
    // (ramping down, up to twice the ramp is allowed, so that frame time jitter cannot leave the rate above the
    // curve that ends on the goal)
    const double step = acc * dt * (s.phase == 2 ? 2.0 : 1.0);
    s.rate = want > s.rate ? std::min(want, s.rate + step) : std::max(want, s.rate - step);
    s.pan += s.rate * dt;
}

}  // namespace lsodiag
