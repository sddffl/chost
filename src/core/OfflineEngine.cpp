// HostEngine's offline mode, only built into chost-render: it links carla's plugin libraries
// (carla_native-plugin and carla_host-plugin) instead of the standalone one.

#include "HostEngine.h"

#include "CarlaNativePlugin.h"

#include <atomic>
#include <chrono>
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

    // Until the render starts a stand-in audio thread keeps the engine turning with the transport
    // stopped: carla waits for the audio thread to take every added plugin and connection.
    std::thread pump;
    std::atomic<bool> pumping{false};

    void process(float* left, float* right, uint32_t frames)
    {
        std::vector<float> silence(frames, 0.0f);
        float* ins[2] = {silence.data(), silence.data()};
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

    void stopPump()
    {
        if (!pumping)
            return;
        pumping = false;
        pump.join();
        // the patchbay applies connection changes on a timer of its own, give it time for the last ones
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
        fDriver.clear();
    };
    return true;
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
    o->stopPump(); // the render takes over as the audio thread
    o->time.playing = true;
    o->process(left, right, frames);
    o->time.frame += frames;
    o->time.usecs = static_cast<uint64_t>(o->time.frame * 1e6 / o->sampleRate);
}

} // namespace chost
