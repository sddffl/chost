// chost-test-offline: chost-render's offline engine.
//   chost-test-offline
//       the session tempo in an export: carla's tempo-synced LFO (sawtooth, 1 beat) must repeat every
//       60 / BPM seconds of rendered audio
//   chost-test-offline make-project <dir> <tracks>
//       saves <dir>/heavy.chost for tests/check_render.py: <dir>/t00.wav ... as tracks, each with two
//       inserts and a send to a bus with an insert. Made offline, because building a big project quickly
//       in the live engine crashes carla 2.5 too often to test with (docs/AUDIT.md, finding #20).

#include "check.h"
#include "core/HostEngine.h"
#include "core/Session.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int makeProject(const std::string& dir, int count)
{
    const std::string gain = "lv2:http://kxstudio.sf.net/carla/plugins/audiogain_s";
    chost::HostEngine host;
    if (!host.startOffline(48000, 256))
    {
        std::printf("no offline engine: %s\n", host.lastError().c_str());
        return 1;
    }
    chost::Session s(host);
    s.init();
    const int bus = s.addTrack("Reverb", chost::TrackKind::Bus);
    bool ok = bus >= 0 && s.addInsert(bus, gain);
    for (int k = 0; k < count && ok; ++k)
    {
        char file[32];
        std::snprintf(file, sizeof file, "/t%02d.wav", k);
        const int t = s.addTrack("track");
        ok = t >= 0 && s.setTrackFile(t, dir + file) && s.addInsert(t, gain) && s.addInsert(t, gain) &&
             s.setSend(t, s.tracks()[static_cast<size_t>(bus)].uid, 0.2f);
    }
    ok = ok && s.save(dir + "/heavy.chost");
    std::printf(ok ? "PROJECT %s/heavy.chost\n" : "ERROR %s\n", ok ? dir.c_str() : s.lastError().c_str());
    host.stop();
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::strcmp(argv[1], "make-project") == 0)
        return makeProject(argv[2], std::atoi(argv[3]));

    chost::HostEngine host;
    if (!host.startOffline(48000, 256))
    {
        std::printf("  FAIL  no offline engine: %s\n", host.lastError().c_str());
        return 1;
    }
    chost::Session s(host);
    s.init();
    host.addInternalPlugin("lfo", "lfo");
    const uint lfo = host.pluginCount() - 1;
    host.setParameter(lfo, 0, 2.0f); // sawtooth
    const uint32_t out = static_cast<uint32_t>(host.findParameter(lfo, "LFO Out"));
    std::printf("the session tempo reaches tempo-synced plugins (offline)\n");
    std::vector<float> l(256), r(256);
    uint64_t frame = 0;
    for (double bpm : {90.0, 120.0, 150.0})
    {
        s.setTempo(bpm);
        std::vector<double> drops;
        float last = 0;
        for (int b = 0; b < 4 * 48000 / 256; ++b, frame += 256)
        {
            host.renderBlock(l.data(), r.data(), 256);
            const float v = host.parameterValue(lfo, out);
            if (v < last - 0.5f)
                drops.push_back(frame / 48000.0);
            last = v;
        }
        const double period = drops.size() < 3 ? 0.0 : (drops.back() - drops[1]) / static_cast<double>(drops.size() - 2);
        const double want = 60.0 / bpm;
        CHECK(std::fabs(period - want) < 0.03 * want, "session at %.0f BPM: the plugin hears %.1f BPM", bpm, period > 0 ? 60.0 / period : 0.0);
    }
    host.stop();
    return check::failures();
}
