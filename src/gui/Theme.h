// Dark colour scheme in the spirit of high-end modern audio workstations and hardware consoles.

#pragma once

#include <QColor>

class QApplication;

namespace theme {

inline const QColor window{0x11, 0x13, 0x17};      // Deep obsidian charcoal
inline const QColor panel{0x17, 0x1a, 0x20};       // Dark console surface
inline const QColor panelLight{0x21, 0x25, 0x2e};  // Elevated control card
inline const QColor border{0x26, 0x2b, 0x35};      // Subtle dark border
inline const QColor borderLight{0x35, 0x3c, 0x4a};
inline const QColor text{0xee, 0xf1, 0xf5};        // Clean high-contrast typography
inline const QColor textDim{0x8c, 0x93, 0xa1};     // Muted slate
inline const QColor accent{0x00, 0xb4, 0xd8};      // Electric Cyan Accent
inline const QColor accentGlow{0x00, 0xe5, 0xff};  // Neon glow
inline const QColor lcd{0x08, 0x0a, 0x0d};         // OLED black
inline const QColor trackColor{0x00, 0xa8, 0xe8};
inline const QColor event{0x1a, 0x24, 0x32};
inline const QColor waveform{0x00, 0xe5, 0xff};
inline const QColor cursor{0x00, 0xe5, 0xff};      // Neon cyan laser playhead
inline const QColor meterLow{0x00, 0xe6, 0x76};    // Vibrant studio green
inline const QColor meterMid{0xff, 0xd6, 0x00};    // Studio amber
inline const QColor meterHigh{0xff, 0x17, 0x44};   // Peak warning red
inline const QColor mute{0xff, 0x91, 0x00};        // Warm studio amber
inline const QColor solo{0xff, 0xea, 0x00};        // Pure studio gold
inline const QColor selection{0x25, 0x2e, 0x3d};

// track colours, handed out in turn like modern studio DAWs
QColor trackColour(int index);

void apply(QApplication& app);

} // namespace theme
