// A starting balance for a mix: every part's fader set so the parts sit at the loudness a typical pop mix
// gives them next to the lead vocal (bass and drums close behind, chords and pads further back). The
// loudness of each part is measured on its own (Loudness.h), after its inserts and fader.

#pragma once

#include <string>
#include <vector>

namespace chost {

enum class VocalStyle { Forward, Normal, Blend }; // how far the vocals stand out of the band

struct BalancePart {
    std::string role;     // "vocals", "bass", "kick"... as the tracks carry them; empty: unknown
    double loudness = 0;  // LUFS as measured, with the fader at volume
    bool silent = false;  // nothing to measure: left alone
    float volume = 1.0f;  // the fader when it was measured (carla volume)
};

// The loudness a part aims for, in LU next to the lead vocal (0).
double balanceTarget(const std::string& role, VocalStyle style);

// New fader volumes, parallel to parts (silent ones keep theirs). The lead sits at reference LUFS; if a
// fader would have to go past its top (+2 dB) everything comes down together, so the balance holds.
std::vector<float> balanceVolumes(const std::vector<BalancePart>& parts, VocalStyle style, double reference = -18.0);

// Pan positions (-1 left .. 1 right), parallel to roles, as a pop mix places them: vocals, kick, snare,
// bass and whole drum kits in the centre; doubled parts (two guitars, backing vocals) spread to both
// sides; a lone guitar on one side and the keys on the other; cymbals as the audience sees the kit.
std::vector<float> autoPan(const std::vector<std::string>& roles);

} // namespace chost
