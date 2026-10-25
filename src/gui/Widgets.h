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

// Project window: track header column, bar ruler and one lane per audio track with its waveform.
class ArrangeView : public QWidget {
    Q_OBJECT

public:
    struct Lane {
        QString name;
        QString clipName;          // empty: the track has no audio yet
        std::vector<float> peaks;  // max abs value per bucket over the whole file
        double seconds = 0.0;
        bool muted = false;
        bool solo = false;
        QColor colour;
    };

    explicit ArrangeView(QWidget* parent = nullptr);

    void setLanes(const std::vector<Lane>& lanes);
    void setLaneState(int lane, bool muted, bool solo);
    void setSelected(int lane);
    void setPlayhead(double seconds, bool followPlayback);
    void setTempo(double bpm);

signals:
    void seekRequested(double seconds);
    void trackSelected(int lane);
    void muteToggled(int lane, bool muted);
    void soloToggled(int lane, bool solo);
    void trackMenuRequested(int lane, const QPoint& globalPos); // lane -1: the empty area below the tracks
    void filesDropped(const QStringList& paths, int lane);       // lane -1: make new tracks

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    static constexpr int kHeaderWidth = 230;
    static constexpr int kRulerHeight = 30;
    static constexpr int kLaneHeight = 60;

    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    int laneAt(int y) const;
    QRect laneRect(int lane) const;
    QRect muteRect(int lane) const;
    QRect soloRect(int lane) const;
    void updateScrollRange();
    void paintLane(QPainter& p, int lane);

    QScrollBar* fScroll = nullptr;
    QScrollBar* fVScroll = nullptr;
    std::vector<Lane> fLanes;
    int fSelected = -1;
    double fLongest = 0.0;
    double fPlayhead = 0.0;
    double fBpm = 120.0;
    double fPixelsPerSecond = 60.0;
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

    InsertRack* inserts() const { return fInserts; }
    void setLevels(float left, float right);
    void setVolume(float volume);  // carla volume, 0 .. 1.27
    void setMuted(bool muted);
    void setSolo(bool solo);
    void setName(const QString& name);
    void setColour(const QColor& colour);
    void setSelected(bool selected);

signals:
    void volumeChanged(float volume);
    void muteToggled(bool muted);
    void soloToggled(bool solo);
    void selected();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updateVolumeLabel(float volume);

    InsertRack* fInserts = nullptr;
    QToolButton* fMute = nullptr;
    QToolButton* fSolo = nullptr;
    QSlider* fFader = nullptr;
    LevelMeter* fMeter = nullptr;
    QLabel* fVolumeLabel = nullptr;
    QToolButton* fName = nullptr;
};
