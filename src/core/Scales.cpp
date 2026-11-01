#include "Scales.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace chost {

namespace {

int mod12(int v)
{
    return ((v % 12) + 12) % 12;
}

int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

// the key whose signature spells the scale: its own for major and minor, else major or minor by its third
Key spellingKey(const Scale& scale)
{
    const bool minor = std::find(scale.steps.begin(), scale.steps.end(), 3) != scale.steps.end() &&
                       std::find(scale.steps.begin(), scale.steps.end(), 4) == scale.steps.end();
    return Key{scale.tonic, minor, true};
}

} // namespace

const std::vector<ScaleType>& scaleTypes()
{
    static const std::vector<ScaleType> list{
        {"major", {0, 2, 4, 5, 7, 9, 11}},
        {"minor", {0, 2, 3, 5, 7, 8, 10}},
        {"harmonic minor", {0, 2, 3, 5, 7, 8, 11}},
        {"melodic minor", {0, 2, 3, 5, 7, 9, 11}},
        {"dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {"lydian", {0, 2, 4, 6, 7, 9, 11}},
        {"mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"major pentatonic", {0, 2, 4, 7, 9}},
        {"minor pentatonic", {0, 3, 5, 7, 10}},
        {"blues", {0, 3, 5, 6, 7, 10}},
    };
    return list;
}

bool Scale::contains(int pitch) const
{
    const int interval = mod12(pitch - tonic);
    return std::find(steps.begin(), steps.end(), interval) != steps.end();
}

bool Scale::isTonic(int pitch) const
{
    return valid() && mod12(pitch) == tonic;
}

Scale makeScale(int tonic, const std::string& type)
{
    Scale s;
    s.tonic = mod12(tonic);
    for (const ScaleType& t : scaleTypes())
        if (t.id == type)
        {
            s.type = t.id;
            s.steps = t.steps;
        }
    return s;
}

Scale scaleOfKey(const Key& key)
{
    return key.valid ? makeScale(key.tonic, key.minor ? "minor" : "major") : Scale{};
}

Scale parseScale(const std::string& text)
{
    const Key key = parseKey(text);
    if (!key.valid)
        return {};
    // the words after the note: the longest type named wins ("harmonic minor" over "minor")
    std::string rest = lower(text);
    rest.erase(0, rest.find_first_not_of(" \t"));
    const size_t space = rest.find_first_of(" \t");
    rest = space == std::string::npos ? std::string() : rest.substr(space + 1);
    rest.erase(0, rest.find_first_not_of(" \t"));
    rest.erase(rest.find_last_not_of(" \t") + 1);
    if (rest.empty() || rest == "m" || rest == "min")
        return scaleOfKey(key);
    if (rest == "aeolian")
        return makeScale(key.tonic, "minor");
    if (rest == "ionian")
        return makeScale(key.tonic, "major");
    Scale s = makeScale(key.tonic, rest);
    return s.valid() ? s : (rest.find("minor") != std::string::npos || rest.find("major") != std::string::npos ? scaleOfKey(key) : Scale{});
}

std::string noteName(int pitchClass, const Key& key)
{
    Chord c;
    c.root = mod12(pitchClass);
    c.intervals = {0};
    return c.name(key);
}

std::string scaleName(const Scale& scale)
{
    if (!scale.valid())
        return {};
    return noteName(scale.tonic, spellingKey(scale)) + " " + scale.type;
}

int scaleIndex(const Scale& scale, int pitch)
{
    if (!scale.valid())
        return pitch;
    const int rel = pitch - scale.tonic;
    const int octave = floorDiv(rel, 12);
    const int within = rel - octave * 12;
    int index = 0;
    for (size_t i = 0; i < scale.steps.size(); ++i)
        if (scale.steps[i] <= within)
            index = static_cast<int>(i);
    return octave * static_cast<int>(scale.steps.size()) + index;
}

int scalePitch(const Scale& scale, int index)
{
    if (!scale.valid())
        return index;
    const int n = static_cast<int>(scale.steps.size());
    const int octave = floorDiv(index, n);
    return scale.tonic + octave * 12 + scale.steps[static_cast<size_t>(index - octave * n)];
}

int snapToScale(int pitch, const Scale& scale, int prefer)
{
    if (!scale.valid() || scale.contains(pitch))
        return pitch;
    const int below = scalePitch(scale, scaleIndex(scale, pitch));
    const int above = scalePitch(scale, scaleIndex(scale, pitch) + 1);
    int to = pitch - below < above - pitch ? below : above - pitch < pitch - below ? above : (prefer > 0 ? above : below);
    if (to < 0 || to > 127)
        to = to < 0 ? above : below;
    return std::clamp(to, 0, 127);
}

int stepInScale(int pitch, const Scale& scale, int steps)
{
    if (!scale.valid())
        return std::clamp(pitch + steps, 0, 127);
    const int from = snapToScale(pitch, scale, steps);
    int to = scalePitch(scale, scaleIndex(scale, from) + steps);
    while (to > 127)
        to = scalePitch(scale, scaleIndex(scale, to) - static_cast<int>(scale.steps.size()));
    while (to < 0)
        to = scalePitch(scale, scaleIndex(scale, to) + static_cast<int>(scale.steps.size()));
    return to;
}

int fitToScale(std::vector<MidiNote>& notes, const Scale& scale, const std::vector<bool>* only)
{
    if (!scale.valid())
        return 0;
    int moved = 0;
    int previous = -1; // the pitch of the last melodic note before, as it ends up
    for (size_t i = 0; i < notes.size(); ++i)
    {
        MidiNote& n = notes[i];
        if (n.channel == 9)
            continue;
        const bool marked = only == nullptr || (i < only->size() && (*only)[i]);
        if (marked && !scale.contains(n.pitch))
        {
            const int prefer = previous < 0 ? 0 : previous > n.pitch ? 1 : -1;
            n.pitch = static_cast<uint8_t>(snapToScale(n.pitch, scale, prefer));
            ++moved;
        }
        previous = n.pitch;
    }
    return moved;
}

int countOutside(const std::vector<MidiNote>& notes, const Scale& scale)
{
    if (!scale.valid())
        return 0;
    return static_cast<int>(std::count_if(notes.begin(), notes.end(), [&](const MidiNote& n) { return n.channel != 9 && !scale.contains(n.pitch); }));
}

Key guessKey(const std::vector<MidiNote>& notes, float* confidence)
{
    // Krumhansl & Kessler's key profiles
    static const double major[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double minor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    if (confidence != nullptr)
        *confidence = 0.0f;

    double weight[12] = {};
    double total = 0.0;
    int count = 0;
    for (const MidiNote& n : notes)
    {
        if (n.channel == 9)
            continue;
        // long notes count more, but a held drone doesn't decide everything
        const double w = std::clamp(n.end - n.start, 0.05, 2.0);
        weight[n.pitch % 12] += w;
        total += w;
        ++count;
    }
    if (count < 8 || total < 2.0)
        return {};

    auto correlate = [&](const double* profile, int tonic) {
        double mx = 0.0, my = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            mx += weight[(i + tonic) % 12];
            my += profile[i];
        }
        mx /= 12.0;
        my /= 12.0;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const double x = weight[(i + tonic) % 12] - mx;
            const double y = profile[i] - my;
            sxy += x * y;
            sxx += x * x;
            syy += y * y;
        }
        return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt(sxx * syy) : 0.0;
    };

    Key best;
    double bestR = -2.0;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (int m = 0; m < 2; ++m)
        {
            const double r = correlate(m ? minor : major, tonic);
            if (r > bestR)
            {
                bestR = r;
                best = Key{tonic, m == 1, true};
            }
        }
    if (confidence != nullptr)
        *confidence = static_cast<float>(std::clamp(bestR, 0.0, 1.0));
    return best;
}

} // namespace chost
