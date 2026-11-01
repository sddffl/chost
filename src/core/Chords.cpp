#include "Chords.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

namespace chost {

namespace {

// the same symbols as tools/chost_arrange.py's QUALITIES
const std::vector<std::pair<std::string, std::vector<int>>>& qualities()
{
    static const std::vector<std::pair<std::string, std::vector<int>>> list{
        {"", {0, 4, 7}}, {"maj", {0, 4, 7}}, {"M", {0, 4, 7}}, {"m", {0, 3, 7}}, {"min", {0, 3, 7}},
        {"7", {0, 4, 7, 10}}, {"maj7", {0, 4, 7, 11}}, {"M7", {0, 4, 7, 11}}, {"m7", {0, 3, 7, 10}}, {"min7", {0, 3, 7, 10}},
        {"6", {0, 4, 7, 9}}, {"m6", {0, 3, 7, 9}}, {"9", {0, 4, 7, 10, 2}}, {"add9", {0, 4, 7, 2}}, {"m9", {0, 3, 7, 10, 2}},
        {"madd9", {0, 3, 7, 2}}, {"sus4", {0, 5, 7}}, {"sus", {0, 5, 7}}, {"sus2", {0, 2, 7}}, {"7sus4", {0, 5, 7, 10}},
        {"dim", {0, 3, 6}}, {"dim7", {0, 3, 6, 9}}, {"m7b5", {0, 3, 6, 10}}, {"aug", {0, 4, 8}}, {"+", {0, 4, 8}},
    };
    return list;
}

const char* const kDefaultNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char* const kSharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kFlatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

int mod12(int v)
{
    return ((v % 12) + 12) % 12;
}

// a note name at the start of text: its pitch class and length, -1 when there is none
int parseNote(const std::string& text, size_t& length)
{
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    if (text.empty())
        return -1;
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (letter < 'A' || letter > 'G')
        return -1;
    int pc = base[letter - 'A'];
    length = 1;
    if (text.size() > 1 && (text[1] == '#' || text[1] == 'b'))
    {
        pc += text[1] == '#' ? 1 : -1;
        length = 2;
    }
    else if (text.compare(1, 3, "\xE2\x99\xAF") == 0 || text.compare(1, 3, "\xE2\x99\xAD") == 0) // ♯ ♭
    {
        pc += text.compare(1, 3, "\xE2\x99\xAF") == 0 ? 1 : -1;
        length = 4;
    }
    return mod12(pc);
}

const char* const* namesFor(const Key& key)
{
    if (!key.valid)
        return kDefaultNames;
    // the key signature, from its major (a minor key's relative major)
    const int major = key.minor ? mod12(key.tonic + 3) : key.tonic;
    switch (major)
    {
    case 7: case 2: case 9: case 4: case 11: case 6:
        return kSharpNames;
    case 5: case 10: case 3: case 8: case 1:
        return kFlatNames;
    default:
        return kDefaultNames;
    }
}

Chord make(int root, const std::string& quality)
{
    Chord c;
    c.root = mod12(root);
    c.quality = quality;
    for (const auto& [name, intervals] : qualities())
        if (name == quality)
            c.intervals = intervals;
    return c;
}

bool hasInterval(const Chord& c, int interval)
{
    return std::find(c.intervals.begin(), c.intervals.end(), interval) != c.intervals.end();
}

// minor, major or neither (sus, dim): what a chord on a degree is, for borrowed chords
int third(const Chord& c)
{
    return hasInterval(c, 3) ? 3 : hasInterval(c, 4) ? 4 : 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// keys and chords

Key parseKey(const std::string& text)
{
    Key key;
    size_t length = 0;
    std::string t = text;
    t.erase(0, t.find_first_not_of(" \t"));
    const int pc = parseNote(t, length);
    if (pc < 0)
        return key;
    std::string rest = t.substr(length);
    std::transform(rest.begin(), rest.end(), rest.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    rest.erase(0, rest.find_first_not_of(" \t"));
    key.tonic = pc;
    key.minor = rest.rfind("min", 0) == 0 || rest == "m" || rest.find("小调") != std::string::npos;
    key.valid = true;
    return key;
}

std::string keyName(const Key& key)
{
    if (!key.valid)
        return {};
    return std::string(namesFor(key)[key.tonic]) + (key.minor ? " minor" : " major");
}

Chord parseChord(const std::string& symbol)
{
    std::string s = symbol;
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    size_t length = 0;
    const int root = parseNote(s, length);
    if (root < 0)
        return {};
    std::string quality = s.substr(length);
    int bass = -1;
    if (const size_t slash = quality.find('/'); slash != std::string::npos)
    {
        size_t bassLength = 0;
        bass = parseNote(quality.substr(slash + 1), bassLength);
        if (bass < 0 || slash + 1 + bassLength != quality.size())
            return {};
        quality = quality.substr(0, slash);
    }
    Chord c = make(root, quality);
    c.bass = c.valid() ? bass : -1;
    return c;
}

std::string Chord::name(const Key& key) const
{
    if (!valid())
        return {};
    const char* const* names = namesFor(key);
    // a minor key's raised seventh, its leading note, is a sharp: G# in A minor, not Ab
    auto spell = [&](int pc) -> std::string {
        if (key.valid && key.minor && pc == mod12(key.tonic + 11) && names[pc][1] == 'b')
            return kSharpNames[pc];
        return names[pc];
    };
    std::string text = spell(root) + quality;
    if (bass >= 0)
        text += "/" + spell(bass);
    return text;
}

std::vector<int> Chord::pitchClasses() const
{
    std::vector<int> pcs;
    for (int i : intervals)
        pcs.push_back(mod12(root + i));
    return pcs;
}

// ---------------------------------------------------------------------------------------------------------------------
// the pads, and what comes next

std::vector<Pad> chordPads(const Key& keyIn, bool sevenths)
{
    const Key key = keyIn.valid ? keyIn : Key{0, false, true};
    struct Degree {
        int step;
        const char* triad;
        const char* seventh;
        const char* numeral;
    };
    static const Degree major[7] = {{0, "", "maj7", "I"}, {2, "m", "m7", "ii"}, {4, "m", "m7", "iii"}, {5, "", "maj7", "IV"},
                                    {7, "", "7", "V"}, {9, "m", "m7", "vi"}, {11, "dim", "m7b5", "vii°"}};
    static const Degree minor[7] = {{0, "m", "m7", "i"}, {2, "dim", "m7b5", "ii°"}, {3, "", "maj7", "III"}, {5, "m", "m7", "iv"},
                                    {7, "m", "m7", "v"}, {8, "", "maj7", "VI"}, {10, "", "7", "VII"}};
    // borrowed and secondary chords songs use most
    static const Degree majorColours[7] = {{7, "sus4", "7sus4", "Vsus4"}, {2, "7", "7", "II7"}, {4, "7", "7", "III7"}, {9, "7", "7", "VI7"},
                                           {5, "m", "m7", "iv"}, {8, "", "maj7", "bVI"}, {10, "", "7", "bVII"}};
    static const Degree minorColours[7] = {{7, "", "", "V"}, {7, "7", "7", "V7"}, {5, "", "7", "IV"}, {1, "", "maj7", "bII"},
                                           {7, "sus4", "7sus4", "Vsus4"}, {0, "madd9", "madd9", "i(add9)"}, {11, "dim7", "dim7", "vii°7"}};
    std::vector<Pad> pads;
    for (const Degree* row : {key.minor ? minor : major, key.minor ? minorColours : majorColours})
        for (int i = 0; i < 7; ++i)
        {
            const Degree& d = row[i];
            Pad pad;
            pad.chord = make(key.tonic + d.step, sevenths ? d.seventh : d.triad);
            pad.numeral = d.numeral;
            // the numeral says the seventh as the symbol does: Imaj7, V7, iiø7
            const std::string q = pad.chord.quality;
            std::string& numeral = pad.numeral;
            if (q == "m7b5" && numeral.find("ø") == std::string::npos)
            {
                if (const size_t deg = numeral.find("°"); deg != std::string::npos)
                    numeral.erase(deg);
                numeral += "ø7";
            }
            else if ((q == "maj7" || q == "7" || q == "m7") && numeral.find('7') == std::string::npos)
                numeral += q == "maj7" ? "maj7" : "7";
            else if (q == "7sus4" && numeral.find('7') == std::string::npos)
                numeral.insert(numeral.find("sus") == std::string::npos ? numeral.size() : numeral.find("sus"), "7");
            pads.push_back(pad);
        }
    return pads;
}

std::vector<float> nextChordScores(const Key& keyIn, const std::vector<Pad>& pads, const Chord* previous)
{
    const Key key = keyIn.valid ? keyIn : Key{0, false, true};
    // from the degree of one chord (semitones above the tonic) to the next: how often songs go there
    using Table = std::map<int, std::map<int, float>>;
    static const Table major{
        {0, {{5, .9f}, {7, .9f}, {9, .8f}, {2, .5f}, {4, .3f}, {10, .3f}, {8, .3f}}},
        {2, {{7, 1.f}, {5, .4f}, {11, .3f}, {0, .3f}}},
        {4, {{9, .9f}, {5, .7f}, {2, .4f}}},
        {5, {{7, .9f}, {0, .8f}, {2, .5f}, {9, .4f}, {5, .6f}, {4, .3f}}},
        {7, {{0, 1.f}, {9, .8f}, {5, .4f}, {4, .2f}}},
        {9, {{5, .9f}, {2, .7f}, {7, .6f}, {4, .4f}, {0, .3f}}},
        {11, {{0, 1.f}, {9, .3f}, {4, .3f}}},
        {10, {{0, .8f}, {5, .6f}, {7, .3f}}},
        {8, {{10, .8f}, {7, .6f}, {0, .5f}, {5, .4f}}},
    };
    static const Table minor{
        {0, {{5, .8f}, {8, .8f}, {10, .7f}, {7, .8f}, {3, .5f}, {2, .4f}}},
        {2, {{7, 1.f}, {5, .3f}, {0, .3f}}},
        {3, {{8, .8f}, {5, .6f}, {10, .5f}, {0, .4f}}},
        {5, {{7, .9f}, {0, .7f}, {10, .6f}, {2, .4f}, {3, .3f}}},
        {7, {{0, 1.f}, {8, .7f}, {5, .3f}}},
        {8, {{10, .8f}, {5, .7f}, {3, .6f}, {2, .4f}, {7, .5f}}},
        {10, {{3, .9f}, {0, .8f}, {8, .4f}, {5, .3f}}},
        {1, {{7, .9f}, {0, .6f}}},
        {11, {{0, 1.f}, {8, .3f}}},
    };
    const Table& table = key.minor ? minor : major;
    // the key's own chords, as triads and as sevenths; anything else on a pad is a colour
    std::vector<std::string> own;
    for (bool sevenths : {false, true})
    {
        const std::vector<Pad> row = chordPads(key, sevenths);
        for (size_t i = 0; i < 7 && i < row.size(); ++i)
            own.push_back(row[i].chord.name());
    }

    std::vector<float> scores;
    for (const Pad& pad : pads)
    {
        const int to = mod12(pad.chord.root - key.tonic);
        float score = 0.1f;
        if (!previous)
        {
            // a song starts at home, sometimes on the relative chord or the subdominant
            score = to == 0 ? 1.f : to == (key.minor ? 8 : 9) ? .6f : to == 5 ? .5f : .1f;
        }
        else
        {
            const int from = mod12(previous->root - key.tonic);
            const auto row = table.find(from);
            const auto& weights = row != table.end() ? row->second : table.at(0);
            if (const auto w = weights.find(to); w != weights.end())
                score = w->second;
        }
        // colours are spice: used less than the key's own chords
        if (std::find(own.begin(), own.end(), pad.chord.name()) == own.end())
            score *= .55f;
        if (previous)
        {
            // a dominant seventh wants to fall a fifth, a sus4 to resolve on its own root
            if (hasInterval(*previous, 4) && hasInterval(*previous, 10) && pad.chord.root == mod12(previous->root + 5))
                score = std::max(score, 1.f);
            if (hasInterval(*previous, 5) && !hasInterval(*previous, 3) && !hasInterval(*previous, 4)
                && pad.chord.root == previous->root && third(pad.chord) == 4)
                score = std::max(score, .95f);
            if (pad.chord.root == previous->root && pad.chord.quality == previous->quality)
                score *= .2f; // the same chord again
        }
        scores.push_back(score);
    }
    const float best = scores.empty() ? 0.f : *std::max_element(scores.begin(), scores.end());
    if (best > 0.f)
        for (float& s : scores)
            s /= best;
    return scores;
}

// ---------------------------------------------------------------------------------------------------------------------
// voicings and parts

std::vector<int> voiceChord(const Chord& chord, const std::vector<int>& previous, int low, int high)
{
    std::vector<int> pcs = chord.pitchClasses();
    // five-note chords: the fifth goes, the colour stays
    if (pcs.size() > 4)
        pcs.erase(pcs.begin() + 2);
    std::vector<std::vector<int>> candidates;
    for (size_t inv = 0; inv < pcs.size(); ++inv)
    {
        std::vector<int> order(pcs.begin() + static_cast<long>(inv), pcs.end());
        order.insert(order.end(), pcs.begin(), pcs.begin() + static_cast<long>(inv));
        for (int start = low; start < low + 12; ++start)
        {
            if (mod12(start) != order[0])
                continue;
            std::vector<int> notes{start};
            for (size_t k = 1; k < order.size(); ++k)
            {
                int n = notes.back() + 1;
                while (mod12(n) != order[k])
                    ++n;
                notes.push_back(n);
            }
            if (notes.back() <= high)
                candidates.push_back(notes);
        }
    }
    if (candidates.empty())
    {
        std::vector<int> notes;
        for (int pc : pcs)
            notes.push_back(low + mod12(pc - low));
        std::sort(notes.begin(), notes.end());
        return notes;
    }
    auto mean = [](const std::vector<int>& v) {
        double sum = 0.0;
        for (int n : v)
            sum += n;
        return sum / static_cast<double>(v.size());
    };
    const double centre = previous.empty() ? (low + high) / 2.0 : mean(previous);
    return *std::min_element(candidates.begin(), candidates.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
        auto cost = [&](const std::vector<int>& v) {
            return std::abs(mean(v) - centre) + (previous.empty() ? 0.0 : 0.1 * std::abs(v.back() - previous.back()));
        };
        return cost(a) < cost(b);
    });
}

int bassNote(const Chord& chord, int low)
{
    const int pc = chord.bass >= 0 ? chord.bass : chord.root;
    return low + mod12(pc - low);
}

std::vector<ProgressionChord> parseProgression(const std::string& textIn, std::string& error)
{
    std::string text = textIn;
    // the separators people type in Chinese too
    for (const char* wide : {"\xEF\xBD\x9C", "\xEF\xBC\x8C", "\xE3\x80\x81"}) // ｜ ， 、
        for (size_t at; (at = text.find(wide)) != std::string::npos;)
            text.replace(at, 3, wide[2] == '\x9C' ? "|" : " ");
    std::replace(text.begin(), text.end(), ',', ' ');
    const bool bars = text.find('|') != std::string::npos;
    std::vector<std::vector<std::string>> groups;
    std::stringstream barStream(text);
    std::string bar;
    while (std::getline(barStream, bar, bars ? '|' : '\n'))
    {
        std::stringstream words(bar);
        std::vector<std::string> tokens;
        for (std::string token; words >> token;)
            tokens.push_back(token);
        if (tokens.empty())
            continue;
        if (bars)
            groups.push_back(tokens);
        else
            for (const std::string& token : tokens)
                groups.push_back({token});
    }
    std::vector<ProgressionChord> chords;
    for (const auto& group : groups)
        for (const std::string& token : group)
        {
            const Chord c = parseChord(token);
            if (!c.valid())
            {
                error = "unknown chord: " + token;
                return {};
            }
            chords.push_back({c, 4.0 / static_cast<double>(group.size())});
        }
    if (chords.empty())
        error = "no chords given";
    return chords;
}

std::string progressionText(const std::vector<ProgressionChord>& chords, const Key& key)
{
    std::vector<std::vector<std::string>> bars;
    double at = 0.0;
    for (const ProgressionChord& c : chords)
    {
        double left = std::max(0.25, c.beats);
        while (left > 1e-6)
        {
            const size_t bar = static_cast<size_t>(std::floor(at / 4.0 + 1e-6));
            if (bars.size() <= bar)
                bars.resize(bar + 1);
            const std::string name = c.chord.name(key);
            if (bars[bar].empty() || bars[bar].back() != name)
                bars[bar].push_back(name);
            const double step = std::min(left, 4.0 * static_cast<double>(bar + 1) - at);
            at += step;
            left -= step;
        }
    }
    std::string text;
    for (const auto& bar : bars)
    {
        if (!text.empty())
            text += " | ";
        for (size_t i = 0; i < bar.size(); ++i)
            text += (i ? " " : "") + bar[i];
    }
    return text;
}

MidiClip chordClip(const std::vector<ProgressionChord>& chords, double tempo, ChordPattern pattern, double startSeconds)
{
    MidiClip clip;
    clip.tempo = tempo > 0.0 ? tempo : 120.0;
    clip.program = 0; // Acoustic Grand Piano
    clip.name = "Chords";
    const double spb = 60.0 / clip.tempo;
    auto add = [&](double beat, double beats, int pitch, int velocity) {
        MidiNote n;
        n.start = startSeconds + beat * spb;
        n.end = n.start + std::max(0.05, beats * spb - 0.01);
        n.pitch = static_cast<uint8_t>(std::clamp(pitch, 0, 127));
        n.velocity = static_cast<uint8_t>(std::clamp(velocity, 1, 127));
        clip.notes.push_back(n);
    };

    std::vector<int> previous;
    double at = 0.0;
    for (const ProgressionChord& c : chords)
    {
        const double length = std::max(0.25, c.beats);
        const std::vector<int> voicing = voiceChord(c.chord, previous, 55, 76);
        previous = voicing;
        const int bass = bassNote(c.chord, 36);
        switch (pattern)
        {
        case ChordPattern::Block:
            add(at, length, bass, 84);
            for (int note : voicing)
                add(at, length, note, 74);
            break;
        case ChordPattern::Arpeggio:
        {
            // up through the chord and an octave of its lowest note, and back
            std::vector<int> line{bass};
            line.insert(line.end(), voicing.begin(), voicing.end());
            line.push_back(voicing.front() + 12);
            for (size_t k = voicing.size(); k-- > 1;)
                line.push_back(voicing[k]);
            for (int step = 0; step * 0.5 < length - 1e-6; ++step)
            {
                const double beat = step * 0.5;
                const int note = line[static_cast<size_t>(step) % line.size()];
                // the bass rings through the chord, the others for a beat and a half
                const double ring = note == bass && step == 0 ? length : std::min(1.5, length - beat);
                add(at + beat, ring, note, step % 2 == 0 ? 80 : 66);
            }
            break;
        }
        case ChordPattern::Rhythm:
        {
            // a pop piano: on one, the and of two, the and of three and four; the bass on one and the and of two
            static const double hits[][2] = {{0.0, 1.5}, {1.5, 1.0}, {2.5, 0.5}, {3.0, 1.0}};
            for (double offset = 0.0; offset < length - 1e-6; offset += 4.0)
            {
                for (const auto& hit : hits)
                {
                    if (offset + hit[0] >= length - 1e-6)
                        continue;
                    const double beats = std::min(hit[1], length - offset - hit[0]);
                    for (int note : voicing)
                        add(at + offset + hit[0], beats, note, hit[0] == 0.0 ? 86 : 70);
                }
                add(at + offset, std::min(1.5, length - offset), bass, 86);
                if (offset + 2.5 < length - 1e-6)
                    add(at + offset + 2.5, std::min(1.5, length - offset - 2.5), bass, 74);
            }
            break;
        }
        }
        at += length;
    }
    std::sort(clip.notes.begin(), clip.notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    clip.seconds = startSeconds + at * spb;
    return clip;
}

} // namespace chost
