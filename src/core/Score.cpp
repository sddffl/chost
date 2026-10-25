#include "Score.h"

#include "Scales.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

namespace chost {

namespace {

constexpr int kBar = 16; // sixteenths in a 4/4 bar
constexpr int kBeat = 4;
const int kNatural[7] = {0, 2, 4, 5, 7, 9, 11}; // C D E F G A B

int mod(int v, int m)
{
    return ((v % m) + m) % m;
}

// the letter of a key's tonic: majors and minors by their number of sharps (> 0) or flats
int tonicLetter(int fifths, bool minor)
{
    // C G D A E B F#, and F Bb Eb Ab Db Gb going down
    static const int majorLetters[13] = {4, 1, 5, 2, 6, 3, 0, 4, 1, 5, 2, 6, 3}; // Gb Db Ab Eb Bb F C G D A E B F#
    const int major = majorLetters[std::clamp(fifths, -6, 6) + 6];
    return minor ? mod(major - 2, 7) : major; // the relative minor is a third (two letters) below
}

// how the key signature alters a letter
int signatureAlter(int letter, int fifths)
{
    static const int sharpOrder[7] = {3, 0, 4, 1, 5, 2, 6}; // F C G D A E B
    static const int flatOrder[7] = {6, 2, 5, 1, 4, 0, 3};  // B E A D G C F
    for (int i = 0; i < std::min(7, std::abs(fifths)); ++i)
        if ((fifths > 0 ? sharpOrder : flatOrder)[i] == letter)
            return fifths > 0 ? 1 : -1;
    return 0;
}

// note lengths notation has: in a bar, on a note's position, without hiding the beats
bool fits(int at, int length, bool rest)
{
    const int end = at + length;
    if (end > kBar)
        return false;
    if (at % kBeat != 0)
        return end <= (at / kBeat + 1) * kBeat; // off the beat: up to the next beat
    if (length <= kBeat)
        return !rest || length != 3;
    switch (length)
    {
    case 6: return !rest && (at == 0 || at == 8); // dotted quarter on beat 1 or 3
    case 8: return at == 0 || at == 8;            // a half note never hides the middle of the bar
    case 12: return !rest && at == 0;
    case 16: return at == 0;
    default: return false;
    }
}

// a length from a position in the bar as the notes (or rests) notation writes it, tied
std::vector<int> pieces(int at, int length, bool rest)
{
    static const int lengths[] = {16, 12, 8, 6, 4, 3, 2, 1};
    std::vector<int> out;
    while (length > 0)
    {
        int take = 1;
        for (int d : lengths)
            if (d <= length && fits(at, d, rest))
            {
                take = d;
                break;
            }
        out.push_back(take);
        at += take;
        length -= take;
    }
    return out;
}

struct Timed {
    int start = 0;
    int end = 0;
    std::vector<int> pitches; // a chord; empty: a rest
};

// one staff's notes as chords and rests from the start to the end of the last bar
std::vector<Timed> chordsAndRests(std::vector<std::pair<int, std::pair<int, int>>> notes /* start, (end, pitch) */, int bars)
{
    std::sort(notes.begin(), notes.end());
    std::vector<Timed> chords;
    for (const auto& [start, endPitch] : notes)
    {
        if (chords.empty() || chords.back().start != start)
            chords.push_back(Timed{start, endPitch.first, {}});
        Timed& c = chords.back();
        c.end = std::max(c.end, endPitch.first);
        if (std::find(c.pitches.begin(), c.pitches.end(), endPitch.second) == c.pitches.end())
            c.pitches.push_back(endPitch.second);
    }
    std::vector<Timed> out;
    int time = 0;
    for (size_t i = 0; i < chords.size(); ++i)
    {
        Timed c = chords[i];
        if (i + 1 < chords.size())
            c.end = std::min(c.end, chords[i + 1].start); // one voice: cut where the next one starts
        if (c.start > time)
            out.push_back(Timed{time, c.start, {}});
        std::sort(c.pitches.begin(), c.pitches.end());
        out.push_back(c);
        time = c.end;
    }
    if (time < bars * kBar)
        out.push_back(Timed{time, bars * kBar, {}});
    return out;
}

ScoreStaff layOut(const std::vector<Timed>& timeline, Clef clef, int bars, const Key& key, int fifths)
{
    ScoreStaff staff;
    staff.clef = clef;
    staff.measures.resize(static_cast<size_t>(bars));
    for (const Timed& t : timeline)
    {
        // across bars first, then as notation writes the lengths
        int at = t.start;
        bool first = true;
        while (at < t.end)
        {
            const int bar = at / kBar;
            const int inBar = at % kBar;
            const int until = std::min(t.end, (bar + 1) * kBar);
            const std::vector<int> lengths = pieces(inBar, until - at, t.pitches.empty());
            int offset = inBar;
            for (int length : lengths)
            {
                ScoreEvent e;
                e.start = offset;
                e.duration = length;
                e.rest = t.pitches.empty();
                const bool last = at + (offset - inBar) + length >= t.end;
                for (int pitch : t.pitches)
                {
                    ScoreNote n = spellPitch(pitch, key);
                    n.tieStop = !first;
                    n.tieStart = !last;
                    e.notes.push_back(n);
                }
                staff.measures[static_cast<size_t>(bar)].events.push_back(e);
                offset += length;
                first = false;
            }
            at = until;
        }
    }

    for (ScoreMeasure& m : staff.measures)
    {
        if (m.events.size() == 1 && m.events[0].rest)
            m.events[0].wholeBar = true;

        // accidentals: what the key signature and the notes before in the bar don't already say
        std::map<std::pair<int, int>, int> state; // (letter, octave) -> alteration in force
        for (ScoreEvent& e : m.events)
            for (ScoreNote& n : e.notes)
            {
                const auto where = std::make_pair(n.step, n.octave);
                const int current = state.count(where) ? state[where] : signatureAlter(n.step, fifths);
                n.accidental = !n.tieStop && n.alter != current;
                state[where] = n.alter;
            }

        // beams: the eighths and sixteenths of a beat, two beats of eighths together
        int nextBeam = 0;
        std::vector<std::vector<size_t>> runs(4);
        for (int beat = 0; beat < 4; ++beat)
        {
            std::vector<size_t> run;
            auto close = [&] {
                if (run.size() >= 2)
                {
                    for (size_t i : run)
                        m.events[i].beam = nextBeam;
                    runs[static_cast<size_t>(beat)] = run;
                    ++nextBeam;
                }
                run.clear();
            };
            for (size_t i = 0; i < m.events.size(); ++i)
            {
                const ScoreEvent& e = m.events[i];
                if (e.start / kBeat != beat)
                    continue;
                if (e.rest || e.duration >= kBeat)
                    close();
                else
                    run.push_back(i);
            }
            close();
        }
        auto twoEighths = [&](int beat) {
            const std::vector<size_t>& run = runs[static_cast<size_t>(beat)];
            return run.size() == 2 && m.events[run[0]].duration == 2 && m.events[run[1]].duration == 2 &&
                   m.events[run[0]].start == beat * kBeat;
        };
        for (int beat : {0, 2})
            if (twoEighths(beat) && twoEighths(beat + 1))
                for (size_t i : runs[static_cast<size_t>(beat + 1)])
                    m.events[i].beam = m.events[runs[static_cast<size_t>(beat)][0]].beam;
    }
    return staff;
}

// how many ledger lines a note needs on a staff
int ledgerLines(int pitch, Clef clef)
{
    static const int steps[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    const int position = (pitch / 12 - 1) * 7 + steps[pitch % 12] - (clef == Clef::Treble ? 34 : 22);
    return position > 5 ? (position - 4) / 2 : position < -5 ? (-position - 4) / 2 : 0;
}

std::string escape(const std::string& text)
{
    std::string out;
    for (char c : text)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

} // namespace

int keyFifths(const Key& key)
{
    if (!key.valid)
        return 0;
    const int major = key.minor ? mod(key.tonic + 3, 12) : key.tonic;
    static const int fifths[12] = {0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5}; // C Db D Eb E F F# G Ab A Bb B
    return fifths[major];
}

ScoreNote spellPitch(int pitch, const Key& key)
{
    const Key k = key.valid ? key : Key{0, false, true};
    const int fifths = keyFifths(k);
    const int tonicStep = tonicLetter(fifths, k.minor);
    static const int majorSteps[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int minorSteps[7] = {0, 2, 3, 5, 7, 8, 10};
    const int* steps = k.minor ? minorSteps : majorSteps;
    const int interval = mod(pitch - k.tonic, 12);

    // the key's own notes on their degree; the others as their usual names: b2 b3 #4 b6 b7 in major,
    // b2 #3 #4 #6 #7 in minor (the raised sixth and leading note of melodic and harmonic minor)
    int degree = -1;
    for (int d = 0; d < 7; ++d)
        if (steps[d] == interval)
            degree = d;
    if (degree < 0)
    {
        const bool raise = k.minor ? interval != 1 : interval == 6;
        for (int d = 0; d < 7; ++d)
            if (steps[d] == mod(interval + (raise ? -1 : 1), 12))
                degree = d;
    }
    ScoreNote n;
    n.pitch = pitch;
    n.step = degree < 0 ? 0 : mod(tonicStep + degree, 7);
    n.alter = mod(pitch - kNatural[n.step] + 6, 12) - 6;
    if (degree < 0 || std::abs(n.alter) > 1)
    {
        // never expected; the plain name with a sharp
        static const int letters[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
        n.step = letters[mod(pitch, 12)];
        n.alter = mod(pitch, 12) - kNatural[n.step];
    }
    n.octave = (pitch - n.alter) / 12 - 1;
    return n;
}

void noteType(int duration, std::string& type, int& dots)
{
    dots = 0;
    switch (duration)
    {
    case 16: type = "whole"; break;
    case 12: type = "half"; dots = 1; break;
    case 8: type = "half"; break;
    case 6: type = "quarter"; dots = 1; break;
    case 4: type = "quarter"; break;
    case 3: type = "eighth"; dots = 1; break;
    case 2: type = "eighth"; break;
    default: type = "16th"; break;
    }
}

Score makeScore(const std::vector<ScoreInput>& inputs, const ScoreOptions& options, std::string& error)
{
    Score score;
    score.title = options.title;
    score.tempo = options.tempo > 0.0 ? options.tempo : 120.0;
    const double perSecond = score.tempo / 60.0 * kBeat; // sixteenths
    const int grid = std::clamp(options.grid, 1, 4);

    struct Part {
        const ScoreInput* input;
        std::vector<std::pair<int, std::pair<int, int>>> notes;
    };
    std::vector<Part> parts;
    std::vector<MidiNote> all;
    int end = 0;
    for (const ScoreInput& input : inputs)
    {
        Part part{&input, {}};
        for (const MidiNote& n : input.clip.notes)
        {
            if (n.channel == 9)
                continue;
            int start = static_cast<int>(std::lround(n.start * perSecond / grid)) * grid;
            int stop = static_cast<int>(std::lround(n.end * perSecond / grid)) * grid;
            start = std::max(0, start);
            stop = std::max(stop, start + grid);
            part.notes.push_back({start, {stop, n.pitch}});
            all.push_back(n);
            end = std::max(end, stop);
        }
        if (!part.notes.empty())
            parts.push_back(part);
    }
    if (parts.empty())
    {
        error = "there are no notes to write (drum parts are left out)";
        return score;
    }

    score.key = options.key.valid ? options.key : guessKey(all);
    if (!score.key.valid)
        score.key = Key{0, false, true};
    score.fifths = keyFifths(score.key);
    score.measures = std::max(1, (end + kBar - 1) / kBar);

    for (const Part& part : parts)
    {
        ScorePart out;
        out.name = part.input->name;
        out.program = part.input->clip.program;

        StaffMode mode = part.input->staff;
        if (mode == StaffMode::Auto)
        {
            // the staff that needs the fewest ledger lines; a grand staff only when one clef would need many
            double treble = 0.0, bass = 0.0, grand = 0.0;
            for (const auto& n : part.notes)
            {
                const int pitch = n.second.second;
                treble += ledgerLines(pitch, Clef::Treble);
                bass += ledgerLines(pitch, Clef::Bass);
                grand += ledgerLines(pitch, pitch >= 60 ? Clef::Treble : Clef::Bass);
            }
            const double single = std::min(treble, bass);
            const double count = static_cast<double>(part.notes.size());
            mode = single / count > 0.6 && grand < 0.5 * single ? StaffMode::Grand : treble <= bass ? StaffMode::Treble : StaffMode::Bass;
        }
        if (mode == StaffMode::Grand)
        {
            std::vector<std::pair<int, std::pair<int, int>>> upper, lower;
            for (const auto& n : part.notes)
                (n.second.second >= 60 ? upper : lower).push_back(n);
            out.staves.push_back(layOut(chordsAndRests(upper, score.measures), Clef::Treble, score.measures, score.key, score.fifths));
            out.staves.push_back(layOut(chordsAndRests(lower, score.measures), Clef::Bass, score.measures, score.key, score.fifths));
        }
        else
        {
            out.staves.push_back(layOut(chordsAndRests(part.notes, score.measures), mode == StaffMode::Bass ? Clef::Bass : Clef::Treble,
                                        score.measures, score.key, score.fifths));
        }
        score.parts.push_back(out);
    }
    return score;
}

std::string musicXml(const Score& score)
{
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
         "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" \"http://www.musicxml.org/dtds/partwise.dtd\">\n"
         "<score-partwise version=\"4.0\">\n";
    if (!score.title.empty())
        x << "  <work><work-title>" << escape(score.title) << "</work-title></work>\n";
    x << "  <identification><encoding><software>chost</software></encoding></identification>\n";
    x << "  <part-list>\n";
    for (size_t p = 0; p < score.parts.size(); ++p)
    {
        const ScorePart& part = score.parts[p];
        x << "    <score-part id=\"P" << p + 1 << "\"><part-name>" << escape(part.name) << "</part-name>";
        if (part.program >= 0)
            x << "<score-instrument id=\"P" << p + 1 << "-I1\"><instrument-name>" << escape(part.name)
              << "</instrument-name></score-instrument><midi-instrument id=\"P" << p + 1 << "-I1\"><midi-channel>" << p % 9 + 1
              << "</midi-channel><midi-program>" << part.program + 1 << "</midi-program></midi-instrument>";
        x << "</score-part>\n";
    }
    x << "  </part-list>\n";

    static const char* const steps = "CDEFGAB";
    for (size_t p = 0; p < score.parts.size(); ++p)
    {
        const ScorePart& part = score.parts[p];
        x << "  <part id=\"P" << p + 1 << "\">\n";
        for (int m = 0; m < score.measures; ++m)
        {
            x << "    <measure number=\"" << m + 1 << "\">\n";
            if (m == 0)
            {
                x << "      <attributes><divisions>4</divisions><key><fifths>" << score.fifths << "</fifths><mode>"
                  << (score.key.minor ? "minor" : "major") << "</mode></key><time><beats>4</beats><beat-type>4</beat-type></time>";
                if (part.staves.size() > 1)
                    x << "<staves>" << part.staves.size() << "</staves>";
                for (size_t s = 0; s < part.staves.size(); ++s)
                {
                    const bool treble = part.staves[s].clef == Clef::Treble;
                    x << "<clef";
                    if (part.staves.size() > 1)
                        x << " number=\"" << s + 1 << "\"";
                    x << "><sign>" << (treble ? "G" : "F") << "</sign><line>" << (treble ? 2 : 4) << "</line></clef>";
                }
                x << "</attributes>\n";
                if (p == 0)
                {
                    char tempo[32];
                    std::snprintf(tempo, sizeof tempo, "%g", std::round(score.tempo * 10.0) / 10.0);
                    x << "      <direction placement=\"above\"><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>" << tempo
                      << "</per-minute></metronome></direction-type><sound tempo=\"" << tempo << "\"/></direction>\n";
                }
            }
            for (size_t s = 0; s < part.staves.size(); ++s)
            {
                if (s > 0)
                    x << "      <backup><duration>" << kBar << "</duration></backup>\n";
                const ScoreMeasure& measure = part.staves[s].measures[static_cast<size_t>(m)];
                for (size_t i = 0; i < measure.events.size(); ++i)
                {
                    const ScoreEvent& e = measure.events[i];
                    std::string type;
                    int dots = 0;
                    noteType(e.duration, type, dots);

                    // beams: one level for eighths, a second for sixteenths, hooks for a lone sixteenth
                    std::string beams;
                    if (e.beam >= 0)
                    {
                        auto inBeam = [&](int j) { return j >= 0 && j < static_cast<int>(measure.events.size()) && measure.events[static_cast<size_t>(j)].beam == e.beam; };
                        const int at = static_cast<int>(i);
                        const bool before = inBeam(at - 1), after = inBeam(at + 1);
                        beams += std::string("<beam number=\"1\">") + (!before ? "begin" : !after ? "end" : "continue") + "</beam>";
                        if (e.duration == 1)
                        {
                            const bool b16 = before && measure.events[i - 1].duration == 1;
                            const bool a16 = after && measure.events[i + 1].duration == 1;
                            const char* level = b16 && a16 ? "continue" : a16 ? "begin" : b16 ? "end" : (!before ? "forward hook" : "backward hook");
                            beams += std::string("<beam number=\"2\">") + level + "</beam>";
                        }
                    }

                    const size_t count = e.rest ? 1 : e.notes.size();
                    for (size_t k = 0; k < count; ++k)
                    {
                        x << "      <note>";
                        if (k > 0)
                            x << "<chord/>";
                        if (e.rest)
                        {
                            x << (e.wholeBar ? "<rest measure=\"yes\"/>" : "<rest/>");
                        }
                        else
                        {
                            const ScoreNote& n = e.notes[k];
                            x << "<pitch><step>" << steps[n.step] << "</step>";
                            if (n.alter != 0)
                                x << "<alter>" << n.alter << "</alter>";
                            x << "<octave>" << n.octave << "</octave></pitch>";
                        }
                        x << "<duration>" << e.duration << "</duration>";
                        if (!e.rest)
                        {
                            if (e.notes[k].tieStop)
                                x << "<tie type=\"stop\"/>";
                            if (e.notes[k].tieStart)
                                x << "<tie type=\"start\"/>";
                        }
                        x << "<voice>" << (s == 0 ? 1 : 5) << "</voice>";
                        if (!e.wholeBar)
                        {
                            x << "<type>" << type << "</type>";
                            for (int d = 0; d < dots; ++d)
                                x << "<dot/>";
                        }
                        if (!e.rest && e.notes[k].accidental)
                        {
                            const int a = e.notes[k].alter;
                            x << "<accidental>" << (a > 0 ? "sharp" : a < 0 ? "flat" : "natural") << "</accidental>";
                        }
                        if (part.staves.size() > 1)
                            x << "<staff>" << s + 1 << "</staff>";
                        if (k == 0)
                            x << beams;
                        if (!e.rest && (e.notes[k].tieStart || e.notes[k].tieStop))
                        {
                            x << "<notations>";
                            if (e.notes[k].tieStop)
                                x << "<tied type=\"stop\"/>";
                            if (e.notes[k].tieStart)
                                x << "<tied type=\"start\"/>";
                            x << "</notations>";
                        }
                        x << "</note>\n";
                    }
                }
            }
            if (m == score.measures - 1)
                x << "      <barline location=\"right\"><bar-style>light-heavy</bar-style></barline>\n";
            x << "    </measure>\n";
        }
        x << "  </part>\n";
    }
    x << "</score-partwise>\n";
    return x.str();
}

} // namespace chost
