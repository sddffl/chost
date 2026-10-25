// Session: audio tracks and a master bus on top of Carla's patchbay.
//
// Every track is a chain of plugins wired in series:
//   [audio file player] -> inserts... -> fader -> master -> Audio Output
// The fader and the master are Carla's internal stereo gain plugin, scaled with carla's volume.

#pragma once

#include "HostEngine.h"

#include <string>
#include <vector>

namespace chost {

struct Track {
    std::string name;
    std::string file;          // original audio file; empty for a track without audio
    int source = -1;           // carla id of the audio file player, -1 without audio
    std::vector<uint> inserts; // carla ids, in processing order
    uint fader = 0;            // carla id of the track's gain plugin
    float volume = 1.0f;       // 0 .. 1.27
    bool muted = false;
    bool solo = false;
};

class Session {
public:
    explicit Session(HostEngine& host);

    // Creates the master bus. The engine must be running in patchbay mode and hold no plugins.
    bool init();
    bool isReady() const { return fReady; }
    std::string lastError() const { return fError; }

    const std::vector<Track>& tracks() const { return fTracks; }
    uint master() const { return fMaster; }
    float masterVolume() const { return fMasterVolume; }

    int addTrack(const std::string& name);                      // returns the new track's index, -1 on error
    bool setTrackFile(int track, const std::string& path);      // loads or replaces the track's audio
    bool removeTrack(int track);
    void renameTrack(int track, const std::string& name);

    bool addInsert(int track, const std::string& spec);
    bool removeInsert(int track, int index);
    bool moveInsert(int track, int index, int delta);

    void setVolume(int track, float volume);
    void setMuted(int track, bool muted);
    void setSolo(int track, bool solo);
    void setMasterVolume(float volume);

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

    HostEngine& fHost;
    std::vector<Track> fTracks;
    uint fMaster = 0;
    float fMasterVolume = 1.0f;
    bool fReady = false;
    std::string fError;
};

} // namespace chost
