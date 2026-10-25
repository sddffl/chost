// HostEngine: a thin C++ wrapper around libcarla's standalone host API.
// Shared by the command line tool and the Qt GUI.

#pragma once

#include "CarlaHost.h"

#include <functional>
#include <string>
#include <vector>

namespace chost {

// One entry found by the plugin scanner, loadable with HostEngine::addPlugin(spec)
struct PluginEntry {
    std::string spec;    // e.g. "lv2:urn:zamaudio:ZamComp", "vst3:/usr/lib/vst3/ZamComp.vst3"
    std::string format;  // "LV2", "VST3", ...
    std::string name;
    std::string maker;
    uint32_t audioIns = 0;
    uint32_t audioOuts = 0;
    uint32_t midiIns = 0;
    bool isSynth = false;
};

struct ParamInfo {
    uint32_t index = 0;
    std::string name;
    std::string symbol;
    std::string unit;
    float min = 0.0f;
    float max = 1.0f;
    float def = 0.0f;
    float value = 0.0f;
    bool isOutput = false;
    bool isBoolean = false;
    bool isInteger = false;
};

struct RackSlot {
    uint id = 0;
    std::string name;
    std::string maker;
    std::string label;   // "audiofile" for carla's audio file player
    bool active = true;
    bool hasCustomUi = false;
    float volume = 1.0f; // carla's post-plugin volume, 0 .. 1.27
};

// Events forwarded from Carla. Called from whichever thread Carla uses, so GUI code must queue them.
enum class HostEvent { PluginAdded, PluginRemoved, PluginRenamed, ParameterChanged, UiClosed, Reload, Error, EngineStopped };
using HostEventFunc = std::function<void(HostEvent event, uint pluginId, int index, float value, const std::string& text)>;

// Default search paths, overridable with LV2_PATH / VST3_PATH / VST2_PATH
std::string defaultPluginPath(CarlaBackend::PluginType type);
std::vector<PluginEntry> scanPlugins();

std::vector<std::string> audioDrivers();
std::string defaultAudioDriver();

class HostEngine {
public:
    HostEngine();
    ~HostEngine();

    HostEngine(const HostEngine&) = delete;
    HostEngine& operator=(const HostEngine&) = delete;

    void setEventCallback(HostEventFunc func) { fEventFunc = std::move(func); }

    bool start(const std::string& driver, const std::string& device = {});
    void stop();
    bool isRunning() const;
    void idle();

    std::string lastError() const;
    std::string driverName() const { return fDriver; }
    double sampleRate() const;
    uint32_t bufferSize() const;
    float dspLoad() const;

    // rack
    bool addPlugin(const std::string& spec);
    bool loadAudioFile(const std::string& path);
    bool removePlugin(uint id);
    bool movePlugin(uint id, int delta);
    uint pluginCount() const;
    RackSlot slot(uint id) const;
    void setActive(uint id, bool active);
    void setVolume(uint id, float volume);
    std::string audioFilePath(uint id) const; // file played by an "audiofile" slot
    void showCustomUi(uint id, bool show);
    std::pair<float, float> outputPeaks(uint id) const;

    // parameters
    std::vector<ParamInfo> parameters(uint id) const;
    float parameterValue(uint id, uint32_t index) const;
    void setParameter(uint id, uint32_t index, float value);
    int findParameter(uint id, const std::string& nameOrIndex) const;

    // transport / project
    void play();
    void pause();
    bool isPlaying() const;
    uint64_t transportFrame() const;
    void relocate(uint64_t frame);
    bool saveProject(const std::string& path);
    bool loadProject(const std::string& path);

private:
    static void engineCallback(void* ptr, CarlaBackend::EngineCallbackOpcode action, uint pluginId,
                               int value1, int value2, int value3, float valuef, const char* valueStr);

    CarlaHostHandle fHandle = nullptr;
    std::string fDriver;
    std::string fError; // errors from chost itself, carla keeps its own
    HostEventFunc fEventFunc;
};

} // namespace chost
