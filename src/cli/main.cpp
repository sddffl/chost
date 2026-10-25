// chost-cli - command line front end for the chost plugin host.
//
// Usage:
//   chost-cli drivers
//   chost-cli scan
//   chost-cli run [--driver NAME] [--file AUDIO] [--plugin SPEC]... [--set N:P=V]... [--seconds S] [--save F]
//   chost-cli mix [--driver NAME] [--track AUDIO]... [--insert T:SPEC]... [--mute T] [--solo T] [--volume T=V]
//                 [--seconds S] [--save F.chost] [--open F.chost]
//
// Plugin SPEC:
//   lv2:<URI>            e.g. lv2:urn:zamaudio:ZamComp
//   vst3:<path.vst3>     e.g. vst3:/usr/lib/vst3/ZamComp.vst3
//   vst2:<path>, ladspa:<path>:<label>, sf2:<file>, sfz:<file>
//
// Without --seconds, run drops into an interactive shell (type "help").

#include "core/HostEngine.h"
#include "core/Session.h"

#include <algorithm>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
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

static int cmdScan()
{
    for (const chost::PluginEntry& e : chost::scanPlugins())
        std::printf("%-5s %-32s %-20s %s\n", e.format.c_str(), e.name.c_str(), e.maker.c_str(), e.spec.c_str());
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
    std::vector<std::string> files, inserts, volumes;
    std::vector<int> mutes, solos;
    double seconds = 3.0;

    for (int i = 0; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--driver" && hasValue)        driver = argv[++i];
        else if (arg == "--track" && hasValue)    files.emplace_back(argv[++i]);
        else if (arg == "--insert" && hasValue)   inserts.emplace_back(argv[++i]);
        else if (arg == "--mute" && hasValue)     mutes.push_back(std::stoi(argv[++i]));
        else if (arg == "--solo" && hasValue)     solos.push_back(std::stoi(argv[++i]));
        else if (arg == "--volume" && hasValue)   volumes.emplace_back(argv[++i]);
        else if (arg == "--seconds" && hasValue)  seconds = std::stod(argv[++i]);
        else if (arg == "--save" && hasValue)     savePath = argv[++i];
        else if (arg == "--open" && hasValue)     openPath = argv[++i];
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
        std::fprintf(stderr, "engine init failed (%s): %s\n", driver.c_str(), host.lastError().c_str());
        return 1;
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
    for (const std::string& ins : inserts)
    {
        const auto colon = ins.find(':');
        if (colon == std::string::npos || !session.addInsert(std::stoi(ins.substr(0, colon)), ins.substr(colon + 1)))
        {
            std::fprintf(stderr, "insert %s: %s\n", ins.c_str(), session.lastError().c_str());
            ok = false;
        }
    }
    for (int t : mutes)
        session.setMuted(t, true);
    for (int t : solos)
        session.setSolo(t, true);
    for (const std::string& v : volumes)
        if (const auto eq = v.find('='); eq != std::string::npos)
            session.setVolume(std::stoi(v.substr(0, eq)), std::stof(v.substr(eq + 1)));

    for (size_t i = 0; i < session.tracks().size(); ++i)
    {
        const chost::Track& t = session.tracks()[i];
        std::printf("track %zu \"%s\": source=%d inserts=%zu fader=%u file=%s\n", i, t.name.c_str(), t.source,
                    t.inserts.size(), t.fader, t.file.c_str());
    }

    host.relocate(0);
    host.play();
    float peak = 0.0f;
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!gQuit && host.isRunning() && std::chrono::steady_clock::now() < until)
    {
        host.idle();
        const auto [l, r] = host.outputPeaks(session.master());
        peak = std::max({peak, l, r});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("master peak %.3f\n", peak);

    if (!savePath.empty() && !session.save(savePath))
    {
        std::fprintf(stderr, "save failed: %s\n", session.lastError().c_str());
        ok = false;
    }
    host.stop();
    return ok ? 0 : 1;
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const std::string cmd = argc > 1 ? argv[1] : "";

    if (cmd == "drivers")
        return cmdDrivers();
    if (cmd == "scan")
        return cmdScan();
    if (cmd == "run")
        return cmdRun(argc - 2, argv + 2);
    if (cmd == "mix")
        return cmdMix(argc - 2, argv + 2);

    std::fprintf(stderr,
                 "usage:\n"
                 "  chost-cli drivers\n"
                 "  chost-cli scan\n"
                 "  chost-cli run [--driver NAME] [--file AUDIO] [--plugin SPEC]... [--set N:P=V]... [--seconds S] [--save F]\n"
                 "  chost-cli mix [--driver NAME] [--track AUDIO]... [--insert T:SPEC]... [--mute T] [--solo T]\n"
                 "                [--volume T=V] [--seconds S] [--save F.chost] [--open F.chost]\n");
    return 2;
}
