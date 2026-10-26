// chost-cli - command line front end for the chost plugin host.
//
// Usage:
//   chost-cli drivers
//   chost-cli scan [--discover]       --discover: ask carla's discovery tool what every VST file holds
//   chost-cli run [--driver NAME] [--file AUDIO] [--plugin SPEC]... [--set N:P=V]... [--seconds S] [--save F]
//   chost-cli mix [--driver NAME] [--track AUDIO]... [--midi FILE.mid@SPEC]... [--insert T:SPEC]... [--mute T] [--solo T] [--volume T=V] [--pan T=P]
//                 [--remove-track T] [--remove-insert T:I] [--seconds S] [--save F.chost] [--open F.chost]
//
// Plugin SPEC:
//   lv2:<URI>            e.g. lv2:urn:zamaudio:ZamComp
//   vst3:<path.vst3>     e.g. vst3:/usr/lib/vst3/ZamComp.vst3
//   vst2:<path>, ladspa:<path>:<label>, sf2:<file>, sfz:<file>
//
// Without --seconds, run drops into an interactive shell (type "help").

#include "core/AudioConvert.h"
#include "core/Chords.h"
#include "core/ClipEdit.h"
#include "core/Click.h"
#include "core/HostEngine.h"
#include "core/Loudness.h"
#include "core/Scales.h"
#include "core/Score.h"
#include "core/Session.h"

#include <algorithm>
#include <filesystem>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using chost::HostEngine;
using chost::HostEvent;

static std::atomic<bool> gQuit{false};

static void onSignal(int) { gQuit = true; }

static int cmdDrivers()
{
    for (const std::string& name : chost::audioDrivers())
        std::printf("%s\n", name.c_str());
    return 0;
}

// Runs a program and returns what it printed (stdout and stderr).
static std::string capture(const std::vector<std::string>& args)
{
    std::string command;
    for (const std::string& a : args)
        command += "\"" + a + "\" ";
    command += "2>&1";
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd.exe strips the outer quotes
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr)
        return {};
    std::string out;
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0)
        out.append(buffer, n);
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

static int cmdScan(int argc, char* argv[])
{
    const bool discover = argc > 0 && std::string(argv[0]) == "--discover";
    const std::string tool = chost::discoveryTool();
    if (discover && tool.empty())
        std::fprintf(stderr, "carla-discovery not found, VST files are listed by name\n");
    for (const chost::PluginEntry& file : chost::scanPlugins())
    {
        std::vector<chost::PluginEntry> entries{file};
        if (discover && !tool.empty() && (file.format == "VST2" || file.format == "VST3"))
        {
            std::vector<std::string> args{tool};
            for (const std::string& a : chost::discoveryArguments(file))
                args.push_back(a);
            std::vector<chost::PluginEntry> found = chost::parseDiscovery(capture(args), file);
            if (found.empty() && !chost::discoveryTool(true).empty())
            {
                args.front() = chost::discoveryTool(true); // maybe an old 32-bit plugin
                found = chost::parseDiscovery(capture(args), file, true);
            }
            if (!found.empty())
                entries = found;
        }
        for (const chost::PluginEntry& e : entries)
            std::printf("%-5s %-32s %-20s %-10s %s\n", e.format.c_str(), e.name.c_str(), e.maker.c_str(),
                        e.isSynth ? "instrument" : e.category.c_str(), e.spec.c_str());
    }
    return 0;
}

// "N:P=V" -> plugin N, parameter P (index, name or symbol), value V
static bool setParam(HostEngine& host, const std::string& expr)
{
    const auto colon = expr.find(':');
    const auto eq = expr.find('=');
    if (colon == std::string::npos || eq == std::string::npos || eq < colon)
    {
        std::fprintf(stderr, "bad parameter expression: %s (want N:P=V)\n", expr.c_str());
        return false;
    }
    const uint id = std::stoul(expr.substr(0, colon));
    const std::string param = expr.substr(colon + 1, eq - colon - 1);
    const int index = host.findParameter(id, param);
    if (index < 0)
    {
        std::fprintf(stderr, "plugin %u has no parameter '%s'\n", id, param.c_str());
        return false;
    }
    host.setParameter(id, static_cast<uint32_t>(index), std::stof(expr.substr(eq + 1)));
    return true;
}

static void listRack(const HostEngine& host)
{
    for (uint id = 0; id < host.pluginCount(); ++id)
    {
        const chost::RackSlot s = host.slot(id);
        const auto [l, r] = host.outputPeaks(id);
        std::printf("[%u] %s (%s)%s peak L/R: %.3f / %.3f\n", id, s.name.c_str(), s.maker.c_str(),
                    s.active ? "" : " [bypassed]", l, r);
    }
}

static void listParams(const HostEngine& host, uint id)
{
    for (const chost::ParamInfo& p : host.parameters(id))
        std::printf("  %2u %-24s = %-10g [%g .. %g] %s%s\n", p.index, p.name.c_str(), p.value,
                    p.min, p.max, p.unit.c_str(), p.isOutput ? " (output)" : "");
}

static void printHelp()
{
    std::printf(
        "commands:\n"
        "  list                     plugins in the rack, with output peaks\n"
        "  add SPEC                 load a plugin (lv2:URI, vst3:PATH, ...)\n"
        "  file PATH                load an audio file player at the start of the rack\n"
        "  remove N                 remove plugin N\n"
        "  params N                 show parameters of plugin N\n"
        "  set N:P=V                set parameter P (index/name/symbol) of plugin N\n"
        "  bypass N on|off          bypass plugin N\n"
        "  ui N on|off              show the plugin's own editor window\n"
        "  save FILE / load FILE    save or load a .carxp project\n"
        "  quit\n");
}

static bool report(bool ok, const HostEngine& host)
{
    if (!ok)
        std::fprintf(stderr, "error: %s\n", host.lastError().c_str());
    return ok;
}

static void runShell(HostEngine& host)
{
    // stdin is read on its own thread so the engine idle loop keeps running
    std::atomic<bool> lineReady{false};
    std::string line;

    std::thread reader([&] {
        std::string buf;
        std::printf("> ");
        std::fflush(stdout);
        while (!gQuit && std::getline(std::cin, buf))
        {
            line = buf;
            lineReady = true;
            while (lineReady && !gQuit)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::printf("> ");
            std::fflush(stdout);
        }
        gQuit = true;
    });

    while (!gQuit && host.isRunning())
    {
        host.idle();

        if (lineReady)
        {
            std::istringstream in(line);
            std::string cmd, a, b;
            in >> cmd >> a >> b;

            if (cmd == "quit" || cmd == "exit")      gQuit = true;
            else if (cmd == "help")                  printHelp();
            else if (cmd == "list")                  listRack(host);
            else if (cmd == "add")                   report(host.addPlugin(a), host);
            else if (cmd == "file")                  report(host.loadAudioFile(a), host);
            else if (cmd == "remove" && !a.empty())  report(host.removePlugin(std::stoul(a)), host);
            else if (cmd == "params" && !a.empty())  listParams(host, std::stoul(a));
            else if (cmd == "set")                   setParam(host, a);
            else if (cmd == "bypass" && !a.empty())  host.setActive(std::stoul(a), b == "off");
            else if (cmd == "ui" && !a.empty())      host.showCustomUi(std::stoul(a), b != "off");
            else if (cmd == "save")                  report(host.saveProject(a), host);
            else if (cmd == "load")                  report(host.loadProject(a), host);
            else if (!cmd.empty())                   std::printf("unknown command, type 'help'\n");

            lineReady = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    reader.detach();
}

static int cmdRun(int argc, char* argv[])
{
    std::string driver = chost::defaultAudioDriver();
    std::string file, savePath;
    std::vector<std::string> plugins, sets;
    double seconds = -1.0;

    for (int i = 0; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--driver" && hasValue)        driver = argv[++i];
        else if (arg == "--file" && hasValue)     file = argv[++i];
        else if (arg == "--plugin" && hasValue)   plugins.emplace_back(argv[++i]);
        else if (arg == "--set" && hasValue)      sets.emplace_back(argv[++i]);
        else if (arg == "--seconds" && hasValue)  seconds = std::stod(argv[++i]);
        else if (arg == "--save" && hasValue)     savePath = argv[++i];
        else
        {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    HostEngine host;
    host.setEventCallback([](HostEvent event, uint id, int, float, const std::string& text) {
        if (event == HostEvent::PluginAdded)
            std::printf("+ plugin %u: %s\n", id, text.c_str());
        else if (event == HostEvent::Error)
            std::fprintf(stderr, "engine error: %s\n", text.c_str());
        else if (event == HostEvent::EngineStopped)
            gQuit = true;
    });

    if (!host.start(driver))
    {
        std::fprintf(stderr, "engine init failed (%s): %s\n", driver.c_str(), host.lastError().c_str());
        return 1;
    }
    std::printf("engine running: driver=%s buffer=%u rate=%.0f\n", driver.c_str(), host.bufferSize(), host.sampleRate());

    bool ok = true;
    for (const std::string& p : plugins)
        ok = report(host.addPlugin(p), host) && ok;
    if (!file.empty())
        ok = report(host.loadAudioFile(file), host) && ok;
    for (const std::string& s : sets)
        ok = setParam(host, s) && ok;
    host.play();

    if (seconds >= 0.0)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!gQuit && host.isRunning() && std::chrono::steady_clock::now() < until)
        {
            host.idle();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        listRack(host);
    }
    else
    {
        printHelp();
        runShell(host);
    }

    if (!savePath.empty())
        ok = report(host.saveProject(savePath), host) && ok;

    host.stop();
    return ok ? 0 : 1;
}

// Multitrack: every --track becomes a track summed into the master bus; prints the master's peak level.
static int cmdMix(int argc, char* argv[])
{
    std::string driver = chost::defaultAudioDriver();
    std::string savePath, openPath;
    std::vector<std::string> files, midis, inserts, volumes, pans;
    std::vector<int> mutes, solos, removeTracks;
    std::vector<std::string> removeInserts, buses, outputs, sends;
    double seconds = 3.0, click = -1.0, tempo = 0.0;
    std::string clickSpec;

    for (int i = 0; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--driver" && hasValue)        driver = argv[++i];
        else if (arg == "--track" && hasValue)    files.emplace_back(argv[++i]);
        else if (arg == "--midi" && hasValue)     midis.emplace_back(argv[++i]);
        else if (arg == "--insert" && hasValue)   inserts.emplace_back(argv[++i]);
        else if (arg == "--mute" && hasValue)     mutes.push_back(std::stoi(argv[++i]));
        else if (arg == "--solo" && hasValue)     solos.push_back(std::stoi(argv[++i]));
        else if (arg == "--volume" && hasValue)   volumes.emplace_back(argv[++i]);
        else if (arg == "--pan" && hasValue)      pans.emplace_back(argv[++i]);
        else if (arg == "--seconds" && hasValue)  seconds = std::stod(argv[++i]);
        else if (arg == "--save" && hasValue)     savePath = argv[++i];
        else if (arg == "--open" && hasValue)     openPath = argv[++i];
        else if (arg == "--remove-track" && hasValue)  removeTracks.push_back(std::stoi(argv[++i]));
        else if (arg == "--remove-insert" && hasValue) removeInserts.emplace_back(argv[++i]);
        else if (arg == "--bus" && hasValue)      buses.emplace_back(argv[++i]);
        else if (arg == "--output" && hasValue)   outputs.emplace_back(argv[++i]);
        else if (arg == "--send" && hasValue)     sends.emplace_back(argv[++i]);
        else if (arg == "--click" && hasValue)    click = std::stod(argv[++i]);
        else if (arg == "--tempo" && hasValue)    tempo = std::stod(argv[++i]);
        else if (arg == "--click-sound" && hasValue) clickSpec = argv[++i];
        else
        {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    HostEngine host;
    host.setEventCallback([](HostEvent event, uint, int, float, const std::string& text) {
        if (event == HostEvent::Error)
            std::fprintf(stderr, "engine error: %s\n", text.c_str());
        else if (event == HostEvent::EngineStopped)
            gQuit = true;
    });
    if (!host.start(driver, HostEngine::Mode::Patchbay))
    {
        // the Dummy driver only has the rack: the tracks are kept but not mixed
        if (!host.start(driver, HostEngine::Mode::Rack))
        {
            std::fprintf(stderr, "engine init failed (%s): %s\n", driver.c_str(), host.lastError().c_str());
            return 1;
        }
        std::printf("%s has no patchbay, tracks are not mixed\n", driver.c_str());
    }

    chost::Session session(host);
    bool ok = true;
    if (!openPath.empty())
    {
        if (!session.load(openPath))
        {
            std::fprintf(stderr, "open failed: %s\n", session.lastError().c_str());
            return 1;
        }
    }
    else if (!session.init())
    {
        std::fprintf(stderr, "session init failed: %s\n", session.lastError().c_str());
        return 1;
    }

    for (const std::string& f : files)
    {
        const int t = session.addTrack("Audio " + std::to_string(session.tracks().size() + 1));
        if (t < 0 || !session.setTrackFile(t, f))
        {
            std::fprintf(stderr, "track %s: %s\n", f.c_str(), session.lastError().c_str());
            ok = false;
        }
    }
    // FILE.mid@SPEC: an instrument track playing FILE.mid through the instrument SPEC
    for (const std::string& m : midis)
    {
        const auto at = m.find('@');
        const int t = session.addTrack("MIDI " + std::to_string(session.tracks().size() + 1), chost::TrackKind::Instrument);
        bool good = t >= 0 && session.setTrackFile(t, m.substr(0, at));
        if (good && at != std::string::npos)
            good = session.setInstrument(t, m.substr(at + 1));
        if (!good)
        {
            std::fprintf(stderr, "midi track %s: %s\n", m.c_str(), session.lastError().c_str());
            ok = false;
        }
    }
    for (const std::string& name : buses)
        if (session.addTrack(name, chost::TrackKind::Bus) < 0)
        {
            std::fprintf(stderr, "bus %s: %s\n", name.c_str(), session.lastError().c_str());
            ok = false;
        }
    // T=B: track T's fader feeds bus B (track indices); T=B:LEVEL: track T sends to bus B
    auto busUid = [&](const std::string& text) -> chost::Uid {
        const int b = std::stoi(text);
        return b >= 0 && b < static_cast<int>(session.tracks().size()) ? session.tracks()[static_cast<size_t>(b)].uid : 0;
    };
    for (const std::string& o : outputs)
        if (const auto eq = o.find('='); eq == std::string::npos || !session.setOutput(std::stoi(o.substr(0, eq)), busUid(o.substr(eq + 1))))
        {
            std::fprintf(stderr, "output %s: %s\n", o.c_str(), session.lastError().c_str());
            ok = false;
        }
    for (const std::string& snd : sends)
    {
        const auto eq = snd.find('='), colon = snd.find(':');
        if (eq == std::string::npos || colon == std::string::npos ||
            !session.setSend(std::stoi(snd.substr(0, eq)), busUid(snd.substr(eq + 1, colon - eq - 1)), std::stof(snd.substr(colon + 1))))
        {
            std::fprintf(stderr, "send %s: %s\n", snd.c_str(), session.lastError().c_str());
            ok = false;
        }
    }
    for (const std::string& ins : inserts)
    {
        const auto colon = ins.find(':');
        if (colon == std::string::npos || !session.addInsert(std::stoi(ins.substr(0, colon)), ins.substr(colon + 1)))
        {
            std::fprintf(stderr, "insert %s: %s\n", ins.c_str(), session.lastError().c_str());
            ok = false;
        }
    }
    for (const std::string& r : removeInserts)
        if (const auto colon = r.find(':'); colon != std::string::npos)
            ok = session.removeInsert(std::stoi(r.substr(0, colon)), std::stoi(r.substr(colon + 1))) && ok;
    for (int t : removeTracks)
        ok = session.removeTrack(t) && ok;
    for (int t : mutes)
        session.setMuted(t, true);
    for (int t : solos)
        session.setSolo(t, true);
    for (const std::string& v : volumes)
        if (const auto eq = v.find('='); eq != std::string::npos)
            session.setVolume(std::stoi(v.substr(0, eq)), std::stof(v.substr(eq + 1)));
    for (const std::string& v : pans)
        if (const auto eq = v.find('='); eq != std::string::npos)
            session.setPan(std::stoi(v.substr(0, eq)), std::stof(v.substr(eq + 1)));

    for (size_t i = 0; i < session.tracks().size(); ++i)
    {
        const chost::Track& t = session.tracks()[i];
        std::printf("track %zu \"%s\": source=%d instrument=%d inserts=%zu fader=%u file=%s%s output=%d sends=%zu\n", i, t.name.c_str(),
                    t.source, t.instrument, t.inserts.size(), t.fader, t.file.c_str(), t.kind == chost::TrackKind::Bus ? " bus" : "",
                    session.trackOf(t.output), t.sends.size());
    }

    if (tempo > 0.0)
        session.setTempo(tempo);
    if (click >= 0.0)
    {
        // the metronome at that volume, played by the given (or the first) MIDI track's instrument spec
        const std::string clickFile = (std::filesystem::temp_directory_path() / "chost-cli-click.mid").u8string();
        std::string error, spec = clickSpec;
        if (spec.empty() && !midis.empty() && midis.front().find('@') != std::string::npos)
            spec = midis.front().substr(midis.front().find('@') + 1);
        if (!chost::writeClickTrack(clickFile, session.tempo(), 4, seconds + 30.0, error) || !session.setClick(clickFile, spec))
        {
            std::fprintf(stderr, "click: %s%s\n", error.c_str(), session.lastError().c_str());
            ok = false;
        }
        session.setClickOn(true);
        session.setClickVolume(static_cast<float>(click));
    }

    host.relocate(0);
    host.play();
    float peak = 0.0f, left = 0.0f, right = 0.0f, clickPeak = 0.0f, clickLate = 0.0f;
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!gQuit && host.isRunning() && std::chrono::steady_clock::now() < until)
    {
        host.idle();
        const auto [l, r] = host.outputPeaks(session.master());
        peak = std::max({peak, l, r});
        left = std::max(left, l);
        right = std::max(right, r);
        if (session.clickGain() >= 0)
        {
            const auto [cl, cr] = host.outputPeaks(static_cast<uint>(session.clickGain()));
            clickPeak = std::max({clickPeak, cl, cr});
            if (host.transportFrame() > static_cast<uint64_t>(2.5 * host.sampleRate()))
                clickLate = std::max({clickLate, cl, cr}); // still clicking after the first bar
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("master peak %.3f (left %.3f, right %.3f)\n", peak, left, right);
    if (click >= 0.0)
        std::printf("click peak %.3f, after 2.5 s %.3f\n", clickPeak, clickLate);
    for (const chost::Track& t : session.tracks())
        if (t.instrument >= 0)
        {
            const auto programs = host.midiPrograms(static_cast<uint>(t.instrument));
            const int current = host.currentMidiProgram(static_cast<uint>(t.instrument));
            std::printf("instrument of \"%s\": %s, program %s\n", t.name.c_str(), host.slot(static_cast<uint>(t.instrument)).name.c_str(),
                        current >= 0 && current < static_cast<int>(programs.size()) ? programs[current].name.c_str() : "-");
        }

    if (!savePath.empty() && !session.save(savePath))
    {
        std::fprintf(stderr, "save failed: %s\n", session.lastError().c_str());
        ok = false;
    }
    host.stop();
    return ok ? 0 : 1;
}

// the chord pad's harmony, without the GUI: the pads of a key and what fits after a chord, or a
// progression written as a MIDI part
// chost-cli clip: a clip edit on a file, the way the arrangement does it
// chost-cli loudness FILE...: integrated loudness (LUFS) and sample peak of each file
static int cmdLoudness(int argc, char* argv[])
{
    if (argc < 1)
    {
        std::fprintf(stderr, "usage: chost-cli loudness FILE...\n");
        return 2;
    }
    int failed = 0;
    for (int i = 0; i < argc; ++i)
    {
        std::vector<float> samples;
        chost::AudioFileInfo info;
        if (!chost::readAudioFile(argv[i], samples, info))
        {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            ++failed;
            continue;
        }
        const chost::Loudness l = chost::measureLoudness(samples, info.channels, info.sampleRate);
        std::printf("LOUDNESS %s\t%.1f LUFS\tpeak %.1f dBFS\n", argv[i], l.integrated, l.peak);
    }
    return failed ? 1 : 0;
}

static int cmdClip(int argc, char* argv[])
{
    std::string in, out, kindText;
    chost::ClipEdit edit;
    for (int i = 0; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--input")
            in = value();
        else if (a == "--output")
            out = value();
        else if (a == "--edit")
            kindText = value();
        else if (a == "--start")
            edit.start = std::atof(value().c_str());
        else if (a == "--end")
            edit.end = std::atof(value().c_str());
        else if (a == "--seconds")
            edit.seconds = std::atof(value().c_str());
        else if (a == "--db")
            edit.db = std::atof(value().c_str());
    }
    bool ok = false;
    edit.kind = chost::parseClipEditKind(kindText, ok);
    if (!ok || in.empty() || out.empty())
    {
        std::fprintf(stderr, "--input, --output and --edit move|silence|keep|fade_in|fade_out|gain|normalize are needed\n");
        return 2;
    }
    std::string error, note;
    const bool midi = in.size() > 4 && (in.compare(in.size() - 4, 4, ".mid") == 0 || in.compare(in.size() - 4, 4, ".MID") == 0);
    if (midi)
    {
        chost::MidiClip clip;
        if (!chost::readMidiFile(in, clip, error) || !chost::editMidiClip(clip, edit, error) || !chost::writeMidiFile(out, clip, error))
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("CLIP %s\t%zu notes\t%.3f s\n", out.c_str(), clip.notes.size(), clip.seconds);
        return 0;
    }
    if (!chost::editAudioFile(in, out, edit, error, &note))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::vector<float> samples;
    chost::AudioFileInfo info;
    chost::readAudioFile(out, samples, info);
    std::printf("CLIP %s\t%.3f s\n", out.c_str(), static_cast<double>(info.frames) / info.sampleRate);
    // the peak of each second, to see what the edit did
    for (uint64_t s = 0; s * info.sampleRate < info.frames; ++s)
    {
        float peak = 0.0f;
        for (uint64_t f = s * info.sampleRate; f < std::min<uint64_t>(info.frames, (s + 1) * info.sampleRate); ++f)
            for (uint32_t c = 0; c < info.channels; ++c)
                peak = std::max(peak, std::abs(samples[f * info.channels + c]));
        std::printf("SECOND %llu\t%.3f\n", static_cast<unsigned long long>(s), peak);
    }
    if (!note.empty())
        std::printf("NOTE %s\n", note.c_str());
    return 0;
}

// chost-cli score: MIDI files as sheet music, written as MusicXML
static int cmdScore(int argc, char* argv[])
{
    std::vector<chost::ScoreInput> inputs;
    chost::ScoreOptions options;
    std::string output, staff = "auto";
    std::string error;
    for (int i = 0; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--staff")
            staff = value();
        else if (a == "--midi")
        {
            chost::ScoreInput input;
            const std::string file = value();
            if (!chost::readMidiFile(file, input.clip, error))
            {
                std::fprintf(stderr, "%s: %s\n", file.c_str(), error.c_str());
                return 1;
            }
            input.name = input.clip.name.empty() ? file.substr(file.find_last_of("/\\") + 1) : input.clip.name;
            input.staff = staff == "treble" ? chost::StaffMode::Treble : staff == "bass" ? chost::StaffMode::Bass
                        : staff == "grand"  ? chost::StaffMode::Grand  : chost::StaffMode::Auto;
            inputs.push_back(input);
        }
        else if (a == "--key")
            options.key = chost::parseKey(value());
        else if (a == "--tempo")
            options.tempo = std::atof(value().c_str());
        else if (a == "--grid")
            options.grid = value() == "8" ? 2 : 1;
        else if (a == "--time")
        {
            const std::string text = value();
            options.meter = chost::parseMeter(text);
            if (!options.meter.valid())
            {
                std::fprintf(stderr, "time signatures: 2/4 3/4 4/4 3/8 6/8 9/8 12/8, not %s\n", text.c_str());
                return 2;
            }
        }
        else if (a == "--no-triplets")
            options.triplets = false;
        else if (a == "--title")
            options.title = value();
        else if (a == "--output")
            output = value();
    }
    if (inputs.empty())
    {
        std::fprintf(stderr, "--midi F.mid is needed\n");
        return 2;
    }
    const chost::Score score = chost::makeScore(inputs, options, error);
    if (score.parts.empty())
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    size_t notes = 0, rests = 0, ties = 0, triplets = 0;
    for (const chost::ScorePart& part : score.parts)
        for (const chost::ScoreStaff& st : part.staves)
            for (const chost::ScoreMeasure& m : st.measures)
                for (const chost::ScoreEvent& e : m.events)
                {
                    e.rest ? ++rests : notes += e.notes.size();
                    if (e.tuplet >= 0 && (&e == &m.events.front() || (&e - 1)->tuplet != e.tuplet))
                        ++triplets;
                    for (const chost::ScoreNote& n : e.notes)
                        ties += n.tieStart ? 1 : 0;
                }
    std::printf("SCORE %s\t%d/%d\t%d bars\t%zu parts\t%zu notes\t%zu rests\t%zu ties\t%zu triplets\n", chost::keyName(score.key).c_str(),
                score.meter.beats, score.meter.beatType, score.measures, score.parts.size(), notes, rests, ties, triplets);
    for (const chost::ScorePart& part : score.parts)
        std::printf("PART %s\t%s\n", part.name.c_str(),
                    part.drums ? "drums" : part.staves.size() > 1 ? "grand" : part.staves[0].clef == chost::Clef::Treble ? "treble" : "bass");
    if (!output.empty())
    {
        FILE* f = std::fopen(output.c_str(), "wb");
        const std::string xml = chost::musicXml(score);
        if (f == nullptr || std::fwrite(xml.data(), 1, xml.size(), f) != xml.size())
        {
            std::fprintf(stderr, "can't write %s\n", output.c_str());
            if (f)
                std::fclose(f);
            return 1;
        }
        std::fclose(f);
        std::printf("MUSICXML %s\n", output.c_str());
    }
    return 0;
}

// chost-cli scale: a scale's notes, the key of a MIDI file, notes pulled into a scale
static int cmdScale(int argc, char* argv[])
{
    std::string scaleText, guess, fit, output;
    for (int i = 0; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--scale")
            scaleText = value();
        else if (a == "--guess")
            guess = value();
        else if (a == "--fit")
            fit = value();
        else if (a == "--output")
            output = value();
    }
    std::string error;
    chost::Scale scale = chost::parseScale(scaleText);
    for (const auto& [file, guessing] : {std::pair{guess, true}, std::pair{fit, false}})
    {
        if (file.empty())
            continue;
        chost::MidiClip clip;
        if (!chost::readMidiFile(file, clip, error))
        {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), error.c_str());
            return 1;
        }
        if (guessing)
        {
            float confidence = 0.0f;
            const chost::Key key = chost::guessKey(clip.notes, &confidence);
            std::printf("GUESS %s\t%.2f\n", key.valid ? chost::keyName(key).c_str() : "-", confidence);
            if (!scale.valid())
                scale = chost::scaleOfKey(key);
        }
        else
        {
            if (!scale.valid())
            {
                std::fprintf(stderr, "--fit needs --scale (or --guess)\n");
                return 1;
            }
            const int moved = chost::fitToScale(clip.notes, scale);
            std::printf("FIT %d of %zu notes moved into %s\n", moved, clip.notes.size(), chost::scaleName(scale).c_str());
            if (!output.empty() && !chost::writeMidiFile(output, clip, error))
            {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
        }
    }
    if (!scale.valid())
    {
        std::fprintf(stderr, "unknown scale: %s\n", scaleText.c_str());
        return 1;
    }
    const chost::Key spelling = chost::parseKey(chost::scaleName(scale));
    std::string notes;
    for (int step : scale.steps)
        notes += (notes.empty() ? "" : " ") + chost::noteName(scale.tonic + step, spelling);
    std::printf("SCALE %s\t%s\n", chost::scaleName(scale).c_str(), notes.c_str());
    return 0;
}

static int cmdChords(int argc, char* argv[])
{
    std::string keyText = "C major", after, progression, output, patternText = "block";
    double tempo = 120.0;
    bool sevenths = false;
    for (int i = 0; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--key")
            keyText = value();
        else if (a == "--sevenths")
            sevenths = true;
        else if (a == "--after")
            after = value();
        else if (a == "--progression")
            progression = value();
        else if (a == "--pattern")
            patternText = value();
        else if (a == "--tempo")
            tempo = std::atof(value().c_str());
        else if (a == "--output")
            output = value();
    }
    const chost::Key key = chost::parseKey(keyText);
    if (!key.valid)
    {
        std::fprintf(stderr, "unknown key: %s\n", keyText.c_str());
        return 1;
    }
    std::printf("KEY %s\n", chost::keyName(key).c_str());
    if (!progression.empty())
    {
        std::string error;
        const auto chords = chost::parseProgression(progression, error);
        if (chords.empty())
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("PROGRESSION %s\n", chost::progressionText(chords, key).c_str());
        if (!output.empty())
        {
            const chost::ChordPattern pattern = patternText == "arpeggio" ? chost::ChordPattern::Arpeggio
                                                : patternText == "rhythm" ? chost::ChordPattern::Rhythm
                                                                          : chost::ChordPattern::Block;
            const chost::MidiClip clip = chost::chordClip(chords, tempo, pattern);
            if (!chost::writeMidiFile(output, clip, error))
            {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("MIDI %s\t%zu notes\t%.2f s\n", output.c_str(), clip.notes.size(), clip.seconds);
        }
        return 0;
    }
    const std::vector<chost::Pad> pads = chost::chordPads(key, sevenths);
    const chost::Chord previous = chost::parseChord(after);
    if (!after.empty() && !previous.valid())
    {
        std::fprintf(stderr, "unknown chord: %s\n", after.c_str());
        return 1;
    }
    const std::vector<float> scores = chost::nextChordScores(key, pads, previous.valid() ? &previous : nullptr);
    for (size_t i = 0; i < pads.size(); ++i)
    {
        std::string notes;
        for (int n : chost::voiceChord(pads[i].chord, {}))
            notes += " " + std::to_string(n);
        std::printf("PAD %zu\t%s\t%s\t%.2f\t%s\n", i, pads[i].chord.name(key).c_str(), pads[i].numeral.c_str(), scores[i], notes.c_str() + 1);
    }
    return 0;
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const std::string cmd = argc > 1 ? argv[1] : "";

    if (cmd == "drivers")
        return cmdDrivers();
    if (cmd == "scan")
        return cmdScan(argc - 2, argv + 2);
    if (cmd == "run")
        return cmdRun(argc - 2, argv + 2);
    if (cmd == "mix")
        return cmdMix(argc - 2, argv + 2);
    if (cmd == "chords")
        return cmdChords(argc - 2, argv + 2);
    if (cmd == "scale")
        return cmdScale(argc - 2, argv + 2);
    if (cmd == "score")
        return cmdScore(argc - 2, argv + 2);
    if (cmd == "clip")
        return cmdClip(argc - 2, argv + 2);
    if (cmd == "loudness")
        return cmdLoudness(argc - 2, argv + 2);

    std::fprintf(stderr,
                 "usage:\n"
                 "  chost-cli drivers\n"
                 "  chost-cli scan [--discover]\n"
                 "  chost-cli run [--driver NAME] [--file AUDIO] [--plugin SPEC]... [--set N:P=V]... [--seconds S] [--save F]\n"
                 "  chost-cli mix [--driver NAME] [--track AUDIO]... [--midi FILE.mid@SPEC]... [--insert T:SPEC]...\n"
                 "                [--mute T] [--solo T]\n"
                 "                [--volume T=V] [--pan T=-1..1] [--remove-track T] [--remove-insert T:I]\n"
                 "                [--bus NAME]... [--output T=BUS] [--send T=BUS:LEVEL] [--click VOLUME [--click-sound SPEC]] [--tempo BPM]\n"
                 "                [--seconds S] [--save F.chost] [--open F.chost]\n"
                 "  chost-cli chords [--key \"A minor\"] [--sevenths] [--after CHORD]\n"
                 "  chost-cli chords [--key K] --progression \"C G | Am F\" [--output F.mid] [--pattern block|arpeggio|rhythm] [--tempo BPM]\n"
                 "  chost-cli scale [--scale \"A minor\"] [--guess F.mid] [--fit F.mid [--output OUT.mid]]\n"
                 "  chost-cli score [--staff auto|treble|bass|grand] --midi F.mid... [--key K] [--tempo BPM] [--grid 16|8] [--time 6/8] [--no-triplets]\n"
                 "                  [--title T] [--output F.musicxml]\n"
                 "  chost-cli clip --input F --output F2 --edit move|silence|keep|fade_in|fade_out|gain|normalize\n"
                 "                 [--start S] [--end S] [--seconds S] [--db DB]\n"
                 "  chost-cli loudness FILE...\n");
    return 2;
}
