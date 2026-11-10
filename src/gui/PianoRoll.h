// The MIDI editor: a piano roll to fix what audio to MIDI got wrong, like Cubase's Key Editor.
//
//   click on empty space    add a note (drag to set its length)
//   drag a note             move it in time and pitch, snapped to the grid
//   drag a note's right end change its length
//   right click / Delete    remove notes
//   Ctrl+click, Ctrl+drag   select several notes
//   arrows                  move the selection (Shift+up/down: an octave)
//
// With a scale set, its rows are lit, the tonic marked and notes outside it outlined; with snapping on,
// added and dragged notes land on scale notes and up/down move by scale steps.
//   Ctrl+Z / Ctrl+Shift+Z   undo / redo

#pragma once

#include "core/MidiFile.h"
#include "core/Scales.h"

#include <QColor>
#include <QDialog>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollBar;
class QTimer;

class PianoRoll : public QWidget {
    Q_OBJECT

public:
    explicit PianoRoll(QWidget* parent = nullptr);

    void setClip(const chost::MidiClip& clip);
    const chost::MidiClip& clip() const { return fClip; }
    void setColour(const QColor& colour) { fColour = colour; update(); }
    void setGrid(double beats);      // snap and new note length, in beats
    void setPlayhead(double seconds);
    void quantize();                 // the selection, or every note without one
    void setScale(const chost::Scale& scale); // invalid: none
    const chost::Scale& scale() const { return fScale; }
    void setScaleSnap(bool on) { fScaleSnap = on; }
    int fitToScale();                // the selection's (or every) note outside the scale into it; how many moved
    int outsideScale() const;        // notes outside the scale
    void undo();
    void redo();
    void scrollToNotes();

signals:
    void edited();                         // the notes changed
    void audition(int pitch, int channel); // a note to play right away

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class Drag { None, Move, Resize, Select };

    int keysWidth() const;
    double secondsPerBeat() const { return 60.0 / (fClip.tempo > 0.0 ? fClip.tempo : 120.0); }
    double gridSeconds() const { return fGrid * secondsPerBeat(); }
    double snap(double seconds) const;
    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    int pitchAt(int y) const;
    int pitchToY(int pitch) const;
    QRect noteRect(const chost::MidiNote& n) const;
    int noteAt(const QPoint& pos, bool* onEnd = nullptr) const;
    double length() const;
    void updateScrollBars();
    void checkpoint(); // remember the notes for undo, before a change
    void changed();
    void removeSelected();
    void shiftSelected(double seconds, int pitch);
    QString rowName(int pitch) const;
    bool snapping() const { return fScaleSnap && fScale.valid() && !fClip.drums; }
    bool outside(const chost::MidiNote& n) const { return fScale.valid() && !fClip.drums && !fScale.contains(n.pitch); }

    chost::MidiClip fClip;
    std::vector<bool> fSelected; // parallel to fClip.notes
    std::vector<std::vector<chost::MidiNote>> fUndo, fRedo;
    QColor fColour{0x3d, 0x7f, 0xc4};
    double fGrid = 0.25;            // beats
    double fPixelsPerSecond = 160.0;
    int fRowHeight = 12;
    double fPlayhead = 0.0;
    chost::Scale fScale;
    bool fScaleSnap = false;
    QScrollBar* fHScroll = nullptr;
    QScrollBar* fVScroll = nullptr;

    Drag fDrag = Drag::None;
    QPoint fPressPos;
    QPoint fMousePos;
    int fPressNote = -1;
    std::vector<chost::MidiNote> fPressNotes; // the notes as they were when the drag started
    bool fDragChanged = false;
};

// The editor's window: the piano roll with a grid and quantize controls. Every change is written to the
// MIDI file right away, so it plays as edited.
class PianoRollDialog : public QDialog {
    Q_OBJECT

public:
    // songKey: the song's, e.g. "A minor", for the scale; empty: guessed from the notes
    PianoRollDialog(const QString& trackName, const QString& midiPath, const QColor& colour, const QString& songKey = {},
                    QWidget* parent = nullptr);

    QString midiPath() const { return fPath; }
    void setPlayhead(double seconds) { fRoll->setPlayhead(seconds); }
    void setScale(const QString& text); // "A minor", "D dorian"; empty or "off": none (tests)
    int fitToScale();                   // as the button does

    // where edits of a MIDI file go: the file itself once it is chost's "<name>_edit.mid", else that copy
    static QString editPath(const QString& midiPath);

signals:
    // the file was written; the first edit of a file chost didn't make goes to a copy, oldPath != newPath
    void saved(const QString& oldPath, const QString& newPath);
    void audition(const QString& midiPath, int pitch, int channel);

private:
    void save();
    void scaleChanged();
    void refreshOutside();

    QString fPath;
    PianoRoll* fRoll = nullptr;
    QComboBox* fGrid = nullptr;
    QComboBox* fTonic = nullptr;
    QComboBox* fScaleType = nullptr;
    QCheckBox* fSnap = nullptr;
    QPushButton* fFit = nullptr;
    QLabel* fOutside = nullptr;
    QLabel* fStatus = nullptr;
    QTimer* fSaveTimer = nullptr;
};
