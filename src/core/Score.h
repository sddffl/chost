// Sheet music from MIDI parts: notes quantized to a grid, chords, rests, bars, notes split and tied the
// way notation writes them, beams, pitches spelt for the key with the accidentals a bar needs, and a
// treble, bass or grand staff for each part. The result is drawn by the GUI's score view and written as
// MusicXML, which MuseScore, Sibelius, Finale and Dorico open.
//
// One time signature (4/4) and one voice per staff: notes that overlap a later one are cut where it starts.

#pragma once

#include "Chords.h"
#include "MidiFile.h"

#include <string>
#include <vector>

namespace chost {

enum class Clef { Treble, Bass };
enum class StaffMode { Auto, Treble, Bass, Grand };

struct ScoreNote {
    int pitch = 60;
    int step = 0;   // the letter: 0 = C ... 6 = B
    int alter = 0;  // -1 flat, +1 sharp
    int octave = 4; // scientific: middle C is C4
    bool tieStart = false;
    bool tieStop = false;
    bool accidental = false; // show its sharp, flat or natural
};

struct ScoreEvent {
    int start = 0;    // in the bar, sixteenths
    int duration = 4; // sixteenths: 1, 2, 3, 4, 6, 8, 12 or 16
    bool rest = false;
    bool wholeBar = false;        // a rest filling the bar, drawn as a whole rest in its middle
    std::vector<ScoreNote> notes; // low to high; empty for a rest
    int beam = -1;                // events of a bar with the same beam (>= 0) are beamed together
};

struct ScoreMeasure {
    std::vector<ScoreEvent> events;
};

struct ScoreStaff {
    Clef clef = Clef::Treble;
    std::vector<ScoreMeasure> measures;
};

struct ScorePart {
    std::string name;
    int program = -1;
    std::vector<ScoreStaff> staves; // two for a grand staff
};

struct Score {
    std::string title;
    Key key;
    int fifths = 0; // the key signature: sharps (> 0) or flats (< 0)
    double tempo = 120.0;
    int beatsPerBar = 4;
    int measures = 0;
    std::vector<ScorePart> parts;
};

struct ScoreInput {
    std::string name;
    MidiClip clip;
    StaffMode staff = StaffMode::Auto;
};

struct ScoreOptions {
    std::string title;
    Key key;              // invalid: guessed from the notes
    double tempo = 120.0; // the song's; what the notes' seconds are in beats
    int grid = 1;         // the shortest note, sixteenths: 1 (a sixteenth) or 2 (an eighth)
};

// Drum parts are left out (they need a percussion staff). Empty, with error, when nothing is left.
Score makeScore(const std::vector<ScoreInput>& parts, const ScoreOptions& options, std::string& error);

std::string musicXml(const Score& score);

int keyFifths(const Key& key);
// A note's letter, alteration and octave as the key spells it.
ScoreNote spellPitch(int pitch, const Key& key);
// "quarter", "eighth", ... and its dots, for a duration in sixteenths
void noteType(int duration, std::string& type, int& dots);

} // namespace chost
