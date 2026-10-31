#pragma once

#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

class QLabel;
class QPushButton;
class QComboBox;
class QSlider;

namespace chost {
struct ClipEdit;
}

class SampleEditor : public QWidget {
    Q_OBJECT

public:
    explicit SampleEditor(QWidget* parent = nullptr);

    void setTrack(int trackIndex, const QString& trackName, const QString& filePath, const QColor& color);
    void setTempo(double bpm);
    void setPlayhead(double seconds);
    void clearTrack();

    int currentTrack() const { return fTrackIndex; }
    const QString& currentFile() const { return fFilePath; }
    double duration() const { return fDuration; }

signals:
    void seekRequested(double seconds);
    void clipEditRequested(int trackIndex, const chost::ClipEdit& edit, const QString& description);
    void rangeChanged(double startSeconds, double endSeconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void buildToolbar();
    void updatePeaks();
    int timeToX(double seconds, int waveLeft) const;
    double xToTime(int x, int waveLeft) const;
    double snapTime(double seconds) const;

    void drawPianoKeys(QPainter& p, const QRect& keysRect);
    void drawRuler(QPainter& p, const QRect& rulerRect);
    void drawWaveform(QPainter& p, const QRect& waveRect);

    enum class Tool { Select, Scissors, Mute, Normalize };
    Tool fTool = Tool::Select;

    int fTrackIndex = -1;
    QString fTrackName;
    QString fFilePath;
    QColor fColor;
    double fTempo = 120.0;
    double fPlayhead = 0.0;
    double fDuration = 0.0;

    double fViewStart = 0.0; // in seconds
    double fPixelsPerSecond = 120.0;
    float fAmplitudeZoom = 1.0f;

    double fSelectionStart = 0.0;
    double fSelectionEnd = 0.0;
    bool fSelecting = false;
    bool fScrubbing = false;

    std::vector<float> fDensePeaks;
    uint32_t fSampleRate = 44100;
    uint32_t fChannels = 2;

    QWidget* fToolbar = nullptr;
    QLabel* fInfoLabel = nullptr;
    QComboBox* fSnapCombo = nullptr;
    QSlider* fAmpSlider = nullptr;
    QPushButton* fSelectBtn = nullptr;
    QPushButton* fScissorsBtn = nullptr;
    QPushButton* fMuteBtn = nullptr;
    QPushButton* fNormalizeBtn = nullptr;
    QPushButton* fFadeInBtn = nullptr;
    QPushButton* fFadeOutBtn = nullptr;
};
