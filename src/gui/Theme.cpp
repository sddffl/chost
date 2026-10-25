#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace theme {

QColor trackColour(int index)
{
    static const QColor colours[] = {
        {0x3d, 0x7f, 0xc4}, {0xc4, 0x5a, 0x3d}, {0x4f, 0xa8, 0x5c}, {0x9a, 0x5f, 0xc4}, {0xc4, 0x9a, 0x3d},
        {0x3d, 0xaa, 0xb0}, {0xc4, 0x4f, 0x86}, {0x7f, 0x96, 0x3d},
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
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, panelLight);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDim);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow::separator { background: #14161a; width: 3px; height: 3px; }
        QSplitter::handle { background: #14161a; }
        QToolBar { background: #2a2e33; border: none; border-bottom: 1px solid #14161a; spacing: 6px; padding: 4px; }
        QMenuBar { background: #2a2e33; }
        QMenuBar::item:selected, QMenu::item:selected { background: #4a90d9; }
        QMenu { background: #2a2e33; border: 1px solid #14161a; }
        QStatusBar { background: #2a2e33; color: #8b9096; }
        QPushButton, QToolButton {
            background: #353a40; border: 1px solid #14161a; border-radius: 3px; padding: 3px 8px; color: #d6d9dc;
        }
        QPushButton:hover, QToolButton:hover { background: #40464d; }
        QPushButton:pressed, QToolButton:pressed { background: #2a2e33; }
        QToolButton:checked { background: #4a90d9; color: white; }
        QPushButton:disabled { color: #5d6268; }
        QLineEdit, QComboBox {
            background: #1a1d20; border: 1px solid #14161a; border-radius: 3px; padding: 3px 6px; color: #d6d9dc;
        }
        QComboBox QAbstractItemView { background: #2a2e33; selection-background-color: #4a90d9; }
        QTreeWidget, QListWidget { background: #24282c; border: 1px solid #14161a; alternate-background-color: #282c31; }
        QHeaderView::section { background: #33383e; color: #b0b4b9; border: none; border-right: 1px solid #14161a; padding: 3px 6px; }
        QScrollBar:horizontal, QScrollBar:vertical { background: #1f2226; border: none; height: 12px; width: 12px; }
        QScrollBar::handle { background: #4a5057; border-radius: 4px; min-width: 24px; min-height: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QSlider::groove:horizontal { height: 4px; background: #14161a; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #4a90d9; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: #c8ccd0; border-radius: 3px; }
        QSlider::groove:vertical { width: 4px; background: #14161a; border-radius: 2px; }
        QSlider::handle:vertical { height: 22px; margin: 0 -9px; background: qlineargradient(y1:0, y2:1, stop:0 #e4e6e8, stop:0.45 #b8bcc0, stop:0.5 #2a2e33, stop:0.55 #b8bcc0, stop:1 #9ea3a8); border: 1px solid #14161a; border-radius: 2px; }
        QCheckBox::indicator { width: 13px; height: 13px; }
        QLabel#sectionTitle { color: #8b9096; font-weight: bold; padding: 4px 6px; background: #33383e; }
        QLabel#lcdTime { background: #101215; color: #e6e6e6; border: 1px solid #14161a; border-radius: 3px; padding: 2px 10px; font-family: "Consolas", "DejaVu Sans Mono", monospace; font-size: 20px; }
        QLabel#lcdSmall { background: #101215; color: #9fb7cf; border: 1px solid #14161a; border-radius: 3px; padding: 2px 8px; font-family: "Consolas", "DejaVu Sans Mono", monospace; font-size: 13px; }
        QToolButton#transport { min-width: 30px; min-height: 26px; font-size: 15px; }
        QToolButton#transport:checked { background: #2f8f4e; }
        QToolButton#mute:checked { background: #e08a2c; color: black; }
        QToolButton#solo:checked { background: #e6c82e; color: black; }
        QWidget#channelStrip[selected="true"] { border: 1px solid #c8ccd0; background: #30353b; }
        QToolButton#insertPower { padding: 0; border-radius: 2px; background: #24282c; }
        QToolButton#insertPower:checked { background: #4a90d9; }
        QToolButton#insertPower:disabled { background: #2a2e33; }
        QPushButton#insertSlot { text-align: left; padding: 2px 6px; background: #24282c; border-radius: 2px; }
        QPushButton#insertSlot[filled="true"] { background: #2f4a63; }
        QPushButton#insertSlot[bypassed="true"] { color: #6c7176; background: #2a2e33; }
        QToolButton#sendSlot { text-align: left; padding: 0 4px; background: #24282c; border-radius: 2px; border: none; }
        QToolButton#sendSlot[filled="true"] { background: #3d5a3a; }
        QToolButton#channelOutput { padding: 1px 4px; background: #24282c; color: #9fb7cf; border-radius: 2px; border: none; }
        QWidget#channelStrip { background: #2a2e33; border: 1px solid #14161a; border-radius: 3px; }
        QLabel#channelName { background: #3d7fc4; color: white; padding: 3px; border-radius: 2px; }
        QToolButton#channelName { background: #3d7fc4; color: white; padding: 3px; border-radius: 2px; border: none; }
        QLabel#channelNameOut { background: #6b4fa0; color: white; padding: 3px; border-radius: 2px; }
    )"));
}

} // namespace theme
