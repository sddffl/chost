// chost-test-unit: the parts of the core that need no audio engine.
//   chost-test-unit [work dir]
// The audio cache key (AudioConvert), the bar and beat of a frame (Transport.h) and cutting a clip in two
// (ClipEdit). Each guards a bug fixed in Phase 0, see docs/AUDIT.md.

#include "check.h"
#include "core/AudioConvert.h"
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
    return check::failures();
}
