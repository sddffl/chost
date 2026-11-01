// Chords for the chord pad: the chords of a key, which chord could come next, voicings that move as
// little as possible, and a progression written out as a MIDI part. Chord symbols are the ones
// tools/chost_arrange.py reads (C, Am, F#m7, Bb, Gsus4, C/E, ...), so a progression goes on to the
// auto-arrangement unchanged.

#pragma once

#include "MidiFile.h"

#include <string>
#include <vector>

namespace chost {

struct Key {
    int tonic = 0;      // pitch class, 0 = C
    bool minor = false;
    bool valid = false;
};
Key parseKey(const std::string& text); // "A minor", "F# major", "Eb"; invalid when it isn't one
std::string keyName(const Key& key);   // "A minor"

struct Chord {
    int root = 0;              // pitch class
    std::string quality;       // "", "m", "7", "maj7", "m7", "sus4", "dim", ... as in the symbol
    int bass = -1;             // a slash chord's bass, -1 without
    std::vector<int> intervals; // from the root, e.g. 0 4 7

    bool valid() const { return !intervals.empty(); }
    std::string name(const Key& key = {}) const; // spelt with sharps or flats as the key has them
    std::vector<int> pitchClasses() const;
};
Chord parseChord(const std::string& symbol); // invalid when it isn't one

// One pad: a chord of the key and its roman numeral ("vi", "V7", "bVII").
struct Pad {
    Chord chord;
    std::string numeral;
};
// Two rows of eight: the key's own chords (triads, or sevenths) with the tonic an octave up to close
// the row, and the colours pop songs borrow (secondary dominants, sus, chords from the parallel key).
std::vector<Pad> chordPads(const Key& key, bool sevenths);

// How well each pad would follow the chord before (none: how well it starts a song), 0 .. 1.
std::vector<float> nextChordScores(const Key& key, const std::vector<Pad>& pads, const Chord* previous);

// Chord tones within low..high from the inversion closest to the previous voicing (none: the middle).
std::vector<int> voiceChord(const Chord& chord, const std::vector<int>& previous, int low = 55, int high = 76);
int bassNote(const Chord& chord, int low = 36); // the root (or slash bass) in the octave from low

// A progression: chords one after the other, each so many beats long (4 to the bar).
struct ProgressionChord {
    Chord chord;
    double beats = 4.0;
};
// "C G | Am F": "|" separates bars, chords in one bar share it; without "|" every chord has a bar.
std::vector<ProgressionChord> parseProgression(const std::string& text, std::string& error);
std::string progressionText(const std::vector<ProgressionChord>& chords, const Key& key = {}); // in bars, as above

enum class ChordPattern { Block, Arpeggio, Rhythm };
// The progression as notes from startSeconds on: chords (and a bass note) played block, broken in
// eighths, or in a pop rhythm. The clip has the tempo and General MIDI piano.
MidiClip chordClip(const std::vector<ProgressionChord>& chords, double tempo, ChordPattern pattern, double startSeconds = 0.0);

} // namespace chost
