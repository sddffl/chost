// chost-test-engine: the parts that need a running audio engine (patchbay mode, e.g. JACK's dummy backend).
//   chost-test-engine <driver> <48 kHz tone.wav> <sound font .sf2> <notes.mid> <work dir>
// Seeking (HostEngine::relocate), the Scissors' second track (Session::duplicateTrack) with undo, and the
// session tempo reaching tempo-synced plugins live. Leaves A.chost and B.chost (a project before and after
// a cut) in the work dir for tests/check_render.py's null test. See docs/AUDIT.md, Phase 0.

#include "check.h"
#include "core/AudioConvert.h"
#include "core/AutoStrip.h"
#include "core/ClipEdit.h"
#include "core/HostEngine.h"
#include "core/Session.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

// every time carla is told a player's file (linked with -Wl,--wrap=carla_set_custom_data)
static int gFileSets = 0;
extern "C" void __real_carla_set_custom_data(CarlaHostHandle, uint, const char*, const char*, const char*);
extern "C" void __wrap_carla_set_custom_data(CarlaHostHandle h, uint id, const char* type, const char* key, const char* value)
{
    if (std::strcmp(key, "file") == 0)
        ++gFileSets;
    __real_carla_set_custom_data(h, id, type, key, value);
}

namespace {

constexpr double kPi = 3.14159265358979323846;
const std::string kGain = "lv2:http://kxstudio.sf.net/carla/plugins/audiogain_s";

float listen(chost::HostEngine& host, const chost::Session& s, int ms)
{
    float peak = 0;
    for (auto end = steady_clock::now() + milliseconds(ms); steady_clock::now() < end;)
    {
        host.idle();
        const auto [l, r] = host.outputPeaks(s.master());
        peak = std::max({peak, l, r});
        std::this_thread::sleep_for(milliseconds(10));
    }
    return peak;
}

void testSeek(chost::HostEngine& host, chost::Session& s, const std::string& tone, const std::string& sf2, const std::string& mid)
{
    std::printf("seeking (HostEngine::relocate): 2 audio tracks, 1 set aside for undo, 1 MIDI track\n");
    for (int i = 0; i < 3; ++i)
        s.setTrackFile(s.addTrack("tone"), tone);
    s.removeTrack(2); // set aside: an inactive player
    const int midi = s.addTrack("midi", chost::TrackKind::Instrument);
    s.setInstrument(midi, "sf2:" + sf2);
    s.setTrackFile(midi, mid);
    const double rate = host.sampleRate();

    host.relocate(0); // as chost does before playing
    host.play();
    CHECK(listen(host, s, 600) > 0.01f, "plays");

    std::this_thread::sleep_for(milliseconds(150));
    gFileSets = 0;
    host.relocate(static_cast<uint64_t>(0.1 * rate));
    CHECK(gFileSets == 2, "one seek reloads the 2 active audio players only (%d reloads)", gFileSets);
    CHECK(listen(host, s, 400) > 0.01f, "sound right after a seek backwards");

    std::this_thread::sleep_for(milliseconds(150));
    gFileSets = 0;
    const auto start = steady_clock::now();
    for (int i = 0; i < 60; ++i)
    {
        host.relocate(static_cast<uint64_t>((1.5 - i * 0.02) * rate));
        if (i % 2 == 0)
            host.idle();
        std::this_thread::sleep_for(milliseconds(16));
    }
    const double elapsed = duration<double, std::milli>(steady_clock::now() - start).count();
    const int during = gFileSets;
    const float after = listen(host, s, 400);
    // at most one refresh of both players per 100 ms, plus the first; before the fix: 4 per seek, 240
    const int bound = 2 * (static_cast<int>(elapsed / 100.0) + 2);
    CHECK(during <= bound, "scrubbing (60 seeks in %.0f ms) is throttled: %d reloads, at most %d", elapsed, during, bound);
    CHECK(gFileSets > during, "a last refresh once scrubbing stops");
    CHECK(after > 0.01f, "sound after scrubbing");
    host.pause();
}

void testDuplicate(chost::HostEngine& host, chost::Session& s, const fs::path& dir)
{
    std::printf("the Scissors' second track (Session::duplicateTrack) and its undo\n");
    const uint32_t rate = 48000, frames = 4 * rate;
    std::vector<float> sig(frames * 2);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    for (uint32_t i = 0; i < frames; ++i)
    {
        sig[2 * i] = static_cast<float>(0.4 * std::sin(2 * kPi * 220 * i / rate)) + noise(rng);
        sig[2 * i + 1] = static_cast<float>(0.4 * std::sin(2 * kPi * 330 * i / rate)) + noise(rng);
    }
    const std::string in = (dir / "lead.wav").u8string(), before = (dir / "lead_1.wav").u8string(), after = (dir / "lead_2.wav").u8string();
    std::string error;
    chost::writeWavFile(in, sig, 2, rate, error);
    chost::splitAudioFile(in, 1.7, before, after, error);

    const int bus = s.addTrack("Reverb", chost::TrackKind::Bus);
    s.addInsert(bus, kGain);
    const int t = s.addTrack("lead");
    s.setTrackFile(t, in);
    s.setVolume(t, 0.8f);
    s.setPan(t, -0.3f);
    s.setColour(t, 0x3366ff);
    s.setRole(t, "vocals");
    const bool inserts = s.addInsert(t, kGain);
    CHECK(inserts && s.addInsert(t, kGain), "two inserts on the track");
    const uint g1 = s.tracks()[t].inserts[0], g2 = s.tracks()[t].inserts[1];
    host.setParameter(g1, static_cast<uint32_t>(host.findParameter(g1, "Gain")), 0.37f);
    host.setParameter(g2, static_cast<uint32_t>(host.findParameter(g2, "Gain")), 0.0f);
    host.setActive(g2, false); // bypassed: a copy that lost this would silence the second half
    s.reroute();
    s.setSend(t, s.tracks()[bus].uid, 0.5f);
    s.save((dir / "A.chost").u8string());

    const chost::SessionState previous = s.state();
    const int copy = s.duplicateTrack(t, "lead (after)");
    CHECK(copy == t + 1, "the copy comes right after the track");
    s.setTrackFile(t, before);
    s.setTrackFile(copy, after);
    const chost::Track& o = s.tracks()[t];
    const chost::Track& c = s.tracks()[copy];
    CHECK(c.volume == o.volume && c.pan == o.pan && c.colour == o.colour && c.role == o.role && c.output == o.output, "fader, pan, colour, role and output copied");
    CHECK(c.sends.size() == 1 && c.sends[0].bus == o.sends[0].bus && c.sends[0].level == 0.5f, "the send copied");
    CHECK(c.inserts.size() == 2 && c.inserts[0] != o.inserts[0] && c.inserts[1] != o.inserts[1], "inserts are new plugins, not shared");
    const float gain = host.parameterValue(c.inserts[0], static_cast<uint32_t>(host.findParameter(c.inserts[0], "Gain")));
    CHECK(std::fabs(gain - 0.37f) < 1e-4f, "insert settings copied (gain %.3f)", gain);
    CHECK(host.slot(c.inserts[0]).active && !host.slot(c.inserts[1]).active, "bypass state copied");
    s.save((dir / "B.chost").u8string());

    s.restore(previous);
    CHECK(s.state() == previous, "undo puts back the state before the cut");
    const size_t plugins = host.pluginCount();
    CHECK(s.duplicateTrack(1000, "x") < 0 && host.pluginCount() == plugins, "a bad track is refused and leaves no plugins");
}

// the period of carla's tempo-synced LFO (sawtooth, 1 beat) in transport time: 60 / BPM when it hears the tempo
double lfoPeriod(chost::HostEngine& host, uint lfo, int out)
{
    std::vector<std::pair<double, float>> v;
    for (int i = 0; i < 2000; ++i)
    {
        v.push_back({host.transportFrame() / host.sampleRate(), host.parameterValue(lfo, static_cast<uint32_t>(out))});
        std::this_thread::sleep_for(milliseconds(2));
        if (i % 15 == 0)
            host.idle();
    }
    std::vector<double> drops;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i].second < v[i - 1].second - 0.5f)
            drops.push_back(v[i].first);
    return drops.size() < 3 ? 0.0 : (drops.back() - drops[1]) / static_cast<double>(drops.size() - 2);
}

void testTempo(chost::HostEngine& host, chost::Session& s)
{
    std::printf("the session tempo reaches tempo-synced plugins (live)\n");
    host.addInternalPlugin("lfo", "lfo");
    const uint lfo = host.pluginCount() - 1;
    host.setParameter(lfo, 0, 2.0f); // sawtooth
    const int out = host.findParameter(lfo, "LFO Out");
    host.relocate(0);
    host.play();
    for (double bpm : {90.0, 150.0})
    {
        s.setTempo(bpm);
        const double period = lfoPeriod(host, lfo, out), want = 60.0 / bpm;
        CHECK(std::fabs(period - want) < 0.03 * want, "session at %.0f BPM: the plugin hears %.1f BPM", bpm, period > 0 ? 60.0 / period : 0.0);
        CHECK(std::fabs(host.transportTempo() - bpm) < 1e-6, "carla's transport says %.1f BPM", host.transportTempo());
    }
    host.pause();
}

// The automatic channel strip on a live engine: carla's own gain as the trim ("internal:" spec), its value
// read from the real plugin's controls, the whole strip put on in one rewiring (Session::RoutingHold), the
// trim's boost heard and the fader taking it back.
void testAutoStrip(chost::HostEngine& host, chost::Session& s, const std::string& tone)
{
    std::printf("the automatic channel strip (AutoStrip.h) on a live track\n");
    const int t = s.addTrack("hh");
    s.setTrackFile(t, tone);
    s.setRole(t, "hh");
    host.relocate(0);
    host.play();
    // JACK's dummy backend runs at its own pace: a change reaches the meters some hundred ms later, so
    // every level is read after the one before it has settled
    auto settled = [&] { listen(host, s, 700); return listen(host, s, 400); };
    const float plain = settled();

    const std::vector<chost::StripSlot> strip = chost::channelStrip("hh", 6.0);
    const size_t before = host.pluginCount();
    {
        chost::Session::RoutingHold hold(s);
        for (const chost::StripSlot& slot : strip)
        {
            const std::string spec = chost::stripPlugin(slot.kind, {}); // no plugins of the user's: only the trim
            if (spec.empty())
                continue;
            CHECK(s.addInsert(t, spec), "insert %s", spec.c_str());
            const uint id = s.tracks()[t].inserts.back();
            host.renamePlugin(id, "Trim +6.0 dB");
            for (const auto& [index, value] : chost::stripValues(slot, host.parameters(id)))
                host.setParameter(id, index, value);
        }
        CHECK(s.routingHeld(), "the rewiring is held while the strip goes on");
    }
    CHECK(!s.routingHeld() && host.pluginCount() == before + 1 && s.tracks()[t].inserts.size() == 1,
          "one insert (the trim; no EQ without an EQ plugin), rewired once when the hold ends");
    const uint trim = s.tracks()[t].inserts[0];
    const float gain = host.parameters(trim)[static_cast<size_t>(host.findParameter(trim, "Gain"))].value;
    CHECK(std::fabs(gain - 1.9953f) < 1e-3f && host.slot(trim).name == "Trim +6.0 dB", "carla's gain set to x%.3f (+6 dB), named \"%s\"", gain, host.slot(trim).name.c_str());
    const float boosted = settled();
    CHECK(std::fabs(boosted / plain - 1.9953f) < 0.05f, "heard +%.2f dB louder through the trim", 20 * std::log10(boosted / plain));
    s.setVolume(t, s.tracks()[t].volume * std::pow(10.0f, -6.0f / 20.0f));
    const float back = settled();
    CHECK(std::fabs(back / plain - 1.0f) < 0.05f, "the fader takes it back: %+.2f dB from before", 20 * std::log10(back / plain));
    host.pause();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 6)
    {
        std::printf("usage: chost-test-engine <driver> <48 kHz tone.wav> <sound font .sf2> <notes.mid> <work dir>\n");
        return 2;
    }
    const fs::path dir = fs::u8path(argv[5]);
    fs::create_directories(dir);
    auto run = [&](auto test) {
        chost::HostEngine host;
        if (!host.start(argv[1], chost::HostEngine::Mode::Patchbay))
        {
            std::printf("  FAIL  no engine with the %s driver: %s\n", argv[1], host.lastError().c_str());
            ++check::count();
            return;
        }
        chost::Session s(host);
        s.init();
        s.setKeepRemoved(true); // as the GUI does, for undo
        test(host, s);
        host.stop();
    };
    run([&](chost::HostEngine& h, chost::Session& s) { testSeek(h, s, argv[2], argv[3], argv[4]); });
    run([&](chost::HostEngine& h, chost::Session& s) { testDuplicate(h, s, dir); });
    run([&](chost::HostEngine& h, chost::Session& s) { testTempo(h, s); });
    run([&](chost::HostEngine& h, chost::Session& s) { testAutoStrip(h, s, argv[2]); });
    return check::failures();
}
