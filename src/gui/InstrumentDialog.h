// "乐器设置": which of the user's instruments plays each kind of MIDI track.

#pragma once

#include "core/HostEngine.h"

#include <QDialog>

#include <vector>

class QComboBox;
class QLabel;

class InstrumentDialog : public QDialog {
    Q_OBJECT

public:
    InstrumentDialog(const std::vector<chost::PluginEntry>& plugins, bool discovering, QWidget* parent = nullptr);

signals:
    void applyRequested(); // put the chosen instruments on the MIDI tracks there are

private:
    void fill();
    void autoMatch();
    void chosen(int row);

    std::vector<chost::PluginEntry> fSynths;
    std::vector<QComboBox*> fBoxes; // one per instruments::roles()
    QLabel* fStatus = nullptr;
};
