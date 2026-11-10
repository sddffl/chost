#pragma once

#include <QWidget>
#include <QString>
#include <QTimer>
#include <QRectF>

class TapeDeckWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TapeDeckWidget(QWidget* parent = nullptr);
    ~TapeDeckWidget() override = default;

    void setLevels(float leftPeak, float rightPeak);
    void setTime(double seconds);
    void setPlaying(bool playing);
    void setTrackName(const QString& name);

    bool isPlaying() const { return fIsPlaying; }

signals:
    void playRequested();
    void pauseRequested();
    void stopRequested();
    void rewindRequested();
    void fastForwardRequested();
    void recordRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void updateAnimation();
    float peakToAngle(float peak) const;

    float fLeftPeak = 0.0f;
    float fRightPeak = 0.0f;
    float fLeftAngle = -38.0f;
    float fRightAngle = -38.0f;
    float fLeftTargetAngle = -38.0f;
    float fRightTargetAngle = -38.0f;

    double fCurrentSeconds = 0.0;
    bool fIsPlaying = false;
    float fReelAngle = 0.0f;

    QString fTrackName{QStringLiteral("MASTER BUS")};

    // Virtual canvas geometry (340 x 380)
    QRectF fRewindBtn{42, 276, 42, 34};
    QRectF fRecordBtn{94, 276, 42, 34};
    QRectF fPlayBtn{146, 270, 48, 44};
    QRectF fStopBtn{204, 276, 42, 34};
    QRectF fFwdBtn{256, 276, 42, 34};

    int fHoveredBtn = -1;
    int fPressedBtn = -1;

    QTimer* fAnimTimer = nullptr;
};
