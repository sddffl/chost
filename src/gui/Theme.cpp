#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace theme {

QColor trackColour(int index)
{
    static const QColor colours[] = {
        {0x00, 0xbf, 0xe6}, // Electric Cyan
        {0xff, 0x70, 0x43}, // Coral Orange
        {0x2e, 0xcc, 0x71}, // Emerald Green
        {0xa8, 0x55, 0xf7}, // Deep Violet
        {0xff, 0xb7, 0x03}, // Amber Gold
        {0x06, 0xd6, 0xa0}, // Mint Teal
        {0xf4, 0x3f, 0x5e}, // Crimson Rose
        {0x3b, 0x82, 0xf6}, // Royal Blue
        {0xec, 0x48, 0x99}, // Hot Pink
        {0x10, 0xb9, 0x81}  // Jade Green
    };
    constexpr int count = sizeof(colours) / sizeof(colours[0]);
    return colours[((index % count) + count) % count];
}

void apply(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, panel);
    p.setColor(QPalette::AlternateBase, panelLight);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panelLight);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::ToolTipBase, panelLight);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDim);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow { background: #111317; }
        QMainWindow::separator { background: #1a1e26; width: 4px; height: 4px; }
        QSplitter::handle { background: #1a1e26; }
        QSplitter::handle:hover { background: #00b4d8; }

        QToolBar {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1a1d24, stop:1 #13151a);
            border: none;
            border-bottom: 1px solid #232833;
            spacing: 6px;
            padding: 5px 8px;
        }

        QMenuBar {
            background: #14161c;
            border-bottom: 1px solid #222630;
            color: #d8dde6;
            font-size: 13px;
        }
        QMenuBar::item {
            background: transparent;
            padding: 4px 10px;
            border-radius: 4px;
        }
        QMenuBar::item:selected {
            background: #232834;
            color: #00e5ff;
        }
        QMenu {
            background: #181b22;
            border: 1px solid #2d3442;
            border-radius: 6px;
            padding: 4px;
        }
        QMenu::item {
            padding: 5px 24px 5px 12px;
            border-radius: 4px;
            color: #dce0e8;
        }
        QMenu::item:selected {
            background: #0077b6;
            color: #ffffff;
        }
        QMenu::separator {
            height: 1px;
            background: #262c38;
            margin: 4px 8px;
        }

        QStatusBar {
            background: #121419;
            border-top: 1px solid #1f232c;
            color: #88909e;
            font-size: 12px;
            padding: 2px 6px;
        }

        QPushButton, QToolButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #252a34, stop:1 #1c2028);
            border: 1px solid #303746;
            border-radius: 4px;
            padding: 4px 10px;
            color: #e2e6ec;
            font-size: 12px;
            font-weight: 500;
        }
        QPushButton:hover, QToolButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2f3644, stop:1 #242934);
            border: 1px solid #4a5568;
            color: #ffffff;
        }
        QPushButton:pressed, QToolButton:pressed {
            background: #15181f;
            border: 1px solid #1f232c;
        }
        QToolButton:checked {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #0099cc, stop:1 #0077aa);
            border: 1px solid #00c3ff;
            color: #ffffff;
            font-weight: bold;
        }
        QPushButton:disabled, QToolButton:disabled {
            background: #181b22;
            border: 1px solid #222630;
            color: #555c68;
        }

        /* Hero AI Action Buttons */
        QToolButton#aiHero {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ff6b35, stop:1 #e03800);
            border: 1px solid #ff9248;
            border-radius: 5px;
            padding: 4px 12px;
            color: #ffffff;
            font-weight: bold;
        }
        QToolButton#aiHero:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ff7e4d, stop:1 #f04400);
            border: 1px solid #ffaa6e;
        }

        /* Hardware Transport Buttons */
        QToolButton#transport {
            min-width: 32px;
            min-height: 26px;
            font-size: 14px;
            border-radius: 4px;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #282d38, stop:1 #1e222a);
            border: 1px solid #363d4c;
        }
        QToolButton#transport:checked {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #00e676, stop:1 #00b050);
            border: 1px solid #69f0ae;
            color: #051408;
            font-weight: bold;
        }
        QToolButton#transportLoop:checked {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #00b4d8, stop:1 #0077b6);
            border: 1px solid #48cae4;
            color: #ffffff;
        }

        QLineEdit, QComboBox, QDoubleSpinBox {
            background: #0f1116;
            border: 1px solid #2a303c;
            border-radius: 4px;
            padding: 3px 8px;
            color: #e2e6ec;
            selection-background-color: #0077b6;
        }
        QLineEdit:focus, QComboBox:focus, QDoubleSpinBox:focus {
            border: 1px solid #00b4d8;
        }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 18px;
            border-left: 1px solid #232834;
        }
        QComboBox QAbstractItemView {
            background: #171a22;
            border: 1px solid #2e3544;
            selection-background-color: #0077b6;
            color: #e2e6ec;
            padding: 2px;
        }

        QTreeWidget, QListWidget {
            background: #13151a;
            border: 1px solid #20242e;
            alternate-background-color: #171920;
            color: #dfe3ea;
            border-radius: 4px;
        }
        QTreeWidget::item:hover, QListWidget::item:hover {
            background: #1f242e;
        }
        QTreeWidget::item:selected, QListWidget::item:selected {
            background: #253346;
            color: #00e5ff;
        }

        QHeaderView::section {
            background: #1c2028;
            color: #9aa1ad;
            font-weight: bold;
            font-size: 11px;
            border: none;
            border-right: 1px solid #252a34;
            border-bottom: 1px solid #252a34;
            padding: 4px 8px;
        }

        /* Modern Minimalist Scrollbars */
        QScrollBar:horizontal {
            background: transparent;
            height: 8px;
            margin: 0;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 0;
        }
        QScrollBar::handle:horizontal, QScrollBar::handle:vertical {
            background: #363d4a;
            border-radius: 4px;
            min-width: 24px;
            min-height: 24px;
        }
        QScrollBar::handle:hover {
            background: #505c6e;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            width: 0;
            height: 0;
        }

        /* Sliders */
        QSlider::groove:horizontal {
            height: 4px;
            background: #13151a;
            border: 1px solid #222630;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #00b4d8;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px;
            margin: -5px 0;
            background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, stop:0 #ffffff, stop:0.8 #bcc3cd, stop:1 #8c94a0);
            border: 1px solid #303642;
            border-radius: 7px;
        }
        QSlider::groove:vertical {
            width: 4px;
            background: #0c0e12;
            border: 1px solid #1c2028;
            border-radius: 2px;
        }
        QSlider::handle:vertical {
            height: 24px;
            margin: 0 -10px;
            background: qlineargradient(y1:0, y2:1, stop:0 #ffffff, stop:0.25 #ccd2da, stop:0.48 #9aa0aa, stop:0.5 #20242c, stop:0.52 #9aa0aa, stop:0.85 #ccd2da, stop:1 #707682);
            border: 1px solid #282d38;
            border-radius: 3px;
        }

        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            background: #15181e;
            border: 1px solid #303746;
            border-radius: 3px;
        }
        QCheckBox::indicator:checked {
            background: #00b4d8;
            border: 1px solid #48cae4;
        }

        QLabel#sectionTitle {
            color: #a4abb8;
            font-weight: bold;
            font-size: 11px;
            letter-spacing: 0.5px;
            padding: 5px 8px;
            background: #1c2028;
            border-bottom: 1px solid #262c38;
        }

        /* Hardware OLED Digital Displays */
        QLabel#lcdTime {
            background: #080a0e;
            color: #00e5ff;
            border: 1px solid #232834;
            border-radius: 4px;
            padding: 2px 10px;
            font-family: "Consolas", "Courier New", monospace;
            font-size: 19px;
            font-weight: bold;
        }
        QLabel#lcdSmall {
            background: #080a0e;
            color: #ffca28;
            border: 1px solid #232834;
            border-radius: 4px;
            padding: 2px 8px;
            font-family: "Consolas", "Courier New", monospace;
            font-size: 13px;
            font-weight: bold;
        }
        QDoubleSpinBox#lcdSmall {
            background: #080a0e;
            color: #ffca28;
            border: 1px solid #232834;
            border-radius: 4px;
            padding: 2px 6px;
            font-family: "Consolas", "Courier New", monospace;
            font-size: 13px;
            font-weight: bold;
        }

        QToolButton#mute:checked {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ff9800, stop:1 #e65100);
            color: #000000;
            font-weight: bold;
            border: 1px solid #ffb74d;
        }
        QToolButton#solo:checked {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffee58, stop:1 #fbc02d);
            color: #000000;
            font-weight: bold;
            border: 1px solid #fff59d;
        }

        /* MixConsole Channel Strips */
        QWidget#channelStrip {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1a1d24, stop:1 #13151a);
            border: 1px solid #242934;
            border-radius: 5px;
        }
        QWidget#channelStrip[selected="true"] {
            border: 1px solid #00b4d8;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #222732, stop:1 #181b22);
        }

        QToolButton#insertPower {
            padding: 0;
            border-radius: 2px;
            background: #181b22;
            border: 1px solid #252a34;
        }
        QToolButton#insertPower:checked {
            background: #00b4d8;
            border: 1px solid #48cae4;
        }
        QToolButton#insertPower:disabled {
            background: #181b22;
        }

        QPushButton#insertSlot {
            text-align: left;
            padding: 2px 6px;
            background: #161820;
            border: 1px solid #242934;
            border-radius: 3px;
            color: #9aa1ad;
        }
        QPushButton#insertSlot[filled="true"] {
            background: #1e3348;
            border: 1px solid #2b4e70;
            color: #e2f0ff;
        }
        QPushButton#insertSlot[bypassed="true"] {
            color: #606670;
            background: #181b20;
        }

        QToolButton#sendSlot {
            text-align: left;
            padding: 0 4px;
            background: #161820;
            border-radius: 3px;
            border: 1px solid #242934;
            color: #9aa1ad;
        }
        QToolButton#sendSlot[filled="true"] {
            background: #1f382a;
            border: 1px solid #2e5840;
            color: #bbf7d0;
        }

        QToolButton#channelOutput {
            padding: 2px 6px;
            background: #161820;
            color: #8da2b8;
            border-radius: 3px;
            border: 1px solid #242934;
        }
        QLabel#channelName, QToolButton#channelName {
            background: #0088cc;
            color: white;
            padding: 3px;
            border-radius: 3px;
            font-weight: bold;
            border: none;
        }
        QLabel#channelNameOut {
            background: #7c4dff;
            color: white;
            padding: 3px;
            border-radius: 3px;
            font-weight: bold;
        }

        /* QDockWidget for PRODUCER Tape Deck & Tool windows */
        QDockWidget {
            color: #dce0e8;
            font-weight: bold;
        }
        QDockWidget::title {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #222630, stop:1 #181b22);
            text-align: left;
            padding: 6px 10px;
            border-bottom: 1px solid #282e3a;
            color: #a4abb8;
            font-size: 11px;
            letter-spacing: 0.5px;
        }
    )"));
}

} // namespace theme
