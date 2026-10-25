// Decoding and sample rate conversion for audio files handed to Carla's audiofile player.
//
// Carla 2.5.x reads past the end of its buffer when it loads a short file (under 30 seconds) whose
// sample rate is higher than the engine's, and crashes (fixed on Carla's main branch, not released).
// Files are therefore converted to the engine rate before Carla sees them.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chost {

struct AudioFileInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint64_t frames = 0;
};

// WAV, FLAC and MP3 are understood; returns false for anything else.
bool probeAudioFile(const std::string& path, AudioFileInfo& info);

// Peak (max absolute sample over all channels) of `buckets` equal slices of the file, for drawing.
bool computePeaks(const std::string& path, size_t buckets, std::vector<float>& peaks, AudioFileInfo& info);

// Returns the path Carla should load: the original file when it is safe as is,
// or a float WAV at `engineRate` written into `cacheDir`. Empty on failure, with `error` set.
std::string prepareAudioFile(const std::string& path, uint32_t engineRate, const std::string& cacheDir, std::string& error);

} // namespace chost
