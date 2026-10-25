// Custom widgets for the Cubase-style window: level meter, arrange view, insert slots, channel strip.

#pragma once

#include <QColor>
#include <QStringList>
#include <QWidget>

#include <vector>

class QLabel;
class QPushButton;
class QScrollBar;
class QSlider;
class QToolButton;

// Vertical stereo peak meter, -60 .. +6 dB, with a falling peak hold.
class LevelMeter : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeter(QWidget* parent = nullptr);
    void setLevels(float left, float right); // linear peaks
    QSize sizeHint() const override { return {18, 160}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    float fLevel[2] = {0.0f, 0.0f};
    float fHold[2] = {0.0f, 0.0f};
};

// A channel's pan: a bar from the centre to where it points, "C", "L30" or "R45" on it. Dragged sideways,
// the wheel moves it in steps, a double-click centres it.
class PanControl : public QWidget {
    Q_OBJECT

public:
    explicit PanControl(QWidget* parent = nullptr);
    void setPan(float pan); // -1 left .. 1 right, without panChanged
    float pan() const { return fPan; }
    QSize sizeHint() const override { return {70, 16}; }
    static QString text(float pan);

signals:
    void panChanged(float pan);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void change(float pan);

    float fPan = 0.0f;
    float fDragPan = 0.0f;
    int fDragX = 0;
};

// Project window: track header column, bar ruler and one lane per audio track with its waveform.
class ArrangeView : public QWidget {
    Q_OBJECT

public:
    struct NoteBar {
        float start = 0.0f; // seconds
        float end = 0.0f;
        int pitch = 0;
    };

    struct Lane {
        QString name;
        QString kindText;          // shown under the name, e.g. "音频" or the instrument
        QString clipName;          // empty: the track has no audio or MIDI yet
        bool midi = false;         // a MIDI clip: notes are drawn instead of a waveform
        std::vector<NoteBar> notes;
        std::vector<float> peaks;  // max abs value per bucket over the whole file
        double seconds = 0.0;
        bool muted = false;
        bool solo = false;
        QColor colour;
        int depth = 0;             // how many groups the track sits in, for the indent
        bool folder = false;       // a group or effect bus: no clip, can fold its tracks away
        bool folded = false;
        bool hidden = false;       // inside a folded group
    };

    struct Marker {
        double seconds = 0.0;
        QString name;
    };

    explicit ArrangeView(QWidget* parent = nullptr);

    void setLanes(const std::vector<Lane>& lanes);
    void setMarkers(const std::vector<Marker>& markers);
    // the range dragged out on the ruler, which clip edits work on; end <= start: none
    void setRange(double start, double end);
    bool hasRange() const { return fRangeEnd > fRangeStart; }
    double rangeStart() const { return fRangeStart; }
    double rangeEnd() const { return fRangeEnd; }
    void setLaneState(int lane, bool muted, bool solo);
    void setSelected(int lane);
    void setPlayhead(double seconds, bool followPlayback);
    void setTempo(double bpm);

signals:
    void seekRequested(double seconds);
    void trackSelected(int lane);
    void clipDoubleClicked(int lane);
    void muteToggled(int lane, bool muted);
    void soloToggled(int lane, bool solo);
    void foldToggled(int lane, bool folded);
    void trackMenuRequested(int lane, const QPoint& globalPos); // lane -1: the empty area below the tracks
    void filesDropped(const QStringList& paths, int lane);       // lane -1: make new tracks
    void rangeChanged(double start, double end);
    void clipMoveRequested(int lane, double seconds);            // a clip dragged later (> 0) or earlier
    void markerMoved(int marker, double seconds);
    void markerRenameRequested(int marker);
    void markerMenuRequested(int marker, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    static constexpr int kHeaderWidth = 230;
    static constexpr int kMarkerBand = 15; // the markers, above the bars
    static constexpr int kRulerHeight = 30 + kMarkerBand;
    static constexpr int kLaneHeight = 60;

    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    int laneAt(int y) const;
    QRect laneRect(int lane) const;
    QRect muteRect(int lane) const;
    QRect soloRect(int lane) const;
    QRect foldRect(int lane) const;
    void updateScrollRange();
    void paintLane(QPainter& p, int lane);
    double snap(double seconds, Qt::KeyboardModifiers modifiers) const; // to the beat, unless Shift
    int markerAt(const QPoint& pos) const;
    QRect markerRect(int marker) const;

    QScrollBar* fScroll = nullptr;
    QScrollBar* fVScroll = nullptr;
    std::vector<Lane> fLanes;
    std::vector<int> fRows; // per lane, its row on screen; -1 when hidden in a folded group
    int fRowCount = 0;
    int fSelected = -1;
    double fLongest = 0.0;
    double fPlayhead = 0.0;
    double fBpm = 120.0;
    double fPixelsPerSecond = 60.0;
    std::vector<Marker> fMarkers;
    double fRangeStart = 0.0;
    double fRangeEnd = 0.0;

    enum class Drag { None, Range, Clip, Marker };
    Drag fDrag = Drag::None;
    bool fDragging = false; // moved far enough to be a drag, not a click
    QPoint fPressPos;
    double fPressSeconds = 0.0;
    int fDragLane = -1;
    int fDragMarker = -1;
    double fDragDelta = 0.0;
};

// A column of insert slots (Cubase "Inserts"), each with a power button and the plugin name.
class InsertRack : public QWidget {
    Q_OBJECT

public:
    struct Slot {
        QString name;
        bool active = true;
    };

    explicit InsertRack(int slotCount, QWidget* parent = nullptr, int rowHeight = 20);
    void setSlots(const std::vector<Slot>& items);

signals:
    void addRequested();
    void editRequested(int index);
    void activeToggled(int index, bool active);
    void removeRequested(int index);
    void moveRequested(int index, int delta);

private:
    struct Row {
        QToolButton* power = nullptr;
        QPushButton* name = nullptr;
    };
    std::vector<Row> fRows;
    int fFilled = 0;
};

// One MixConsole channel: optional inserts, mute and solo, fader with meter, name.
class ChannelStrip : public QWidget {
    Q_OBJECT

public:
    ChannelStrip(const QString& name, bool isOutput, QWidget* parent = nullptr);

    struct SendSlot {
        QString bus;        // the bus's name
        float level = 0.0f; // carla volume
    };
    static constexpr int kSendSlots = 2;

    InsertRack* inserts() const { return fInserts; }
    void setSends(const std::vector<SendSlot>& sends);
    void setOutput(const QString& name); // where the fader goes: "Stereo Out" or a group
    void setLevels(float left, float right);
    void setVolume(float volume);  // carla volume, 0 .. 1.27
    void setPan(float pan);
    void setMuted(bool muted);
    void setSolo(bool solo);
    void setName(const QString& name);
    void setColour(const QColor& colour);
    void setSelected(bool selected);

signals:
    void volumeChanged(float volume);
    void panChanged(float pan);
    void muteToggled(bool muted);
    void soloToggled(bool solo);
    void selected();
    void sendMenuRequested(int slot, const QPoint& globalPos);
    void sendLevelChanged(int slot, float level);
    void outputMenuRequested(const QPoint& globalPos);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updateVolumeLabel(float volume);

    InsertRack* fInserts = nullptr;
    QToolButton* fMute = nullptr;
    QToolButton* fSolo = nullptr;
    PanControl* fPan = nullptr;
    QSlider* fFader = nullptr;
    LevelMeter* fMeter = nullptr;
    QLabel* fVolumeLabel = nullptr;
    QToolButton* fName = nullptr;
    QToolButton* fOutput = nullptr;
    struct SendRow {
        QToolButton* name = nullptr;
        QSlider* level = nullptr;
    };
    std::vector<SendRow> fSendRows;
    int fSendCount = 0;
};
