#include "MidiFile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;

namespace chost {

namespace {

struct Event {
    uint64_t tick = 0;
    uint8_t status = 0;  // 0xFF for meta events
    uint8_t data1 = 0;   // meta type for meta events
    uint8_t data2 = 0;
    std::string text;    // meta payload
};

class Reader {
public:
    Reader(const std::vector<uint8_t>& data, size_t pos, size_t end) : fData(data), fPos(pos), fEnd(end) {}

    bool atEnd() const { return fPos >= fEnd; }
    bool ok() const { return !fFailed; }
    size_t pos() const { return fPos; }

    uint8_t byte()
    {
        if (fPos >= fEnd)
        {
            fFailed = true;
            return 0;
        }
        return fData[fPos++];
    }
    uint8_t peek() const { return fPos < fEnd ? fData[fPos] : 0; }

    uint32_t varLen()
    {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const uint8_t b = byte();
            value = (value << 7) | (b & 0x7F);
            if ((b & 0x80) == 0)
                return value;
        }
        fFailed = true;
        return value;
    }

    std::string bytes(uint32_t count)
    {
        if (fPos + count > fEnd)
        {
            fFailed = true;
            fPos = fEnd;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(fData.data() + fPos), count);
        fPos += count;
        return out;
    }

private:
    const std::vector<uint8_t>& fData;
    size_t fPos;
    size_t fEnd;
    bool fFailed = false;
};

uint32_t be32(const std::vector<uint8_t>& d, size_t p)
{
    return (uint32_t(d[p]) << 24) | (uint32_t(d[p + 1]) << 16) | (uint32_t(d[p + 2]) << 8) | d[p + 3];
}

uint16_t be16(const std::vector<uint8_t>& d, size_t p)
{
    return static_cast<uint16_t>((d[p] << 8) | d[p + 1]);
}

bool readTrack(Reader& r, std::vector<Event>& events)
{
    uint64_t tick = 0;
    uint8_t running = 0;
    while (!r.atEnd() && r.ok())
    {
        tick += r.varLen();
        uint8_t status = r.peek();
        if (status & 0x80)
            r.byte();
        else if (running != 0)
            status = running; // running status: the data byte is read below
        else
            return false;

        Event e;
        e.tick = tick;
        e.status = status;
        if (status == 0xFF)
        {
            e.data1 = r.byte();
            e.text = r.bytes(r.varLen());
            events.push_back(e);
            if (e.data1 == 0x2F)
                break; // end of track
        }
        else if (status == 0xF0 || status == 0xF7)
        {
            r.bytes(r.varLen()); // sysex, not needed
        }
        else
        {
            running = status;
            e.data1 = r.byte();
            const uint8_t type = status & 0xF0;
            if (type != 0xC0 && type != 0xD0)
                e.data2 = r.byte();
            events.push_back(e);
        }
    }
    return r.ok();
}

} // namespace

bool readMidiFile(const std::string& path, MidiClip& clip, std::string& error)
{
    clip = MidiClip();
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 14 || std::string(data.begin(), data.begin() + 4) != "MThd")
    {
        error = "not a MIDI file";
        return false;
    }

    const uint32_t headerLength = be32(data, 4);
    const uint16_t tracks = be16(data, 10);
    const uint16_t division = be16(data, 12);
    const bool smpte = (division & 0x8000) != 0;
    // SMPTE timing counts ticks per frame instead of per beat, tempo changes don't apply
    const double secondsPerTickSmpte =
        smpte ? 1.0 / (static_cast<double>(-static_cast<int8_t>(division >> 8)) * (division & 0xFF)) : 0.0;
    const double ticksPerBeat = smpte ? 1.0 : std::max<uint16_t>(1, division);

    std::vector<Event> events;
    size_t pos = 8 + headerLength;
    for (uint16_t t = 0; t < tracks && pos + 8 <= data.size(); ++t)
    {
        const std::string id(data.begin() + static_cast<long>(pos), data.begin() + static_cast<long>(pos) + 4);
        const size_t length = be32(data, pos + 4);
        const size_t end = std::min(data.size(), pos + 8 + length);
        if (id == "MTrk")
        {
            Reader r(data, pos + 8, end);
            if (!readTrack(r, events))
            {
                error = "damaged MIDI track";
                return false;
            }
        }
        pos = end;
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });

    // ticks to seconds through the tempo map, which may come from any track
    double seconds = 0.0;
    uint64_t lastTick = 0;
    double secondsPerTick = smpte ? secondsPerTickSmpte : 0.5 / ticksPerBeat; // 120 bpm until told otherwise
    bool tempoSeen = false;
    std::map<std::pair<int, int>, std::vector<size_t>> open; // (channel, pitch) -> notes still sounding
    bool anyDrums = false, anyOther = false;

    for (const Event& e : events)
    {
        seconds += static_cast<double>(e.tick - lastTick) * secondsPerTick;
        lastTick = e.tick;

        if (e.status == 0xFF)
        {
            if (e.data1 == 0x51 && e.text.size() == 3 && !smpte)
            {
                const uint32_t microseconds = (uint8_t(e.text[0]) << 16) | (uint8_t(e.text[1]) << 8) | uint8_t(e.text[2]);
                if (microseconds > 0)
                {
                    secondsPerTick = microseconds / 1e6 / ticksPerBeat;
                    if (!tempoSeen)
                        clip.tempo = 60e6 / microseconds;
                    tempoSeen = true;
                }
            }
            else if (e.data1 == 0x03 && clip.name.empty())
            {
                clip.name = e.text;
            }
            continue;
        }

        const uint8_t type = e.status & 0xF0;
        const uint8_t channel = e.status & 0x0F;
        if (type == 0xC0 && clip.program < 0 && channel != 9)
        {
            clip.program = e.data1;
        }
        else if (type == 0x90 && e.data2 > 0)
        {
            MidiNote n;
            n.start = n.end = seconds;
            n.pitch = e.data1 & 0x7F;
            n.velocity = e.data2;
            n.channel = channel;
            open[{channel, n.pitch}].push_back(clip.notes.size());
            clip.notes.push_back(n);
            (channel == 9 ? anyDrums : anyOther) = true;
        }
        else if (type == 0x80 || (type == 0x90 && e.data2 == 0))
        {
            auto it = open.find({channel, e.data1 & 0x7F});
            if (it != open.end() && !it->second.empty())
            {
                // the oldest sounding note of that pitch ends first
                clip.notes[it->second.front()].end = seconds;
                it->second.erase(it->second.begin());
            }
        }
    }
    // notes never released end with the file
    for (auto& [key, indexes] : open)
        for (size_t i : indexes)
            clip.notes[i].end = std::max(clip.notes[i].start, seconds);

    std::stable_sort(clip.notes.begin(), clip.notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    for (const MidiNote& n : clip.notes)
        clip.seconds = std::max(clip.seconds, n.end);
    clip.drums = anyDrums && !anyOther;
    return true;
}

} // namespace chost
