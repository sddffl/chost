// Standard MIDI file reading, enough to draw a MIDI track's notes and to pick its instrument.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chost {

struct MidiNote {
    double start = 0.0; // seconds
    double end = 0.0;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
    uint8_t channel = 0; // 0-15, 9 is General MIDI drums
};

struct MidiClip {
    std::vector<MidiNote> notes; // sorted by start
    double seconds = 0.0;        // end of the last note
    double tempo = 120.0;        // the first tempo in the file, beats per minute
    int program = -1;            // the first program change, -1 without one
    bool drums = false;          // all notes on channel 10
    std::string name;            // the first track name
};

// Format 0 and 1 files, with tempo changes; false and an error message for anything unreadable.
bool readMidiFile(const std::string& path, MidiClip& clip, std::string& error);

} // namespace chost
