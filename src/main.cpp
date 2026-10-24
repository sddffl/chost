// chost - a minimal standalone audio plugin host built on libcarla.
//
// Usage:
//   chost drivers
//   chost scan [lv2|vst3|all]
//   chost run [--driver NAME] [--file AUDIO] [--plugin SPEC]... [--set N:P=V]... [--seconds S] [--save F]
//
// Plugin SPEC:
//   lv2:<URI>            e.g. lv2:urn:zamaudio:ZamComp
//   vst3:<path.vst3>     e.g. vst3:/usr/lib/vst3/ZamComp.vst3
//   vst2:<path.so>, ladspa:<path.so>:<label>, sf2:<file>, sfz:<file>
//
// Without --seconds, run drops into an interactive shell (type "help").

#include "CarlaHost.h"
#include "CarlaUtils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

CARLA_BACKEND_USE_NAMESPACE

namespace fs = std::filesystem;

static std::atomic<bool> gQuit{false};

static void onSignal(int) { gQuit = true; }

// ---------------------------------------------------------------------------------------------------------------------
// helpers

static const char* kCarlaLibDir = "/usr/lib/carla";

static std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        out.push_back(item);
    return out;
}

static std::string envOr(const char* name, const std::string& fallback)
{
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : fallback;
}

static std::string defaultLv2Path()
{
    const std::string home = envOr("HOME", "");
    return envOr("LV2_PATH", home + "/.lv2:/usr/local/lib/lv2:/usr/lib/lv2");
}

static std::string defaultVst3Path()
{
    const std::string home = envOr("HOME", "");
    return envOr("VST3_PATH", home + "/.vst3:/usr/local/lib/vst3:/usr/lib/vst3");
}

// ---------------------------------------------------------------------------------------------------------------------
// commands: drivers / scan

static int cmdDrivers()
{
    const uint count = carla_get_engine_driver_count();
    for (uint i = 0; i < count; ++i)
    {
        std::printf("%s\n", carla_get_engine_driver_name(i));
        if (const char* const* devices = carla_get_engine_driver_device_names(i))
            for (; *devices != nullptr; ++devices)
                std::printf("    device: %s\n", *devices);
    }
    return 0;
}

static void scanLv2()
{
    const uint count = carla_get_cached_plugin_count(PLUGIN_LV2, defaultLv2Path().c_str());
    for (uint i = 0; i < count; ++i)
    {
        const CarlaCachedPluginInfo* info = carla_get_cached_plugin_info(PLUGIN_LV2, i);
        if (info == nullptr || !info->valid)
            continue;
        // Carla returns LV2 labels as "<bundle>/<URI>", only the URI is needed to load it
        std::string uri = info->label;
        if (const auto slash = uri.find('/'); slash != std::string::npos && uri.find(':') > slash)
            uri = uri.substr(slash + 1);
        std::printf("lv2:%-48s  %s [%s] in:%u out:%u midi-in:%u params:%u\n",
                    uri.c_str(), info->name, info->maker,
                    info->audioIns, info->audioOuts, info->midiIns, info->parameterIns);
    }
}

static void scanVst3()
{
    for (const std::string& dir : split(defaultVst3Path(), ':'))
    {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        {
            if (entry.path().extension() == ".vst3")
                std::printf("vst3:%s\n", entry.path().c_str());
        }
    }
}

static int cmdScan(const std::string& what)
{
    if (what == "lv2" || what == "all")
        scanLv2();
    if (what == "vst3" || what == "all")
        scanVst3();
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// host session

struct PluginSpec {
    PluginType type = PLUGIN_NONE;
    std::string filename;
    std::string label;
};

static bool parseSpec(const std::string& spec, PluginSpec& out)
{
    const auto colon = spec.find(':');
    if (colon == std::string::npos)
        return false;

    const std::string kind = spec.substr(0, colon);
    const std::string rest = spec.substr(colon + 1);

    if (kind == "lv2")         { out.type = PLUGIN_LV2;  out.label = rest; }
    else if (kind == "vst3")   { out.type = PLUGIN_VST3; out.filename = rest; }
    else if (kind == "vst2")   { out.type = PLUGIN_VST2; out.filename = rest; }
    else if (kind == "sf2")    { out.type = PLUGIN_SF2;  out.filename = rest; }
    else if (kind == "sfz")    { out.type = PLUGIN_SFZ;  out.filename = rest; }
    else if (kind == "ladspa")
    {
        const auto sep = rest.rfind(':');
        if (sep == std::string::npos)
            return false;
        out.type = PLUGIN_LADSPA;
        out.filename = rest.substr(0, sep);
        out.label = rest.substr(sep + 1);
    }
    else
        return false;

    return true;
}

class Host {
public:
    bool start(const std::string& driver)
    {
        fHandle = carla_standalone_host_init();
        if (fHandle == nullptr)
            return false;

        carla_set_engine_callback(fHandle, engineCallback, this);

        // rack mode: plugins are processed in series, in load order
        carla_set_engine_option(fHandle, ENGINE_OPTION_PROCESS_MODE, ENGINE_PROCESS_MODE_CONTINUOUS_RACK, nullptr);
        carla_set_engine_option(fHandle, ENGINE_OPTION_PATH_BINARIES, 0, kCarlaLibDir);
        carla_set_engine_option(fHandle, ENGINE_OPTION_PATH_RESOURCES, 0, (std::string(kCarlaLibDir) + "/resources").c_str());
        carla_set_engine_option(fHandle, ENGINE_OPTION_PLUGIN_PATH, PLUGIN_LV2, defaultLv2Path().c_str());
        carla_set_engine_option(fHandle, ENGINE_OPTION_PLUGIN_PATH, PLUGIN_VST3, defaultVst3Path().c_str());

        if (!carla_engine_init(fHandle, driver.c_str(), "chost"))
        {
            std::fprintf(stderr, "engine init failed (%s): %s\n", driver.c_str(), carla_get_last_error(fHandle));
            return false;
        }

        std::printf("engine running: driver=%s buffer=%u rate=%.0f\n", driver.c_str(),
                    carla_get_buffer_size(fHandle), carla_get_sample_rate(fHandle));
        return true;
    }

    void stop()
    {
        if (fHandle == nullptr)
            return;
        carla_set_engine_about_to_close(fHandle);
        carla_remove_all_plugins(fHandle);
        carla_engine_close(fHandle);
        fHandle = nullptr;
    }

    bool addPlugin(const std::string& specStr)
    {
        PluginSpec spec;
        if (!parseSpec(specStr, spec))
        {
            std::fprintf(stderr, "bad plugin spec: %s\n", specStr.c_str());
            return false;
        }
        if (!carla_add_plugin(fHandle, BINARY_NATIVE, spec.type,
                              spec.filename.empty() ? nullptr : spec.filename.c_str(), nullptr,
                              spec.label.empty() ? nullptr : spec.label.c_str(), 0, nullptr, PLUGIN_OPTIONS_NULL))
        {
            std::fprintf(stderr, "failed to load %s: %s\n", specStr.c_str(), carla_get_last_error(fHandle));
            return false;
        }
        return true;
    }

    bool loadFile(const std::string& path)
    {
        // audio files become Carla's internal audiofile player, placed first in the rack
        if (!carla_load_file(fHandle, path.c_str()))
        {
            std::fprintf(stderr, "failed to load file %s: %s\n", path.c_str(), carla_get_last_error(fHandle));
            return false;
        }
        return true;
    }

    // "N:P=V" -> plugin N, parameter P (index or name), value V
    bool setParam(const std::string& expr)
    {
        const auto colon = expr.find(':');
        const auto eq = expr.find('=');
        if (colon == std::string::npos || eq == std::string::npos || eq < colon)
        {
            std::fprintf(stderr, "bad parameter expression: %s (want N:P=V)\n", expr.c_str());
            return false;
        }
        const uint pluginId = std::stoul(expr.substr(0, colon));
        const std::string param = expr.substr(colon + 1, eq - colon - 1);
        const float value = std::stof(expr.substr(eq + 1));

        const int index = findParam(pluginId, param);
        if (index < 0)
        {
            std::fprintf(stderr, "plugin %u has no parameter '%s'\n", pluginId, param.c_str());
            return false;
        }
        carla_set_parameter_value(fHandle, pluginId, static_cast<uint32_t>(index), value);
        return true;
    }

    void list() const
    {
        const uint32_t count = carla_get_current_plugin_count(fHandle);
        for (uint32_t i = 0; i < count; ++i)
        {
            const CarlaPluginInfo* info = carla_get_plugin_info(fHandle, i);
            std::printf("[%u] %s (%s) params:%u peak L/R: %.3f / %.3f\n", i, info->name, info->maker,
                        carla_get_parameter_count(fHandle, i),
                        carla_get_output_peak_value(fHandle, i, true),
                        carla_get_output_peak_value(fHandle, i, false));
        }
    }

    void params(uint pluginId) const
    {
        const uint32_t count = carla_get_parameter_count(fHandle, pluginId);
        for (uint32_t i = 0; i < count; ++i)
        {
            const CarlaParameterInfo* info = carla_get_parameter_info(fHandle, pluginId, i);
            const ParameterRanges* ranges = carla_get_parameter_ranges(fHandle, pluginId, i);
            std::printf("  %2u %-24s = %-10g [%g .. %g] %s\n", i, info->name,
                        carla_get_current_parameter_value(fHandle, pluginId, i),
                        ranges->min, ranges->max, info->unit);
        }
    }

    void bypass(uint pluginId, bool on) { carla_set_active(fHandle, pluginId, !on); }
    void showUi(uint pluginId, bool on) { carla_show_custom_ui(fHandle, pluginId, on); }
    bool save(const std::string& path) { return carla_save_project(fHandle, path.c_str()); }
    bool loadProject(const std::string& path) { return carla_load_project(fHandle, path.c_str()); }
    void idle() { carla_engine_idle(fHandle); }
    bool running() const { return fHandle != nullptr && carla_is_engine_running(fHandle); }
    void play() { carla_transport_play(fHandle); }

private:
    int findParam(uint pluginId, const std::string& nameOrIndex) const
    {
        const uint32_t count = carla_get_parameter_count(fHandle, pluginId);
        char* end = nullptr;
        const long asIndex = std::strtol(nameOrIndex.c_str(), &end, 10);
        if (end != nameOrIndex.c_str() && *end == '\0')
            return (asIndex >= 0 && static_cast<uint32_t>(asIndex) < count) ? static_cast<int>(asIndex) : -1;

        for (uint32_t i = 0; i < count; ++i)
        {
            const CarlaParameterInfo* info = carla_get_parameter_info(fHandle, pluginId, i);
            if (nameOrIndex == info->name || nameOrIndex == info->symbol)
                return static_cast<int>(i);
        }
        return -1;
    }

    static void engineCallback(void*, EngineCallbackOpcode action, uint pluginId,
                               int, int, int, float, const char* valueStr)
    {
        switch (action)
        {
        case ENGINE_CALLBACK_PLUGIN_ADDED:
            std::printf("+ plugin %u: %s\n", pluginId, valueStr != nullptr ? valueStr : "");
            break;
        case ENGINE_CALLBACK_PLUGIN_REMOVED:
            std::printf("- plugin %u\n", pluginId);
            break;
        case ENGINE_CALLBACK_ERROR:
            std::fprintf(stderr, "engine error: %s\n", valueStr != nullptr ? valueStr : "");
            break;
        case ENGINE_CALLBACK_QUIT:
            gQuit = true;
            break;
        default:
            break;
        }
    }

    CarlaHostHandle fHandle = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------------
// interactive shell

static void printHelp()
{
    std::printf(
        "commands:\n"
        "  list                     plugins in the rack, with output peaks\n"
        "  add SPEC                 load a plugin (lv2:URI, vst3:PATH, ...)\n"
        "  file PATH                load an audio file player at the start of the rack\n"
        "  params N                 show parameters of plugin N\n"
        "  set N:P=V                set parameter P (index/name/symbol) of plugin N\n"
        "  bypass N on|off          bypass plugin N\n"
        "  ui N on|off              show the plugin's own editor window\n"
        "  save FILE / load FILE    save or load a .carxp project\n"
        "  quit\n");
}

static void runShell(Host& host)
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

    while (!gQuit && host.running())
    {
        host.idle();

        if (lineReady)
        {
            std::istringstream in(line);
            std::string cmd, a, b;
            in >> cmd >> a >> b;

            if (cmd == "quit" || cmd == "exit")      gQuit = true;
            else if (cmd == "help")                  printHelp();
            else if (cmd == "list")                  host.list();
            else if (cmd == "add")                   host.addPlugin(a);
            else if (cmd == "file")                  host.loadFile(a);
            else if (cmd == "params" && !a.empty())  host.params(std::stoul(a));
            else if (cmd == "set")                   host.setParam(a);
            else if (cmd == "bypass" && !a.empty())  host.bypass(std::stoul(a), b != "off");
            else if (cmd == "ui" && !a.empty())      host.showUi(std::stoul(a), b != "off");
            else if (cmd == "save")                  std::printf(host.save(a) ? "saved\n" : "save failed\n");
            else if (cmd == "load")                  std::printf(host.loadProject(a) ? "loaded\n" : "load failed\n");
            else if (!cmd.empty())                   std::printf("unknown command, type 'help'\n");

            lineReady = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    reader.detach();
}

// ---------------------------------------------------------------------------------------------------------------------

static int cmdRun(int argc, char* argv[])
{
#ifdef __linux__
    std::string driver = "JACK";
#elif defined(_WIN32)
    std::string driver = "WASAPI";
#else
    std::string driver = "CoreAudio";
#endif
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

    Host host;
    if (!host.start(driver))
        return 1;

    bool ok = true;
    if (!file.empty())
        ok = host.loadFile(file) && ok;
    for (const std::string& p : plugins)
        ok = host.addPlugin(p) && ok;
    for (const std::string& s : sets)
        ok = host.setParam(s) && ok;
    host.play();

    if (seconds >= 0.0)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!gQuit && host.running() && std::chrono::steady_clock::now() < until)
        {
            host.idle();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        host.list();
    }
    else
    {
        printHelp();
        runShell(host);
    }

    if (!savePath.empty())
        ok = host.save(savePath) && ok;

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
        return cmdScan(argc > 2 ? argv[2] : "all");
    if (cmd == "run")
        return cmdRun(argc - 2, argv + 2);

    std::fprintf(stderr,
                 "usage:\n"
                 "  chost drivers\n"
                 "  chost scan [lv2|vst3|all]\n"
                 "  chost run [--driver NAME] [--file AUDIO] [--plugin SPEC]... [--set N:P=V]... [--seconds S] [--save F]\n");
    return 2;
}
