#include "Session.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace chost {

static const char* const kGainLabel = "audiogain_s"; // carla's internal stereo gain

Session::Session(HostEngine& host)
    : fHost(host)
{
}

bool Session::init()
{
    fTracks.clear();
    fReady = false;
    fError.clear();
    if (!fHost.isRunning())
    {
        fError = "the audio engine is not running";
        return false;
    }
    if (!addFader("Stereo Out", fMaster))
        return false;
    fMasterVolume = 1.0f;
    fReady = true;
    reroute();
    return true;
}

bool Session::addFader(const std::string& name, uint& id)
{
    if (!fHost.addInternalPlugin(kGainLabel, name))
    {
        fError = fHost.lastError();
        return false;
    }
    id = fHost.pluginCount() - 1;
    return true;
}

// carla renumbers every plugin after a removed one, the model follows
void Session::removePlugin(uint id)
{
    fHost.removePlugin(id);
    auto shift = [id](uint& other) {
        if (other > id)
            --other;
    };
    shift(fMaster);
    for (Track& t : fTracks)
    {
        if (t.source > static_cast<int>(id))
            --t.source;
        shift(t.fader);
        for (uint& insert : t.inserts)
            shift(insert);
    }
}

int Session::addTrack(const std::string& name)
{
    if (!fReady)
        return -1;
    Track t;
    t.name = name;
    if (!addFader(name, t.fader))
        return -1;
    fTracks.push_back(t);
    reroute();
    return static_cast<int>(fTracks.size()) - 1;
}

bool Session::setTrackFile(int track, const std::string& path)
{
    if (!valid(track))
        return false;
    if (fTracks[track].source >= 0)
    {
        removePlugin(static_cast<uint>(fTracks[track].source));
        fTracks[track].source = -1;
        fTracks[track].file.clear();
    }
    if (!fHost.loadAudioFile(path))
    {
        fError = fHost.lastError();
        reroute();
        return false;
    }
    uint id = fHost.pluginCount() - 1;
    if (!isRouted())
    {
        // the rack moved the player to the front, everything else moved down one
        id = 0;
        auto shift = [](uint& other) { ++other; };
        shift(fMaster);
        for (Track& t : fTracks)
        {
            if (t.source >= 0)
                ++t.source;
            shift(t.fader);
            for (uint& insert : t.inserts)
                shift(insert);
        }
    }
    // the player follows the timeline once instead of looping
    if (const int loop = fHost.findParameter(id, "Loop Mode"); loop >= 0)
        fHost.setParameter(id, static_cast<uint32_t>(loop), 0.0f);

    fTracks[track].source = static_cast<int>(id);
    fTracks[track].file = path;
    reroute();
    return true;
}

bool Session::removeTrack(int track)
{
    if (!valid(track))
        return false;
    // remove from the highest id down so the remaining ids stay valid while we go
    std::vector<uint> ids = fTracks[track].inserts;
    ids.push_back(fTracks[track].fader);
    if (fTracks[track].source >= 0)
        ids.push_back(static_cast<uint>(fTracks[track].source));
    std::sort(ids.rbegin(), ids.rend());
    fTracks.erase(fTracks.begin() + track);
    for (uint id : ids)
        removePlugin(id);
    reroute();
    return true;
}

void Session::renameTrack(int track, const std::string& name)
{
    if (valid(track))
        fTracks[track].name = name;
}

bool Session::moveTrack(int from, int to)
{
    if (!valid(from) || !valid(to))
        return false;
    const Track t = fTracks[from];
    fTracks.erase(fTracks.begin() + from);
    fTracks.insert(fTracks.begin() + to, t);
    return true;
}

bool Session::addInsert(int track, const std::string& spec)
{
    if (!valid(track))
        return false;
    if (!fHost.addPlugin(spec))
    {
        fError = fHost.lastError();
        return false;
    }
    fTracks[track].inserts.push_back(fHost.pluginCount() - 1);
    reroute();
    return true;
}

bool Session::removeInsert(int track, int index)
{
    if (!valid(track) || index < 0 || index >= static_cast<int>(fTracks[track].inserts.size()))
        return false;
    const uint id = fTracks[track].inserts[index];
    fTracks[track].inserts.erase(fTracks[track].inserts.begin() + index);
    removePlugin(id);
    reroute();
    return true;
}

bool Session::moveInsert(int track, int index, int delta)
{
    if (!valid(track))
        return false;
    std::vector<uint>& inserts = fTracks[track].inserts;
    const int target = index + delta;
    if (index < 0 || target < 0 || index >= static_cast<int>(inserts.size()) || target >= static_cast<int>(inserts.size()))
        return false;
    // order lives in the wiring, the plugins themselves don't move
    std::swap(inserts[index], inserts[target]);
    reroute();
    return true;
}

void Session::setVolume(int track, float volume)
{
    if (!valid(track))
        return;
    fTracks[track].volume = volume;
    applyGains();
}

void Session::setMuted(int track, bool muted)
{
    if (!valid(track))
        return;
    fTracks[track].muted = muted;
    applyGains();
}

void Session::setSolo(int track, bool solo)
{
    if (!valid(track))
        return;
    fTracks[track].solo = solo;
    applyGains();
}

void Session::setMasterVolume(float volume)
{
    fMasterVolume = volume;
    applyGains();
}

void Session::applyGains()
{
    const bool anySolo = std::any_of(fTracks.begin(), fTracks.end(), [](const Track& t) { return t.solo; });
    for (const Track& t : fTracks)
    {
        const bool silent = t.muted || (anySolo && !t.solo);
        fHost.setVolume(t.fader, silent ? 0.0f : t.volume);
    }
    if (fReady)
        fHost.setVolume(fMaster, fMasterVolume);
}

void Session::reroute()
{
    if (!fReady || !isRouted())
    {
        applyGains();
        return;
    }
    fHost.refreshPatchbay();
    fHost.disconnectAll();

    for (const Track& t : fTracks)
    {
        std::vector<uint> chain;
        if (t.source >= 0)
            chain.push_back(static_cast<uint>(t.source));
        // a bypassed insert is wired around: carla doesn't process an inactive plugin at all
        for (uint insert : t.inserts)
            if (fHost.slot(insert).active)
                chain.push_back(insert);
        chain.push_back(t.fader);
        chain.push_back(fMaster);
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            fHost.connect(chain[i], chain[i + 1]);
    }
    fHost.connectToOutput(fMaster);
    applyGains();
}

// ---------------------------------------------------------------------------------------------------------------------
// files
//
//   chost-session 1
//   carla <file name of the .carxp, next to this file>
//   master <id> <volume>
//   track <source> <fader> <volume> <muted> <solo> <insert count> <inserts...>
//   name <track name>
//   file <audio file path>

bool Session::save(const std::string& path)
{
    const fs::path sessionPath = fs::u8path(path);
    fs::path carlaPath = sessionPath;
    carlaPath.replace_extension(".carxp");

    if (!fHost.saveProject(carlaPath.u8string()))
    {
        fError = fHost.lastError();
        return false;
    }

    std::ofstream out(sessionPath);
    if (!out)
    {
        fError = "could not write " + path;
        return false;
    }
    out << "chost-session 1\n";
    out << "carla " << carlaPath.filename().u8string() << "\n";
    out << "master " << fMaster << " " << fMasterVolume << "\n";
    for (const Track& t : fTracks)
    {
        out << "track " << t.source << " " << t.fader << " " << t.volume << " " << t.muted << " " << t.solo << " "
            << t.inserts.size();
        for (uint id : t.inserts)
            out << " " << id;
        out << "\nname " << t.name << "\nfile " << t.file << "\n";
    }
    return static_cast<bool>(out);
}

bool Session::load(const std::string& path)
{
    std::ifstream in(fs::u8path(path));
    std::string line;
    if (!in || !std::getline(in, line) || line != "chost-session 1")
    {
        fError = "not a chost session: " + path;
        return false;
    }

    std::vector<Track> tracks;
    uint master = 0;
    float masterVolume = 1.0f;
    std::string carlaFile;
    while (std::getline(in, line))
    {
        const auto space = line.find(' ');
        const std::string key = line.substr(0, space);
        const std::string rest = space == std::string::npos ? "" : line.substr(space + 1);
        std::istringstream fields(rest);

        if (key == "carla")
            carlaFile = rest;
        else if (key == "master")
            fields >> master >> masterVolume;
        else if (key == "track")
        {
            Track t;
            size_t count = 0;
            fields >> t.source >> t.fader >> t.volume >> t.muted >> t.solo >> count;
            t.inserts.resize(count);
            for (uint& id : t.inserts)
                fields >> id;
            tracks.push_back(t);
        }
        else if (key == "name" && !tracks.empty())
            tracks.back().name = rest;
        else if (key == "file" && !tracks.empty())
            tracks.back().file = rest;
    }

    const fs::path carlaPath = fs::u8path(path).parent_path() / fs::u8path(carlaFile);
    fHost.removeAllPlugins();
    fReady = false;
    if (!fHost.loadProject(carlaPath.u8string()))
    {
        fError = fHost.lastError();
        fReady = false;
        return false;
    }

    fTracks = tracks;
    fMaster = master;
    fMasterVolume = masterVolume;
    fReady = true;
    reroute();
    return true;
}

} // namespace chost
