// Where chost finds its helper scripts, bundled sounds and the AI environment.

#pragma once

#include <QString>

namespace resources {

// A file shipped with chost: next to chost.exe in a release (tools/..., sounds/...), in the source tree
// when run from a build folder. Empty when it can't be found.
QString toolScript(const QString& name);   // tools/<name>
QString soundFile(const QString& name);    // sounds/<name>, resources/sounds/<name> in the source tree

// chost-render, the offline mixdown tool next to chost; empty when missing.
QString renderTool();

// The Python that has the AI components, as set up in the stem separation window; empty until then.
QString aiPython();
void setAiPython(const QString& python);

// The instrument new MIDI tracks get: the bundled General MIDI sound font.
QString defaultInstrumentSpec();

} // namespace resources
