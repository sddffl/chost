// AI > Auto-arrangement: tools/chost_arrange.py writes drums, bass, keys, guitar and strings as MIDI
// files, from chords typed in or recognised in a track, in a style; the main window puts them on
// instrument tracks with the user's instruments.

#pragma once

#include "ScriptJob.h"

#include <QDialog>

#include <map>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;

class ArrangeDialog : public QDialog {
    Q_OBJECT

public:
    struct Source {
        QString name;
        QString file; // audio or MIDI
        bool midi = false;
    };
    struct Part {
        QString file;
        QString role; // instrument role: drums, bass, piano, guitar, other
        QString name; // 鼓, 贝斯, ...
    };

    ArrangeDialog(const std::vector<Source>& sources, int selected, double tempo, const QString& outputFolder,
                  QWidget* parent = nullptr);

    void setChords(const QString& chords); // typed chords, e.g. for tests
    void setAutoStart(bool autoStart);

signals:
    // the parts of a finished arrangement; replace: the previous arrangement's tracks should go
    void arranged(const std::vector<ArrangeDialog::Part>& parts, bool replace);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void start();
    void styleChanged();
    void sourceChanged();
    void setBusy(bool busy);

    std::vector<Source> fSources;
    double fTempo;
    QString fOutputFolder;
    std::vector<Part> fParts;
    ScriptJob fJob;
    bool fBusy = false;

    QRadioButton* fFromTrack = nullptr;
    QRadioButton* fFromChords = nullptr;
    QComboBox* fTrack = nullptr;
    QLineEdit* fChords = nullptr;
    QComboBox* fStyle = nullptr;
    QComboBox* fForm = nullptr;
    std::map<QString, QCheckBox*> fPartBoxes;
    QCheckBox* fReplace = nullptr;
    QLabel* fStatus = nullptr;
    QLabel* fChordView = nullptr;
    QProgressBar* fProgress = nullptr;
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
};
