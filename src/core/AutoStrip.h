// A starting channel strip for a separated part, as an engineer would set one up before mixing: a trim that
// brings a quiet part up to a working level (the fader takes it back), a high pass on an EQ, a compressor
// set for what the part is (fast and firm on a lead vocal, slower on bass, gentle on keys), a de-esser on
// vocals; and on the bus all the parts go through, a glue compressor and a limiter.
//
// The settings are in engineering terms (Hz, dB, ms, ratio). Carla has no EQ or compressor of its own, so
// those come from the user's plugins: stripPlugin() picks one by name, stripValues() finds its controls by
// name and unit and converts. A control it can't read safely (VST2 plugins report every control as 0..1
// with no unit) is left as the plugin set it. The trim is carla's own stereo gain, so it is always there.

#pragma once

#include "HostEngine.h"

#include <string>
#include <utility>
#include <vector>

namespace chost {

enum class StripKind { Trim, Eq, Compressor, DeEsser, Limiter };

struct StripParam {
    std::string what; // gain_db, highpass_hz, threshold_db, ratio, attack_ms, release_ms, makeup_db, ceiling_db, frequency_hz
    float value = 0.0f;
};

struct StripSlot {
    StripKind kind = StripKind::Trim;
    std::vector<StripParam> params;
    float param(const std::string& what, float fallback = 0.0f) const;
};

// The working level the strip's thresholds are set for: a part at -18 LUFS, as on an analogue desk.
constexpr double kStripLevel = -18.0;

// The channel strip for a part, trim first, in processing order; just the trim for a role it doesn't know.
// trimDb: what the trim does, see trimFor(); level: the part's loudness after it (LUFS), which the
// thresholds follow.
std::vector<StripSlot> channelStrip(const std::string& role, double trimDb, double level = kStripLevel);

// The bus all the parts of a song go through: glue compression and a limiter just under 0 dBFS.
std::vector<StripSlot> mixBusStrip();

// The trim that brings a quiet part measured at loudness LUFS up to kStripLevel, at most +12 dB; 0 for a
// part already there or louder (its thresholds follow it instead) and for a silent one. Only ever a boost,
// so the fader can always take it back and the balance between the parts stays as it was.
double trimFor(double loudness, bool silent);

// The plugin for a slot from the user's plugins: their own well-known ones first (FabFilter, TDR, ...),
// then free ones, then anything named like the kind. Empty when there is none; for a trim, carla's gain.
std::string stripPlugin(StripKind kind, const std::vector<PluginEntry>& plugins);

// What to set the plugin's controls to, as (parameter index, value) within each control's range.
std::vector<std::pair<uint32_t, float>> stripValues(const StripSlot& slot, const std::vector<ParamInfo>& params);

const char* stripKindName(StripKind kind); // "EQ", "压缩", ... as the GUI reports them

} // namespace chost
