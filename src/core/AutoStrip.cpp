#include "AutoStrip.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace chost {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool has(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// a short keyword ("EQ", "Comp") only as a word of its own: "EQ" is in "Sequencer" too
bool hasWord(const std::string& name, const std::string& word)
{
    const std::string n = lower(name), w = lower(word);
    for (size_t at = n.find(w); at != std::string::npos; at = n.find(w, at + 1))
    {
        const bool before = at == 0 || !std::isalpha(static_cast<unsigned char>(n[at - 1]));
        const size_t after = at + w.size();
        const bool behind = after >= n.size() || !std::isalpha(static_cast<unsigned char>(n[after]));
        if (before && behind)
            return true;
    }
    return false;
}

StripSlot slot(StripKind kind, std::vector<StripParam> params)
{
    StripSlot s;
    s.kind = kind;
    s.params = std::move(params);
    return s;
}

StripSlot eq(float highpass, float presenceHz = 0.0f, float presenceDb = 0.0f, float airHz = 0.0f, float airDb = 0.0f)
{
    std::vector<StripParam> p{{"highpass_hz", highpass}};
    if (presenceHz > 0.0f) {
        p.push_back({"presence_hz", presenceHz});
        p.push_back({"presence_db", presenceDb});
    }
    if (airHz > 0.0f) {
        p.push_back({"air_hz", airHz});
        p.push_back({"air_db", airDb});
    }
    return slot(StripKind::Eq, std::move(p));
}

StripSlot comp(float threshold, float ratio, float attack, float release, float makeup)
{
    return slot(StripKind::Compressor, {{"threshold_db", threshold}, {"ratio", ratio}, {"attack_ms", attack},
                                        {"release_ms", release}, {"makeup_db", makeup}});
}

// ---------------------------------------------------------------------------------------------------------------------
// reading a plugin's controls

enum class Unit { Unknown, Hz, KHz, Db, Ms, Seconds, Ratio, Linear };

bool normalised(const ParamInfo& p)
{
    // what a VST2 plugin reports for every control: no unit, 0..1; its real scale is the plugin's secret
    return p.unit.empty() && p.min == 0.0f && p.max == 1.0f && !p.isBoolean;
}

Unit unitOf(const ParamInfo& p, const std::string& what)
{
    const std::string u = lower(p.unit);
    if (has(u, "khz"))
        return Unit::KHz;
    if (has(u, "hz"))
        return Unit::Hz;
    if (has(u, "db"))
        return Unit::Db;
    if (u == "ms" || has(u, "msec"))
        return Unit::Ms;
    if (u == "s" || u == "sec" || has(u, "second"))
        return Unit::Seconds;
    if (has(u, ":1") || u == "ratio")
        return Unit::Ratio;
    if (!u.empty() || normalised(p))
        return Unit::Unknown;
    // no unit: what the range says
    if (what == "highpass_hz" || what == "frequency_hz")
        return p.min >= 0.0f && p.max >= 1000.0f ? Unit::Hz : Unit::Unknown;
    if (what == "attack_ms" || what == "release_ms")
        return p.max > 10.0f ? Unit::Ms : Unit::Unknown;
    if (what == "ratio")
        return p.max > 1.5f ? Unit::Ratio : Unit::Unknown;
    // dB or a plain factor: dB ranges reach below 0, a factor (carla's gain: 0 .. 4) doesn't
    if (p.min < 0.0f)
        return Unit::Db;
    if (what == "gain_db" && p.min >= 0.0f && p.max > 1.0f && p.max <= 16.0f)
        return Unit::Linear;
    return Unit::Unknown;
}

// the setting in the control's own unit; false when the two don't fit
bool convert(const std::string& what, float value, Unit unit, float& out)
{
    const bool frequency = what == "highpass_hz" || what == "frequency_hz";
    const bool time = what == "attack_ms" || what == "release_ms";
    const bool level = what == "gain_db" || what == "threshold_db" || what == "makeup_db" || what == "ceiling_db";
    if (frequency && unit == Unit::Hz)
        out = value;
    else if (frequency && unit == Unit::KHz)
        out = value / 1000.0f;
    else if (time && unit == Unit::Ms)
        out = value;
    else if (time && unit == Unit::Seconds)
        out = value / 1000.0f;
    else if (what == "ratio" && unit == Unit::Ratio)
        out = value;
    else if (level && unit == Unit::Db)
        out = value;
    else if (what == "gain_db" && unit == Unit::Linear)
        out = std::pow(10.0f, value / 20.0f);
    else
        return false;
    return true;
}

// names a control goes by, best first
const std::vector<std::string>& namesFor(const std::string& what)
{
    static const std::vector<std::string> gain{"trim", "input gain", "gain"};
    static const std::vector<std::string> highpass{"high pass freq", "highpass freq", "high-pass freq", "hpf freq", "hp freq",
                                                   "low cut freq", "lowcut freq", "low-cut freq", "lc freq", "high pass",
                                                   "highpass", "high-pass", "hpf", "low cut", "lowcut", "low-cut"};
    static const std::vector<std::string> threshold{"threshold", "thresh"};
    static const std::vector<std::string> ratio{"ratio"};
    static const std::vector<std::string> attack{"attack"};
    static const std::vector<std::string> release{"release"};
    static const std::vector<std::string> makeup{"makeup", "make-up", "make up", "output gain", "out gain", "output"};
    static const std::vector<std::string> ceiling{"ceiling", "output level", "out level", "max level", "limit level"};
    static const std::vector<std::string> frequency{"frequency", "freq"};
    static const std::vector<std::string> none;
    if (what == "gain_db") return gain;
    if (what == "highpass_hz") return highpass;
    if (what == "threshold_db") return threshold;
    if (what == "ratio") return ratio;
    if (what == "attack_ms") return attack;
    if (what == "release_ms") return release;
    if (what == "makeup_db") return makeup;
    if (what == "ceiling_db") return ceiling;
    if (what == "frequency_hz") return frequency;
    return none;
}

// controls that sound like the setting but aren't it: a gate's threshold, the side chain's filter, a
// meter, the automatic make-up switch
bool unrelated(const std::string& name)
{
    for (const char* word : {"sidechain", "side chain", "sc ", "gate", "meter", "auto", "knee", "lookahead", "look-ahead"})
        if (has(name, word))
            return true;
    return false;
}

} // namespace

float StripSlot::param(const std::string& what, float fallback) const
{
    for (const StripParam& p : params)
        if (p.what == what)
            return p.value;
    return fallback;
}

std::vector<StripSlot> channelStrip(const std::string& role, double trimDb, double level)
{
    std::vector<StripSlot> strip{slot(StripKind::Trim, {{"gain_db", static_cast<float>(trimDb)}})};
    // thresholds for a part at kStripLevel: a compressor working on the peaks, a few dB of reduction.
    // A part that reaches the strip louder (or quieter) gets them that much higher.
    const float offset = std::isfinite(level) && level > -69.0 ? static_cast<float>(std::clamp(level - kStripLevel, -12.0, 24.0)) : 0.0f;
    auto add = [&](StripSlot s) {
        if (s.kind == StripKind::Compressor || s.kind == StripKind::DeEsser)
            for (StripParam& p : s.params)
                if (p.what == "threshold_db")
                    p.value += offset;
        strip.push_back(std::move(s));
    };
    if (role == "lead" || role == "vocals")
    {
        add(eq(90.0f, 3500.0f, 2.0f));
        add(comp(-22.0f, 3.0f, 5.0f, 80.0f, 3.0f));
        add(slot(StripKind::DeEsser, {{"threshold_db", -26.0f}, {"frequency_hz", 6500.0f}}));
    }
    else if (role == "backing")
    {
        add(eq(150.0f, 5000.0f, 1.5f));
        add(comp(-24.0f, 4.0f, 10.0f, 120.0f, 2.0f));
        add(slot(StripKind::DeEsser, {{"threshold_db", -28.0f}, {"frequency_hz", 6500.0f}}));
    }
    else if (role == "bass")
    {
        add(eq(35.0f));
        add(comp(-22.0f, 4.0f, 15.0f, 150.0f, 3.0f));
    }
    else if (role == "kick")
    {
        add(eq(30.0f));
        add(comp(-16.0f, 3.0f, 25.0f, 90.0f, 2.0f));
    }
    else if (role == "snare")
    {
        add(eq(80.0f));
        add(comp(-18.0f, 3.0f, 10.0f, 120.0f, 2.0f));
    }
    else if (role == "drums")
    {
        add(eq(30.0f));
        add(comp(-18.0f, 2.5f, 20.0f, 120.0f, 2.0f));
    }
    else if (role == "toms")
        add(eq(60.0f));
    else if (role == "hh")
        add(eq(300.0f));
    else if (role == "ride" || role == "crash")
        add(eq(250.0f));
    else if (role == "guitar")
    {
        add(eq(90.0f));
        add(comp(-22.0f, 2.5f, 15.0f, 150.0f, 1.0f));
    }
    else if (role == "piano")
    {
        add(eq(50.0f));
        add(comp(-22.0f, 2.0f, 20.0f, 200.0f, 1.0f));
    }
    else if (role == "other")
        add(eq(80.0f));
    return strip;
}

std::vector<StripSlot> mixBusStrip()
{
    return {comp(-14.0f, 2.0f, 30.0f, 200.0f, 1.0f),
            slot(StripKind::Limiter, {{"ceiling_db", -1.0f}, {"threshold_db", -3.0f}, {"release_ms", 60.0f}})};
}

double trimFor(double loudness, bool silent)
{
    if (silent || !std::isfinite(loudness) || loudness <= -69.0)
        return 0.0;
    return std::clamp(kStripLevel - loudness, 0.0, 12.0);
}

std::string stripPlugin(StripKind kind, const std::vector<PluginEntry>& plugins)
{
    if (kind == StripKind::Trim)
        return "internal:audiogain_s";
    // the user's own first, then free ones; a short word only as a word
    struct Wanted { std::vector<std::string> names; std::vector<std::string> words; std::vector<std::string> not_; };
    static const Wanted eqs{{"Pro-Q", "Nova", "SlickEQ", "ReaEQ", "MEqualizer", "ZamEQ2", "Calf Equalizer", "x42-eq",
                             "Parametric Equalizer", "Parametric EQ", "Equalizer", "Equaliser"},
                            {"EQ"},
                            {"graphic", "dynamic", "match", "carla"}};
    static const Wanted comps{{"Pro-C", "Kotelnikov", "ReaComp", "MCompressor", "ZamComp", "Calf Compressor", "LSP Compressor",
                               "Compressor"},
                              {"Comp"},
                              {"multiband", "multi-band", "multi band", "sidechain", "expander", "carla"}};
    static const Wanted limiters{{"Pro-L", "Limiter No6", "LoudMax", "ZaMaximX2", "Digital Peak Limiter", "x42-dpl",
                                  "Calf Limiter", "LSP Limiter", "Limiter", "Maximizer", "Maximiser"},
                                 {},
                                 {"multiband", "multi-band", "multi band", "sidechain", "carla"}};
    static const Wanted deessers{{"Pro-DS", "DeEsser", "De-Esser", "Deesser", "De Esser", "Sibilance"}, {}, {"carla"}};
    const Wanted& w = kind == StripKind::Eq ? eqs : kind == StripKind::Compressor ? comps : kind == StripKind::Limiter ? limiters : deessers;

    auto usable = [&](const PluginEntry& p, bool stereo) {
        if (p.isSynth || p.audioIns == 0 || p.audioOuts == 0 || (stereo && (p.audioIns < 2 || p.audioOuts < 2)))
            return false;
        const std::string name = lower(p.name);
        for (const std::string& no : w.not_)
            if (has(name, no))
                return false;
        return true;
    };
    for (const bool stereo : {true, false})
    {
        for (const std::string& name : w.names)
            for (const PluginEntry& p : plugins)
                if (usable(p, stereo) && has(lower(p.name), lower(name)))
                    return p.spec;
        for (const std::string& word : w.words)
            for (const PluginEntry& p : plugins)
                if (usable(p, stereo) && hasWord(p.name, word))
                    return p.spec;
        if (kind == StripKind::Eq)
            for (const PluginEntry& p : plugins)
                if (usable(p, stereo) && lower(p.category) == "eq")
                    return p.spec;
    }
    return {};
}

static const ParamInfo* findParamByName(const std::vector<ParamInfo>& params, const std::string& name)
{
    for (const ParamInfo& p : params)
        if (lower(p.name) == lower(name))
            return &p;
    return nullptr;
}

static bool hasParamByName(const std::vector<ParamInfo>& params, const std::string& name)
{
    return findParamByName(params, name) != nullptr;
}

std::vector<std::pair<uint32_t, float>> stripValues(const StripSlot& slot, const std::vector<ParamInfo>& params)
{
    // 1. FabFilter Pro-Q (Pro-Q, Pro-Q 2, Pro-Q 3, Pro-Q 4)
    const bool isProQ = hasParamByName(params, "Band 1 Frequency") &&
                        (hasParamByName(params, "Num Active Bands") || hasParamByName(params, "Band 1 Shape") || hasParamByName(params, "Band 1 Enabled"));
    if (isProQ && slot.kind == StripKind::Eq)
    {
        std::vector<std::pair<uint32_t, float>> values;
        auto setP = [&](const std::string& name, float v) {
            if (const ParamInfo* p = findParamByName(params, name))
                values.emplace_back(p->index, v);
        };
        auto normFreq = [](float f) {
            return std::clamp(std::log10(std::max(10.0f, f) / 10.0f) / std::log10(3000.0f), 0.0f, 1.0f);
        };
        auto normGain = [](float db) {
            return std::clamp(0.5f + (db / 60.0f), 0.0f, 1.0f);
        };

        const float hp = slot.param("highpass_hz", 80.0f);
        const float presenceHz = slot.param("presence_hz", 0.0f);
        const float airHz = slot.param("air_hz", 0.0f);
        const bool hasBand2 = presenceHz > 0.0f || airHz > 0.0f;

        setP("Num Active Bands", hasBand2 ? 2.0f : 1.0f);

        // Band 1: Low Cut / High Pass
        setP("Band 1 Enabled", 1.0f);
        setP("Band 1 Shape", 0.4f); // Low Cut
        setP("Band 1 Frequency", normFreq(hp));
        setP("Band 1 Q", 0.5f);
        setP("Band 1 LP/HP Slope", 0.25f); // 12 dB/oct

        if (hasBand2)
        {
            setP("Band 2 Enabled", 1.0f);
            if (presenceHz > 0.0f)
            {
                setP("Band 2 Shape", 0.0f); // Bell
                setP("Band 2 Frequency", normFreq(presenceHz));
                setP("Band 2 Gain", normGain(slot.param("presence_db", 2.0f)));
                setP("Band 2 Q", 0.5f);
            }
            else
            {
                setP("Band 2 Shape", 0.6f); // High Shelf
                setP("Band 2 Frequency", normFreq(airHz));
                setP("Band 2 Gain", normGain(slot.param("air_db", 2.0f)));
                setP("Band 2 Q", 0.5f);
            }
        }
        return values;
    }

    // 2. FabFilter Pro-C (Pro-C, Pro-C 2, Pro-C 3)
    const bool isProC = hasParamByName(params, "Characteristic") ||
                        (hasParamByName(params, "Threshold") && hasParamByName(params, "Knee Shape"));
    if (isProC && slot.kind == StripKind::Compressor)
    {
        std::vector<std::pair<uint32_t, float>> values;
        auto setP = [&](const std::string& name, float v) {
            if (const ParamInfo* p = findParamByName(params, name))
                values.emplace_back(p->index, v);
        };
        const float thDb = slot.param("threshold_db", -20.0f);
        setP("Threshold", std::clamp((thDb + 60.0f) / 60.0f, 0.0f, 1.0f));

        const float r = slot.param("ratio", 3.0f);
        setP("Ratio", std::clamp((r - 1.0f) / 10.0f * 0.5f + 0.1f, 0.0f, 1.0f));

        const float atkMs = slot.param("attack_ms", 10.0f);
        setP("Attack", std::clamp(std::log10(std::max(0.1f, atkMs) / 0.005f) / std::log10(250.0f / 0.005f), 0.0f, 1.0f));

        const float relMs = slot.param("release_ms", 100.0f);
        setP("Release", std::clamp(std::log10(std::max(10.0f, relMs) / 10.0f) / std::log10(2500.0f / 10.0f), 0.0f, 1.0f));

        const float makeup = slot.param("makeup_db", 0.0f);
        setP("Output Level", std::clamp(0.5f + (makeup / 48.0f), 0.0f, 1.0f));
        setP("Auto Gain", 0.0f);
        return values;
    }

    // 3. FabFilter Pro-DS
    const bool isProDS = hasParamByName(params, "Audition Triggering") ||
                         (hasParamByName(params, "Band Processing") && hasParamByName(params, "Threshold"));
    if (isProDS && slot.kind == StripKind::DeEsser)
    {
        std::vector<std::pair<uint32_t, float>> values;
        auto setP = [&](const std::string& name, float v) {
            if (const ParamInfo* p = findParamByName(params, name))
                values.emplace_back(p->index, v);
        };
        const float thDb = slot.param("threshold_db", -26.0f);
        setP("Threshold", std::clamp((thDb + 40.0f) / 40.0f, 0.0f, 1.0f));
        setP("Range", 0.5f);
        setP("Band Processing", 1.0f);
        setP("High-Pass Frequency", 0.65f);
        return values;
    }

    // 4. FabFilter Pro-L (Pro-L, Pro-L 2)
    const bool isProL = hasParamByName(params, "Lookahead") &&
                        (hasParamByName(params, "Oversampling") || hasParamByName(params, "Output Level") || hasParamByName(params, "ISP"));
    if (isProL && slot.kind == StripKind::Limiter)
    {
        std::vector<std::pair<uint32_t, float>> values;
        auto setP = [&](const std::string& name, float v) {
            if (const ParamInfo* p = findParamByName(params, name))
                values.emplace_back(p->index, v);
        };
        const float ceilDb = slot.param("ceiling_db", -1.0f);
        const float thDb = slot.param("threshold_db", -2.0f);
        setP("Output Level", std::clamp(1.0f + (ceilDb / 24.0f), 0.0f, 1.0f));
        setP("Ceiling", std::clamp(1.0f + (ceilDb / 24.0f), 0.0f, 1.0f));
        setP("Threshold", std::clamp((thDb + 30.0f) / 30.0f, 0.0f, 1.0f));
        return values;
    }

    std::vector<std::pair<uint32_t, float>> values;
    std::vector<uint32_t> used;
    auto set = [&](const ParamInfo& p, float v) {
        v = std::clamp(v, std::min(p.min, p.max), std::max(p.min, p.max));
        if (p.isInteger || p.isBoolean)
            v = std::round(v);
        values.emplace_back(p.index, v);
        used.push_back(p.index);
    };
    auto taken = [&](uint32_t index) { return std::find(used.begin(), used.end(), index) != used.end(); };

    bool ceiling = false;
    for (const StripParam& want : slot.params)
    {
        bool done = false;
        // a limiter's threshold drives it into a ceiling of its own; without one, the threshold is the ceiling
        const float value = slot.kind == StripKind::Limiter && want.what == "threshold_db" && !ceiling
                                ? slot.param("ceiling_db", want.value) : want.value;
        for (const std::string& name : namesFor(want.what))
        {
            for (const ParamInfo& p : params)
            {
                const std::string n = lower(p.name);
                if (p.isOutput || p.isBoolean || taken(p.index) || !has(n, name))
                    continue;
                if (want.what != "gain_db" && unrelated(n))
                    continue;
                // a compressor's "output" may be its meter or its make-up; only the latter has dB that go up
                if (want.what == "makeup_db" && p.max <= 0.0f)
                    continue;
                float v = 0.0f;
                if (convert(want.what, value, unitOf(p, want.what), v))
                {
                    set(p, v);
                    done = true;
                    ceiling = ceiling || want.what == "ceiling_db";
                    break;
                }
            }
            if (done)
                break;
        }
        // the high pass of an EQ is often a band that has to be switched on as well
        if (want.what == "highpass_hz")
            for (const ParamInfo& p : params)
            {
                const std::string n = lower(p.name);
                if (!p.isBoolean || p.isOutput || taken(p.index) || has(n, "bypass"))
                    continue;
                for (const std::string& name : namesFor("highpass_hz"))
                    if (has(n, name))
                    {
                        set(p, p.max);
                        break;
                    }
            }
    }
    return values;
}

const char* stripKindName(StripKind kind)
{
    switch (kind)
    {
    case StripKind::Trim: return "增益";
    case StripKind::Eq: return "EQ";
    case StripKind::Compressor: return "压缩";
    case StripKind::DeEsser: return "齿音消除";
    case StripKind::Limiter: return "限制器";
    }
    return "";
}

} // namespace chost
