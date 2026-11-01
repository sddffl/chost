// Loudness as people hear it (ITU-R BS.1770-4, EBU R128): K-weighting, 400 ms blocks, the -70 LUFS and
// relative -10 LU gates. What the AI mix balance sets the faders by, and what streaming services normalise.

#pragma once

#include <cstdint>
#include <vector>

namespace chost {

struct Loudness {
    double integrated = -70.0; // LUFS; -70 for silence (below the absolute gate)
    double peak = -120.0;      // sample peak, dBFS
    bool silent = true;        // no block above the absolute gate
};

// samples interleaved, 1 or 2 channels (more: the first two)
Loudness measureLoudness(const std::vector<float>& samples, uint32_t channels, uint32_t sampleRate);

} // namespace chost
