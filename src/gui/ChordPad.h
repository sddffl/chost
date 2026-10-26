// AI > Chord pad: the chords of the song's key on two rows of pads, played on an instrument track at a
// click (or keys 1-7 and Q-U), with the chords that would follow best lit up. Chords clicked in a row
// make a progression, which becomes a MIDI part (block, broken or in a rhythm) or goes on to the
// auto-arrangement; the chords of a song can be recognised into it, and while the song plays the pads
// show what fits after the chord playing now.

#pragma once

#include "ScriptJob.h"
#include "core/Chords.h"

#include <QDialog>
#include <QTimer>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

class ChordPadButton;

class ChordPadDialog : public QDialog {
    Q_OBJECT

public:
    struct Track {
        QString name;
        int index = -1; // in the session
        QString file;   // audio tracks: what recognition listens to
    };

    // key: the song's, e.g. "A minor", empty when unknown; outputFolder: where the MIDI parts go
    ChordPadDialog(const QString& key, double tempo, const QString& outputFolder, QWidget* parent = nullptr);
    ~ChordPadDialog() override;

    // instrument tracks to hear the pads on (selected: the session's selected track), audio tracks to recognise
    void setTracks(const std::vector<Track>& instruments, const std::vector<Track>& audio, int selected);
    void setSong(const QString& key, double tempo);
    void setPlayhead(double seconds, bool playing);
    void setProgression(const QString& text, double startSeconds = 0.0); // also for tests
    QString progression() const;    // as text, "Am F | C G"
    void recognise(int audioTrack); // the chords of an audio track into the progression (tests)
    void write();                   // the progression as a MIDI file, then written()

signals:
    void notes(int track, const std::vector<int>& pitches, bool on); // a chord to sound or to stop
    void newTrackRequested();                                       // an instrument track to hear the pads on
    void written(int track, const QString& midiPath);               // the progression as MIDI, for that track (-1: a new one)
    void arrangeRequested(const QString& chords);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void rebuildPads();
    void press(int pad);
    void release(int pad);
    void suggest(const chost::Chord* previous);
    void addToProgression(const chost::Chord& chord);
    void refreshProgression();
    void playProgression();
    void stopPreview();
    void previewStep();
    void chordMenu(const QPoint& pos);
    int target() const;      // the instrument track the pads play on, -1 without one
    chost::Key key() const;

    QString fSongKey;
    double fTempo;
    QString fOutputFolder;
    std::vector<chost::Pad> fPads;
    std::vector<chost::ProgressionChord> fProgression;
    double fStart = 0.0;       // where the progression's first bar is in the song, seconds
    int fPlaying = -1;         // the chord of the progression at the playhead, -1 when not playing
    std::vector<int> fVoicing; // the last chord played, for the next voicing
    std::vector<int> fSounding;
    int fSoundingTrack = -1;
    QTimer fPreview;
    int fPreviewIndex = -1;
    ScriptJob fRecognition;
    std::vector<Track> fAudio;

    QComboBox* fKey = nullptr;
    QCheckBox* fSevenths = nullptr;
    QComboBox* fTarget = nullptr;
    std::vector<ChordPadButton*> fButtons;
    QLabel* fSuggestion = nullptr;
    QCheckBox* fRecord = nullptr;
    QListWidget* fList = nullptr;
    QPushButton* fPlay = nullptr;
    QComboBox* fSource = nullptr;
    QPushButton* fRecognise = nullptr;
    QComboBox* fPattern = nullptr;
    QLabel* fStatus = nullptr;
};
