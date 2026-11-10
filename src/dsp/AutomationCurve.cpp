#include "AutomationCurve.h"

namespace chost::dsp {

const char* autoShapeName(int shape) {
    static const char* names[NumAutoShapes] = {
        "Single curve", "Double curve", "Hold", "Stairs", "Pulse", "Wave"
    };
    if (shape < 0 || shape >= NumAutoShapes) return names[0];
    return names[shape];
}

static double bend(double x, float tension) {
    if (tension == 0.0f) return x;
    const double p = std::pow(2.0, 4.0 * std::fabs(static_cast<double>(tension)));
    return tension > 0.0f ? std::pow(x, p) : 1.0 - std::pow(1.0 - x, p);
}

static int countFor(float tension) {
    return 2 + static_cast<int>(std::lround((static_cast<double>(tension) + 1.0) * 7.0));
}

float autoSegment(float a, float b, double x, int shape, float tension) {
    x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    switch (shape) {
        case ShapeDouble: {
            const double y = x < 0.5 ? 0.5 * bend(2.0 * x, tension) : 1.0 - 0.5 * bend(2.0 * (1.0 - x), tension);
            return a + (b - a) * static_cast<float>(y);
        }
        case ShapeHold:
            return a;
        case ShapeStairs: {
            const int n = countFor(tension);
            const double step = std::min(static_cast<double>(n) - 1.0, std::floor(x * n));
            return a + (b - a) * static_cast<float>(step / static_cast<double>(n - 1));
        }
        case ShapePulse: {
            const int n = countFor(tension);
            return (static_cast<int>(std::floor(x * n)) & 1) ? b : a;
        }
        case ShapeWave: {
            constexpr double kTwoPi = 6.28318530717958647692;
            const double cycles = static_cast<double>(countFor(tension)) - 1.5;
            return a + (b - a) * static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * cycles * x));
        }
        default:
            return a + (b - a) * static_cast<float>(bend(x, tension));
    }
}

static float evalPoints(const std::vector<AutoPoint>& pts, double tick) {
    if (pts.empty()) return 0.5f;
    if (tick <= static_cast<double>(pts.front().tick)) return pts.front().value;
    if (tick >= static_cast<double>(pts.back().tick)) return pts.back().value;

    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (static_cast<double>(pts[mid].tick) <= tick) lo = mid;
        else hi = mid;
    }
    const AutoPoint& p = pts[lo];
    const AutoPoint& q = pts[lo + 1];
    const double span = static_cast<double>(q.tick - p.tick);
    return autoSegment(p.value, q.value, span > 0.0 ? (tick - static_cast<double>(p.tick)) / span : 1.0, p.shape, p.tension);
}

float evalAuto(const std::vector<AutoPoint>& points, const AutoLfo& lfo, double tick, double ticksPerStep) {
    float v = evalPoints(points, tick);
    if (lfo.on && lfo.periodSteps > 0.0f) {
        constexpr double kTwoPi = 6.28318530717958647692;
        double ph = tick / (static_cast<double>(lfo.periodSteps) * ticksPerStep) + static_cast<double>(lfo.phase);
        ph -= std::floor(ph);
        double w;
        switch (lfo.shape) {
            case 1: w = ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0); break;
            case 2: w = ph < 0.5 ? 1.0 : -1.0; break;
            case 3: w = 2.0 * ph - 1.0; break;
            default: w = std::sin(kTwoPi * ph); break;
        }
        v += lfo.depth * static_cast<float>(w);
    }
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace chost::dsp
