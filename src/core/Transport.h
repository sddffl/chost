// Where a frame falls in bars and beats, for plugins that follow the host's tempo (delays, LFOs,
// arpeggiators). A constant tempo from frame 0, as a chost session has.

#pragma once

#include <cmath>
#include <cstdint>

namespace chost {

struct BarBeat {
    int32_t bar = 1;           // from 1
    int32_t beat = 1;          // from 1, within the bar
    double tick = 0.0;         // within the beat, 0 .. ticksPerBeat
    double barStartTick = 0.0; // ticks before this bar
};

// bpm counts the beats of the time signature's denominator (a quarter in 4/4, an eighth in 6/8)
inline BarBeat barBeatAt(uint64_t frame, double sampleRate, double bpm, double beatsPerBar, double ticksPerBeat)
{
    BarBeat out;
    if (!(sampleRate > 0.0) || !(bpm > 0.0) || !(beatsPerBar > 0.0) || !(ticksPerBeat > 0.0))
        return out;
    // a hair over, so a frame exactly on a beat isn't counted as the end of the one before
    const double beats = static_cast<double>(frame) * bpm / (60.0 * sampleRate) + 1e-9;
    const double bars = std::floor(beats / beatsPerBar);
    const double inBar = beats - bars * beatsPerBar;
    const double beat = std::floor(inBar);
    out.bar = static_cast<int32_t>(bars) + 1;
    out.beat = static_cast<int32_t>(beat) + 1;
    out.tick = (inBar - beat) * ticksPerBeat;
    out.barStartTick = bars * beatsPerBar * ticksPerBeat;
    return out;
}

} // namespace chost
