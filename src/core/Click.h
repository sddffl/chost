// The metronome's part: a woodblock on every beat, higher and louder on the first beat of each bar, as a
// General MIDI drum part (small at any length; any GM sound font plays it).

#pragma once

#include <string>

namespace chost {

// A MIDI file of clicks at the tempo, from the song's start, seconds long.
bool writeClickTrack(const std::string& path, double bpm, int beatsPerBar, double seconds, std::string& error);

} // namespace chost
