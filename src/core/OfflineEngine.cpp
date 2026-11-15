// HostEngine's offline mode, only built into chost-render: it links carla's plugin libraries
// (carla_native-plugin and carla_host-plugin) instead of the standalone one.

#include "HostEngine.h"

#include "Transport.h"

#include "CarlaNativePlugin.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

CARLA_BACKEND_USE_NAMESPACE

namespace chost {

namespace {

struct Offline {
    const NativePluginDescriptor* desc = nullptr;
    NativePluginHandle plugin = nullptr;
    NativeHostDescriptor host{};
    NativeTimeInfo time{};
    double sampleRate = 48000.0;
    uint32_t bufferSize = 512;
    std::string resources;

    // the song's tempo and time signature, set from the main thread, read by whichever thread processes
    static constexpr double kTicksPerBeat = 1920.0;
    std::atomic<double> bpm{120.0};
    std::atomic<double> beatsPerBar{4.0};
    std::atomic<double> beatType{4.0};

    // what the engine's audio inputs get: silence, except while settleOffline() looks for its probe
    std::atomic<float> inputLevel{0.0f};
    bool settled = false; // settleOffline() has run
    bool offline = false; // carla has been told it renders offline

    // Until the render starts a stand-in audio thread keeps the engine turning with the transport
    // stopped: carla waits for the audio thread to take every added plugin and connection.
    std::thread pump;
    std::atomic<bool> pumping{false};

    // the bar and beat of the block about to be processed: what tempo-synced plugins lock to
    void updateTime()
    {
        const double tempo = bpm, perBar = beatsPerBar;
        const BarBeat at = barBeatAt(time.frame, sampleRate, tempo, perBar, kTicksPerBeat);
        time.bbt.valid = true;
        time.bbt.bar = at.bar;
        time.bbt.beat = at.beat;
        time.bbt.tick = at.tick;
        time.bbt.barStartTick = at.barStartTick;
        time.bbt.beatsPerBar = static_cast<float>(perBar);
        time.bbt.beatType = static_cast<float>(beatType.load());
        time.bbt.ticksPerBeat = kTicksPerBeat;
        time.bbt.beatsPerMinute = tempo;
    }

    void process(float* left, float* right, uint32_t frames)
    {
        updateTime();
        std::vector<float> input(frames, inputLevel.load());
        float* ins[2] = {input.data(), input.data()};
        float* outs[2] = {left, right};
        desc->process(plugin, ins, outs, frames, nullptr, 0);
    }

    void startPump()
    {
        pumping = true;
        pump = std::thread([this] {
            std::vector<float> l(bufferSize), r(bufferSize);
            while (pumping)
            {
                process(l.data(), r.data(), bufferSize);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }

    void haltPump()
    {
        if (!pumping)
            return;
        pumping = false;
        pump.join();
    }

    void stopPump()
    {
        haltPump();
        if (offline)
            return;
        offline = true;
        // offline: plugins wait for their locks and file readers read on the spot instead of skipping
        if (desc->dispatcher != nullptr)
            desc->dispatcher(plugin, NATIVE_PLUGIN_OPCODE_OFFLINE_CHANGED, 0, 1, nullptr, 0.0f);
    }
};

Offline& self(NativeHostHandle handle) { return *static_cast<Offline*>(handle); }

uint32_t getBufferSize(NativeHostHandle h) { return self(h).bufferSize; }
double getSampleRate(NativeHostHandle h) { return self(h).sampleRate; }
bool isOffline(NativeHostHandle) { return true; }
const NativeTimeInfo* getTimeInfo(NativeHostHandle h) { return &self(h).time; }
bool writeMidiEvent(NativeHostHandle, const NativeMidiEvent*) { return false; }
void uiParameterChanged(NativeHostHandle, uint32_t, float) {}
void uiMidiProgramChanged(NativeHostHandle, uint8_t, uint32_t, uint32_t) {}
void uiCustomDataChanged(NativeHostHandle, const char*, const char*) {}
void uiClosed(NativeHostHandle) {}
const char* uiOpenFile(NativeHostHandle, bool, const char*, const char*) { return nullptr; }
const char* uiSaveFile(NativeHostHandle, bool, const char*, const char*) { return nullptr; }
intptr_t dispatcher(NativeHostHandle, NativeHostDispatcherOpcode, int32_t, intptr_t, void*, float) { return 0; }

} // namespace

bool HostEngine::startOffline(double sampleRate, uint32_t bufferSize)
{
    stop();
    {
        const std::lock_guard<std::mutex> lock(fPatchbayMutex);
        fGroups.clear();
        fConnections.clear();
    }
    fMode = Mode::Patchbay;

    auto* o = new Offline;
    o->sampleRate = sampleRate;
    o->bufferSize = bufferSize;
    const std::string libDir = carla_get_library_folder();
#ifdef _WIN32
    o->resources = libDir + "\\resources";
#else
    o->resources = libDir + "/resources";
#endif
    NativeHostDescriptor& h = o->host;
    h.handle = o;
    h.resourceDir = o->resources.c_str();
    h.uiName = "chost-render";
    h.get_buffer_size = getBufferSize;
    h.get_sample_rate = getSampleRate;
    h.is_offline = isOffline;
    h.get_time_info = getTimeInfo;
    h.write_midi_event = writeMidiEvent;
    h.ui_parameter_changed = uiParameterChanged;
    h.ui_midi_program_changed = uiMidiProgramChanged;
    h.ui_custom_data_changed = uiCustomDataChanged;
    h.ui_closed = uiClosed;
    h.ui_open_file = uiOpenFile;
    h.ui_save_file = uiSaveFile;
    h.dispatcher = dispatcher;
    o->time.playing = false;
    o->bpm = fTempo;
    o->beatsPerBar = fBeatsPerBar;
    o->beatType = fBeatType;

    o->desc = carla_get_native_patchbay_plugin();
    o->plugin = o->desc != nullptr ? o->desc->instantiate(&o->host) : nullptr;
    if (o->plugin == nullptr)
    {
        fError = "cannot start carla's engine";
        delete o;
        return false;
    }
    fHandle = carla_create_native_plugin_host_handle(o->desc, o->plugin);
    carla_set_engine_callback(fHandle, engineCallback, this);
    for (PluginType type : {PLUGIN_LV2, PLUGIN_VST3, PLUGIN_VST2})
        carla_set_engine_option(fHandle, ENGINE_OPTION_PLUGIN_PATH, type, defaultPluginPath(type).c_str());
    if (o->desc->activate != nullptr)
        o->desc->activate(o->plugin);
    o->startPump();

    fOffline = o;
    fDriver = "Offline";
    fOfflineTempo = [this, o] {
        o->bpm = fTempo;
        o->beatsPerBar = fBeatsPerBar;
        o->beatType = fBeatType;
    };
    fOfflineIdle = [o] {
        if (o->desc->dispatcher != nullptr)
            o->desc->dispatcher(o->plugin, NATIVE_PLUGIN_OPCODE_IDLE, 0, 0, nullptr, 0.0f);
    };
    fOfflineStop = [this, o] {
        // removing plugins waits for the audio thread too
        o->time.playing = false;
        if (!o->pumping)
            o->startPump();
        carla_remove_all_plugins(fHandle);
        o->pumping = false;
        o->pump.join();
        if (o->desc->deactivate != nullptr)
            o->desc->deactivate(o->plugin);
        carla_host_handle_free(fHandle);
        o->desc->cleanup(o->plugin);
        delete o;
        fHandle = nullptr;
        fOffline = nullptr;
        fOfflineIdle = nullptr;
        fOfflineTempo = nullptr;
        fDriver.clear();
    };
    return true;
}

// Carla applies changes to its graph (the session's connections) on a timer of its own, some tens of
// milliseconds after they are made; a render that starts before that is silent from start to end. Nothing in
// carla's API says when it has happened, so it is watched for: a probe is wired last, the engine's audio
// input into a gain of its own (going nowhere, so it never reaches the mix), and the input gets a steady
// signal. Once the probe's output meter shows it, the graph holds that connection and every one made before
// it. A slow computer or a heavy project simply takes longer; the old fixed wait is only the fallback.
//
// Nothing may process while carla rebuilds its graph: under heavy load its graph doesn't survive that (it
// asserts and crashes). So once the probe is taken in, the stand-in audio thread stops, as it did for the
// old fixed wait, and a single block every 15 ms shows whether the rebuild has happened.
void HostEngine::settleOffline()
{
    using namespace std::chrono;
    auto* o = static_cast<Offline*>(fOffline);
    constexpr auto kTimeout = seconds(60);
    const auto start = steady_clock::now();
    fSettleObserved = false;
    if (addInternalPlugin("audiogain_s", "chost-render settle probe"))
    {
        const uint probe = pluginCount() - 1;
        refreshPatchbay();
        const int input = groupNamed("Audio Input");
        const int target = groupOfPlugin(probe);
        if (input >= 0 && target >= 0 && connectGroups(static_cast<uint>(input), static_cast<uint>(target)))
        {
            o->inputLevel = 1.0f;
            std::this_thread::sleep_for(milliseconds(5)); // the stand-in audio thread takes the probe in
            o->haltPump();
            std::vector<float> left(o->bufferSize), right(o->bufferSize);
            while (steady_clock::now() - start < kTimeout)
            {
                std::this_thread::sleep_for(milliseconds(15));
                o->process(left.data(), right.data(), o->bufferSize);
                if (carla_get_output_peak_value(fHandle, probe, true) > 0.5f)
                {
                    fSettleObserved = true;
                    break;
                }
            }
            o->inputLevel = 0.0f;
            // carla ramps every gain to its value after a rebuild (what made the old fixed wait fade the first
            // 50 ms of every export in): 100 ms of silence lets the ramps finish (they take about 40) and the
            // probe's signal go, so the render is right from its first sample
            const auto tail = static_cast<int>(std::ceil(0.1 * o->sampleRate / o->bufferSize));
            for (int i = 0; i < tail; ++i)
                o->process(left.data(), right.data(), o->bufferSize);
        }
    }
    if (!fSettleObserved)
    {
        std::fprintf(stderr, "chost-render: could not see carla apply the connections, waiting a fixed 500 ms instead\n");
        o->haltPump();
        std::this_thread::sleep_for(milliseconds(500));
    }
    fSettleMs = duration<double, std::milli>(steady_clock::now() - start).count();
}

void HostEngine::renderBlock(float* left, float* right, uint32_t frames)
{
    auto* o = static_cast<Offline*>(fOffline);
    if (o == nullptr)
    {
        std::memset(left, 0, frames * sizeof(float));
        std::memset(right, 0, frames * sizeof(float));
        return;
    }
    if (!o->settled)
    {
        o->settled = true;
        settleOffline();
    }
    o->stopPump(); // the render takes over as the audio thread
    o->time.playing = true;
    o->process(left, right, frames);
    o->time.frame += frames;
    o->time.usecs = static_cast<uint64_t>(o->time.frame * 1e6 / o->sampleRate);
}

} // namespace chost
