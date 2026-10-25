// Sheet music from MIDI parts: notes quantized to a grid, chords, rests, bars, notes split and tied the
// way notation writes them, beams, pitches spelt for the key with the accidentals a bar needs, and a
// treble, bass or grand staff for each part. The result is drawn by the GUI's score view and written as
// MusicXML, which MuseScore, Sibelius, Finale and Dorico open.
//
// One time signature for the whole score (2/4, 3/4, 4/4, 3/8, 6/8, 9/8, 12/8); beats of quarter notes can be
// written as triplets where the notes were played that way. One voice per staff: notes that overlap a
// later one are cut where it starts.
//
// Times and lengths are in ticks, 12 to a quarter note: a sixteenth is 3, an eighth of a triplet 4.

#pragma once

#include "Chords.h"
#include "MidiFile.h"

#include <string>
#include <vector>

namespace chost {

enum class Clef { Treble, Bass };

struct Meter {
    int beats = 4;
    int beatType = 4; // 4 or 8
    bool valid() const
    {
        return (beatType == 4 && beats >= 2 && beats <= 4) || (beatType == 8 && (beats == 3 || beats == 6 || beats == 9 || beats == 12));
    }
    bool compound() const { return beatType == 8; } // in dotted quarters
    int barTicks() const { return beats * (beatType == 8 ? 6 : 12); }
    int pulse() const { return compound() ? 18 : 12; } // the beat, what notes are beamed and split by
};

// "6/8" -> {6, 8}; an invalid meter (beats 0) when it isn't one
Meter parseMeter(const std::string& text);
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
    int start = 0;     // in the bar, ticks
    int duration = 12; // ticks: 3, 6, 9, 12, 18, 24, 36, 48, 72, or in a triplet 4 and 8
    int tuplet = -1;   // the triplet (>= 0) it's part of: three in the time of two
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
    Meter meter;
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
    Meter meter;          // 4/4 unless valid
    bool triplets = true; // beats played in triplets written so
};

// Drum parts are left out (they need a percussion staff). Empty, with error, when nothing is left.
Score makeScore(const std::vector<ScoreInput>& parts, const ScoreOptions& options, std::string& error);

std::string musicXml(const Score& score);

int keyFifths(const Key& key);
// A note's letter, alteration and octave as the key spells it.
ScoreNote spellPitch(int pitch, const Key& key);
// how long an event looks: a triplet eighth (4) is drawn as an eighth (6)
int displayLength(const ScoreEvent& e);
// "quarter", "eighth", ... and its dots, for a displayed length in ticks
void noteType(int duration, std::string& type, int& dots);

} // namespace chost
