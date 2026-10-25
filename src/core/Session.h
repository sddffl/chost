// Session: audio tracks and a master bus on top of Carla's patchbay.
//
// Every track is a chain of plugins wired in series:
//   audio track:       [audio file player] -> inserts... -> fader -> master -> Audio Output
//   instrument track:  [MIDI file player] =MIDI=> [instrument] -> inserts... -> fader -> master
// The fader and the master are Carla's internal stereo gain plugin, scaled with carla's volume.

#pragma once

#include "HostEngine.h"

#include <string>
#include <vector>

namespace chost {

enum class TrackKind { Audio, Instrument };

struct Track {
    TrackKind kind = TrackKind::Audio;
    std::string name;
    std::string role;          // what the track holds, e.g. "kick" or "bass" after stem separation; may be empty
    std::string file;          // original audio file, or the MIDI file of an instrument track; may be empty
    int source = -1;           // carla id of the audio or MIDI file player, -1 without a file
    int instrument = -1;       // instrument tracks: carla id of the instrument plugin, -1 without one
    std::string instrumentSpec; // what was loaded as the instrument, e.g. "vst3:C:/.../Kontakt.vst3"
    std::vector<uint> inserts; // carla ids, in processing order
    uint fader = 0;            // carla id of the track's gain plugin
    float volume = 1.0f;       // 0 .. 1.27
    bool muted = false;
    bool solo = false;
};

class Session {
public:
    explicit Session(HostEngine& host);

    // Creates the master bus. The engine must be running and hold no plugins.
    bool init();
    bool isReady() const { return fReady; }
    // Tracks are only mixed in patchbay mode. In rack mode (the Dummy driver, which has no sound anyway)
    // the plugins sit in one chain and the model is kept without wiring.
    bool isRouted() const { return fHost.mode() == HostEngine::Mode::Patchbay; }
    std::string lastError() const { return fError; }

    const std::vector<Track>& tracks() const { return fTracks; }
    uint master() const { return fMaster; }
    float masterVolume() const { return fMasterVolume; }

    int addTrack(const std::string& name, TrackKind kind = TrackKind::Audio); // the new track's index, -1 on error
    bool setTrackFile(int track, const std::string& path);      // loads or replaces the track's audio or MIDI file
    bool reloadMidiFile(int track, const std::string& path);    // an edited MIDI file, without replacing the player
    bool setInstrument(int track, const std::string& spec);     // loads or replaces an instrument track's instrument
    void setRole(int track, const std::string& role);
    bool removeTrack(int track);
    void renameTrack(int track, const std::string& name);
    bool moveTrack(int from, int to); // display order only, the wiring doesn't depend on it

    bool addInsert(int track, const std::string& spec);
    bool removeInsert(int track, int index);
    bool moveInsert(int track, int index, int delta);

    void setVolume(int track, float volume);
    void setMuted(int track, bool muted);
    void setSolo(int track, bool solo);
    void setMasterVolume(float volume);

    // The song's tempo (the grid, bars and beats, the tempo of new MIDI) and key, e.g. "A minor";
    // found by the analysis or typed in. The key is empty until known.
    double tempo() const { return fTempo; }
    void setTempo(double bpm);
    const std::string& key() const { return fKey; }
    void setKey(const std::string& key) { fKey = key; }

    // A .chost file is a small text file next to a Carla .carxp holding the plugins and their state.
    bool save(const std::string& path);
    bool load(const std::string& path);

    // Rewires every track and the master; called after any change to the plugin layout.
    void reroute();

private:
    bool valid(int track) const { return track >= 0 && track < static_cast<int>(fTracks.size()); }
    bool addFader(const std::string& name, uint& id);
    void removePlugin(uint id);
    void applyGains();
    void matchProgram(int track);

    HostEngine& fHost;
    std::vector<Track> fTracks;
    uint fMaster = 0;
    float fMasterVolume = 1.0f;
    double fTempo = 120.0;
    std::string fKey;
    bool fReady = false;
    std::string fError;
};

} // namespace chost
