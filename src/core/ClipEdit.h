// Editing a track's clip: moving it in time, silencing or keeping a range, fades, gain and normalising.
// A track plays one file from the song's start, so an edit writes a new file (the original stays as it
// was) and the track takes that: undo goes back to the file before.

#pragma once

#include "MidiFile.h"

#include <string>

namespace chost {

enum class ClipEditKind { Move, Silence, Keep, FadeIn, FadeOut, Gain, Normalize };

struct ClipEdit {
    ClipEditKind kind = ClipEditKind::Gain;
    double start = 0.0; // the range, seconds in the song; end < 0: to the clip's end
    double end = -1.0;
    double seconds = 0.0; // Move: later (> 0) or earlier
    double db = 0.0;      // Gain: change; Normalize: the peak it brings the range to (e.g. -1)
};

ClipEditKind parseClipEditKind(const std::string& name, bool& ok); // "move", "silence", "keep", "fade_in", ...

// An audio file edited into a new float WAV at its own rate. note: what a user should know (clipping).
bool editAudioFile(const std::string& in, const std::string& out, const ClipEdit& edit, std::string& error, std::string* note = nullptr);

// A MIDI part edited: moved, notes in (silence) or out of (keep) the range removed, velocities scaled
// by the gain. Fades and normalising are for audio.
bool editMidiClip(MidiClip& clip, const ClipEdit& edit, std::string& error);

} // namespace chost
