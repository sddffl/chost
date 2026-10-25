// Session: audio tracks and a master bus on top of Carla's patchbay.
//
// Every track is a chain of plugins wired in series:
//   audio track:       [audio file player] -> inserts... -> fader -> master -> Audio Output
//   instrument track:  [MIDI file player] =MIDI=> [instrument] -> inserts... -> fader -> master
// The fader and the master are Carla's internal stereo gain plugin, scaled with carla's volume.

#pragma once

#include "HostEngine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace chost {

enum class TrackKind { Audio, Instrument };

// Carla numbers its plugins 0, 1, 2... and renumbers them whenever one goes; a Uid names a track or a
// plugin for as long as the session lives, which is what undo needs to find them again.
using Uid = uint64_t;

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
    uint32_t colour = 0;       // 0xRRGGBB as shown in the GUI, 0 until it has one
    Uid uid = 0;
};

// A named place in the song (verse, chorus, ...), shown on the ruler.
struct Marker {
    double seconds = 0.0;
    std::string name;
    bool operator==(const Marker& o) const { return seconds == o.seconds && name == o.name; }
};

// Everything a user can change about a session, plugins by Uid: what undo goes back to.
struct SessionState {
    struct TrackState {
        Uid uid = 0;
        TrackKind kind = TrackKind::Audio;
        std::string name, role, file, instrumentSpec;
        Uid instrument = 0;              // 0: none
        std::vector<Uid> inserts;
        std::vector<bool> insertsActive; // bypassed or not, parallel to inserts
        float volume = 1.0f;
        bool muted = false, solo = false;
        uint32_t colour = 0;
        bool operator==(const TrackState& o) const;
    };
    std::vector<TrackState> tracks;
    float masterVolume = 1.0f;
    double tempo = 120.0;
    std::string key;
    std::vector<Marker> markers;
    bool operator==(const SessionState& o) const;
    bool operator!=(const SessionState& o) const { return !(*this == o); }
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
    void setColour(int track, uint32_t colour);
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

    // Markers, in time order.
    const std::vector<Marker>& markers() const { return fMarkers; }
    void setMarkers(std::vector<Marker> markers);

    // Undo. With keepRemoved, a removed track, insert or replaced instrument isn't deleted but set aside,
    // silent and switched off, so restore() can bring it back exactly as it was (the library loaded in
    // Kontakt, every knob); dropUnused() deletes what no state kept for undo refers to any more.
    void setKeepRemoved(bool keep) { fKeepRemoved = keep; }
    SessionState state() const;
    void restore(const SessionState& state);
    void dropUnused(const std::vector<const SessionState*>& keep);

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
    Uid uidOf(int id) const;           // 0 for -1
    int idOf(Uid uid) const;           // -1 when gone
    void registerAdded(bool atFront = false); // a uid for the plugin carla just added
    void park(uint id);                // an insert or instrument set aside
    int unpark(Uid uid);               // its carla id again, -1 when it's gone
    void parkTrack(int track);
    bool setPlayer(int track, const std::string& path); // setTrackFile without the rewiring

    HostEngine& fHost;
    std::vector<Track> fTracks;
    std::vector<Uid> fUids;              // per carla plugin id
    Uid fNextUid = 1;
    bool fKeepRemoved = false;
    std::vector<Track> fParkedTracks;    // removed tracks, their plugins still loaded
    std::vector<uint> fParkedPlugins;    // removed inserts and replaced instruments
    uint fMaster = 0;
    float fMasterVolume = 1.0f;
    double fTempo = 120.0;
    std::string fKey;
    std::vector<Marker> fMarkers;
    bool fReady = false;
    std::string fError;
};

} // namespace chost
