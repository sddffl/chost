// Scales for the piano roll: which notes belong to the song's key, moving notes by scale steps, pulling
// stray notes into the key, and guessing the key of a part from its notes.

#pragma once

#include "Chords.h"
#include "MidiFile.h"

#include <string>
#include <vector>

namespace chost {

struct ScaleType {
    std::string id;         // "major", "minor", "harmonic minor", "dorian", "major pentatonic", ...
    std::vector<int> steps; // semitones from the tonic, rising, starting with 0
};
const std::vector<ScaleType>& scaleTypes();

struct Scale {
    int tonic = 0; // pitch class
    std::string type;
    std::vector<int> steps;

    bool valid() const { return !steps.empty(); }
    bool contains(int pitch) const;
    bool isTonic(int pitch) const;
};
Scale makeScale(int tonic, const std::string& type); // invalid for a type scaleTypes() lacks
Scale scaleOfKey(const Key& key);                     // major, or natural minor
Scale parseScale(const std::string& text);            // "A minor", "D dorian", "C major pentatonic", "Eb"
std::string scaleName(const Scale& scale);            // "A minor", spelt as the key has it
std::string noteName(int pitchClass, const Key& key = {});

// The scale's notes counted up from far below: index of the scale note at or just below pitch, and back.
int scaleIndex(const Scale& scale, int pitch);
int scalePitch(const Scale& scale, int index);

// The nearest note of the scale (itself when it belongs); a tie goes up when prefer > 0, else down.
int snapToScale(int pitch, const Scale& scale, int prefer = 0);
// So many scale steps up or down from pitch (from its nearest scale note when it is outside the scale).
int stepInScale(int pitch, const Scale& scale, int steps);

// Moves every note outside the scale (only the ones marked, with only) to the nearest scale note, a tie
// going the way of the note before it so melodies move less. Drum notes stay. Returns how many moved.
int fitToScale(std::vector<MidiNote>& notes, const Scale& scale, const std::vector<bool>* only = nullptr);
int countOutside(const std::vector<MidiNote>& notes, const Scale& scale);

// The key the notes are most likely in (Krumhansl-Schmuckler, weighted by length); invalid without
// enough notes. confidence: how clearly it beats the others, 0 .. 1.
Key guessKey(const std::vector<MidiNote>& notes, float* confidence = nullptr);

} // namespace chost
