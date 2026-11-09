#pragma once

#include <QWidget>
#include <QString>
#include <QTimer>
#include <QRectF>
#include <QPointF>
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
    void vowelSelected(int vowelIndex);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void updateAnimation();
    void computeVocalAcoustics();
    void drawAnatomicalAirway(QPainter& p, const QRectF& rect);
    void drawVowelRadar(QPainter& p, const QRectF& rect);
    void drawWaveguideTube(QPainter& p, const QRectF& rect);
    void drawAcousticTelemetry(QPainter& p, const QRectF& rect);

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

    // --- Voice Workshop Genius Physical Acoustic Modeling & Visualization ---
    int fSelectedVowel = 0; // 0: Auto (follows playback), 1: [A], 2: [I], 3: [U], 4: [E], 5: [O]
    float fVocalLevel = 0.0f;
    float fVocalF0 = 220.0f;       // Fundamental frequency (Hz)
    float fFormant1 = 700.0f;      // F1 (Hz) - Jaw/Tongue height
    float fFormant2 = 1250.0f;     // F2 (Hz) - Tongue back/front
    float fTargetF1 = 700.0f;
    float fTargetF2 = 1250.0f;
    float fTongueX = 0.0f;         // -1.0 (Back) to +1.0 (Front)
    float fTongueY = 0.0f;         // -1.0 (Low/Open) to +1.0 (High/Constricted)
    float fJawOpen = 0.5f;         // 0.0 (Closed) to 1.0 (Open)
    float fLipProtrusion = 0.0f;   // 0.0 to 1.0 (Rounding / length)
    float fVelumOpen = 0.0f;       // 0.0 (Oral) to 1.0 (Nasal)
    float fGlottalRd = 0.95f;      // Liljencrants-Fant Rd shape (0.3 pressed ~ 2.5 breathy)
    float fReynoldsRe = 1950.0f;   // Airflow Reynolds number
    float fTractLengthCm = 17.5f;  // Effective acoustic tube length (cm)

    // 44-Section Kelly-Lochbaum waveguide area function
    std::vector<float> fTubeAreas;

    // Formant radar trajectory trail
    std::vector<QPointF> fFormantTrail;

    // Interactive vowel buttons
    std::vector<QRectF> fVowelRects;
};
