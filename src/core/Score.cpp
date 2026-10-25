#include "Score.h"

#include "Scales.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace chost {

namespace {

constexpr int kQuarter = 12; // ticks: a sixteenth is 3, an eighth of a triplet 4
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

// note lengths notation has: in a bar, on a note's position, without hiding the beats. Inside a beat of
// triplets only triplet eighths (4) and quarters (8), or the whole beat; elsewhere none of those.
bool fits(int at, int length, bool rest, const Meter& meter, const std::vector<bool>& triplets)
{
    const int end = at + length;
    const int bar = meter.barTicks();
    const int pulse = meter.pulse();
    if (end > bar)
        return false;
    auto triplet = [&](int beat) { return beat < static_cast<int>(triplets.size()) && triplets[static_cast<size_t>(beat)]; };
    if (triplet(at / pulse))
    {
        if (end > (at / pulse + 1) * pulse)
            return false;
        const int offset = at % pulse;
        return length == 4 || (length == 8 && offset != 8) || (length == pulse && offset == 0);
    }
    for (int beat = at / pulse; beat * pulse < end; ++beat)
        if (triplet(beat))
            return false;
    static const int plain[] = {72, 48, 36, 24, 18, 12, 9, 6, 3};
    if (std::find(std::begin(plain), std::end(plain), length) == std::end(plain))
        return false;
    if (length == bar && at == 0)
        return true;
    if (at % pulse != 0)
        return end <= (at / pulse + 1) * pulse; // off the beat: up to the next beat
    if (length <= pulse)
        return !rest || length != 9;
    if (meter.compound())
        return length % pulse == 0 && at % (2 * pulse) == 0; // dotted halves on the first or third beat
    // simple time: a half note or dotted quarter never hides the middle of a 4/4 or 2/4 bar
    const bool even = meter.beats % 2 == 0;
    switch (length)
    {
    case 18: return !rest && (!even || at % 24 == 0);
    case 24: return !even || at % 24 == 0;
    case 36: return !rest && at == 0;
    case 48: return at == 0;
    default: return false;
    }
}

// a length from a position in the bar as the notes (or rests) notation writes it, tied
std::vector<int> pieces(int at, int length, bool rest, const Meter& meter, const std::vector<bool>& triplets)
{
    static const int lengths[] = {72, 48, 36, 24, 18, 12, 9, 8, 6, 4, 3};
    std::vector<int> out;
    while (length > 0)
    {
        int take = 0;
        for (int d : lengths)
            if (d <= length && fits(at, d, rest, meter, triplets))
            {
                take = d;
                break;
            }
        if (take == 0)
            take = std::min(length, length % 3 == 0 ? 3 : length % 4 == 0 ? 4 : 1); // never expected
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
std::vector<Timed> chordsAndRests(std::vector<std::pair<int, std::pair<int, int>>> notes /* start, (end, pitch) */, int bars, int barTicks)
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
    if (time < bars * barTicks)
        out.push_back(Timed{time, bars * barTicks, {}});
    return out;
}

ScoreStaff layOut(const std::vector<Timed>& timeline, Clef clef, int bars, const Key& key, int fifths, const Meter& meter,
                  const std::vector<std::vector<bool>>& triplets /* per bar, per beat */)
{
    const int barTicks = meter.barTicks();
    const int pulse = meter.pulse();
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
            const int bar = at / barTicks;
            const int inBar = at % barTicks;
            const int until = std::min(t.end, (bar + 1) * barTicks);
            const std::vector<int> lengths = pieces(inBar, until - at, t.pitches.empty(), meter, triplets[static_cast<size_t>(bar)]);
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

    const int beats = barTicks / pulse;
    for (size_t bar = 0; bar < staff.measures.size(); ++bar)
    {
        ScoreMeasure& m = staff.measures[bar];
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

        // triplets: the notes and rests of each beat of triplets
        int nextTuplet = 0;
        for (int beat = 0; beat < beats; ++beat)
        {
            if (beat >= static_cast<int>(triplets[bar].size()) || !triplets[bar][static_cast<size_t>(beat)])
                continue;
            bool any = false;
            for (ScoreEvent& e : m.events)
                if (e.start / pulse == beat && e.duration < pulse)
                {
                    e.tuplet = nextTuplet;
                    any = true;
                }
            if (any)
                ++nextTuplet;
        }

        // beams: the flagged notes of a beat, and in 4/4 and 2/4 two beats of plain eighths together
        int nextBeam = 0;
        std::vector<std::vector<size_t>> runs(static_cast<size_t>(beats));
        for (int beat = 0; beat < beats; ++beat)
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
                if (e.start / pulse != beat)
                    continue;
                if (e.rest || displayLength(e) >= kQuarter)
                    close();
                else
                    run.push_back(i);
            }
            close();
        }
        if (!meter.compound() && meter.beatType == 4 && meter.beats % 2 == 0)
        {
            auto twoEighths = [&](int beat) {
                const std::vector<size_t>& run = runs[static_cast<size_t>(beat)];
                return run.size() == 2 && m.events[run[0]].duration == 6 && m.events[run[1]].duration == 6 && m.events[run[0]].tuplet < 0 &&
                       m.events[run[0]].start == beat * pulse;
            };
            for (int beat = 0; beat + 1 < beats; beat += 2)
                if (twoEighths(beat) && twoEighths(beat + 1))
                    for (size_t i : runs[static_cast<size_t>(beat + 1)])
                        m.events[i].beam = m.events[runs[static_cast<size_t>(beat)][0]].beam;
        }
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

Meter parseMeter(const std::string& text)
{
    Meter m;
    const size_t slash = text.find('/');
    if (slash == std::string::npos)
        return Meter{0, 4};
    try
    {
        m.beats = std::stoi(text.substr(0, slash));
        m.beatType = std::stoi(text.substr(slash + 1));
    }
    catch (...)
    {
        return Meter{0, 4};
    }
    return m.valid() ? m : Meter{0, 4};
}

int displayLength(const ScoreEvent& e)
{
    return e.tuplet >= 0 ? e.duration * 3 / 2 : e.duration;
}

void noteType(int duration, std::string& type, int& dots)
{
    dots = 0;
    switch (duration)
    {
    case 72: type = "whole"; dots = 1; break;
    case 48: type = "whole"; break;
    case 36: type = "half"; dots = 1; break;
    case 24: type = "half"; break;
    case 18: type = "quarter"; dots = 1; break;
    case 12: type = "quarter"; break;
    case 9: type = "eighth"; dots = 1; break;
    case 6: type = "eighth"; break;
    default: type = "16th"; break;
    }
}

Score makeScore(const std::vector<ScoreInput>& inputs, const ScoreOptions& options, std::string& error)
{
    Score score;
    score.title = options.title;
    score.tempo = options.tempo > 0.0 ? options.tempo : 120.0;
    score.meter = options.meter.valid() ? options.meter : Meter{};
    const Meter meter = score.meter;
    const int barTicks = meter.barTicks();
    const int pulse = meter.pulse();
    const double perSecond = score.tempo / 60.0 * kQuarter; // ticks
    const int step = 3 * std::clamp(options.grid, 1, 2);   // the shortest plain note: a sixteenth or an eighth
    // triplets only where beats are quarters; in 6/8 and 12/8 the beat divides in three anyway
    const bool triplets = options.triplets && !meter.compound() && pulse == kQuarter;

    struct Part {
        const ScoreInput* input;
        std::vector<std::pair<int, std::pair<int, int>>> notes;
        std::set<int> tripletBeats; // beats (counted from the start) written as triplets
    };
    std::vector<Part> parts;
    std::vector<MidiNote> all;
    int end = 0;
    for (const ScoreInput& input : inputs)
    {
        Part part{&input, {}, {}};
        std::vector<std::pair<double, double>> raw; // start, end in ticks
        std::vector<int> pitches;
        for (const MidiNote& n : input.clip.notes)
        {
            if (n.channel == 9)
                continue;
            raw.push_back({std::max(0.0, n.start * perSecond), std::max(0.0, n.end * perSecond)});
            pitches.push_back(n.pitch);
            all.push_back(n);
        }
        if (raw.empty())
            continue;

        // a note let go a little before the next one (or the bar line) is written as lasting until it,
        // so detached playing doesn't fill the page with short rests
        {
            std::vector<double> onsets;
            for (const auto& r : raw)
                onsets.push_back(r.first);
            std::sort(onsets.begin(), onsets.end());
            for (auto& [a, b] : raw)
            {
                const auto next = std::upper_bound(onsets.begin(), onsets.end(), a + 0.5);
                const double until = next != onsets.end() ? *next : std::ceil(b / barTicks - 1e-9) * barTicks;
                const double gap = until - b;
                if (gap > 0.0 && gap <= std::max(2.0, 0.3 * (b - a)))
                    b = until;
            }
        }

        // a beat is in triplets when its starts and ends sit clearly closer to thirds of the beat than to
        // the plain grid, and at least one is on a third the plain grid doesn't have
        if (triplets)
        {
            std::map<int, std::pair<double, double>> errors; // beat -> (plain, triplet)
            std::set<int> thirds;
            auto distance = [](double x, int grid) { return std::abs(x - std::round(x / grid) * grid); };
            for (const auto& [a, b] : raw)
                for (double x : {a, b})
                {
                    const double weight = x == a ? 1.0 : 0.5; // where a note stops is played less exactly
                    const int beat = static_cast<int>(std::floor(x / pulse));
                    const double inBeat = x - beat * pulse;
                    if (inBeat < 0.75 || inBeat > pulse - 0.75)
                        continue; // on a beat: either way
                    errors[beat].first += weight * distance(x, step);
                    errors[beat].second += weight * distance(x, 4);
                    const int third = static_cast<int>(std::lround(inBeat / 4.0)) * 4;
                    if (third % step != 0 && x == a)
                        thirds.insert(beat);
                }
            for (const auto& [beat, error] : errors)
                if (thirds.count(beat) && error.second < 0.6 * error.first)
                    part.tripletBeats.insert(beat);
        }
        auto snap = [&](double x) {
            const int beat = static_cast<int>(std::floor(x / pulse));
            const int grid = part.tripletBeats.count(beat) ? 4 : step;
            return static_cast<int>(std::lround(x / grid)) * grid;
        };
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const int start = snap(raw[i].first);
            int stop = snap(raw[i].second);
            if (stop <= start)
                stop = start + (part.tripletBeats.count(start / pulse) ? 4 : step);
            part.notes.push_back({start, {stop, pitches[i]}});
            end = std::max(end, stop);
        }
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
    score.measures = std::max(1, (end + barTicks - 1) / barTicks);

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
        // the part's beats of triplets, bar by bar
        std::vector<std::vector<bool>> tripletMap(static_cast<size_t>(score.measures), std::vector<bool>(static_cast<size_t>(barTicks / pulse), false));
        for (int beat : part.tripletBeats)
            if (beat / (barTicks / pulse) < score.measures)
                tripletMap[static_cast<size_t>(beat / (barTicks / pulse))][static_cast<size_t>(beat % (barTicks / pulse))] = true;
        if (mode == StaffMode::Grand)
        {
            std::vector<std::pair<int, std::pair<int, int>>> upper, lower;
            for (const auto& n : part.notes)
                (n.second.second >= 60 ? upper : lower).push_back(n);
            out.staves.push_back(layOut(chordsAndRests(upper, score.measures, barTicks), Clef::Treble, score.measures, score.key, score.fifths, meter, tripletMap));
            out.staves.push_back(layOut(chordsAndRests(lower, score.measures, barTicks), Clef::Bass, score.measures, score.key, score.fifths, meter, tripletMap));
        }
        else
        {
            out.staves.push_back(layOut(chordsAndRests(part.notes, score.measures, barTicks), mode == StaffMode::Bass ? Clef::Bass : Clef::Treble,
                                        score.measures, score.key, score.fifths, meter, tripletMap));
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
                x << "      <attributes><divisions>12</divisions><key><fifths>" << score.fifths << "</fifths><mode>"
                  << (score.key.minor ? "minor" : "major") << "</mode></key><time><beats>" << score.meter.beats << "</beats><beat-type>"
                  << score.meter.beatType << "</beat-type></time>";
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
                    char tempo[32], beat[32];
                    std::snprintf(tempo, sizeof tempo, "%g", std::round(score.tempo * 10.0) / 10.0);
                    // the beat: a dotted quarter in 6/8 and the like (the sound's tempo is in quarters either way)
                    const bool dotted = score.meter.compound();
                    std::snprintf(beat, sizeof beat, "%g", std::round((dotted ? score.tempo / 1.5 : score.tempo) * 10.0) / 10.0);
                    x << "      <direction placement=\"above\"><direction-type><metronome><beat-unit>quarter</beat-unit>"
                      << (dotted ? "<beat-unit-dot/>" : "") << "<per-minute>" << beat << "</per-minute></metronome></direction-type><sound tempo=\""
                      << tempo << "\"/></direction>\n";
                }
            }
            for (size_t s = 0; s < part.staves.size(); ++s)
            {
                if (s > 0)
                    x << "      <backup><duration>" << score.meter.barTicks() << "</duration></backup>\n";
                const ScoreMeasure& measure = part.staves[s].measures[static_cast<size_t>(m)];
                for (size_t i = 0; i < measure.events.size(); ++i)
                {
                    const ScoreEvent& e = measure.events[i];
                    std::string type;
                    int dots = 0;
                    noteType(displayLength(e), type, dots);
                    // a triplet's bracket and number, from its first to its last note or rest
                    auto inTuplet = [&](int j) {
                        return e.tuplet >= 0 && j >= 0 && j < static_cast<int>(measure.events.size()) && measure.events[static_cast<size_t>(j)].tuplet == e.tuplet;
                    };
                    const bool tupletStart = e.tuplet >= 0 && !inTuplet(static_cast<int>(i) - 1);
                    const bool tupletStop = e.tuplet >= 0 && !inTuplet(static_cast<int>(i) + 1);

                    // beams: one level for eighths, a second for sixteenths, hooks for a lone sixteenth
                    std::string beams;
                    if (e.beam >= 0)
                    {
                        auto inBeam = [&](int j) { return j >= 0 && j < static_cast<int>(measure.events.size()) && measure.events[static_cast<size_t>(j)].beam == e.beam; };
                        const int at = static_cast<int>(i);
                        const bool before = inBeam(at - 1), after = inBeam(at + 1);
                        beams += std::string("<beam number=\"1\">") + (!before ? "begin" : !after ? "end" : "continue") + "</beam>";
                        if (displayLength(e) < 6)
                        {
                            const bool b16 = before && displayLength(measure.events[i - 1]) < 6;
                            const bool a16 = after && displayLength(measure.events[i + 1]) < 6;
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
                        if (e.tuplet >= 0)
                            x << "<time-modification><actual-notes>3</actual-notes><normal-notes>2</normal-notes></time-modification>";
                        if (part.staves.size() > 1)
                            x << "<staff>" << s + 1 << "</staff>";
                        if (k == 0)
                            x << beams;
                        const bool tied = !e.rest && (e.notes[k].tieStart || e.notes[k].tieStop);
                        const bool bracket = k == 0 && (tupletStart || tupletStop);
                        if (tied || bracket)
                        {
                            x << "<notations>";
                            if (tied && e.notes[k].tieStop)
                                x << "<tied type=\"stop\"/>";
                            if (tied && e.notes[k].tieStart)
                                x << "<tied type=\"start\"/>";
                            if (bracket && tupletStart)
                                x << "<tuplet type=\"start\"/>";
                            if (bracket && tupletStop)
                                x << "<tuplet type=\"stop\"/>";
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
