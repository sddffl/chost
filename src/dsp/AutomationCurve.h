#pragma once
// FL Studio / PULP style Parameter Automation & Curve System for chost
// Supports Single Curve, Double Curve (S-curve), Hold, Stairs, Pulse, Wave,
// Bezier Tension warping, and LFO modulators.

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace chost::dsp {

enum AutoShape {
    ShapeSingle, // Standard exponential/logarithmic curve with tension
    ShapeDouble, // S-curve (two curves mirrored about midpoint)
    ShapeHold,   // Square step hold
    ShapeStairs, // Stepped staircase
    ShapePulse,  // Alternating square pulse
    ShapeWave,   // Sine wave burst
    NumAutoShapes
};

struct AutoPoint {
    int64_t tick = 0;       // Position in sequencer ticks (e.g. 960 ticks / beat)
    float value = 0.5f;     // Normalized value 0.0 .. 1.0
    float tension = 0.0f;   // -1.0 (fast start) .. +1.0 (slow start), 0 = linear
    int shape = ShapeSingle; // AutoShape
};

struct AutoLfo {
    bool on = false;
    float periodSteps = 4.0f; // Period in sequencer steps
    float depth = 0.2f;       // Modulation depth
    float phase = 0.0f;       // 0.0 .. 1.0
    int shape = 0;            // 0=sine, 1=triangle, 2=square, 3=saw
};

const char* autoShapeName(int shape);

// Evaluates a single automation curve segment from value a to b at normalized x (0..1)
float autoSegment(float a, float b, double x, int shape, float tension);

// Evaluates the continuous value at a given sequencer tick
float evalAuto(const std::vector<AutoPoint>& points, const AutoLfo& lfo, double tick, double ticksPerStep = 240.0);

} // namespace chost::dsp
