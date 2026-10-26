// AI > pitch correction: tools/chost_tune.py puts a vocal (or any solo line) in tune note by note and
// keeps its vibrato and slides. The tuned take replaces the track's sound, its inserts and fader stay, and
// the A/B switch goes back to the original at any time.

#pragma once

#include "ScriptJob.h"

#include <QDialog>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSlider;

// What was sung (grey) and the corrected pitch (orange) over the notes of the key.
class PitchView : public QWidget {
public:
    struct Note {
        double start, end, sung, corrected; // seconds, MIDI numbers
    };

    explicit PitchView(QWidget* parent = nullptr);
    void setScale(const QList<int>& pitchClasses); // empty: every semitone
    void setNotes(const std::vector<Note>& notes);
    void setCurves(double hop, const std::vector<float>& sung, const std::vector<float>& tuned);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void fit();

    QList<int> fScale;
    std::vector<Note> fNotes;
    std::vector<float> fSung, fTuned;
    double fHop = 0.01;
    double fLow = 55.0, fHigh = 79.0;
};

class TuneDialog : public QDialog {
    Q_OBJECT

public:
    struct Source {
        QString name;
        QString file; // the original take, never the tuned one
        bool vocal;   // a vocal stem: the first choice
    };

    // key: the project's, e.g. "A minor", empty when unknown
    TuneDialog(const std::vector<Source>& sources, int selected, const QString& key, QWidget* parent = nullptr);

    void setAutoStart(bool autoStart); // start right away (tests)
    void setListening(bool tuned);     // the A/B switch, as the track is now

    static QString tunedFileFor(const QString& original); // where the tuned take of a file goes

signals:
    void aboutToStart(const QString& original); // the track should play the original: its tuned file is rewritten
    void tuned(const QString& original, const QString& tunedFile);
    void listen(const QString& original, bool tuned); // A/B
    void playToggled();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void start();
    void finish(bool ok, const QString& message);
    void showResult(const QString& key, const QString& value);
    void applyPreset();
    void setBusy(bool busy);
    QList<int> scaleOf(const QString& key) const;
    QString chosenKey() const;          // as tools/chost_tune.py takes it: "A minor", "auto" or empty
    static QString keyName(const QString& key); // "A minor" -> "A 小调"

    std::vector<Source> fSources;
    QString fProjectKey;
    ScriptJob fScript;
    QString fOriginal;
    QString fOutput;
    QString fOutputPartial;
    std::vector<PitchView::Note> fNotes;
    std::vector<float> fSungCurve;
    QString fFoundKey; // what --key auto heard
    bool fBusy = false;
    bool fDone = false;
    bool fNotSolo = false;

    QComboBox* fTrack = nullptr;
    QComboBox* fKey = nullptr;
    QCheckBox* fStrict = nullptr;
    QComboBox* fPreset = nullptr;
    QSlider* fStrength = nullptr;
    QLabel* fStrengthLabel = nullptr;
    QLabel* fStatus = nullptr;
    QProgressBar* fProgress = nullptr;
    QLabel* fResult = nullptr;
    QScrollArea* fScroll = nullptr;
    PitchView* fView = nullptr;
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
    QPushButton* fAB = nullptr;
    QPushButton* fPlay = nullptr;
    QPushButton* fOpenFolder = nullptr;
};
