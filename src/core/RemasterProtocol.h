// The lines tools/chost_workflow_remaster.py prints for chost, one event each:
//
//   PROGRESS <0-100> <message>
//   ANALYZE <bpm>\t<key>
//   STEM <id>\t<label>\t<path>
//   TUNED <role>\t<path>
//   MIDI <role>\t<path>
//   BALANCE <role>\t<volume dB>\t<pan>
//   MASTER <path>
//
// Fields are separated by tabs, so paths may hold spaces. The workflow runs other tools whose own lines
// look alike but mean something else (chost_transcribe.py: "MIDI <path>\t<note count>", chost_tune.py:
// "TUNED <hop>\t<pitch curve>"); it indents what they print, and a line whose role is not a role (holds a
// path separator, a dot, a space) or whose path is a bare number is not taken as an event either.

#pragma once

#include <string>

namespace chost {

struct RemasterEvent {
    enum class Kind { None, Progress, Analyze, Stem, Tuned, Midi, Balance, Master };
    Kind kind = Kind::None;
    std::string role;  // STEM: the stem id; TUNED, MIDI, BALANCE: the role
    std::string label; // STEM: its label; ANALYZE: the key; PROGRESS: the message
    std::string path;  // STEM, TUNED, MIDI, MASTER
    int percent = 0;   // PROGRESS
    double number = 0; // ANALYZE: bpm; BALANCE: volume dB
    double pan = 0;    // BALANCE
};

RemasterEvent parseRemasterLine(const std::string& line);

} // namespace chost
