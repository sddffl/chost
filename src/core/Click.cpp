#include "Click.h"

#include "MidiFile.h"

#include <cmath>

namespace chost {

bool writeClickTrack(const std::string& path, double bpm, int beatsPerBar, double seconds, std::string& error)
{
    if (bpm <= 0.0 || seconds <= 0.0 || beatsPerBar <= 0)
    {
        error = "no tempo or length for the click";
        return false;
    }
    MidiClip clip;
    clip.tempo = bpm;
    clip.drums = true;
    clip.name = "Metronome";
    const double beat = 60.0 / bpm;
    for (long n = 0; n * beat < seconds; ++n)
    {
        MidiNote note;
        note.start = n * beat;
        note.end = note.start + std::min(0.1, beat / 2.0);
        const bool downbeat = n % beatsPerBar == 0;
        note.pitch = downbeat ? 76 : 77; // General MIDI hi and low wood block
        note.velocity = downbeat ? 127 : 96;
        note.channel = 9;
        clip.notes.push_back(note);
    }
    clip.seconds = clip.notes.empty() ? 0.0 : clip.notes.back().end;
    return writeMidiFile(path, clip, error);
}

} // namespace chost
