// HostEngine: a thin C++ wrapper around libcarla's standalone host API.
// Shared by the command line tool and the Qt GUI.

#pragma once

#include "CarlaHost.h"

#include <functional>
#include <map>
#include <mutex>
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
    std::string category; // carla's category ("synth", "delay", "eq", ...), known after discovery
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

// Search paths: the platform defaults (overridable with LV2_PATH / VST3_PATH / VST2_PATH) plus the
// user's own plugin folders, which are searched for every format.
std::string defaultPluginPath(CarlaBackend::PluginType type);
void setExtraPluginFolders(const std::vector<std::string>& folders); // UTF-8 paths
std::vector<std::string> extraPluginFolders();
std::vector<PluginEntry> scanPlugins();

// VST files are found by name only; carla's discovery tool loads each one in a separate process (so a
// crashing plugin can't take chost down) and reports what it really is. VST2 "shells" such as Waves hold
// many plugins, each gets its own entry with a "#uniqueId" spec suffix.
std::string discoveryTool(bool win32 = false); // empty when missing; win32: the 32-bit one (Windows)
std::vector<std::string> discoveryArguments(const PluginEntry& file);
std::vector<PluginEntry> parseDiscovery(const std::string& output, const PluginEntry& file, bool win32 = false);

std::vector<std::string> audioDrivers();
std::string defaultAudioDriver();

class HostEngine {
public:
    // Rack: plugins run in series in slot order (works with every driver, including Dummy).
    // Patchbay: plugins are free nodes wired with connect(); needed for several tracks in parallel.
    // Carla's Dummy driver only supports the rack.
    enum class Mode { Rack, Patchbay };

    HostEngine();
    ~HostEngine();

    HostEngine(const HostEngine&) = delete;
    HostEngine& operator=(const HostEngine&) = delete;

    void setEventCallback(HostEventFunc func) { fEventFunc = std::move(func); }

    bool start(const std::string& driver, Mode mode = Mode::Rack, const std::string& device = {});
    // Offline (chost-render only, OfflineEngine.cpp): carla's engine runs inside chost as a patchbay plugin and
    // renderBlock() drives it one block at a time, as fast as the computer can, with the transport playing.
    bool startOffline(double sampleRate, uint32_t bufferSize);
    void renderBlock(float* left, float* right, uint32_t frames);
    Mode mode() const { return fMode; }
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
    bool addInternalPlugin(const std::string& label, const std::string& name);
    bool loadAudioFile(const std::string& path); // in rack mode it is moved to the first slot
    bool loadMidiFile(const std::string& path);  // carla's MIDI file player, appended at the end
    bool removePlugin(uint id);
    bool removeAllPlugins();
    bool movePlugin(uint id, int delta);
    uint pluginCount() const;
    RackSlot slot(uint id) const;
    void setActive(uint id, bool active);
    void setVolume(uint id, float volume);
    std::string audioFilePath(uint id) const; // file played by an "audiofile" or "midifile" slot
    void setPlayerFile(uint id, const std::string& path); // a MIDI file player reads (another) file in place
    void showCustomUi(uint id, bool show);
    std::pair<float, float> outputPeaks(uint id) const;

    // MIDI programs (the instruments of a sound font, a synth's presets)
    struct MidiProgram {
        uint32_t bank = 0;
        uint32_t program = 0;
        std::string name;
    };
    std::vector<MidiProgram> midiPrograms(uint id) const;
    int currentMidiProgram(uint id) const; // index into midiPrograms(), -1 without one
    void setMidiProgram(uint id, uint32_t index);

    // plays a note on an instrument right away (velocity 0: note off), e.g. while editing notes
    void sendNote(uint id, uint8_t channel, uint8_t note, uint8_t velocity);

    // a plugin's whole state (e.g. the library loaded in Kontakt) in a .carxs file
    bool savePluginState(uint id, const std::string& path);
    bool loadPluginState(uint id, const std::string& path);

    // parameters
    std::vector<ParamInfo> parameters(uint id) const;
    float parameterValue(uint id, uint32_t index) const;
    void setParameter(uint id, uint32_t index, float value);
    int findParameter(uint id, const std::string& nameOrIndex) const;

    // patchbay wiring, plugin outputs to plugin inputs (stereo, mono is spread or summed)
    void refreshPatchbay(); // re-reads groups and ports; plugin ids shift when a plugin is removed
    void disconnectAll();
    bool connect(uint fromPlugin, uint toPlugin);
    bool connectMidi(uint fromPlugin, uint toPlugin); // first MIDI output to first MIDI input
    bool connectToOutput(uint plugin);

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

    struct Group {
        int pluginId = -1;
        std::string name;
        std::vector<uint> audioIns, audioOuts;
        std::vector<uint> midiIns, midiOuts;
    };
    void trackPatchbay(CarlaBackend::EngineCallbackOpcode action, uint id, int value1, int value2, const char* valueStr);
    bool connectGroups(uint fromGroup, uint toGroup);
    int groupOfPlugin(uint plugin) const;
    int groupNamed(const std::string& name) const;

    CarlaHostHandle fHandle = nullptr;
    Mode fMode = Mode::Rack;
    std::function<void()> fOfflineStop; // tears down the offline engine
    std::function<void()> fOfflineIdle; // the offline engine's idle work (file reading, graph changes)
    void* fOffline = nullptr;           // OfflineEngine.cpp's state
    mutable std::mutex fPatchbayMutex;
    std::map<uint, Group> fGroups;
    std::vector<uint> fConnections;
    std::string fDriver;
    std::string fError; // errors from chost itself, carla keeps its own
    HostEventFunc fEventFunc;
};

} // namespace chost
