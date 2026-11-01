#include "ClipEdit.h"

#include "AudioConvert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace chost {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRamp = 0.005; // seconds: a gain change slides in this long, never a click

double fromDb(double db)
{
    return std::pow(10.0, db / 20.0);
}

} // namespace

ClipEditKind parseClipEditKind(const std::string& name, bool& ok)
{
    ok = true;
    if (name == "move")
        return ClipEditKind::Move;
    if (name == "silence")
        return ClipEditKind::Silence;
    if (name == "keep")
        return ClipEditKind::Keep;
    if (name == "fade_in")
        return ClipEditKind::FadeIn;
    if (name == "fade_out")
        return ClipEditKind::FadeOut;
    if (name == "gain")
        return ClipEditKind::Gain;
    if (name == "normalize")
        return ClipEditKind::Normalize;
    ok = false;
    return ClipEditKind::Gain;
}

bool editAudioFile(const std::string& in, const std::string& out, const ClipEdit& edit, std::string& error, std::string* note)
{
    std::vector<float> samples;
    AudioFileInfo info;
    if (!readAudioFile(in, samples, info))
    {
        error = "could not read " + in;
        return false;
    }
    const size_t channels = info.channels;
    const double rate = info.sampleRate;
    const size_t frames = samples.size() / channels;
    auto frameAt = [&](double seconds) { return static_cast<size_t>(std::clamp(std::llround(seconds * rate), 0LL, static_cast<long long>(frames))); };
    const size_t a = frameAt(edit.start);
    const size_t b = edit.end < 0.0 ? frames : frameAt(edit.end);
    if (edit.kind != ClipEditKind::Move && b <= a)
    {
        error = "the range is empty or after the end of the clip";
        return false;
    }
    const size_t ramp = std::max<size_t>(1, static_cast<size_t>(kRamp * rate));

    // a gain for every frame, from a function of the frame
    auto apply = [&](auto gainOf) {
        for (size_t f = 0; f < frames; ++f)
        {
            const float g = static_cast<float>(gainOf(f));
            if (g == 1.0f)
                continue;
            for (size_t c = 0; c < channels; ++c)
                samples[f * channels + c] *= g;
        }
    };
    // inside [a, b): g, outside 1, sliding over a few milliseconds at the edges
    auto rangeGain = [&](double g) {
        return [&, g](size_t f) {
            if (f + ramp < a || f >= b + ramp)
                return 1.0;
            double inside = 1.0;
            if (f < a)
                inside = 1.0 - static_cast<double>(a - f) / ramp;
            else if (f >= b)
                inside = 1.0 - static_cast<double>(f - b + 1) / ramp;
            inside = std::clamp(inside, 0.0, 1.0);
            return 1.0 + (g - 1.0) * inside;
        };
    };

    switch (edit.kind)
    {
    case ClipEditKind::Move:
    {
        const long long shift = std::llround(edit.seconds * rate);
        if (shift >= 0)
            samples.insert(samples.begin(), static_cast<size_t>(shift) * channels, 0.0f);
        else
        {
            const size_t drop = static_cast<size_t>(-shift);
            if (drop >= frames)
            {
                error = "the clip would move out of the song";
                return false;
            }
            samples.erase(samples.begin(), samples.begin() + static_cast<long long>(drop * channels));
            // the new first sample may be mid-wave: fade it in over a few milliseconds
            for (size_t f = 0; f < std::min(ramp, frames - drop); ++f)
                for (size_t c = 0; c < channels; ++c)
                    samples[f * channels + c] *= static_cast<float>(f) / ramp;
        }
        break;
    }
    case ClipEditKind::Silence:
        apply(rangeGain(0.0));
        break;
    case ClipEditKind::Keep:
        apply([&](size_t f) {
            // the range stays, the rest goes, edges sliding
            if (f >= a && f < b)
                return 1.0;
            const double d = f < a ? static_cast<double>(a - f) : static_cast<double>(f - b + 1);
            return std::clamp(1.0 - d / ramp, 0.0, 1.0);
        });
        break;
    case ClipEditKind::FadeIn:
        apply([&](size_t f) {
            if (f < a)
                return 0.0;
            if (f >= b)
                return 1.0;
            return std::sin(kPi / 2.0 * static_cast<double>(f - a) / static_cast<double>(b - a));
        });
        break;
    case ClipEditKind::FadeOut:
        apply([&](size_t f) {
            if (f < a)
                return 1.0;
            if (f >= b)
                return 0.0;
            return std::cos(kPi / 2.0 * static_cast<double>(f - a) / static_cast<double>(b - a));
        });
        break;
    case ClipEditKind::Gain:
        apply(rangeGain(fromDb(edit.db)));
        break;
    case ClipEditKind::Normalize:
    {
        float peak = 0.0f;
        for (size_t i = a * channels; i < b * channels; ++i)
            peak = std::max(peak, std::abs(samples[i]));
        if (peak <= 1e-6f)
        {
            error = "the range is silent";
            return false;
        }
        apply(rangeGain(fromDb(edit.db) / peak));
        break;
    }
    }

    if (note != nullptr)
    {
        float peak = 0.0f;
        for (float s : samples)
            peak = std::max(peak, std::abs(s));
        note->clear();
        if (peak > 1.0f)
        {
            char text[96];
            std::snprintf(text, sizeof text, "peaks at %+.1f dBFS: it will clip unless turned down later", 20.0 * std::log10(peak));
            *note = text;
        }
    }
    return writeWavFile(out, samples, info.channels, info.sampleRate, error);
}

bool editMidiClip(MidiClip& clip, const ClipEdit& edit, std::string& error)
{
    const double end = edit.end < 0.0 ? 1e12 : edit.end;
    std::vector<MidiNote> kept;
    switch (edit.kind)
    {
    case ClipEditKind::Move:
        for (MidiNote n : clip.notes)
        {
            n.start += edit.seconds;
            n.end += edit.seconds;
            if (n.end <= 0.0)
                continue;
            n.start = std::max(0.0, n.start);
            kept.push_back(n);
        }
        break;
    case ClipEditKind::Silence:
    case ClipEditKind::Keep:
        for (const MidiNote& n : clip.notes)
        {
            const bool inside = n.start >= edit.start && n.start < end;
            if (inside == (edit.kind == ClipEditKind::Keep))
                kept.push_back(n);
        }
        break;
    case ClipEditKind::Gain:
        for (MidiNote n : clip.notes)
        {
            if (n.start >= edit.start && n.start < end)
                n.velocity = static_cast<uint8_t>(std::clamp(std::lround(n.velocity * fromDb(edit.db)), 1L, 127L));
            kept.push_back(n);
        }
        break;
    default:
        error = "fades and normalising are for audio tracks";
        return false;
    }
    clip.notes = kept;
    clip.seconds = 0.0;
    for (const MidiNote& n : clip.notes)
        clip.seconds = std::max(clip.seconds, n.end);
    return true;
}

} // namespace chost
