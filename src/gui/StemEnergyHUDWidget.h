#pragma once

#include <QWidget>
#include <QString>
#include <QTimer>
#include <QRectF>
#include <vector>

class StemEnergyHUDWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StemEnergyHUDWidget(QWidget* parent = nullptr);
    ~StemEnergyHUDWidget() override = default;

    // Stem levels: 0.0 to 1.0+ (linear peak)
    void setStemLevels(float vocal, float guitar, float drums, float bass, float keys);
    void setMasterLevel(float left, float right);
    void setPlaybackInfo(double seconds, double bpm, const QString& key, const QString& chord, bool isPlaying);
    void setCurrentStep(int step); // 0 to 4

signals:
    void stepClicked(int step);
    void playToggled();
    void autoRemasterRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void updateAnimation();

    // 5 Stem data
    struct StemInfo {
        QString name;
        QString tag;
        QColor color;
        float currentLevel = 0.0f;
        float targetLevel = 0.0f;
        float peakHold = 0.0f;
        int peakHoldTimer = 0;
    };
    std::vector<StemInfo> fStems;

    float fMasterL = 0.0f;
    float fMasterR = 0.0f;
    double fSeconds = 0.0;
    double fBpm = 120.0;
    QString fKey{QStringLiteral("E Major")};
    QString fChord{QStringLiteral("B")};
    bool fIsPlaying = false;
    int fCurrentStep = 4; // 0 to 4

    float fAnimPhase = 0.0f;
    QTimer* fAnimTimer = nullptr;

    // Step button regions
    std::vector<QRectF> fStepRects;
};
