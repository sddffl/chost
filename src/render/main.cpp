// chost-render - renders a .chost session to a WAV file faster than real time.
//
//   chost-render --session song.chost --output mix.wav [--seconds S] [--rate HZ] [--bits 16|24|32]
//                [--solo T]... [--mute T]...
//
// Prints "PROGRESS <percent>" while rendering and "DONE <file>\t<seconds>\t<peak dB>" at the end, or
// "ERROR <message>". --solo renders only the given tracks, in place of the session's own solos; --mute
// leaves tracks out. Runs in its own process so a plugin crashing during the render can't take chost down.

#include "core/AudioConvert.h"
#include "core/HostEngine.h"
#include "core/MidiFile.h"
#include "core/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int fail(const std::string& message)
{
    std::printf("ERROR %s\n", message.c_str());
    std::fflush(stdout);
    return 1;
}

static void put16(std::ofstream& out, uint16_t v) { out.put(char(v & 0xFF)).put(char(v >> 8)); }
static void put32(std::ofstream& out, uint32_t v) { put16(out, uint16_t(v & 0xFFFF)); put16(out, uint16_t(v >> 16)); }

class WavWriter {
public:
    bool open(const std::string& path, uint32_t rate, int bits)
    {
        fBits = bits;
        fOut.open(fs::u8path(path), std::ios::binary | std::ios::trunc);
        if (!fOut)
            return false;
        const uint16_t format = bits == 32 ? 3 : 1; // IEEE float or PCM
        const uint16_t blockAlign = uint16_t(2 * bits / 8);
        fOut.write("RIFF", 4);
        put32(fOut, 0);
        fOut.write("WAVEfmt ", 8);
        put32(fOut, 16);
        put16(fOut, format);
        put16(fOut, 2);
        put32(fOut, rate);
        put32(fOut, rate * blockAlign);
        put16(fOut, blockAlign);
        put16(fOut, uint16_t(bits));
        fOut.write("data", 4);
        put32(fOut, 0);
        return bool(fOut);
    }

    void write(const float* left, const float* right, uint32_t frames)
    {
        for (uint32_t i = 0; i < frames; ++i)
            for (float v : {left[i], right[i]})
            {
                if (fBits == 32)
                {
                    uint32_t raw;
                    std::memcpy(&raw, &v, 4);
                    put32(fOut, raw);
                    continue;
                }
                const double clamped = std::clamp(double(v), -1.0, 1.0);
                if (fBits == 16)
                {
                    put16(fOut, uint16_t(int16_t(std::lround(clamped * 32767.0))));
                }
                else
                {
                    const int32_t s = int32_t(std::lround(clamped * 8388607.0));
                    fOut.put(char(s & 0xFF)).put(char((s >> 8) & 0xFF)).put(char((s >> 16) & 0xFF));
                }
            }
        fData += uint64_t(frames) * 2 * uint64_t(fBits / 8);
    }

    bool close()
    {
        fOut.seekp(4);
        put32(fOut, uint32_t(36 + fData));
        fOut.seekp(40);
        put32(fOut, uint32_t(fData));
        fOut.close();
        return !fOut.fail();
    }

private:
    std::ofstream fOut;
    int fBits = 32;
    uint64_t fData = 0;
};

int main(int argc, char* argv[])
{
    std::string sessionPath, output;
    double seconds = 0.0;
    uint32_t rate = 0;
    int bits = 24;
    std::vector<int> solos, mutes;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--session")
            sessionPath = value();
        else if (arg == "--output")
            output = value();
        else if (arg == "--seconds")
            seconds = std::atof(value().c_str());
        else if (arg == "--rate")
            rate = uint32_t(std::atoi(value().c_str()));
        else if (arg == "--bits")
            bits = std::atoi(value().c_str());
        else if (arg == "--solo")
            solos.push_back(std::atoi(value().c_str()));
        else if (arg == "--mute")
            mutes.push_back(std::atoi(value().c_str()));
        else
            return fail("unknown option " + arg);
    }
    if (sessionPath.empty() || output.empty())
        return fail("usage: chost-render --session FILE.chost --output FILE.wav [--seconds S] [--rate HZ] [--bits 16|24|32] [--solo T] [--mute T]");
    if (bits != 16 && bits != 24 && bits != 32)
        return fail("--bits must be 16, 24 or 32");

    // the players' files were prepared at the engine's rate, render at that rate
    std::ifstream sessionFile(fs::u8path(sessionPath));
    std::string line;
    double length = 0.0;
    uint32_t savedRate = 0, fileRate = 0;
    while (std::getline(sessionFile, line))
    {
        if (line.rfind("rate ", 0) == 0)
            savedRate = uint32_t(std::atol(line.c_str() + 5));
        if (line.rfind("file ", 0) != 0 || line.size() <= 5)
            continue;
        const std::string file = line.substr(5);
        chost::AudioFileInfo info;
        chost::MidiClip clip;
        std::string error;
        if (chost::probeAudioFile(file, info) && info.sampleRate > 0)
        {
            if (fileRate == 0)
                fileRate = info.sampleRate;
            length = std::max(length, double(info.frames) / info.sampleRate);
        }
        else if (chost::readMidiFile(file, clip, error))
        {
            length = std::max(length, clip.seconds);
        }
    }
    if (rate == 0)
        rate = savedRate > 0 ? savedRate : fileRate; // sessions before chost 0.7 don't say
    if (rate == 0)
        rate = 48000;
    if (seconds <= 0.0)
        seconds = length + 2.0; // reverb and release tails
    if (seconds <= 0.0)
        return fail("the session has nothing to render");

    constexpr uint32_t block = 512;
    chost::HostEngine host;
    if (!host.startOffline(rate, block))
        return fail("cannot start the engine: " + host.lastError());
    chost::Session session(host);
    if (!session.load(sessionPath))
        return fail("cannot open the session: " + session.lastError());
    if (!solos.empty())
        for (size_t t = 0; t < session.tracks().size(); ++t)
            session.setSolo(int(t), std::find(solos.begin(), solos.end(), int(t)) != solos.end());
    for (int t : mutes)
    {
        session.setSolo(t, false); // a soloed track would silence the others
        session.setMuted(t, true);
    }

    WavWriter wav;
    if (!wav.open(output, rate, bits))
        return fail("cannot write " + output);

    const uint64_t total = uint64_t(seconds * rate);
    std::vector<float> left(block), right(block);
    float peak = 0.0f;
    int lastPercent = -1;
    for (uint64_t done = 0; done < total;)
    {
        const uint32_t frames = uint32_t(std::min<uint64_t>(block, total - done));
        host.renderBlock(left.data(), right.data(), block);
        for (uint32_t i = 0; i < frames; ++i)
            peak = std::max({peak, std::fabs(left[i]), std::fabs(right[i])});
        wav.write(left.data(), right.data(), frames);
        done += frames;
        const int percent = int(done * 100 / total);
        if (percent != lastPercent)
        {
            std::printf("PROGRESS %d\n", percent);
            std::fflush(stdout);
            lastPercent = percent;
        }
    }
    if (!wav.close())
        return fail("cannot write " + output);
    host.stop();

    const double peakDb = peak > 0.0f ? 20.0 * std::log10(peak) : -120.0;
    std::printf("DONE %s\t%.2f\t%.1f\n", output.c_str(), seconds, peakDb);
    std::fflush(stdout);
    return 0;
}
