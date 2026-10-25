#include "AudioConvert.h"

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace chost {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Decoded {
    AudioFileInfo info;
    std::vector<float> samples; // interleaved
};

std::string lowerExtension(const std::string& path)
{
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// dr_libs take char paths, which on Windows are in the ANSI code page; the _w variants take UTF-16
#ifdef _WIN32
#define CHOST_DR_PATH(p) fs::u8path(p).wstring().c_str()
#define CHOST_DR(fn) fn##_w
#else
#define CHOST_DR_PATH(p) (p).c_str()
#define CHOST_DR(fn) fn
#endif

bool decode(const std::string& path, Decoded& out, bool headerOnly)
{
    const std::string ext = lowerExtension(path);

    if (ext == ".wav" || ext == ".wave")
    {
        drwav wav;
        if (!CHOST_DR(drwav_init_file)(&wav, CHOST_DR_PATH(path), nullptr))
            return false;
        out.info = {wav.sampleRate, wav.channels, wav.totalPCMFrameCount};
        if (!headerOnly)
        {
            out.samples.resize(static_cast<size_t>(wav.totalPCMFrameCount) * wav.channels);
            out.info.frames = drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, out.samples.data());
            out.samples.resize(static_cast<size_t>(out.info.frames) * wav.channels);
        }
        drwav_uninit(&wav);
        return true;
    }

    if (ext == ".flac")
    {
        drflac* flac = CHOST_DR(drflac_open_file)(CHOST_DR_PATH(path), nullptr);
        if (flac == nullptr)
            return false;
        out.info = {flac->sampleRate, flac->channels, flac->totalPCMFrameCount};
        if (!headerOnly)
        {
            out.samples.resize(static_cast<size_t>(flac->totalPCMFrameCount) * flac->channels);
            out.info.frames = drflac_read_pcm_frames_f32(flac, flac->totalPCMFrameCount, out.samples.data());
            out.samples.resize(static_cast<size_t>(out.info.frames) * flac->channels);
        }
        drflac_close(flac);
        return true;
    }

    if (ext == ".mp3")
    {
        drmp3 mp3;
        if (!CHOST_DR(drmp3_init_file)(&mp3, CHOST_DR_PATH(path), nullptr))
            return false;
        const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
        out.info = {mp3.sampleRate, mp3.channels, frames};
        if (!headerOnly)
        {
            out.samples.resize(static_cast<size_t>(frames) * mp3.channels);
            out.info.frames = drmp3_read_pcm_frames_f32(&mp3, frames, out.samples.data());
            out.samples.resize(static_cast<size_t>(out.info.frames) * mp3.channels);
        }
        drmp3_uninit(&mp3);
        return true;
    }

    return false;
}

// Windowed-sinc resampler (Blackman window), good enough for previewing effects on a clip.
std::vector<float> resample(const std::vector<float>& in, uint32_t channels, uint32_t inRate, uint32_t outRate)
{
    constexpr int kHalfTaps = 32;
    const double ratio = static_cast<double>(inRate) / outRate;
    const double cutoff = std::min(1.0, 1.0 / ratio) * 0.95; // relative to the input Nyquist
    const int64_t inFrames = static_cast<int64_t>(in.size() / channels);
    const int64_t outFrames = static_cast<int64_t>(std::floor(static_cast<double>(inFrames) / ratio));

    std::vector<float> out(static_cast<size_t>(outFrames) * channels);
    std::vector<double> weights(2 * kHalfTaps);

    for (int64_t n = 0; n < outFrames; ++n)
    {
        const double t = static_cast<double>(n) * ratio;
        const int64_t base = static_cast<int64_t>(std::floor(t));

        double weightSum = 0.0;
        for (int k = 0; k < 2 * kHalfTaps; ++k)
        {
            const double x = t - static_cast<double>(base - kHalfTaps + 1 + k);
            const double arg = kPi * cutoff * x;
            const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(arg) / arg;
            const double w = (x + kHalfTaps) / (2.0 * kHalfTaps);
            const double window = (w <= 0.0 || w >= 1.0) ? 0.0
                                : 0.42 - 0.5 * std::cos(2.0 * kPi * w) + 0.08 * std::cos(4.0 * kPi * w);
            weights[k] = sinc * window;
            weightSum += weights[k];
        }

        for (uint32_t c = 0; c < channels; ++c)
        {
            double acc = 0.0;
            for (int k = 0; k < 2 * kHalfTaps; ++k)
            {
                const int64_t idx = base - kHalfTaps + 1 + k;
                if (idx >= 0 && idx < inFrames)
                    acc += weights[k] * in[static_cast<size_t>(idx) * channels + c];
            }
            out[static_cast<size_t>(n) * channels + c] = static_cast<float>(weightSum != 0.0 ? acc / weightSum : 0.0);
        }
    }
    return out;
}

} // namespace

bool probeAudioFile(const std::string& path, AudioFileInfo& info)
{
    Decoded d;
    if (!decode(path, d, true))
        return false;
    info = d.info;
    return true;
}

bool computePeaks(const std::string& path, size_t buckets, std::vector<float>& peaks, AudioFileInfo& info)
{
    Decoded d;
    if (buckets == 0 || !decode(path, d, false) || d.info.channels == 0 || d.info.frames == 0)
        return false;
    info = d.info;

    peaks.assign(buckets, 0.0f);
    const uint32_t channels = d.info.channels;
    for (uint64_t frame = 0; frame < d.info.frames; ++frame)
    {
        const size_t bucket = static_cast<size_t>(frame * buckets / d.info.frames);
        float& peak = peaks[bucket];
        for (uint32_t c = 0; c < channels; ++c)
            peak = std::max(peak, std::fabs(d.samples[static_cast<size_t>(frame) * channels + c]));
    }
    return true;
}

std::string prepareAudioFile(const std::string& path, uint32_t engineRate, const std::string& cacheDir, std::string& error)
{
    AudioFileInfo info;
    if (!probeAudioFile(path, info) || engineRate == 0 || info.sampleRate == engineRate)
        return path; // nothing to fix, or a format Carla handles on its own

    Decoded d;
    if (!decode(path, d, false) || d.info.channels == 0)
    {
        error = "could not decode " + path;
        return {};
    }

    const std::vector<float> converted = resample(d.samples, d.info.channels, d.info.sampleRate, engineRate);

    std::error_code ec;
    fs::create_directories(fs::u8path(cacheDir), ec);
    // keep the original name, carla shows the file name in the rack
    const fs::path outPath = fs::u8path(cacheDir) / (fs::u8path(path).stem().string() + ".wav");

    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = d.info.channels;
    format.sampleRate = engineRate;
    format.bitsPerSample = 32;

    drwav wav;
#ifdef _WIN32
    const bool opened = drwav_init_file_write_w(&wav, outPath.wstring().c_str(), &format, nullptr);
#else
    const bool opened = drwav_init_file_write(&wav, outPath.string().c_str(), &format, nullptr);
#endif
    if (!opened)
    {
        error = "could not write " + outPath.u8string();
        return {};
    }
    const drwav_uint64 frames = converted.size() / d.info.channels;
    const drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, converted.data());
    drwav_uninit(&wav);

    if (written != frames)
    {
        error = "could not write " + outPath.u8string();
        return {};
    }
    return outPath.u8string();
}

} // namespace chost
