// Custom widgets for the Cubase-style window: level meter, arrange view, insert slots, channel strip.

#pragma once

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

// Project window: track header column, bar ruler and one audio track lane with its waveform.
class ArrangeView : public QWidget {
    Q_OBJECT

public:
    explicit ArrangeView(QWidget* parent = nullptr);

    void setTrack(const QString& trackName, const QString& clipName, const std::vector<float>& peaks, double seconds);
    void clearTrack();
    void setPlayhead(double seconds, bool followPlayback);
    void setMuted(bool muted);
    void setTempo(double bpm);

signals:
    void seekRequested(double seconds);
    void muteToggled(bool muted);
    void fileDropped(const QString& path);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    static constexpr int kHeaderWidth = 210;
    static constexpr int kRulerHeight = 30;
    static constexpr int kLaneHeight = 96;

    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    QRect laneRect() const;
    QRect muteRect() const;
    void updateScrollRange();

    QScrollBar* fScroll = nullptr;
    bool fHasTrack = false;
    bool fMuted = false;
    QString fTrackName;
    QString fClipName;
    std::vector<float> fPeaks;
    double fSeconds = 0.0;
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

    explicit InsertRack(int slotCount, QWidget* parent = nullptr);
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

// One MixConsole channel: optional inserts, mute, fader with meter, name.
class ChannelStrip : public QWidget {
    Q_OBJECT

public:
    ChannelStrip(const QString& name, bool isOutput, QWidget* parent = nullptr);

    InsertRack* inserts() const { return fInserts; }
    void setLevels(float left, float right);
    void setVolume(float volume);  // carla volume, 0 .. 1.27
    void setMuted(bool muted);
    void setName(const QString& name);
    void setFaderEnabled(bool enabled);

signals:
    void volumeChanged(float volume);
    void muteToggled(bool muted);

private:
    void updateVolumeLabel(float volume);

    InsertRack* fInserts = nullptr;
    QToolButton* fMute = nullptr;
    QSlider* fFader = nullptr;
    LevelMeter* fMeter = nullptr;
    QLabel* fVolumeLabel = nullptr;
    QLabel* fName = nullptr;
};
