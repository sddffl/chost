#include "HostEngine.h"

#include "AudioConvert.h"
#include "CarlaUtils.h"

#include <algorithm>
#include <cctype>
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

static std::vector<std::string> gExtraFolders;

void setExtraPluginFolders(const std::vector<std::string>& folders) { gExtraFolders = folders; }
std::vector<std::string> extraPluginFolders() { return gExtraFolders; }

static std::string platformPluginPath(PluginType type);

std::string defaultPluginPath(PluginType type)
{
    std::string path = platformPluginPath(type);
    if (type == PLUGIN_LV2 || type == PLUGIN_VST3 || type == PLUGIN_VST2)
        for (const std::string& folder : gExtraFolders)
            path += (path.empty() ? "" : std::string(1, kPathSep)) + folder;
    return path;
}

static std::string platformPluginPath(PluginType type)
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
    for (const std::string& dirName : splitPath(defaultPluginPath(type)))
    {
        std::error_code ec;
        const fs::path dir = fs::u8path(dirName);
        if (!fs::is_directory(dir, ec))
            continue;

        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            const fs::path& p = it->path();
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

            // never look inside plugin bundles (a .vst3 or .lv2 is usually a directory), their helper
            // libraries are not plugins
            const bool bundle = (ext == ".vst3" || ext == ".lv2") && it->is_directory(ec);
            if (bundle)
                it.disable_recursion_pending();
            if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
                continue;

            PluginEntry e;
            e.spec = std::string(prefix) + p.u8string();
            if (std::any_of(out.begin(), out.end(), [&](const PluginEntry& o) { return o.spec == e.spec; }))
                continue; // a folder listed twice, or inside another one
            e.format = format;
            e.name = p.stem().u8string();
            out.push_back(std::move(e));
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
    BinaryType binary = BINARY_NATIVE;
    std::string filename;
    std::string label;
    int64_t uniqueId = 0;
};

// "path#123": one plugin of a VST2 shell
void splitUniqueId(std::string& path, int64_t& uniqueId)
{
    const auto hash = path.rfind('#');
    if (hash == std::string::npos || hash + 1 >= path.size())
        return;
    const std::string id = path.substr(hash + 1);
    if (id.find_first_not_of("-0123456789") != std::string::npos)
        return;
    uniqueId = std::stoll(id);
    path.erase(hash);
}

bool parseSpec(const std::string& spec, PluginSpec& out)
{
    const auto colon = spec.find(':');
    if (colon == std::string::npos)
        return false;

    const std::string kind = spec.substr(0, colon);
    const std::string rest = spec.substr(colon + 1);

    if (kind == "lv2")       { out.type = PLUGIN_LV2;  out.label = rest; }
    else if (kind == "vst3") { out.type = PLUGIN_VST3; out.filename = rest; }
    else if (kind == "vst2" || kind == "vst2-32")
    {
        out.type = PLUGIN_VST2;
        out.filename = rest;
        splitUniqueId(out.filename, out.uniqueId);
        if (kind == "vst2-32")
            out.binary = BINARY_WIN32; // an old 32-bit plugin, run through carla's bridge
    }
    // carla needs a label for sound fonts too: their name
    else if (kind == "sf2")  { out.type = PLUGIN_SF2;  out.filename = rest; out.label = fs::u8path(rest).stem().u8string(); }
    else if (kind == "sfz")  { out.type = PLUGIN_SFZ;  out.filename = rest; out.label = fs::u8path(rest).stem().u8string(); }
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
// discovery

std::string discoveryTool(bool win32)
{
    const char* folder = carla_get_library_folder();
    std::vector<fs::path> candidates;
#ifdef _WIN32
    const char* name = win32 ? "carla-discovery-win32.exe" : "carla-discovery-native.exe";
#else
    if (win32)
        return {};
    const char* name = "carla-discovery-native";
#endif
    if (folder != nullptr && folder[0] != '\0')
        candidates.push_back(fs::u8path(folder) / name);
#ifndef _WIN32
    candidates.push_back(fs::path("/usr/lib/carla") / name);
    candidates.push_back(fs::path("/usr/local/lib/carla") / name);
#endif
    for (const fs::path& path : candidates)
    {
        std::error_code ec;
        if (fs::is_regular_file(path, ec))
            return path.u8string();
    }
    return {};
}

std::vector<std::string> discoveryArguments(const PluginEntry& file)
{
    PluginSpec spec;
    parseSpec(file.spec, spec);
    return {file.format == "VST3" ? "vst3" : "vst", spec.filename};
}

std::vector<PluginEntry> parseDiscovery(const std::string& output, const PluginEntry& file, bool win32)
{
    std::vector<PluginEntry> found;
    PluginEntry current;
    int64_t uniqueId = 0;
    bool inside = false;
    size_t pos = 0;
    while (pos < output.size())
    {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos)
            end = output.size();
        std::string line = output.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        static const std::string prefix = "carla-discovery::";
        if (line.compare(0, prefix.size(), prefix) != 0)
            continue;
        const auto sep = line.find("::", prefix.size());
        if (sep == std::string::npos)
            continue;
        const std::string key = line.substr(prefix.size(), sep - prefix.size());
        const std::string value = line.substr(sep + 2);

        if (key == "init")
        {
            current = file;
            current.name.clear();
            current.isSynth = false;
            uniqueId = 0;
            inside = true;
        }
        else if (!inside)
        {
            continue;
        }
        else if (key == "name")
            current.name = value;
        else if (key == "maker")
            current.maker = value;
        else if (key == "category")
            current.category = value;
        else if (key == "uniqueId")
            uniqueId = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "hints")
            current.isSynth = current.isSynth || (std::strtoul(value.c_str(), nullptr, 10) & PLUGIN_IS_SYNTH) != 0;
        else if (key == "audio.ins")
            current.audioIns = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "audio.outs")
            current.audioOuts = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "midi.ins")
            current.midiIns = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "end")
        {
            inside = false;
            if (current.name.empty())
                continue;
            current.isSynth = current.isSynth || current.category == "synth";
            found.push_back(current);
            found.back().spec = file.spec;
            if (win32)
                found.back().spec.replace(0, 4, "vst2-32");
            found.back().spec += "#" + std::to_string(uniqueId);
        }
    }
    // a single plugin keeps the plain spec; carla 2.5 always loads a VST3 bundle's first plugin anyway
    if (file.format == "VST3" && found.size() > 1)
        found.resize(1);
    if (found.size() == 1)
    {
        const auto hash = found.front().spec.rfind('#');
        found.front().spec.erase(hash);
    }
    return found;
}

// ---------------------------------------------------------------------------------------------------------------------
// engine

HostEngine::HostEngine() = default;

HostEngine::~HostEngine()
{
    stop();
}

bool HostEngine::start(const std::string& driver, Mode mode, const std::string& device)
{
    stop();
    {
        const std::lock_guard<std::mutex> lock(fPatchbayMutex);
        fGroups.clear();
        fConnections.clear();
    }
    fMode = mode;

    fHandle = carla_standalone_host_init();
    if (fHandle == nullptr)
        return false;

    carla_set_engine_callback(fHandle, engineCallback, this);

    carla_set_engine_option(fHandle, ENGINE_OPTION_PROCESS_MODE,
                            mode == Mode::Rack ? ENGINE_PROCESS_MODE_CONTINUOUS_RACK : ENGINE_PROCESS_MODE_PATCHBAY, nullptr);
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
    if (fOfflineStop)
    {
        auto offlineStop = std::move(fOfflineStop);
        fOfflineStop = nullptr;
        offlineStop();
        return;
    }
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
    if (fOfflineIdle)
    {
        fOfflineIdle();
        return;
    }
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
    return carla_add_plugin(fHandle, spec.binary, spec.type,
                            spec.filename.empty() ? nullptr : spec.filename.c_str(), nullptr,
                            spec.label.empty() ? nullptr : spec.label.c_str(), spec.uniqueId, nullptr, PLUGIN_OPTIONS_NULL);
}

void HostEngine::setPlayerFile(uint id, const std::string& path)
{
    if (fHandle != nullptr)
        carla_set_custom_data(fHandle, id, CUSTOM_DATA_TYPE_STRING, "file", path.c_str());
}

void HostEngine::sendNote(uint id, uint8_t channel, uint8_t note, uint8_t velocity)
{
    if (fHandle != nullptr)
        carla_send_midi_note(fHandle, id, channel, note, velocity);
}

bool HostEngine::savePluginState(uint id, const std::string& path)
{
    return fHandle != nullptr && carla_save_plugin_state(fHandle, id, path.c_str());
}

bool HostEngine::loadPluginState(uint id, const std::string& path)
{
    return fHandle != nullptr && carla_load_plugin_state(fHandle, id, path.c_str());
}

bool HostEngine::addInternalPlugin(const std::string& label, const std::string& name)
{
    fError.clear();
    return carla_add_plugin(fHandle, BINARY_NATIVE, PLUGIN_INTERNAL, nullptr, name.c_str(), label.c_str(),
                            0, nullptr, PLUGIN_OPTIONS_NULL);
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
    if (fMode == Mode::Rack)
        for (uint id = pluginCount() - 1; id > 0; --id)
        carla_switch_plugins(fHandle, id - 1, id);
    return true;
}

bool HostEngine::loadMidiFile(const std::string& path)
{
    fError.clear();
    return carla_load_file(fHandle, path.c_str());
}

bool HostEngine::removePlugin(uint id) { return carla_remove_plugin(fHandle, id); }
bool HostEngine::removeAllPlugins() { return carla_remove_all_plugins(fHandle); }

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
    s.label = info->label != nullptr ? info->label : "";
    s.volume = carla_get_internal_parameter_value(fHandle, id, PARAMETER_VOLUME);
    s.hasCustomUi = (info->hints & PLUGIN_HAS_CUSTOM_UI) != 0;
    s.active = carla_get_internal_parameter_value(fHandle, id, PARAMETER_ACTIVE) >= 0.5f;
    return s;
}

void HostEngine::setActive(uint id, bool active) { carla_set_active(fHandle, id, active); }
void HostEngine::setVolume(uint id, float volume) { carla_set_volume(fHandle, id, volume); }

void HostEngine::setPan(uint id, float pan)
{
    // right of centre the left output moves over to the right, left of centre the other way
    carla_set_balance_left(fHandle, id, pan > 0.0f ? -1.0f + 2.0f * pan : -1.0f);
    carla_set_balance_right(fHandle, id, pan < 0.0f ? 1.0f + 2.0f * pan : 1.0f);
}

std::string HostEngine::audioFilePath(uint id) const
{
    const char* value = carla_get_custom_data_value(fHandle, id, CUSTOM_DATA_TYPE_STRING, "file");
    return value != nullptr ? value : "";
}
void HostEngine::showCustomUi(uint id, bool show) { carla_show_custom_ui(fHandle, id, show); }

std::pair<float, float> HostEngine::outputPeaks(uint id) const
{
    return {carla_get_output_peak_value(fHandle, id, true), carla_get_output_peak_value(fHandle, id, false)};
}

std::vector<HostEngine::MidiProgram> HostEngine::midiPrograms(uint id) const
{
    std::vector<MidiProgram> out;
    const uint32_t count = carla_get_midi_program_count(fHandle, id);
    for (uint32_t i = 0; i < count; ++i)
        if (const MidiProgramData* data = carla_get_midi_program_data(fHandle, id, i))
            out.push_back({data->bank, data->program, data->name != nullptr ? data->name : ""});
    return out;
}

int HostEngine::currentMidiProgram(uint id) const { return carla_get_current_midi_program_index(fHandle, id); }
void HostEngine::setMidiProgram(uint id, uint32_t index) { carla_set_midi_program(fHandle, id, index); }

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

uint64_t HostEngine::transportFrame() const { return isRunning() ? carla_get_current_transport_frame(fHandle) : 0; }
void HostEngine::relocate(uint64_t frame) { carla_transport_relocate(fHandle, frame); }

// ---------------------------------------------------------------------------------------------------------------------
// patchbay

void HostEngine::trackPatchbay(EngineCallbackOpcode action, uint id, int value1, int value2, const char* valueStr)
{
    const std::lock_guard<std::mutex> lock(fPatchbayMutex);
    switch (action)
    {
    case ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED:
        fGroups[id] = Group{value2, valueStr != nullptr ? valueStr : "", {}, {}, {}, {}};
        break;
    case ENGINE_CALLBACK_PATCHBAY_CLIENT_REMOVED:
        fGroups.erase(id);
        break;
    case ENGINE_CALLBACK_PATCHBAY_CLIENT_DATA_CHANGED:
        fGroups[id].pluginId = value2;
        break;
    case ENGINE_CALLBACK_PATCHBAY_PORT_ADDED:
    {
        Group& g = fGroups[id];
        const bool input = (value2 & PATCHBAY_PORT_IS_INPUT) != 0;
        if ((value2 & PATCHBAY_PORT_TYPE_AUDIO) != 0)
            (input ? g.audioIns : g.audioOuts).push_back(static_cast<uint>(value1));
        else if ((value2 & PATCHBAY_PORT_TYPE_MIDI) != 0)
            (input ? g.midiIns : g.midiOuts).push_back(static_cast<uint>(value1));
        break;
    }
    case ENGINE_CALLBACK_PATCHBAY_PORT_REMOVED:
    {
        Group& g = fGroups[id];
        for (std::vector<uint>* ports : {&g.audioIns, &g.audioOuts, &g.midiIns, &g.midiOuts})
            ports->erase(std::remove(ports->begin(), ports->end(), static_cast<uint>(value1)), ports->end());
        break;
    }
    case ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED:
        fConnections.push_back(id);
        break;
    case ENGINE_CALLBACK_PATCHBAY_CONNECTION_REMOVED:
        fConnections.erase(std::remove(fConnections.begin(), fConnections.end(), id), fConnections.end());
        break;
    default:
        break;
    }
}

int HostEngine::groupOfPlugin(uint plugin) const
{
    const std::lock_guard<std::mutex> lock(fPatchbayMutex);
    for (const auto& [id, group] : fGroups)
        if (group.pluginId == static_cast<int>(plugin))
            return static_cast<int>(id);
    return -1;
}

int HostEngine::groupNamed(const std::string& name) const
{
    const std::lock_guard<std::mutex> lock(fPatchbayMutex);
    for (const auto& [id, group] : fGroups)
        if (group.pluginId < 0 && group.name == name)
            return static_cast<int>(id);
    return -1;
}

void HostEngine::refreshPatchbay()
{
    if (fMode != Mode::Patchbay || !isRunning())
        return;
    {
        const std::lock_guard<std::mutex> lock(fPatchbayMutex);
        fGroups.clear();
        fConnections.clear();
    }
    // carla re-announces every client, port and connection from inside this call
    carla_patchbay_refresh(fHandle, false);
}

void HostEngine::disconnectAll()
{
    if (fMode != Mode::Patchbay || !isRunning())
        return;
    std::vector<uint> connections;
    {
        const std::lock_guard<std::mutex> lock(fPatchbayMutex);
        connections = fConnections;
    }
    for (uint c : connections)
        carla_patchbay_disconnect(fHandle, false, c);
}

bool HostEngine::connectGroups(uint fromGroup, uint toGroup)
{
    std::vector<uint> outs, ins;
    {
        const std::lock_guard<std::mutex> lock(fPatchbayMutex);
        const auto from = fGroups.find(fromGroup);
        const auto to = fGroups.find(toGroup);
        if (from == fGroups.end() || to == fGroups.end())
            return false;
        outs = from->second.audioOuts;
        ins = to->second.audioIns;
    }
    if (outs.empty() || ins.empty())
        return false;
    std::sort(outs.begin(), outs.end());
    std::sort(ins.begin(), ins.end());

    // pair the first two channels; a mono side is spread to (or summed from) both
    const size_t channels = std::min<size_t>(2, std::max(outs.size(), ins.size()));
    bool ok = true;
    for (size_t c = 0; c < channels; ++c)
        ok = carla_patchbay_connect(fHandle, false, fromGroup, outs[std::min(c, outs.size() - 1)],
                                    toGroup, ins[std::min(c, ins.size() - 1)]) && ok;
    return ok;
}

bool HostEngine::connect(uint fromPlugin, uint toPlugin)
{
    const int from = groupOfPlugin(fromPlugin);
    const int to = groupOfPlugin(toPlugin);
    return from >= 0 && to >= 0 && connectGroups(static_cast<uint>(from), static_cast<uint>(to));
}

bool HostEngine::connectMidi(uint fromPlugin, uint toPlugin)
{
    const int from = groupOfPlugin(fromPlugin);
    const int to = groupOfPlugin(toPlugin);
    if (from < 0 || to < 0)
        return false;
    uint out = 0, in = 0;
    {
        const std::lock_guard<std::mutex> lock(fPatchbayMutex);
        const Group& a = fGroups[static_cast<uint>(from)];
        const Group& b = fGroups[static_cast<uint>(to)];
        if (a.midiOuts.empty() || b.midiIns.empty())
            return false;
        out = *std::min_element(a.midiOuts.begin(), a.midiOuts.end());
        in = *std::min_element(b.midiIns.begin(), b.midiIns.end());
    }
    return carla_patchbay_connect(fHandle, false, static_cast<uint>(from), out, static_cast<uint>(to), in);
}

bool HostEngine::connectToOutput(uint plugin)
{
    const int from = groupOfPlugin(plugin);
    const int to = groupNamed("Audio Output");
    return from >= 0 && to >= 0 && connectGroups(static_cast<uint>(from), static_cast<uint>(to));
}

bool HostEngine::saveProject(const std::string& path) { fError.clear(); return carla_save_project(fHandle, path.c_str()); }
bool HostEngine::loadProject(const std::string& path) { fError.clear(); return carla_load_project(fHandle, path.c_str()); }

void HostEngine::engineCallback(void* ptr, EngineCallbackOpcode action, uint pluginId,
                                int value1, int value2, int, float valuef, const char* valueStr)
{
    auto* self = static_cast<HostEngine*>(ptr);
    if (action >= ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED && action <= ENGINE_CALLBACK_PATCHBAY_CONNECTION_REMOVED)
    {
        self->trackPatchbay(action, pluginId, value1, value2, valueStr);
        return;
    }
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
