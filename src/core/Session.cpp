#include "Session.h"

#include "MidiFile.h"

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
    fUids.clear();
    fParkedTracks.clear();
    fParkedPlugins.clear();
    fTempo = 120.0;
    fKey.clear();
    fMarkers.clear();
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
    registerAdded();
    return true;
}

void Session::registerAdded(bool atFront)
{
    // anything added behind the session's back gets a uid too, so the list stays parallel to carla's
    while (fUids.size() + 1 < fHost.pluginCount())
        fUids.push_back(fNextUid++);
    if (atFront)
        fUids.insert(fUids.begin(), fNextUid++);
    else
        fUids.push_back(fNextUid++);
}

Uid Session::uidOf(int id) const
{
    return id >= 0 && id < static_cast<int>(fUids.size()) ? fUids[static_cast<size_t>(id)] : 0;
}

int Session::idOf(Uid uid) const
{
    if (uid == 0)
        return -1;
    const auto it = std::find(fUids.begin(), fUids.end(), uid);
    return it == fUids.end() ? -1 : static_cast<int>(it - fUids.begin());
}

// carla renumbers every plugin after a removed one, the model follows
void Session::removePlugin(uint id)
{
    fHost.removePlugin(id);
    if (id < fUids.size())
        fUids.erase(fUids.begin() + id);
    auto shift = [id](uint& other) {
        if (other > id)
            --other;
    };
    auto shiftTrack = [&](Track& t) {
        for (int* optional : {&t.source, &t.instrument})
            if (*optional > static_cast<int>(id))
                --*optional;
        shift(t.fader);
        for (uint& insert : t.inserts)
            shift(insert);
        for (Send& send : t.sends)
            shift(send.gain);
    };
    shift(fMaster);
    for (Track& t : fTracks)
        shiftTrack(t);
    for (Track& t : fParkedTracks)
        shiftTrack(t);
    for (uint& parked : fParkedPlugins)
        shift(parked);
}

int Session::addTrack(const std::string& name, TrackKind kind)
{
    if (!fReady)
        return -1;
    Track t;
    t.kind = kind;
    t.name = name;
    t.uid = fNextUid++;
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
    const bool ok = setPlayer(track, path);
    reroute();
    return ok;
}

bool Session::setPlayer(int track, const std::string& path)
{
    if (fTracks[track].source >= 0)
    {
        removePlugin(static_cast<uint>(fTracks[track].source));
        fTracks[track].source = -1;
        fTracks[track].file.clear();
    }
    if (path.empty())
        return true;
    const bool midi = fTracks[track].kind == TrackKind::Instrument;
    if (!(midi ? fHost.loadMidiFile(path) : fHost.loadAudioFile(path)))
    {
        fError = fHost.lastError();
        return false;
    }
    uint id = fHost.pluginCount() - 1;
    if (!isRouted() && !midi)
    {
        // the rack moved the player to the front, everything else moved down one
        id = 0;
        auto shift = [](Track& t) {
            for (int* optional : {&t.source, &t.instrument})
                if (*optional >= 0)
                    ++*optional;
            ++t.fader;
            for (uint& insert : t.inserts)
                ++insert;
            for (Send& send : t.sends)
                ++send.gain;
        };
        ++fMaster;
        for (Track& t : fTracks)
            shift(t);
        for (Track& t : fParkedTracks)
            shift(t);
        for (uint& parked : fParkedPlugins)
            ++parked;
        registerAdded(true);
    }
    else
        registerAdded();
    // the player follows the timeline once instead of looping
    if (const int loop = fHost.findParameter(id, "Loop Mode"); loop >= 0)
        fHost.setParameter(id, static_cast<uint32_t>(loop), 0.0f);

    fTracks[track].source = static_cast<int>(id);
    fTracks[track].file = path;
    matchProgram(track);
    return true;
}

// A sound font (or any instrument with MIDI programs) plays the instrument the MIDI file asks for, the
// General MIDI program of its first program change; plugins without programs, like most samplers, keep theirs.
void Session::matchProgram(int track)
{
    const Track& t = fTracks[track];
    if (t.kind != TrackKind::Instrument || t.instrument < 0 || t.file.empty())
        return;
    const std::vector<HostEngine::MidiProgram> programs = fHost.midiPrograms(static_cast<uint>(t.instrument));
    if (programs.empty())
        return;
    MidiClip clip;
    std::string error;
    if (!readMidiFile(t.file, clip, error) || clip.drums || clip.program < 0)
        return; // drums play on channel 10, which General MIDI synths keep for the drum kit
    for (size_t i = 0; i < programs.size(); ++i)
        if (programs[i].bank == 0 && programs[i].program == static_cast<uint32_t>(clip.program))
        {
            fHost.setMidiProgram(static_cast<uint>(t.instrument), static_cast<uint32_t>(i));
            return;
        }
}

bool Session::reloadMidiFile(int track, const std::string& path)
{
    if (!valid(track) || fTracks[track].kind != TrackKind::Instrument || fTracks[track].source < 0)
        return setTrackFile(track, path);
    fHost.setPlayerFile(static_cast<uint>(fTracks[track].source), path);
    fTracks[track].file = path;
    return true;
}

bool Session::setInstrument(int track, const std::string& spec)
{
    if (!valid(track) || fTracks[track].kind != TrackKind::Instrument)
        return false;
    if (fTracks[track].instrument >= 0)
    {
        if (fKeepRemoved)
            park(static_cast<uint>(fTracks[track].instrument));
        else
            removePlugin(static_cast<uint>(fTracks[track].instrument));
        fTracks[track].instrument = -1;
    }
    if (!fHost.addPlugin(spec))
    {
        fError = fHost.lastError();
        reroute();
        return false;
    }
    registerAdded();
    fTracks[track].instrument = static_cast<int>(fHost.pluginCount() - 1);
    fTracks[track].instrumentSpec = spec;
    matchProgram(track);
    reroute();
    return true;
}

void Session::setRole(int track, const std::string& role)
{
    if (valid(track))
        fTracks[track].role = role;
}

void Session::setColour(int track, uint32_t colour)
{
    if (valid(track))
        fTracks[track].colour = colour;
}

bool Session::removeTrack(int track)
{
    if (!valid(track))
        return false;
    if (fTracks[track].kind == TrackKind::Bus)
        dropSends(fTracks[track].uid);
    if (fKeepRemoved)
    {
        parkTrack(track);
        reroute();
        return true;
    }
    // remove from the highest id down so the remaining ids stay valid while we go
    std::vector<uint> ids = fTracks[track].inserts;
    ids.push_back(fTracks[track].fader);
    for (const Send& send : fTracks[track].sends)
        ids.push_back(send.gain);
    for (int optional : {fTracks[track].source, fTracks[track].instrument})
        if (optional >= 0)
            ids.push_back(static_cast<uint>(optional));
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

int Session::trackOf(Uid uid) const
{
    for (size_t i = 0; i < fTracks.size(); ++i)
        if (uid != 0 && fTracks[i].uid == uid)
            return static_cast<int>(i);
    return -1;
}

bool Session::isBus(Uid uid) const
{
    const int i = trackOf(uid);
    return i >= 0 && fTracks[static_cast<size_t>(i)].kind == TrackKind::Bus;
}

bool Session::feeds(Uid from, Uid to) const
{
    if (from == to)
        return true;
    const int i = trackOf(from);
    if (i < 0)
        return false;
    const Track& t = fTracks[static_cast<size_t>(i)];
    if (t.output != 0 && feeds(t.output, to))
        return true;
    return std::any_of(t.sends.begin(), t.sends.end(), [&](const Send& send) { return feeds(send.bus, to); });
}

bool Session::setOutput(int track, Uid bus)
{
    if (!valid(track))
        return false;
    if (bus != 0 && (!isBus(bus) || feeds(bus, fTracks[track].uid)))
    {
        fError = isBus(bus) ? "a bus can't feed itself" : "not a bus";
        return false;
    }
    fTracks[track].output = bus;
    reroute();
    return true;
}

bool Session::addSendGain(Track& t, Uid bus, float level)
{
    Send send;
    send.bus = bus;
    send.level = level;
    if (!addFader(t.name + " send", send.gain))
        return false;
    t.sends.push_back(send);
    return true;
}

bool Session::setSend(int track, Uid bus, float level)
{
    if (!valid(track))
        return false;
    level = std::clamp(level, 0.0f, 1.27f);
    for (Send& send : fTracks[track].sends)
        if (send.bus == bus)
        {
            send.level = level;
            applyGains();
            return true;
        }
    if (!isBus(bus) || feeds(bus, fTracks[track].uid))
    {
        fError = isBus(bus) ? "a bus can't feed itself" : "not a bus";
        return false;
    }
    if (!addSendGain(fTracks[track], bus, level))
        return false;
    reroute();
    return true;
}

bool Session::removeSend(int track, Uid bus)
{
    if (!valid(track))
        return false;
    std::vector<Send>& sends = fTracks[track].sends;
    const auto it = std::find_if(sends.begin(), sends.end(), [bus](const Send& send) { return send.bus == bus; });
    if (it == sends.end())
        return false;
    const uint gain = it->gain;
    sends.erase(it);
    removePlugin(gain); // a send has no state of its own: undo makes a new one
    reroute();
    return true;
}

void Session::dropSends(Uid bus)
{
    std::vector<uint> gains;
    for (Track& t : fTracks)
    {
        if (t.output == bus)
            t.output = 0;
        for (size_t i = 0; i < t.sends.size();)
            if (t.sends[i].bus == bus)
            {
                gains.push_back(t.sends[i].gain);
                t.sends.erase(t.sends.begin() + static_cast<long>(i));
            }
            else
                ++i;
    }
    std::sort(gains.rbegin(), gains.rend());
    for (uint gain : gains)
        removePlugin(gain);
}

void Session::setFolded(int track, bool folded)
{
    if (valid(track))
        fTracks[track].folded = folded;
}

uint Session::inputOf(const Track& bus) const
{
    for (uint insert : bus.inserts)
        if (fHost.slot(insert).active)
            return insert;
    return bus.fader;
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
    registerAdded();
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
    if (fKeepRemoved)
        park(id);
    else
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
    // a soloed bus solos what feeds it (a group its tracks, an effect the tracks sending to it), as in
    // Cubase; buses themselves stay open, so a soloed track keeps its reverb
    std::vector<Uid> soloedBuses;
    for (const Track& t : fTracks)
        if (t.solo && t.kind == TrackKind::Bus)
            soloedBuses.push_back(t.uid);
    auto soloed = [&](const Track& t) {
        return t.solo || std::any_of(soloedBuses.begin(), soloedBuses.end(), [&](Uid bus) { return feeds(t.uid, bus); });
    };
    for (const Track& t : fTracks)
    {
        const bool silent = t.muted || (anySolo && t.kind != TrackKind::Bus && !soloed(t));
        fHost.setVolume(t.fader, silent ? 0.0f : t.volume);
        for (const Send& send : t.sends)
            fHost.setVolume(send.gain, send.level); // after the fader: a silent track sends nothing
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
        if (t.kind == TrackKind::Instrument)
        {
            // the MIDI player drives the instrument, the instrument's sound starts the audio chain
            if (t.instrument >= 0)
            {
                if (t.source >= 0)
                    fHost.connectMidi(static_cast<uint>(t.source), static_cast<uint>(t.instrument));
                chain.push_back(static_cast<uint>(t.instrument));
            }
        }
        else if (t.source >= 0)
        {
            chain.push_back(static_cast<uint>(t.source));
        }
        // a bypassed insert is wired around: carla doesn't process an inactive plugin at all
        for (uint insert : t.inserts)
            if (fHost.slot(insert).active)
                chain.push_back(insert);
        chain.push_back(t.fader);
        const int out = trackOf(t.output);
        chain.push_back(out >= 0 ? inputOf(fTracks[static_cast<size_t>(out)]) : fMaster);
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            fHost.connect(chain[i], chain[i + 1]);
        for (const Send& send : t.sends)
        {
            const int bus = trackOf(send.bus);
            if (bus < 0)
                continue;
            fHost.connect(t.fader, send.gain);
            fHost.connect(send.gain, inputOf(fTracks[static_cast<size_t>(bus)]));
        }
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
//   tempo <bpm>
//   rate <Hz>                    the engine's, the rate the players' files were prepared at
//   key <tonic> <major|minor>    once known
//   marker <seconds> <name>      any number, in time order
//   track <source> <fader> <volume> <muted> <solo> <insert count> <inserts...>
//   name <track name>
//   file <audio or MIDI file path>
//   instrument <id>              only on instrument tracks, -1 without an instrument
//   role <stem id>               optional
//   bus                          a bus (group or effect return) instead of an audio track
//   output <track>               the bus the fader feeds, by its index in the file; else the master
//   send <track> <level> <id>    a send to a bus, its level and gain plugin
//   folded                       a bus whose tracks are hidden in the arrangement

void Session::setMarkers(std::vector<Marker> markers)
{
    for (Marker& m : markers)
        m.seconds = std::max(0.0, m.seconds);
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.seconds < b.seconds; });
    fMarkers = markers;
}

void Session::setTempo(double bpm)
{
    fTempo = bpm >= 20.0 && bpm <= 400.0 ? bpm : 120.0;
}

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
    out << "tempo " << fTempo << "\n";
    out << "rate " << static_cast<long>(fHost.sampleRate()) << "\n";
    if (!fKey.empty())
        out << "key " << fKey << "\n";
    for (const Marker& m : fMarkers)
        out << "marker " << m.seconds << " " << m.name << "\n";
    for (const Track& t : fTracks)
    {
        out << "track " << t.source << " " << t.fader << " " << t.volume << " " << t.muted << " " << t.solo << " "
            << t.inserts.size();
        for (uint id : t.inserts)
            out << " " << id;
        out << "\nname " << t.name << "\nfile " << t.file << "\n";
        if (t.kind == TrackKind::Instrument)
            out << "instrument " << t.instrument << "\n";
        if (!t.role.empty())
            out << "role " << t.role << "\n";
        if (!t.instrumentSpec.empty())
            out << "instrument-spec " << t.instrumentSpec << "\n";
        if (t.colour)
            out << "colour " << t.colour << "\n";
        if (t.kind == TrackKind::Bus)
            out << "bus\n";
        if (trackOf(t.output) >= 0)
            out << "output " << trackOf(t.output) << "\n";
        for (const Send& send : t.sends)
            if (trackOf(send.bus) >= 0)
                out << "send " << trackOf(send.bus) << " " << send.level << " " << send.gain << "\n";
        if (t.folded)
            out << "folded\n";
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
    double tempo = 120.0;
    std::string songKey;
    std::vector<Marker> markers;
    struct Links {
        int output = -1;
        std::vector<std::pair<int, Send>> sends; // to a track index
    };
    std::vector<Links> links;
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
        else if (key == "tempo")
            fields >> tempo;
        else if (key == "key")
            songKey = rest;
        else if (key == "marker")
        {
            Marker m;
            fields >> m.seconds;
            std::getline(fields >> std::ws, m.name);
            markers.push_back(m);
        }
        else if (key == "track")
        {
            Track t;
            size_t count = 0;
            fields >> t.source >> t.fader >> t.volume >> t.muted >> t.solo >> count;
            t.inserts.resize(count);
            for (uint& id : t.inserts)
                fields >> id;
            tracks.push_back(t);
            links.emplace_back();
        }
        else if (key == "name" && !tracks.empty())
            tracks.back().name = rest;
        else if (key == "file" && !tracks.empty())
            tracks.back().file = rest;
        else if (key == "instrument" && !tracks.empty())
        {
            tracks.back().kind = TrackKind::Instrument;
            fields >> tracks.back().instrument;
        }
        else if (key == "role" && !tracks.empty())
            tracks.back().role = rest;
        else if (key == "instrument-spec" && !tracks.empty())
            tracks.back().instrumentSpec = rest;
        else if (key == "colour" && !tracks.empty())
            fields >> tracks.back().colour;
        else if (key == "bus" && !tracks.empty())
            tracks.back().kind = TrackKind::Bus;
        else if (key == "output" && !tracks.empty())
            fields >> links.back().output;
        else if (key == "send" && !tracks.empty())
        {
            int to = -1;
            Send send;
            if (fields >> to >> send.level >> send.gain)
                links.back().sends.emplace_back(to, send);
        }
        else if (key == "folded" && !tracks.empty())
            tracks.back().folded = true;
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
    setTempo(tempo);
    fKey = songKey;
    setMarkers(markers);
    fParkedTracks.clear();
    fParkedPlugins.clear();
    fUids.clear();
    while (fUids.size() < fHost.pluginCount())
        fUids.push_back(fNextUid++);
    for (Track& t : fTracks)
        t.uid = fNextUid++;
    const int pluginCount = static_cast<int>(fHost.pluginCount());
    auto busUid = [&](int index) -> Uid {
        return index >= 0 && index < static_cast<int>(fTracks.size()) && fTracks[static_cast<size_t>(index)].kind == TrackKind::Bus
                   ? fTracks[static_cast<size_t>(index)].uid
                   : 0;
    };
    for (size_t i = 0; i < fTracks.size(); ++i)
    {
        fTracks[i].output = busUid(links[i].output);
        for (auto& [to, send] : links[i].sends)
            if (busUid(to) != 0 && static_cast<int>(send.gain) < pluginCount)
            {
                send.bus = busUid(to);
                fTracks[i].sends.push_back(send);
            }
    }
    // plugins no track uses: set aside for undo when the session was saved, nothing to keep now
    std::vector<bool> used(fHost.pluginCount(), false);
    auto use = [&](int id) {
        if (id >= 0 && id < static_cast<int>(used.size()))
            used[static_cast<size_t>(id)] = true;
    };
    use(static_cast<int>(fMaster));
    for (const Track& t : fTracks)
    {
        use(t.source);
        use(t.instrument);
        use(static_cast<int>(t.fader));
        for (uint insert : t.inserts)
            use(static_cast<int>(insert));
        for (const Send& send : t.sends)
            use(static_cast<int>(send.gain));
    }
    for (int id = static_cast<int>(used.size()) - 1; id >= 0; --id)
        if (!used[static_cast<size_t>(id)])
            removePlugin(static_cast<uint>(id));
    fReady = true;
    reroute();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// undo

bool SessionState::TrackState::operator==(const TrackState& o) const
{
    return uid == o.uid && kind == o.kind && name == o.name && role == o.role && file == o.file && instrumentSpec == o.instrumentSpec
           && instrument == o.instrument && inserts == o.inserts && insertsActive == o.insertsActive && volume == o.volume
           && muted == o.muted && solo == o.solo && colour == o.colour && output == o.output && sends == o.sends;
}

bool SessionState::operator==(const SessionState& o) const
{
    return tracks == o.tracks && masterVolume == o.masterVolume && tempo == o.tempo && key == o.key && markers == o.markers;
}

SessionState Session::state() const
{
    SessionState s;
    s.masterVolume = fMasterVolume;
    s.tempo = fTempo;
    s.key = fKey;
    s.markers = fMarkers;
    for (const Track& t : fTracks)
    {
        SessionState::TrackState ts;
        ts.uid = t.uid;
        ts.kind = t.kind;
        ts.name = t.name;
        ts.role = t.role;
        ts.file = t.file;
        ts.instrumentSpec = t.instrumentSpec;
        ts.instrument = uidOf(t.instrument);
        for (uint insert : t.inserts)
        {
            ts.inserts.push_back(uidOf(static_cast<int>(insert)));
            ts.insertsActive.push_back(fHost.slot(insert).active);
        }
        ts.volume = t.volume;
        ts.muted = t.muted;
        ts.solo = t.solo;
        ts.colour = t.colour;
        ts.output = t.output;
        for (const Send& send : t.sends)
            ts.sends.push_back({send.bus, send.level});
        s.tracks.push_back(ts);
    }
    return s;
}

void Session::park(uint id)
{
    fHost.setActive(id, false); // silent and no load on the processor while set aside
    fParkedPlugins.push_back(id);
}

int Session::unpark(Uid uid)
{
    const int id = idOf(uid);
    const auto it = std::find(fParkedPlugins.begin(), fParkedPlugins.end(), static_cast<uint>(id));
    if (id < 0 || it == fParkedPlugins.end())
        return -1;
    fParkedPlugins.erase(it);
    fHost.setActive(static_cast<uint>(id), true);
    return id;
}

void Session::parkTrack(int track)
{
    Track t = fTracks[track];
    fTracks.erase(fTracks.begin() + track);
    for (int id : {t.source, t.instrument, static_cast<int>(t.fader)})
        if (id >= 0)
            fHost.setActive(static_cast<uint>(id), false);
    for (uint insert : t.inserts)
        fHost.setActive(insert, false);
    for (const Send& send : t.sends)
        fHost.setActive(send.gain, false);
    fParkedTracks.push_back(t);
}

void Session::restore(const SessionState& s)
{
    auto wanted = [&](Uid uid) {
        return std::any_of(s.tracks.begin(), s.tracks.end(), [uid](const SessionState::TrackState& ts) { return ts.uid == uid; });
    };
    for (int i = static_cast<int>(fTracks.size()) - 1; i >= 0; --i)
        if (!wanted(fTracks[static_cast<size_t>(i)].uid))
            parkTrack(i);

    // the tracks in the state's order, the ones set aside brought back
    std::vector<Track> ordered;
    for (const SessionState::TrackState& ts : s.tracks)
    {
        auto live = std::find_if(fTracks.begin(), fTracks.end(), [&](const Track& t) { return t.uid == ts.uid; });
        if (live != fTracks.end())
        {
            ordered.push_back(*live);
            continue;
        }
        auto parked = std::find_if(fParkedTracks.begin(), fParkedTracks.end(), [&](const Track& t) { return t.uid == ts.uid; });
        if (parked == fParkedTracks.end())
            continue; // gone for good: nothing to bring back
        for (int id : {parked->source, parked->instrument, static_cast<int>(parked->fader)})
            if (id >= 0)
                fHost.setActive(static_cast<uint>(id), true);
        for (const Send& send : parked->sends)
            fHost.setActive(send.gain, true);
        ordered.push_back(*parked);
        fParkedTracks.erase(parked);
    }
    fTracks = ordered;

    for (Track& t : fTracks)
    {
        const auto ts = std::find_if(s.tracks.begin(), s.tracks.end(), [&](const SessionState::TrackState& x) { return x.uid == t.uid; });
        t.name = ts->name;
        t.role = ts->role;
        t.volume = ts->volume;
        t.muted = ts->muted;
        t.solo = ts->solo;
        t.colour = ts->colour;
        const int index = static_cast<int>(&t - fTracks.data());
        if (t.file != ts->file)
            setPlayer(index, ts->file); // an audio or MIDI file player has no state of its own: just load the file

        if (uidOf(t.instrument) != ts->instrument)
        {
            if (t.instrument >= 0)
                park(static_cast<uint>(t.instrument));
            t.instrument = ts->instrument ? unpark(ts->instrument) : -1;
        }
        t.instrumentSpec = ts->instrumentSpec;

        std::vector<uint> inserts;
        for (size_t i = 0; i < ts->inserts.size(); ++i)
        {
            int id = idOf(ts->inserts[i]);
            if (std::find(fParkedPlugins.begin(), fParkedPlugins.end(), static_cast<uint>(id)) != fParkedPlugins.end())
                id = unpark(ts->inserts[i]);
            if (id < 0)
                continue;
            fHost.setActive(static_cast<uint>(id), ts->insertsActive[i]);
            inserts.push_back(static_cast<uint>(id));
        }
        for (uint insert : t.inserts)
            if (std::find(inserts.begin(), inserts.end(), insert) == inserts.end())
                park(insert);
        t.inserts = inserts;
    }
    // outputs and sends: a send's gain plugin has no state, so the ones not wanted go and missing ones are made
    std::vector<uint> unwanted;
    for (Track& t : fTracks)
    {
        const auto ts = std::find_if(s.tracks.begin(), s.tracks.end(), [&](const SessionState::TrackState& x) { return x.uid == t.uid; });
        t.output = trackOf(ts->output) >= 0 ? ts->output : 0;
        for (size_t i = 0; i < t.sends.size();)
        {
            const auto want = std::find_if(ts->sends.begin(), ts->sends.end(), [&](const auto& x) { return x.bus == t.sends[i].bus; });
            if (want == ts->sends.end() || trackOf(want->bus) < 0)
            {
                unwanted.push_back(t.sends[i].gain);
                t.sends.erase(t.sends.begin() + static_cast<long>(i));
                continue;
            }
            t.sends[i].level = want->level;
            ++i;
        }
    }
    std::sort(unwanted.rbegin(), unwanted.rend());
    for (uint gain : unwanted)
        removePlugin(gain);
    for (Track& t : fTracks)
    {
        const auto ts = std::find_if(s.tracks.begin(), s.tracks.end(), [&](const SessionState::TrackState& x) { return x.uid == t.uid; });
        std::vector<Send> ordered;
        for (const auto& want : ts->sends)
        {
            if (trackOf(want.bus) < 0)
                continue;
            const auto have = std::find_if(t.sends.begin(), t.sends.end(), [&](const Send& x) { return x.bus == want.bus; });
            if (have != t.sends.end())
                ordered.push_back(*have);
            else if (Track probe; addSendGain(probe, want.bus, want.level))
                ordered.push_back(probe.sends.back());
        }
        t.sends = ordered;
    }
    fMasterVolume = s.masterVolume;
    setTempo(s.tempo);
    fKey = s.key;
    fMarkers = s.markers;
    reroute();
}

void Session::dropUnused(const std::vector<const SessionState*>& keep)
{
    std::vector<Uid> used;
    for (const SessionState* s : keep)
        for (const SessionState::TrackState& ts : s->tracks)
        {
            used.push_back(ts.uid);
            used.push_back(ts.instrument);
            used.insert(used.end(), ts.inserts.begin(), ts.inserts.end());
        }
    auto isUsed = [&](Uid uid) { return std::find(used.begin(), used.end(), uid) != used.end(); };

    std::vector<uint> doomed;
    for (size_t i = 0; i < fParkedTracks.size();)
    {
        const Track& t = fParkedTracks[i];
        if (isUsed(t.uid))
        {
            ++i;
            continue;
        }
        doomed.push_back(t.fader);
        for (int optional : {t.source, t.instrument})
            if (optional >= 0)
                doomed.push_back(static_cast<uint>(optional));
        doomed.insert(doomed.end(), t.inserts.begin(), t.inserts.end());
        for (const Send& send : t.sends)
            doomed.push_back(send.gain);
        fParkedTracks.erase(fParkedTracks.begin() + static_cast<long>(i));
    }
    for (size_t i = 0; i < fParkedPlugins.size();)
        if (isUsed(uidOf(static_cast<int>(fParkedPlugins[i]))))
            ++i;
        else
        {
            doomed.push_back(fParkedPlugins[i]);
            fParkedPlugins.erase(fParkedPlugins.begin() + static_cast<long>(i));
        }
    if (doomed.empty())
        return;
    // from the highest id down, so the ones still to go keep their numbers
    std::sort(doomed.rbegin(), doomed.rend());
    for (uint id : doomed)
        removePlugin(id);
    reroute();
}

} // namespace chost
