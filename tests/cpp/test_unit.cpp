// chost-test-unit: the parts of the core that need no audio engine.
//   chost-test-unit [work dir]
// The audio cache key (AudioConvert), the bar and beat of a frame (Transport.h) and cutting a clip in two
// (ClipEdit). Each guards a bug fixed in Phase 0, see docs/AUDIT.md.

#include "check.h"
#include "core/AudioConvert.h"
#include "core/AutoStrip.h"
#include "core/ClipEdit.h"
#include "core/RemasterProtocol.h"
#include "core/Transport.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace chost;

namespace {

constexpr double kPi = 3.14159265358979323846;

void tone(const fs::path& p, double hz, uint32_t rate, double seconds = 0.5)
{
    const size_t frames = static_cast<size_t>(rate * seconds);
    std::vector<float> s(frames * 2);
    for (size_t i = 0; i < frames; ++i)
        s[2 * i] = s[2 * i + 1] = static_cast<float>(0.5 * std::sin(2 * kPi * hz * i / rate));
    std::string error;
    writeWavFile(p.u8string(), s, 2, rate, error);
}

// frequency of a file's left channel, by zero crossings
double frequencyOf(const std::string& path)
{
    std::vector<float> s;
    AudioFileInfo info;
    if (!readAudioFile(path, s, info) || info.frames < 2)
        return -1;
    int crossings = 0;
    for (size_t i = 1; i < info.frames; ++i)
        if ((s[2 * (i - 1)] < 0) != (s[2 * i] < 0))
            ++crossings;
    return crossings / 2.0 / (static_cast<double>(info.frames) / info.sampleRate);
}

void testCache(const fs::path& root)
{
    std::printf("audio cache key (AudioConvert::prepareAudioFile)\n");
    fs::create_directories(root / "songA");
    fs::create_directories(root / "songB");
    const std::string cache = (root / "cache").u8string();
    std::string error;
    tone(root / "songA" / "vocals.wav", 440, 44100);
    tone(root / "songB" / "vocals.wav", 880, 44100);
    const std::string a = prepareAudioFile((root / "songA" / "vocals.wav").u8string(), 48000, cache, error);
    const std::string b = prepareAudioFile((root / "songB" / "vocals.wav").u8string(), 48000, cache, error);
    CHECK(!a.empty() && !b.empty() && a != b, "two files called vocals.wav get two cached copies");
    CHECK(std::fabs(frequencyOf(a) - 440) < 10 && std::fabs(frequencyOf(b) - 880) < 10, "each plays its own audio (%.0f / %.0f Hz)", frequencyOf(a), frequencyOf(b));
    CHECK(fs::u8path(a).filename().u8string().rfind("vocals.", 0) == 0, "the readable name is kept for Carla");
    CHECK(prepareAudioFile((root / "songA" / "vocals.wav").u8string(), 48000, cache, error) == a, "the same file again is a cache hit");

    tone(root / "songA" / "vocals.wav", 660, 44100);
    const fs::path songA = root / "songA" / "vocals.wav";
    fs::last_write_time(songA, fs::last_write_time(songA) + std::chrono::seconds(5));
    const std::string a2 = prepareAudioFile(songA.u8string(), 48000, cache, error);
    CHECK(a2 != a && std::fabs(frequencyOf(a2) - 660) < 10, "a file rewritten in place plays its new audio, not the stale copy");

    AudioFileInfo info;
    const std::string a96 = prepareAudioFile(songA.u8string(), 96000, cache, error);
    CHECK(a96 != a2 && probeAudioFile(a96, info) && info.sampleRate == 96000, "another engine rate gets its own copy");
    CHECK(prepareAudioFile(songA.u8string(), 44100, cache, error) == songA.u8string(), "a WAV at the engine rate is used directly");

    tone(root / "songA" / u8"主唱.wav", 440, 44100);
    const std::string c = prepareAudioFile((root / "songA" / u8"主唱.wav").u8string(), 48000, cache, error);
    CHECK(!c.empty() && isPureAscii(c), "a non-ASCII name gets an ASCII cache name");

    int parts = 0;
    for (const auto& entry : fs::directory_iterator(cache))
        parts += entry.path().u8string().find(".part") != std::string::npos;
    CHECK(parts == 0, "no partial files left behind");
}

void testBarBeat()
{
    std::printf("bar and beat of a frame (Transport.h)\n");
    struct Case {
        uint64_t frame;
        double bpm, perBar;
        int bar, beat;
        double tick;
        const char* what;
    };
    const Case cases[] = {
        {0, 120, 4, 1, 1, 0, "the start"},
        {24000, 120, 4, 1, 2, 0, "4/4 at 120: one beat is 0.5 s"},
        {36000, 120, 4, 1, 2, 960, "half way through beat 2"},
        {96000, 120, 4, 2, 1, 0, "exactly bar 2"},
        {95999, 120, 4, 1, 4, 1919.92, "one frame before bar 2"},
        {48000 * 60, 120, 4, 31, 1, 0, "one minute is 120 beats: bar 31"},
        {144000, 120, 6, 2, 1, 0, "6/8 at 120 eighths: bar 2 at 3 s"},
        {48000 * 4, 90, 3, 3, 1, 0, "3/4 at 90: bar 3 at 4 s"},
        {uint64_t(48000) * 3600 * 10, 137, 4, 20551, 1, 0, "no drift after 10 hours at 137"},
    };
    for (const Case& c : cases)
    {
        const BarBeat b = barBeatAt(c.frame, 48000, c.bpm, c.perBar, 1920);
        CHECK(b.bar == c.bar && b.beat == c.beat && std::fabs(b.tick - c.tick) < 0.01, "%-36s bar %d beat %d tick %.2f", c.what, b.bar, b.beat, b.tick);
    }
}

void testSplit(const fs::path& root)
{
    std::printf("cutting a clip in two (ClipEdit::splitAudioFile)\n");
    const uint32_t rate = 48000, frames = 4 * rate;
    std::vector<float> sig(frames * 2);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    for (uint32_t i = 0; i < frames; ++i)
    {
        sig[2 * i] = static_cast<float>(0.4 * std::sin(2 * kPi * 220 * i / rate)) + noise(rng);
        sig[2 * i + 1] = static_cast<float>(0.4 * std::sin(2 * kPi * 330 * i / rate)) + noise(rng);
    }
    const std::string in = (root / "lead.wav").u8string(), before = (root / "before.wav").u8string(), after = (root / "after.wav").u8string();
    std::string error;
    writeWavFile(in, sig, 2, rate, error);
    CHECK(splitAudioFile(in, 1.7, before, after, error), "cut at 1.7 s %s", error.c_str());
    std::vector<float> a, b;
    AudioFileInfo ia, ib;
    readAudioFile(before, a, ia);
    readAudioFile(after, b, ib);
    CHECK(a.size() == sig.size() && b.size() == sig.size(), "both parts as long as the original");
    double sum = 0, aAfter = 0, bBefore = 0, stepA = 0, stepB = 0, stepSig = 0;
    const size_t cut = static_cast<size_t>(1.7 * rate);
    for (size_t i = 0; i < sig.size() && a.size() == sig.size() && b.size() == sig.size(); ++i)
    {
        const size_t f = i / 2;
        sum = std::max(sum, static_cast<double>(std::fabs(a[i] + b[i] - sig[i])));
        if (f > cut + 48)
            aAfter = std::max(aAfter, static_cast<double>(std::fabs(a[i])));
        if (f + 48 < cut)
            bBefore = std::max(bBefore, static_cast<double>(std::fabs(b[i])));
        if (i >= 2 && f + 480 > cut && f < cut + 480)
        {
            stepA = std::max(stepA, static_cast<double>(std::fabs(a[i] - a[i - 2])));
            stepB = std::max(stepB, static_cast<double>(std::fabs(b[i] - b[i - 2])));
            stepSig = std::max(stepSig, static_cast<double>(std::fabs(sig[i] - sig[i - 2])));
        }
    }
    CHECK(sum < 1e-6, "the two parts add up to the original (largest error %.1g)", sum);
    CHECK(aAfter == 0 && bBefore == 0, "each part is silent on the other side of the cut");
    CHECK(stepA <= stepSig + 0.01 && stepB <= stepSig + 0.01, "no jump at the cut (%.3f / %.3f, the original's own %.3f)", stepA, stepB, stepSig);
    CHECK(!splitAudioFile(in, 0.0, before, after, error) && !splitAudioFile(in, 9.0, before, after, error), "cuts at 0 and past the end are refused");
}

// What the remaster dialog takes from the workflow's output. The workflow passes on what the tools it runs
// print, and chost_transcribe.py's "MIDI <file>\t<note count>" was read as a MIDI file named "1077".
void testRemasterLines()
{
    std::printf("remaster workflow lines\n");
    using Kind = RemasterEvent::Kind;
    const std::string spaced = "C:\\Users\\A B\\My Song_remaster_work\\bass.mid";

    RemasterEvent e = parseRemasterLine("MIDI bass\t" + spaced);
    CHECK(e.kind == Kind::Midi && e.role == "bass" && e.path == spaced, "MIDI <role>\\t<path with spaces>: role %s, path %s", e.role.c_str(), e.path.c_str());
    e = parseRemasterLine("MIDI " + spaced + "\t1077");
    CHECK(e.kind == Kind::None, "chost_transcribe.py's MIDI <path>\\t<note count> is not a MIDI event");
    e = parseRemasterLine("MIDI /home/a b/piano.mid\t360\r");
    CHECK(e.kind == Kind::None, "... nor with a Unix path and a CR");
    e = parseRemasterLine("    MIDI bass\t/x/bass.mid");
    CHECK(e.kind == Kind::None, "an indented line (a tool's, passed on) is never an event");
    e = parseRemasterLine("TUNED 0.0100\t0,0,60.12,60.20");
    CHECK(e.kind == Kind::None, "chost_tune.py's TUNED <hop>\\t<curve> is not a tuned vocal");
    e = parseRemasterLine("TUNED lead\t/tmp/my work/tuned_lead.wav");
    CHECK(e.kind == Kind::Tuned && e.role == "lead" && e.path == "/tmp/my work/tuned_lead.wav", "TUNED <role>\\t<path with spaces>");
    e = parseRemasterLine("STEM kick\t底鼓\t/tmp/my work/stems/song_kick.wav");
    CHECK(e.kind == Kind::Stem && e.role == "kick" && e.label == "底鼓" && e.path == "/tmp/my work/stems/song_kick.wav", "STEM <id>\\t<label>\\t<path with spaces>");
    e = parseRemasterLine("MASTER /home/a b/Desktop/My Song_顶级母带.wav");
    CHECK(e.kind == Kind::Master && e.path == "/home/a b/Desktop/My Song_顶级母带.wav", "MASTER <path with spaces>, all of it");
    e = parseRemasterLine("PROGRESS 45 14 轨细分完成");
    CHECK(e.kind == Kind::Progress && e.percent == 45 && e.label == "14 轨细分完成", "PROGRESS <pct> <message>");
    e = parseRemasterLine("BALANCE guitar\t-1.4\t-0.65");
    CHECK(e.kind == Kind::Balance && e.role == "guitar" && std::abs(e.number + 1.4) < 1e-9 && std::abs(e.pan + 0.65) < 1e-9, "BALANCE <role>\\t<dB>\\t<pan>");
    e = parseRemasterLine("ANALYZE 128.0\tA minor");
    CHECK(e.kind == Kind::Analyze && e.number == 128.0 && e.label == "A minor", "ANALYZE <bpm>\\t<key>");
}

ParamInfo param(uint32_t index, const char* name, float lo, float hi, const char* unit = "", bool boolean = false)
{
    ParamInfo p;
    p.index = index;
    p.name = name;
    p.unit = unit;
    p.min = lo;
    p.max = hi;
    p.isBoolean = boolean;
    return p;
}

float valueOf(const std::vector<std::pair<uint32_t, float>>& values, uint32_t index, float missing = -9999.0f)
{
    for (const auto& [i, v] : values)
        if (i == index)
            return v;
    return missing;
}

// The automatic channel strip: what goes on each part, which of the user's plugins, set how.
void testAutoStrip()
{
    std::printf("automatic channel strip\n");
    auto kinds = [](const std::vector<StripSlot>& strip) {
        std::string s;
        for (const StripSlot& slot : strip)
            s += std::to_string(static_cast<int>(slot.kind));
        return s;
    };
    const auto lead = channelStrip("lead", 0.0);
    CHECK(kinds(lead) == "0123", "lead vocal: trim, EQ, compressor, de-esser (%s)", kinds(lead).c_str());
    CHECK(lead[1].param("highpass_hz") == 90.0f, "lead vocal high pass at 90 Hz");
    CHECK(kinds(channelStrip("bass", 0.0)) == "012" && kinds(channelStrip("hh", 0.0)) == "01" && kinds(channelStrip("", 0.0)) == "0",
          "bass: EQ and compressor; hi-hat: EQ; an unknown part: just the trim");
    // a part twice as loud as the working level gets thresholds 6 dB higher
    const float at18 = channelStrip("bass", 0.0, -18.0)[2].param("threshold_db");
    const float at12 = channelStrip("bass", 0.0, -12.0)[2].param("threshold_db");
    CHECK(std::abs(at12 - at18 - 6.0f) < 1e-4f, "thresholds follow the part's level (%.1f at -18 LUFS, %.1f at -12)", at18, at12);
    CHECK(trimFor(-30.0, false) == 12.0 && trimFor(-24.0, false) == 6.0 && trimFor(-10.0, false) == 0.0 && trimFor(-80.0, true) == 0.0,
          "trim: quiet parts up to the working level (at most +12 dB), loud and silent ones left alone");
    const auto bus = mixBusStrip();
    CHECK(bus.size() == 2 && bus[0].kind == StripKind::Compressor && bus[1].kind == StripKind::Limiter && bus[1].param("ceiling_db") == -1.0f,
          "mix bus: glue compressor, then a limiter at -1 dB");

    auto entry = [](const char* name, const char* spec, uint32_t ins = 2, bool synth = false) {
        PluginEntry p;
        p.name = name;
        p.spec = spec;
        p.audioIns = ins;
        p.audioOuts = ins;
        p.isSynth = synth;
        return p;
    };
    const std::vector<PluginEntry> library{entry("MIDI Sequencer", "lv2:seq"), entry("Calf Multiband Compressor", "lv2:mb"),
                                           entry("x42-eq - Parametric Equalizer Mono", "lv2:eqm", 1),
                                           entry("x42-eq - Parametric Equalizer Stereo", "lv2:eqs"),
                                           entry("ZamComp", "lv2:zamcomp"), entry("Pro-L 2", "vst3:prol"),
                                           entry("Compressor Synth", "lv2:synth", 2, true)};
    CHECK(stripPlugin(StripKind::Trim, library) == "internal:audiogain_s", "the trim is carla's own gain, always there");
    CHECK(stripPlugin(StripKind::Eq, library) == "lv2:eqs", "EQ: the stereo parametric one, not the mono one or a \"Sequencer\" (%s)",
          stripPlugin(StripKind::Eq, library).c_str());
    CHECK(stripPlugin(StripKind::Compressor, library) == "lv2:zamcomp", "compressor: not the multiband one (%s)", stripPlugin(StripKind::Compressor, library).c_str());
    CHECK(stripPlugin(StripKind::Limiter, library) == "vst3:prol", "limiter: the user's Pro-L");
    CHECK(stripPlugin(StripKind::DeEsser, library).empty(), "no de-esser in the library: none");

    // carla's gain: a factor 0..4
    const auto trim = stripValues(channelStrip("hh", 6.0)[0], {param(0, "Gain", 0, 4), param(1, "Apply Left", 0, 1, "", true)});
    CHECK(std::abs(valueOf(trim, 0) - 1.9953f) < 1e-3f && valueOf(trim, 1) == -9999.0f, "trim +6 dB on carla's gain: x%.3f", valueOf(trim, 0));
    // an LV2 compressor with units, a meter and a side chain filter in the way
    const std::vector<ParamInfo> zam{param(0, "Attack", 0.1f, 100, "ms"), param(1, "Release", 1, 500, "ms"),
                                     param(2, "Knee", 0, 8, "dB"), param(3, "Ratio", 1, 20), param(4, "Sidechain Threshold", -80, 0, "dB"),
                                     param(5, "Threshold", -80, 0, "dB"), param(6, "Output", -60, 0, "dB"), param(7, "Makeup", 0, 30, "dB")};
    StripSlot c = channelStrip("bass", 0.0)[2];
    const auto cv = stripValues(c, zam);
    CHECK(valueOf(cv, 0) == 15.0f && valueOf(cv, 1) == 150.0f && valueOf(cv, 3) == 4.0f && valueOf(cv, 5) == -22.0f && valueOf(cv, 7) == 3.0f,
          "bass compressor: attack 15 ms, release 150 ms, ratio 4, threshold -22 dB, make-up 3 dB");
    CHECK(valueOf(cv, 4) == -9999.0f && valueOf(cv, 6) == -9999.0f && valueOf(cv, 2) == -9999.0f, "the side chain threshold, the output meter and the knee are left alone");
    // times in seconds, a VST2 plugin's 0..1 controls
    const auto secs = stripValues(c, {param(0, "Attack", 0.0001f, 1, "s"), param(1, "Release", 0, 1), param(2, "Threshold", 0, 1)});
    CHECK(std::abs(valueOf(secs, 0) - 0.015f) < 1e-6f && valueOf(secs, 1) == -9999.0f && valueOf(secs, 2) == -9999.0f,
          "attack in seconds converted; unitless 0..1 controls (VST2) are not guessed at");
    // an EQ's high pass: frequency and its switch
    const auto hp = stripValues(channelStrip("lead", 0.0)[1], {param(0, "Highpass", 0, 1, "", true), param(1, "Highpass Frequency", 5, 1250, "Hz"),
                                                              param(2, "Lowpass Frequency", 500, 20000, "Hz")});
    CHECK(valueOf(hp, 1) == 90.0f && valueOf(hp, 0) == 1.0f && valueOf(hp, 2) == -9999.0f, "vocal EQ: high pass switched on at 90 Hz, low pass left alone");
    // a limiter with only a threshold: that is its ceiling
    const auto lim = stripValues(bus[1], {param(0, "Threshold", -30, 0, "dB"), param(1, "Release", 1, 1000, "ms")});
    CHECK(valueOf(lim, 0) == -1.0f && valueOf(lim, 1) == 60.0f, "limiter without a ceiling control: threshold at the -1 dB ceiling");
    const auto lim2 = stripValues(bus[1], {param(0, "Threshold", -30, 0, "dB"), param(1, "Output Ceiling", -30, 0, "dB")});
    CHECK(valueOf(lim2, 1) == -1.0f && valueOf(lim2, 0) == -3.0f, "limiter with a ceiling: ceiling -1 dB, driven from -3 dB");
}

} // namespace

int main(int argc, char** argv)
{
    const fs::path root = argc > 1 ? fs::u8path(argv[1]) : fs::temp_directory_path() / "chost-test-unit";
    fs::remove_all(root);
    fs::create_directories(root);
    testCache(root / "cache-test");
    testBarBeat();
    testSplit(root);
    testRemasterLines();
    testAutoStrip();
    return check::failures();
}
