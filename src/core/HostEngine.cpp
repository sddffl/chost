#include "HostEngine.h"

#include "AudioConvert.h"
#include "CarlaUtils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

CARLA_BACKEND_USE_NAMESPACE

namespace fs = std::filesystem;

namespace chost {

// ---------------------------------------------------------------------------------------------------------------------
// paths and scanning

#ifdef _WIN32
static constexpr char kPathSep = ';';
#else
static constexpr char kPathSep = ':';
#endif

static std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

static std::vector<std::string> splitPath(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, kPathSep))
        if (!item.empty())
            out.push_back(item);
    return out;
}

std::string defaultPluginPath(PluginType type)
{
#ifdef _WIN32
    const std::string common = env("COMMONPROGRAMFILES");
    const std::string programs = env("PROGRAMFILES");
    const std::string appdata = env("APPDATA");
    switch (type)
    {
    case PLUGIN_LV2:  return !env("LV2_PATH").empty() ? env("LV2_PATH") : appdata + "\\LV2;" + common + "\\LV2";
    case PLUGIN_VST3: return !env("VST3_PATH").empty() ? env("VST3_PATH") : common + "\\VST3";
    case PLUGIN_VST2: return !env("VST2_PATH").empty() ? env("VST2_PATH")
                           : programs + "\\VSTPlugins;" + programs + "\\Steinberg\\VSTPlugins;" + common + "\\VST2";
    default:          return {};
    }
#else
    const std::string home = env("HOME");
    switch (type)
    {
    case PLUGIN_LV2:  return !env("LV2_PATH").empty() ? env("LV2_PATH") : home + "/.lv2:/usr/local/lib/lv2:/usr/lib/lv2";
    case PLUGIN_VST3: return !env("VST3_PATH").empty() ? env("VST3_PATH") : home + "/.vst3:/usr/local/lib/vst3:/usr/lib/vst3";
    case PLUGIN_VST2: return !env("VST2_PATH").empty() ? env("VST2_PATH")
                           : home + "/.vst:/usr/local/lib/vst:/usr/lib/vst";
    default:          return {};
    }
#endif
}

static void scanLv2(std::vector<PluginEntry>& out)
{
    const uint count = carla_get_cached_plugin_count(PLUGIN_LV2, defaultPluginPath(PLUGIN_LV2).c_str());
    for (uint i = 0; i < count; ++i)
    {
        const CarlaCachedPluginInfo* info = carla_get_cached_plugin_info(PLUGIN_LV2, i);
        if (info == nullptr || !info->valid)
            continue;

        // Carla reports LV2 labels as "<bundle>/<URI>"; only the URI is needed to load one
        std::string uri = info->label;
        if (const auto slash = uri.find('/'); slash != std::string::npos && uri.find(':') > slash)
            uri = uri.substr(slash + 1);

        PluginEntry e;
        e.spec = "lv2:" + uri;
        e.format = "LV2";
        e.name = info->name;
        e.maker = info->maker;
        e.audioIns = info->audioIns;
        e.audioOuts = info->audioOuts;
        e.midiIns = info->midiIns;
        e.isSynth = (info->hints & PLUGIN_IS_SYNTH) != 0;
        out.push_back(std::move(e));
    }
}

// VST bundles/binaries are listed by file name only, opening each one to query it would mean
// running carla-discovery in a subprocess per plugin, which is left for a later version.
static void scanFiles(std::vector<PluginEntry>& out, PluginType type, const char* format,
                      const char* prefix, const std::vector<std::string>& extensions)
{
    for (const std::string& dir : splitPath(defaultPluginPath(type)))
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;

        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            const fs::path& p = it->path();
            if (std::find(extensions.begin(), extensions.end(), p.extension().string()) == extensions.end())
                continue;

            PluginEntry e;
            e.spec = std::string(prefix) + p.string();
            e.format = format;
            e.name = p.stem().string();
            out.push_back(std::move(e));

            // don't descend into bundles (a .vst3 is a directory on Linux and macOS)
            if (it->is_directory(ec))
                it.disable_recursion_pending();
        }
    }
}

std::vector<PluginEntry> scanPlugins()
{
    std::vector<PluginEntry> out;
    scanLv2(out);
    scanFiles(out, PLUGIN_VST3, "VST3", "vst3:", {".vst3"});
#ifdef _WIN32
    scanFiles(out, PLUGIN_VST2, "VST2", "vst2:", {".dll"});
#else
    scanFiles(out, PLUGIN_VST2, "VST2", "vst2:", {".so"});
#endif
    std::sort(out.begin(), out.end(), [](const PluginEntry& a, const PluginEntry& b) { return a.name < b.name; });
    return out;
}

std::vector<std::string> audioDrivers()
{
    std::vector<std::string> out;
    const uint count = carla_get_engine_driver_count();
    for (uint i = 0; i < count; ++i)
        out.emplace_back(carla_get_engine_driver_name(i));
    out.emplace_back("Dummy");
    return out;
}

std::string defaultAudioDriver()
{
    const std::vector<std::string> drivers = audioDrivers();
    auto has = [&](const char* name) { return std::find(drivers.begin(), drivers.end(), name) != drivers.end(); };
#if defined(_WIN32)
    for (const char* name : {"WASAPI", "Windows Audio", "DirectSound", "ASIO"})
#elif defined(__APPLE__)
    for (const char* name : {"CoreAudio"})
#else
    for (const char* name : {"PulseAudio", "JACK", "ALSA"})
#endif
        if (has(name))
            return name;
    return drivers.empty() ? "Dummy" : drivers.front();
}

// ---------------------------------------------------------------------------------------------------------------------
// spec parsing

namespace {

struct PluginSpec {
    PluginType type = PLUGIN_NONE;
    std::string filename;
    std::string label;
};

bool parseSpec(const std::string& spec, PluginSpec& out)
{
    const auto colon = spec.find(':');
    if (colon == std::string::npos)
        return false;

    const std::string kind = spec.substr(0, colon);
    const std::string rest = spec.substr(colon + 1);

    if (kind == "lv2")       { out.type = PLUGIN_LV2;  out.label = rest; }
    else if (kind == "vst3") { out.type = PLUGIN_VST3; out.filename = rest; }
    else if (kind == "vst2") { out.type = PLUGIN_VST2; out.filename = rest; }
    else if (kind == "sf2")  { out.type = PLUGIN_SF2;  out.filename = rest; }
    else if (kind == "sfz")  { out.type = PLUGIN_SFZ;  out.filename = rest; }
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

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// engine

HostEngine::HostEngine() = default;

HostEngine::~HostEngine()
{
    stop();
}

bool HostEngine::start(const std::string& driver, const std::string& device)
{
    stop();

    fHandle = carla_standalone_host_init();
    if (fHandle == nullptr)
        return false;

    carla_set_engine_callback(fHandle, engineCallback, this);

    // rack mode: plugins are processed in series, in rack order
    carla_set_engine_option(fHandle, ENGINE_OPTION_PROCESS_MODE, ENGINE_PROCESS_MODE_CONTINUOUS_RACK, nullptr);
    carla_set_engine_option(fHandle, ENGINE_OPTION_FORCE_STEREO, 1, nullptr);

    // carla's bridges and resources live next to the library, wherever it was installed
    const std::string libDir = carla_get_library_folder();
#ifdef _WIN32
    const std::string resources = libDir + "\\resources";
#else
    const std::string resources = libDir + "/resources";
#endif
    carla_set_engine_option(fHandle, ENGINE_OPTION_PATH_BINARIES, 0, libDir.c_str());
    carla_set_engine_option(fHandle, ENGINE_OPTION_PATH_RESOURCES, 0, resources.c_str());

    for (PluginType type : {PLUGIN_LV2, PLUGIN_VST3, PLUGIN_VST2})
        carla_set_engine_option(fHandle, ENGINE_OPTION_PLUGIN_PATH, type, defaultPluginPath(type).c_str());

    if (!device.empty())
        carla_set_engine_option(fHandle, ENGINE_OPTION_AUDIO_DEVICE, 0, device.c_str());

    if (!carla_engine_init(fHandle, driver.c_str(), "chost"))
        return false;

    fDriver = driver;
    return true;
}

void HostEngine::stop()
{
    if (fHandle == nullptr || !carla_is_engine_running(fHandle))
        return;
    carla_set_engine_about_to_close(fHandle);
    carla_remove_all_plugins(fHandle);
    carla_engine_close(fHandle);
    fDriver.clear();
}

bool HostEngine::isRunning() const { return fHandle != nullptr && carla_is_engine_running(fHandle); }

void HostEngine::idle()
{
    if (isRunning())
        carla_engine_idle(fHandle);
}

std::string HostEngine::lastError() const
{
    if (!fError.empty())
        return fError;
    const char* err = fHandle != nullptr ? carla_get_last_error(fHandle) : nullptr;
    return err != nullptr ? err : "";
}

double HostEngine::sampleRate() const { return isRunning() ? carla_get_sample_rate(fHandle) : 0.0; }
uint32_t HostEngine::bufferSize() const { return isRunning() ? carla_get_buffer_size(fHandle) : 0; }

float HostEngine::dspLoad() const
{
    const CarlaRuntimeEngineInfo* info = isRunning() ? carla_get_runtime_engine_info(fHandle) : nullptr;
    return info != nullptr ? info->load : 0.0f;
}

bool HostEngine::addPlugin(const std::string& specStr)
{
    fError.clear();
    PluginSpec spec;
    if (!parseSpec(specStr, spec))
        return false;
    return carla_add_plugin(fHandle, BINARY_NATIVE, spec.type,
                            spec.filename.empty() ? nullptr : spec.filename.c_str(), nullptr,
                            spec.label.empty() ? nullptr : spec.label.c_str(), 0, nullptr, PLUGIN_OPTIONS_NULL);
}

bool HostEngine::loadAudioFile(const std::string& path)
{
    fError.clear();
    // see AudioConvert.h: carla 2.5 crashes on some files that are not at the engine's sample rate
    std::string error;
    const std::string cacheDir = (fs::temp_directory_path() / "chost-audio").u8string();
    const std::string playable = prepareAudioFile(path, static_cast<uint32_t>(sampleRate()), cacheDir, error);
    if (playable.empty())
    {
        fError = error;
        return false;
    }

    // becomes carla's internal audiofile player, which plays while the transport rolls
    if (!carla_load_file(fHandle, playable.c_str()))
        return false;

    // carla appends it at the end of the rack, a source belongs at the start
    for (uint id = pluginCount() - 1; id > 0; --id)
        carla_switch_plugins(fHandle, id - 1, id);
    return true;
}

bool HostEngine::removePlugin(uint id) { return carla_remove_plugin(fHandle, id); }

bool HostEngine::movePlugin(uint id, int delta)
{
    const long target = static_cast<long>(id) + delta;
    if (target < 0 || target >= static_cast<long>(pluginCount()))
        return false;
    return carla_switch_plugins(fHandle, id, static_cast<uint>(target));
}

uint HostEngine::pluginCount() const { return isRunning() ? carla_get_current_plugin_count(fHandle) : 0; }

RackSlot HostEngine::slot(uint id) const
{
    RackSlot s;
    const CarlaPluginInfo* info = carla_get_plugin_info(fHandle, id);
    if (info == nullptr)
        return s;
    s.id = id;
    s.name = info->name != nullptr ? info->name : "";
    s.maker = info->maker != nullptr ? info->maker : "";
    s.hasCustomUi = (info->hints & PLUGIN_HAS_CUSTOM_UI) != 0;
    s.active = carla_get_internal_parameter_value(fHandle, id, PARAMETER_ACTIVE) >= 0.5f;
    return s;
}

void HostEngine::setActive(uint id, bool active) { carla_set_active(fHandle, id, active); }
void HostEngine::showCustomUi(uint id, bool show) { carla_show_custom_ui(fHandle, id, show); }

std::pair<float, float> HostEngine::outputPeaks(uint id) const
{
    return {carla_get_output_peak_value(fHandle, id, true), carla_get_output_peak_value(fHandle, id, false)};
}

std::vector<ParamInfo> HostEngine::parameters(uint id) const
{
    std::vector<ParamInfo> out;
    const uint32_t count = carla_get_parameter_count(fHandle, id);
    for (uint32_t i = 0; i < count; ++i)
    {
        const ParameterData* data = carla_get_parameter_data(fHandle, id, i);
        if (data == nullptr || (data->hints & PARAMETER_IS_ENABLED) == 0)
            continue;

        const CarlaParameterInfo* info = carla_get_parameter_info(fHandle, id, i);
        const ParameterRanges* ranges = carla_get_parameter_ranges(fHandle, id, i);

        ParamInfo p;
        p.index = i;
        p.name = info->name != nullptr ? info->name : "";
        p.symbol = info->symbol != nullptr ? info->symbol : "";
        p.unit = info->unit != nullptr ? info->unit : "";
        p.min = ranges->min;
        p.max = ranges->max;
        p.def = ranges->def;
        p.value = carla_get_current_parameter_value(fHandle, id, i);
        p.isOutput = data->type == PARAMETER_OUTPUT;
        p.isBoolean = (data->hints & PARAMETER_IS_BOOLEAN) != 0;
        p.isInteger = (data->hints & PARAMETER_IS_INTEGER) != 0;
        out.push_back(std::move(p));
    }
    return out;
}

float HostEngine::parameterValue(uint id, uint32_t index) const
{
    return carla_get_current_parameter_value(fHandle, id, index);
}

void HostEngine::setParameter(uint id, uint32_t index, float value)
{
    carla_set_parameter_value(fHandle, id, index, value);
}

int HostEngine::findParameter(uint id, const std::string& nameOrIndex) const
{
    const uint32_t count = carla_get_parameter_count(fHandle, id);
    char* end = nullptr;
    const long asIndex = std::strtol(nameOrIndex.c_str(), &end, 10);
    if (end != nameOrIndex.c_str() && *end == '\0')
        return (asIndex >= 0 && static_cast<uint32_t>(asIndex) < count) ? static_cast<int>(asIndex) : -1;

    for (uint32_t i = 0; i < count; ++i)
    {
        const CarlaParameterInfo* info = carla_get_parameter_info(fHandle, id, i);
        if (nameOrIndex == info->name || nameOrIndex == info->symbol)
            return static_cast<int>(i);
    }
    return -1;
}

void HostEngine::play() { carla_transport_play(fHandle); }
void HostEngine::pause() { carla_transport_pause(fHandle); }

bool HostEngine::isPlaying() const
{
    const CarlaTransportInfo* info = isRunning() ? carla_get_transport_info(fHandle) : nullptr;
    return info != nullptr && info->playing;
}

bool HostEngine::saveProject(const std::string& path) { fError.clear(); return carla_save_project(fHandle, path.c_str()); }
bool HostEngine::loadProject(const std::string& path) { fError.clear(); return carla_load_project(fHandle, path.c_str()); }

void HostEngine::engineCallback(void* ptr, EngineCallbackOpcode action, uint pluginId,
                                int value1, int, int, float valuef, const char* valueStr)
{
    auto* self = static_cast<HostEngine*>(ptr);
    if (!self->fEventFunc)
        return;

    const std::string text = valueStr != nullptr ? valueStr : "";
    switch (action)
    {
    case ENGINE_CALLBACK_PLUGIN_ADDED:            self->fEventFunc(HostEvent::PluginAdded, pluginId, 0, 0.0f, text); break;
    case ENGINE_CALLBACK_PLUGIN_REMOVED:          self->fEventFunc(HostEvent::PluginRemoved, pluginId, 0, 0.0f, text); break;
    case ENGINE_CALLBACK_PLUGIN_RENAMED:          self->fEventFunc(HostEvent::PluginRenamed, pluginId, 0, 0.0f, text); break;
    case ENGINE_CALLBACK_PARAMETER_VALUE_CHANGED: self->fEventFunc(HostEvent::ParameterChanged, pluginId, value1, valuef, text); break;
    case ENGINE_CALLBACK_UI_STATE_CHANGED:
        if (value1 != 1)
            self->fEventFunc(HostEvent::UiClosed, pluginId, value1, 0.0f, text);
        break;
    case ENGINE_CALLBACK_RELOAD_ALL:              self->fEventFunc(HostEvent::Reload, pluginId, 0, 0.0f, text); break;
    case ENGINE_CALLBACK_ERROR:                   self->fEventFunc(HostEvent::Error, pluginId, 0, 0.0f, text); break;
    case ENGINE_CALLBACK_ENGINE_STOPPED:          self->fEventFunc(HostEvent::EngineStopped, 0, 0, 0.0f, text); break;
    default: break;
    }
}

} // namespace chost
