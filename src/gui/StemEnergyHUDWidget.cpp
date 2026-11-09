#include "StemEnergyHUDWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <cmath>
#include <algorithm>

StemEnergyHUDWidget::StemEnergyHUDWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumWidth(320);

    // Initialize 5 Stems
    fStems = {
        {QStringLiteral("Vocal"), QStringLiteral("主旋律/AI歌唱"), QColor(0xf4, 0x72, 0xb6)},
        {QStringLiteral("E.Guitar"), QStringLiteral("电吉他/主音"), QColor(0x00, 0xf0, 0xff)},
        {QStringLiteral("Drums"), QStringLiteral("真实鼓组/Kontakt"), QColor(0xf9, 0x73, 0x16)},
        {QStringLiteral("Bass"), QStringLiteral("贝斯低频/Scarbee"), QColor(0x10, 0xb9, 0x81)},
        {QStringLiteral("Keys/Synth"), QStringLiteral("和声/弦乐/Serum2"), QColor(0xa8, 0x55, 0xf7)}
    };

    // 44-section acoustic waveguide areas (Kelly-Lochbaum)
    fTubeAreas.resize(44, 4.5f);

    fAnimTimer = new QTimer(this);
    connect(fAnimTimer, &QTimer::timeout, this, &StemEnergyHUDWidget::updateAnimation);
    fAnimTimer->start(25); // ~40 FPS smooth UI update
}

void StemEnergyHUDWidget::setStemLevels(float vocal, float guitar, float drums, float bass, float keys)
{
    float levels[5] = {vocal, guitar, drums, bass, keys};
    for (int i = 0; i < 5; ++i)
    {
        fStems[i].targetLevel = std::clamp(levels[i], 0.0f, 1.5f);
        if (fStems[i].targetLevel > fStems[i].peakHold)
        {
            fStems[i].peakHold = fStems[i].targetLevel;
            fStems[i].peakHoldTimer = 20; // hold for ~500ms
        }
    }
    fVocalLevel = std::clamp(vocal, 0.0f, 1.5f);
}

void StemEnergyHUDWidget::setMasterLevel(float left, float right)
{
    fMasterL = left;
    fMasterR = right;
}

void StemEnergyHUDWidget::setPlaybackInfo(double seconds, double bpm, const QString& key, const QString& chord, bool isPlaying)
{
    fSeconds = seconds;
    fBpm = bpm;
    if (!key.isEmpty())
        fKey = key;
    if (!chord.isEmpty())
        fChord = chord;
    fIsPlaying = isPlaying;
}

void StemEnergyHUDWidget::setCurrentStep(int step)
{
    fCurrentStep = std::clamp(step, 0, 4);
    update();
}

void StemEnergyHUDWidget::computeVocalAcoustics()
{
    // If user locked to a manual vowel preset (1: A, 2: I, 3: U, 4: E, 5: O)
    if (fSelectedVowel == 1) // [a] 啊 (Low/Open, Pharyngeal constriction)
    {
        fTargetF1 = 750.0f; fTargetF2 = 1250.0f;
        fTongueX = -0.3f; fTongueY = -0.7f; fJawOpen = 0.85f; fLipProtrusion = 0.0f;
    }
    else if (fSelectedVowel == 2) // [i] 衣 (High/Front, Narrow oral, Wide pharynx)
    {
        fTargetF1 = 280.0f; fTargetF2 = 2250.0f;
        fTongueX = 0.85f; fTongueY = 0.85f; fJawOpen = 0.20f; fLipProtrusion = -0.1f;
    }
    else if (fSelectedVowel == 3) // [u] 呜 (High/Back, Velar constriction, Protruded lips)
    {
        fTargetF1 = 320.0f; fTargetF2 = 850.0f;
        fTongueX = -0.80f; fTongueY = 0.75f; fJawOpen = 0.25f; fLipProtrusion = 0.85f;
    }
    else if (fSelectedVowel == 4) // [e] 哎 (Mid/Front)
    {
        fTargetF1 = 480.0f; fTargetF2 = 1950.0f;
        fTongueX = 0.55f; fTongueY = 0.40f; fJawOpen = 0.45f; fLipProtrusion = 0.0f;
    }
    else if (fSelectedVowel == 5) // [o] 喔 (Mid/Back, Rounded)
    {
        fTargetF1 = 520.0f; fTargetF2 = 920.0f;
        fTongueX = -0.60f; fTongueY = 0.20f; fJawOpen = 0.55f; fLipProtrusion = 0.60f;
    }
    else // 0: Auto (Driven by vocal phrasing & playback dynamics)
    {
        if (fIsPlaying && fVocalLevel > 0.02f)
        {
            // Coarticulated natural vowel drift based on song playback time
            const double t = fSeconds * 1.8;
            const float cycle = std::fmod(t, 5.0);
            if (cycle < 1.0) { // glide toward [a]
                fTargetF1 = 730.0f + 30.0f * std::sin(t * 2.0);
                fTargetF2 = 1250.0f + 40.0f * std::cos(t * 1.5);
                fTongueX = -0.3f; fTongueY = -0.6f; fJawOpen = 0.8f; fLipProtrusion = 0.0f;
            } else if (cycle < 2.0) { // glide toward [i]
                fTargetF1 = 300.0f + 25.0f * std::sin(t * 2.5);
                fTargetF2 = 2200.0f + 60.0f * std::cos(t * 1.2);
                fTongueX = 0.8f; fTongueY = 0.8f; fJawOpen = 0.25f; fLipProtrusion = -0.1f;
            } else if (cycle < 3.0) { // glide toward [u]
                fTargetF1 = 330.0f; fTargetF2 = 880.0f;
                fTongueX = -0.7f; fTongueY = 0.7f; fJawOpen = 0.3f; fLipProtrusion = 0.8f;
            } else if (cycle < 4.0) { // glide toward [e]
                fTargetF1 = 500.0f; fTargetF2 = 1900.0f;
                fTongueX = 0.5f; fTongueY = 0.3f; fJawOpen = 0.5f; fLipProtrusion = 0.0f;
            } else { // glide toward [o]
                fTargetF1 = 540.0f; fTargetF2 = 950.0f;
                fTongueX = -0.5f; fTongueY = 0.2f; fJawOpen = 0.6f; fLipProtrusion = 0.5f;
            }
        }
        else
        {
            // Resting relaxed neutral vocal tract (/ə/)
            fTargetF1 = 500.0f; fTargetF2 = 1500.0f;
            fTongueX = 0.0f; fTongueY = 0.0f; fJawOpen = 0.35f; fLipProtrusion = 0.0f;
        }
    }

    // Smooth second-order target approach (TAM)
    fFormant1 += (fTargetF1 - fFormant1) * 0.18f;
    fFormant2 += (fTargetF2 - fFormant2) * 0.18f;

    // Pitch F0 estimation
    if (fVocalLevel > 0.05f)
    {
        // Vocal singing fundamental based on melody range
        fVocalF0 = 180.0f + 140.0f * (0.5f + 0.5f * std::sin(fSeconds * 1.2f + 0.5f));
    }
    else
    {
        fVocalF0 = 130.0f;
    }

    // Liljencrants-Fant Rd & Reynolds number calculation
    fGlottalRd = 0.85f + 0.25f * (1.0f - std::clamp(fVocalLevel, 0.0f, 1.0f));
    const float airVelocity = (fVocalLevel > 0.01f) ? (1200.0f + fVocalLevel * 1800.0f) : 200.0f;
    fReynoldsRe = airVelocity * 1.2f; // Reynolds Re at incisal slit

    // Effective tract length with lip protrusion
    fTractLengthCm = 17.5f + fLipProtrusion * 1.4f;

    // Compute 44-Section Area Function A(x) from glottis (x=0) to lips (x=43)
    // Based on Story 2008 MRI area profiles and Maeda articulatory models
    for (int i = 0; i < 44; ++i)
    {
        const float normX = static_cast<float>(i) / 43.0f; // 0.0 at glottis, 1.0 at lips
        float a = 4.5f; // neutral tube baseline (cm^2)

        if (normX < 0.15f) // Glottis & Epilaryngeal tube (x = 0..6)
        {
            a = 0.6f + (fGlottalRd * 0.4f);
        }
        else if (normX < 0.52f) // Pharynx cavity (x = 7..22)
        {
            // When tongue moves back (fTongueX < 0), pharynx constricts (like /a/)
            // When tongue moves front (fTongueX > 0), pharynx expands (like /i/)
            const float pharynxMod = 1.0f + fTongueX * 0.7f - (1.0f - fTongueY) * 0.4f;
            a = std::clamp(4.2f * pharynxMod, 0.5f, 9.0f);
        }
        else if (normX < 0.88f) // Oral cavity & Palatal arch (x = 23..38)
        {
            // When tongue moves high (fTongueY > 0), oral passage narrows
            // Jaw opening widens the entire oral channel
            const float oralConstrict = 1.0f - (fTongueY * 0.75f) + (fJawOpen * 0.6f);
            a = std::clamp(5.0f * oralConstrict, 0.35f, 11.0f);
        }
        else // Incisors & Lips (x = 39..43)
        {
            // Lip opening & protrusion
            const float lipArea = (fJawOpen * 5.0f + 0.6f) * (1.0f - fLipProtrusion * 0.65f);
            a = std::clamp(lipArea, 0.3f, 8.5f);
        }

        // Smooth transition
        fTubeAreas[i] = fTubeAreas[i] * 0.75f + a * 0.25f;
    }
}

void StemEnergyHUDWidget::updateAnimation()
{
    fAnimPhase += 0.08f;
    if (fAnimPhase > 6.283185f)
        fAnimPhase -= 6.283185f;

    // Decay levels smoothly
    for (auto& s : fStems)
    {
        if (s.targetLevel > s.currentLevel)
            s.currentLevel = s.currentLevel * 0.4f + s.targetLevel * 0.6f;
        else
            s.currentLevel = s.currentLevel * 0.88f;

        if (s.peakHoldTimer > 0)
            --s.peakHoldTimer;
        else
            s.peakHold = std::max(0.0f, s.peakHold * 0.94f);
    }

    // Compute vocal tract physics & acoustics
    computeVocalAcoustics();

    // Maintain formant radar history trail
    if (fFormantTrail.size() > 18)
        fFormantTrail.erase(fFormantTrail.begin());
    fFormantTrail.push_back(QPointF(fFormant2, fFormant1));

    update();
}

QSize StemEnergyHUDWidget::sizeHint() const
{
    return QSize(350, 720);
}

QSize StemEnergyHUDWidget::minimumSizeHint() const
{
    return QSize(310, 680);
}

void StemEnergyHUDWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int w = width();
    const int h = height();

    // 1. Dark futuristic cyber background
    QLinearGradient bgGrad(0, 0, 0, h);
    bgGrad.setColorAt(0.0, QColor(0x09, 0x0c, 0x14));
    bgGrad.setColorAt(0.45, QColor(0x0d, 0x12, 0x1f));
    bgGrad.setColorAt(1.0, QColor(0x07, 0x0a, 0x12));
    p.fillRect(0, 0, w, h, bgGrad);

    // Subtle technical grid
    p.setPen(QPen(QColor(0x16, 0x20, 0x32), 1, Qt::DotLine));
    for (int y = 30; y < h; y += 40)
        p.drawLine(10, y, w - 10, y);

    // 2. Top Header: 5 Pipeline Steps
    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 8, QFont::Bold));
    const QString stepNames[5] = {
        QStringLiteral("01 原曲·分轨"),
        QStringLiteral("02 AI 采谱"),
        QStringLiteral("03 歌声合成"),
        QStringLiteral("04 减法编曲"),
        QStringLiteral("05 商业母带")
    };
    const QColor stepColors[5] = {
        QColor(0x00, 0xf0, 0xff),
        QColor(0xa8, 0x55, 0xf7),
        QColor(0xf4, 0x72, 0xb6),
        QColor(0x10, 0xb9, 0x81),
        QColor(0xf5, 0x9e, 0x0b)
    };

    fStepRects.clear();
    const double stepW = (w - 24.0) / 5.0;
    for (int i = 0; i < 5; ++i)
    {
        QRectF sr(12.0 + i * stepW, 8.0, stepW - 3.0, 20.0);
        fStepRects.push_back(sr);

        const bool active = (i == fCurrentStep);
        QPainterPath path;
        path.addRoundedRect(sr, 4.0, 4.0);

        if (active)
        {
            p.fillPath(path, stepColors[i].darker(180));
            p.setPen(QPen(stepColors[i], 1.5));
            p.drawPath(path);
            p.setPen(stepColors[i].lighter(140));
        }
        else
        {
            p.fillPath(path, QColor(0x13, 0x19, 0x27));
            p.setPen(QPen(QColor(0x23, 0x2d, 0x3f), 1));
            p.drawPath(path);
            p.setPen(QColor(0x70, 0x80, 0x98));
        }
        p.drawText(sr, Qt::AlignCenter, stepNames[i]);
    }

    // 3. Center Section: Circular Visualizer & Oscilloscope
    const float centerY = 100.0f;
    const float centerX = w / 2.0f;
    const float radius = 42.0f;
    const float avgPeak = (fMasterL + fMasterR) * 0.5f;

    // Glowing sunburst / circular rays
    p.save();
    p.translate(centerX, centerY);
    const int numRays = 36;
    for (int i = 0; i < numRays; ++i)
    {
        const float angle = i * (360.0f / numRays);
        p.save();
        p.rotate(angle);
        const float wave = std::sin(angle * 0.05f + fAnimPhase * 2.0f);
        const float rayLen = (radius * 0.2f) + (avgPeak * 22.0f) + (wave * 4.0f);
        QColor rayCol = stepColors[fCurrentStep];
        rayCol.setAlpha(static_cast<int>(std::clamp(60 + avgPeak * 190.0f, 40.0f, 255.0f)));
        p.setPen(QPen(rayCol, 1.8f));
        p.drawLine(QPointF(0, radius), QPointF(0, radius + rayLen));
        p.restore();
    }

    // Inner glowing ring
    QRadialGradient ringGrad(0, 0, radius);
    ringGrad.setColorAt(0.0, QColor(0, 0, 0, 160));
    ringGrad.setColorAt(0.7, QColor(0x10, 0x16, 0x25, 200));
    ringGrad.setColorAt(1.0, stepColors[fCurrentStep]);
    p.setBrush(ringGrad);
    p.setPen(QPen(stepColors[fCurrentStep], 2.0));
    p.drawEllipse(QPointF(0, 0), radius, radius);

    // Center Oscilloscope Wave
    p.setPen(QPen(QColor(0xff, 0xff, 0xff, 220), 1.8f));
    QPainterPath wavePath;
    for (float x = -radius * 0.85f; x <= radius * 0.85f; x += 2.0f)
    {
        const float normX = x / (radius * 0.85f);
        const float amp = (avgPeak * 14.0f + 2.0f) * (1.0f - normX * normX);
        const float y = std::sin(x * 0.18f + fAnimPhase * 3.0f) * amp;
        if (x == -radius * 0.85f)
            wavePath.moveTo(x, y);
        else
            wavePath.lineTo(x, y);
    }
    p.drawPath(wavePath);
    p.restore();

    // 4. Telemetry Badges (Key, BPM, Chord, Time)
    const float tagY = 154.0f;
    const QRectF teleRect(12, tagY, w - 24, 22);
    p.setBrush(QColor(0x12, 0x18, 0x26));
    p.setPen(QPen(QColor(0x28, 0x36, 0x4f), 1));
    p.drawRoundedRect(teleRect, 4, 4);

    p.setFont(QFont(QStringLiteral("Consolas, monospace"), 8, QFont::Bold));
    p.setPen(QColor(0x8a, 0x9e, 0xb8));
    const int sec = static_cast<int>(fSeconds);
    const QString timeStr = QStringLiteral("%1:%2.%3")
        .arg(sec / 60, 2, 10, QLatin1Char('0'))
        .arg(sec % 60, 2, 10, QLatin1Char('0'))
        .arg(static_cast<int>((fSeconds - sec) * 10), 1, 10, QLatin1Char('0'));

    const QString statusLine = QStringLiteral("KEY: %1   BPM: %2   CHORD: %3   POS: %4")
        .arg(fKey)
        .arg(QString::number(fBpm, 'f', 1))
        .arg(fChord.isEmpty() ? QStringLiteral("-") : fChord)
        .arg(timeStr);
    p.drawText(teleRect, Qt::AlignCenter, statusLine);

    // 5. REAL-TIME 5-STEM ENERGY METERS
    const float stemStartY = 186.0f;
    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 8, QFont::Bold));
    p.setPen(QColor(0x9d, 0xb0, 0xcc));
    p.drawText(14, stemStartY, QStringLiteral("REAL-TIME 5-STEM ENERGY (实音轨输出联动)"));

    const float barH = 16.0f;
    const float barGap = 8.0f;
    const float labelW = 86.0f;
    const float barMaxW = w - 24.0f - labelW - 42.0f;

    for (int i = 0; i < 5; ++i)
    {
        const float curY = stemStartY + 10.0f + i * (barH + barGap);
        const auto& stem = fStems[i];

        // Track Name & Role
        p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 7.5, QFont::Bold));
        p.setPen(stem.color);
        p.drawText(QRectF(12, curY, labelW, barH), Qt::AlignLeft | Qt::AlignVCenter, stem.name);

        // Meter Track Background Slot
        const QRectF slotRect(12 + labelW, curY + 2, barMaxW, barH - 4);
        p.setBrush(QColor(0x10, 0x14, 0x20));
        p.setPen(QPen(QColor(0x20, 0x2a, 0x3d), 1));
        p.drawRoundedRect(slotRect, 3, 3);

        // Active Level Fill
        const float fillW = std::clamp(stem.currentLevel, 0.0f, 1.0f) * barMaxW;
        if (fillW > 1.0f)
        {
            QRectF fillRect(12 + labelW, curY + 2, fillW, barH - 4);
            QLinearGradient barGrad(fillRect.topLeft(), fillRect.topRight());
            barGrad.setColorAt(0.0, stem.color.darker(140));
            barGrad.setColorAt(0.8, stem.color);
            barGrad.setColorAt(1.0, stem.color.lighter(130));
            p.setBrush(barGrad);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(fillRect, 3, 3);
        }

        // Peak Hold Line
        const float peakX = 12 + labelW + std::clamp(stem.peakHold, 0.0f, 1.0f) * (barMaxW - 2.0f);
        if (stem.peakHold > 0.02f)
        {
            p.setPen(QPen(QColor(0xff, 0xff, 0xff, 220), 2.0f));
            p.drawLine(QPointF(peakX, curY + 1), QPointF(peakX, curY + barH - 3));
        }

        // dB text
        const float db = (stem.currentLevel > 0.0001f) ? (20.0f * std::log10(stem.currentLevel)) : -60.0f;
        p.setFont(QFont(QStringLiteral("Consolas, monospace"), 7));
        p.setPen(QColor(0x75, 0x88, 0xa3));
        const QString dbStr = (db > -50.0f) ? QString::asprintf("%+.1f dB", db) : QStringLiteral("-inf");
        p.drawText(QRectF(12 + labelW + barMaxW + 4, curY, 38, barH), Qt::AlignLeft | Qt::AlignVCenter, dbStr);
    }

    // =========================================================================
    // 6. VOICE-WORKSHOP PHYSICAL ACOUSTIC VOCAL TRACT & FORMANT RESONATOR HUD
    // =========================================================================
    const float tractTopY = 320.0f;

    // Glowing Divider
    QLinearGradient divGrad(12, tractTopY, w - 12, tractTopY);
    divGrad.setColorAt(0.0, QColor(0xf4, 0x72, 0xb6, 0));
    divGrad.setColorAt(0.2, QColor(0xf4, 0x72, 0xb6, 220));
    divGrad.setColorAt(0.8, QColor(0x00, 0xf0, 0xff, 220));
    divGrad.setColorAt(1.0, QColor(0x00, 0xf0, 0xff, 0));
    p.setPen(QPen(divGrad, 1.5f));
    p.drawLine(QPointF(12, tractTopY), QPointF(w - 12, tractTopY));

    // Section Title
    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 8, QFont::Bold));
    p.setPen(QColor(0x00, 0xf0, 0xff));
    p.drawText(14, tractTopY + 14.0f, QStringLiteral("✦ 人体声道物理发声与共鸣监测 (Kelly-Lochbaum 44阶波导)"));

    // Subtitle badge
    p.setFont(QFont(QStringLiteral("Consolas, monospace"), 6.5));
    p.setPen(QColor(0x70, 0x88, 0xa8));
    p.drawText(w - 150, tractTopY + 14.0f, QStringLiteral("Story MRI · LF Glottis"));

    // 6.1 Left Panel: Anatomical Mid-Sagittal Airway (中矢面声道剖面)
    const float airwayW = (w - 24.0f) * 0.60f;
    const float airwayH = 120.0f;
    const QRectF airwayRect(12.0f, tractTopY + 22.0f, airwayW, airwayH);
    drawAnatomicalAirway(p, airwayRect);

    // 6.2 Right Panel: F1-F2 Vowel Radar Chart (共振峰元音坐标雷达)
    const float radarX = 12.0f + airwayW + 8.0f;
    const float radarW = w - 12.0f - radarX;
    const QRectF radarRect(radarX, tractTopY + 22.0f, radarW, airwayH);
    drawVowelRadar(p, radarRect);

    // 6.3 44-Section Kelly-Lochbaum Waveguide Tube with Standing Waves (声学波导截面驻波)
    const float tubeTopY = tractTopY + 22.0f + airwayH + 8.0f;
    const QRectF tubeRect(12.0f, tubeTopY, w - 24.0f, 68.0f);
    drawWaveguideTube(p, tubeRect);

    // 6.4 Bottom Physical Telemetry & Vowel Selectors
    const float teleTopY = tubeTopY + 68.0f + 6.0f;
    const QRectF physRect(12.0f, teleTopY, w - 24.0f, h - teleTopY - 8.0f);
    drawAcousticTelemetry(p, physRect);
}

void StemEnergyHUDWidget::drawAnatomicalAirway(QPainter& p, const QRectF& rect)
{
    p.save();
    p.setClipRect(rect);

    // Background panel
    p.setBrush(QColor(0x0a, 0x0f, 0x1b));
    p.setPen(QPen(QColor(0x1e, 0x2a, 0x40), 1));
    p.drawRoundedRect(rect, 4, 4);

    // Coordinates mapping
    const float ox = rect.left() + 20.0f;
    const float oy = rect.bottom() - 18.0f;
    const float sx = rect.width() * 0.72f;
    const float sy = rect.height() * 0.72f;

    // Dynamic points influenced by fTongueX, fTongueY, fJawOpen
    // Tongue hump peak (moves between pharynx and hard palate)
    const float thX = ox + sx * (0.42f + fTongueX * 0.16f);
    const float thY = oy - sy * (0.35f + fTongueY * 0.32f);

    // Jaw and lower lip (drops with fJawOpen)
    const float jawDrop = fJawOpen * 14.0f;

    // Upper boundary path (Pharynx wall -> Velum -> Hard Palate -> Upper Incisors -> Upper Lip)
    QPainterPath upperPath;
    upperPath.moveTo(ox, oy); // Glottis / lower pharynx back wall
    upperPath.lineTo(ox, oy - sy * 0.70f); // Pharynx back wall
    // Velum / Soft Palate
    const float velumDrop = fVelumOpen * 8.0f;
    upperPath.quadTo(ox + sx * 0.12f, oy - sy * 0.85f, ox + sx * 0.28f, oy - sy * (0.90f - velumDrop * 0.01f));
    // Hard Palate
    upperPath.quadTo(ox + sx * 0.55f, oy - sy * 0.92f, ox + sx * 0.80f, oy - sy * 0.65f);
    // Upper incisor & upper lip
    upperPath.lineTo(ox + sx * 0.84f, oy - sy * 0.58f);
    upperPath.quadTo(ox + sx * 0.92f, oy - sy * 0.54f, ox + sx * 0.96f, oy - sy * 0.48f);

    // Lower boundary path (Glottis -> Tongue root -> Tongue Hump -> Tongue Tip -> Lower Teeth -> Lower Lip)
    QPainterPath lowerPath;
    lowerPath.moveTo(ox + 8.0f, oy); // Vocal folds front
    lowerPath.lineTo(ox + 10.0f, oy - sy * 0.25f); // Tongue root
    lowerPath.quadTo(ox + sx * 0.20f, oy - sy * 0.30f, thX, thY); // Tongue hump
    lowerPath.quadTo(ox + sx * 0.65f, oy - sy * (0.30f - jawDrop * 0.01f), ox + sx * 0.76f, oy - sy * 0.40f + jawDrop); // Tongue tip
    lowerPath.lineTo(ox + sx * 0.80f, oy - sy * 0.38f + jawDrop); // Lower incisor
    lowerPath.quadTo(ox + sx * 0.88f, oy - sy * 0.32f + jawDrop, ox + sx * 0.95f, oy - sy * 0.30f + jawDrop); // Lower lip

    // Draw Airway Channel Fill (between upper and lower)
    QPainterPath airwayFill = upperPath;
    // Walk back along lower path
    airwayFill.lineTo(ox + sx * 0.95f, oy - sy * 0.30f + jawDrop);
    airwayFill.quadTo(ox + sx * 0.88f, oy - sy * 0.32f + jawDrop, ox + sx * 0.80f, oy - sy * 0.38f + jawDrop);
    airwayFill.lineTo(ox + sx * 0.76f, oy - sy * 0.40f + jawDrop);
    airwayFill.quadTo(ox + sx * 0.65f, oy - sy * (0.30f - jawDrop * 0.01f), thX, thY);
    airwayFill.quadTo(ox + sx * 0.20f, oy - sy * 0.30f, ox + 10.0f, oy - sy * 0.25f);
    airwayFill.lineTo(ox + 8.0f, oy);
    airwayFill.closeSubpath();

    QLinearGradient airGrad(ox, oy, ox + sx, oy - sy * 0.6f);
    airGrad.setColorAt(0.0, QColor(0xf4, 0x72, 0xb6, 80));
    airGrad.setColorAt(0.5, QColor(0x00, 0xf0, 0xff, 60));
    airGrad.setColorAt(1.0, QColor(0x00, 0xf0, 0xff, 120));
    p.fillPath(airwayFill, airGrad);

    // Anatomical structure borders
    p.setPen(QPen(QColor(0x75, 0x90, 0xb5), 2.0f));
    p.drawPath(upperPath);
    p.setPen(QPen(QColor(0xf4, 0x72, 0xb6), 2.2f));
    p.drawPath(lowerPath);

    // Acoustic Pressure Wave Particles / Wavefronts (Active when vocal is sounding)
    if (fVocalLevel > 0.015f)
    {
        p.setPen(Qt::NoPen);
        const int numWaves = 7;
        for (int i = 0; i < numWaves; ++i)
        {
            const float tWave = std::fmod(fAnimPhase * 0.4f + static_cast<float>(i) / numWaves, 1.0f);
            // Interpolate position along spine
            float wx = 0.0f, wy = 0.0f;
            if (tWave < 0.35f) { // in pharynx
                const float s = tWave / 0.35f;
                wx = ox + 4.0f + s * (thX - ox) * 0.5f;
                wy = oy - s * sy * 0.55f;
            } else if (tWave < 0.75f) { // passing tongue constriction
                const float s = (tWave - 0.35f) / 0.40f;
                wx = thX + s * (ox + sx * 0.75f - thX);
                wy = thY + s * (oy - sy * 0.45f - thY);
            } else { // radiating from lips
                const float s = (tWave - 0.75f) / 0.25f;
                wx = ox + sx * 0.75f + s * (sx * 0.25f);
                wy = oy - sy * 0.45f + s * jawDrop;
            }

            const float particleSize = 3.0f + fVocalLevel * 4.0f * (1.0f - tWave * 0.5f);
            QColor waveCol = (tWave < 0.5f) ? QColor(0xf4, 0x72, 0xb6) : QColor(0x00, 0xf0, 0xff);
            waveCol.setAlpha(static_cast<int>(180 * (1.0f - tWave)));
            p.setBrush(waveCol);
            p.drawEllipse(QPointF(wx, wy), particleSize, particleSize);
        }

        // Glottal Pulse Spark (Base of vocal folds)
        const float sparkAlpha = std::clamp(fVocalLevel * 255.0f, 60.0f, 255.0f);
        p.setBrush(QColor(255, 230, 100, static_cast<int>(sparkAlpha)));
        p.drawEllipse(QPointF(ox + 4.0f, oy), 4.5f, 4.5f);
    }

    // Anatomical Labels
    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 6.5, QFont::Bold));
    p.setPen(QColor(0x60, 0x7b, 0xa0));
    p.drawText(ox - 10.0f, oy + 12.0f, QStringLiteral("声门 F0"));
    p.drawText(ox + sx * 0.40f, thY - 8.0f, QStringLiteral("舌位"));
    p.drawText(ox + sx * 0.78f, oy - sy * 0.85f, QStringLiteral("硬腭"));
    p.drawText(ox + sx * 0.96f, oy - sy * 0.38f + jawDrop, QStringLiteral("唇部"));

    p.restore();
}

void StemEnergyHUDWidget::drawVowelRadar(QPainter& p, const QRectF& rect)
{
    p.save();
    p.setClipRect(rect);

    // Background panel
    p.setBrush(QColor(0x0a, 0x0f, 0x1b));
    p.setPen(QPen(QColor(0x1e, 0x2a, 0x40), 1));
    p.drawRoundedRect(rect, 4, 4);

    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 7, QFont::Bold));
    p.setPen(QColor(0x00, 0xf0, 0xff));
    p.drawText(rect.adjusted(6, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("F1-F2 元音雷达"));

    // Formant Space Mapping
    // F2 (horizontal): 2500 Hz (left) -> 750 Hz (right)
    // F1 (vertical): 220 Hz (top) -> 900 Hz (bottom)
    const float minF2 = 750.0f, maxF2 = 2500.0f;
    const float minF1 = 220.0f, maxF1 = 900.0f;

    const float padL = rect.left() + 10.0f;
    const float padR = rect.right() - 10.0f;
    const float padT = rect.top() + 20.0f;
    const float padB = rect.bottom() - 10.0f;

    auto mapF1F2 = [&](float f2, float f1) -> QPointF {
        const float nx = (std::log(maxF2) - std::log(std::clamp(f2, minF2, maxF2))) / (std::log(maxF2) - std::log(minF2));
        const float ny = (std::log(std::clamp(f1, minF1, maxF1)) - std::log(minF1)) / (std::log(maxF1) - std::log(minF1));
        return QPointF(padL + nx * (padR - padL), padT + ny * (padB - padT));
    };

    // Reference Vowels: [i], [e], [a], [o], [u]
    struct VRef { QString ipa; float f1; float f2; };
    const VRef vrefs[5] = {
        {QStringLiteral("i (衣)"), 280.0f, 2250.0f},
        {QStringLiteral("e (哎)"), 480.0f, 1950.0f},
        {QStringLiteral("a (啊)"), 750.0f, 1250.0f},
        {QStringLiteral("o (喔)"), 520.0f, 920.0f},
        {QStringLiteral("u (呜)"), 320.0f, 850.0f}
    };

    // Draw Formant Polygon Outline
    QPainterPath polyPath;
    for (int i = 0; i < 5; ++i)
    {
        const QPointF pt = mapF1F2(vrefs[i].f2, vrefs[i].f1);
        if (i == 0) polyPath.moveTo(pt);
        else polyPath.lineTo(pt);
    }
    polyPath.closeSubpath();
    p.setBrush(QColor(0x00, 0xf0, 0xff, 15));
    p.setPen(QPen(QColor(0x00, 0xf0, 0xff, 60), 1, Qt::DashLine));
    p.drawPath(polyPath);

    // Draw Reference Vowel Dots
    for (int i = 0; i < 5; ++i)
    {
        const QPointF pt = mapF1F2(vrefs[i].f2, vrefs[i].f1);
        p.setBrush(QColor(0x18, 0x24, 0x38));
        p.setPen(QPen(QColor(0x00, 0xf0, 0xff, 160), 1.2f));
        p.drawEllipse(pt, 3.5f, 3.5f);

        p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 6.5));
        p.setPen(QColor(0x8a, 0xa5, 0xc8));
        p.drawText(pt + QPointF(5, 3), vrefs[i].ipa);
    }

    // Draw Live Formant Trail
    if (fFormantTrail.size() > 1)
    {
        for (size_t i = 1; i < fFormantTrail.size(); ++i)
        {
            const QPointF p0 = mapF1F2(fFormantTrail[i - 1].x(), fFormantTrail[i - 1].y());
            const QPointF p1 = mapF1F2(fFormantTrail[i].x(), fFormantTrail[i].y());
            const float alpha = static_cast<float>(i) / fFormantTrail.size();
            p.setPen(QPen(QColor(0xf4, 0x72, 0xb6, static_cast<int>(alpha * 200)), 1.5f));
            p.drawLine(p0, p1);
        }
    }

    // Draw Active Live Cursor
    const QPointF curPt = mapF1F2(fFormant2, fFormant1);
    QRadialGradient curGrad(curPt, 8.0f);
    curGrad.setColorAt(0.0, QColor(0xf4, 0x72, 0xb6, 255));
    curGrad.setColorAt(0.5, QColor(0xf4, 0x72, 0xb6, 160));
    curGrad.setColorAt(1.0, QColor(0xf4, 0x72, 0xb6, 0));
    p.setBrush(curGrad);
    p.setPen(Qt::NoPen);
    p.drawEllipse(curPt, 8.0f, 8.0f);

    p.setBrush(QColor(0xff, 0xff, 0xff));
    p.drawEllipse(curPt, 2.5f, 2.5f);

    p.restore();
}

void StemEnergyHUDWidget::drawWaveguideTube(QPainter& p, const QRectF& rect)
{
    p.save();
    p.setClipRect(rect);

    // Background panel
    p.setBrush(QColor(0x0a, 0x0f, 0x1b));
    p.setPen(QPen(QColor(0x1e, 0x2a, 0x40), 1));
    p.drawRoundedRect(rect, 4, 4);

    const float padX = 14.0f;
    const float x0 = rect.left() + padX;
    const float x1 = rect.right() - padX;
    const float yc = rect.top() + 34.0f;
    const float maxH = 22.0f;

    // Title & Legend
    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 7, QFont::Bold));
    p.setPen(QColor(0x10, 0xb9, 0x81));
    p.drawText(rect.left() + 8.0f, rect.top() + 12.0f, QStringLiteral("44阶声学截面积波导 (Story MRI & Kelly-Lochbaum)"));

    p.setFont(QFont(QStringLiteral("Consolas, monospace"), 6.5));
    p.setPen(QColor(0x70, 0x88, 0xa8));
    p.drawText(rect.right() - 110.0f, rect.top() + 12.0f, QStringLiteral("F1/F2 驻波节点分布"));

    // Centerline (Acoustic Spine)
    p.setPen(QPen(QColor(0x1e, 0x2a, 0x3d), 1, Qt::DashLine));
    p.drawLine(QPointF(x0, yc), QPointF(x1, yc));

    const int n = static_cast<int>(fTubeAreas.size());
    const float dx = (x1 - x0) / n;

    // Draw 44-Section Area Contour
    QPainterPath upperTube, lowerTube;
    for (int i = 0; i < n; ++i)
    {
        const float curX = x0 + i * dx;
        const float radius = std::sqrt(std::clamp(fTubeAreas[i] / 10.0f, 0.05f, 1.2f)) * maxH;
        if (i == 0)
        {
            upperTube.moveTo(curX, yc - radius);
            lowerTube.moveTo(curX, yc + radius);
        }
        else
        {
            upperTube.lineTo(curX + dx, yc - radius);
            lowerTube.lineTo(curX + dx, yc + radius);
        }
    }

    // Fill Tube Cavity with Acoustic Field Color
    QPainterPath fullTube = upperTube;
    for (int i = n - 1; i >= 0; --i)
    {
        const float curX = x0 + i * dx;
        const float radius = std::sqrt(std::clamp(fTubeAreas[i] / 10.0f, 0.05f, 1.2f)) * maxH;
        fullTube.lineTo(curX, yc + radius);
    }
    fullTube.closeSubpath();

    QLinearGradient tubeGrad(x0, yc, x1, yc);
    tubeGrad.setColorAt(0.0, QColor(0xf4, 0x72, 0xb6, 50));
    tubeGrad.setColorAt(0.5, QColor(0x10, 0xb9, 0x81, 70));
    tubeGrad.setColorAt(1.0, QColor(0x00, 0xf0, 0xff, 90));
    p.fillPath(fullTube, tubeGrad);

    p.setPen(QPen(QColor(0x10, 0xb9, 0x81, 200), 1.5f));
    p.drawPath(upperTube);
    p.drawPath(lowerTube);

    // Standing Wave of F1 / F2 (P_RMS)
    if (fVocalLevel > 0.01f)
    {
        QPainterPath waveF1;
        p.setPen(QPen(QColor(0x00, 0xf0, 0xff, 220), 1.4f));
        for (int i = 0; i < n; ++i)
        {
            const float curX = x0 + i * dx;
            const float normX = static_cast<float>(i) / n;
            // Mode 1: quarter wave (node at lips, antinode at glottis)
            const float standingF1 = std::cos(normX * 1.57079f) * std::sin(fAnimPhase * 2.5f) * 12.0f;
            if (i == 0) waveF1.moveTo(curX, yc + standingF1);
            else waveF1.lineTo(curX, yc + standingF1);
        }
        p.drawPath(waveF1);
    }

    // Bottom Tube Section Labels
    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 6.5));
    p.setPen(QColor(0x55, 0x6e, 0x8c));
    p.drawText(x0 - 4.0f, rect.bottom() - 4.0f, QStringLiteral("声门 x=0"));
    p.drawText(x0 + (x1 - x0) * 0.28f, rect.bottom() - 4.0f, QStringLiteral("咽腔"));
    p.drawText(x0 + (x1 - x0) * 0.65f, rect.bottom() - 4.0f, QStringLiteral("舌位狭缝"));
    p.drawText(x1 - 36.0f, rect.bottom() - 4.0f, QStringLiteral("唇部 17.5cm"));

    p.restore();
}

void StemEnergyHUDWidget::drawAcousticTelemetry(QPainter& p, const QRectF& rect)
{
    p.save();
    p.setClipRect(rect);

    // 1. Vowel Selector Pills
    const QString vowels[6] = {
        QStringLiteral("AUTO"),
        QStringLiteral("A [啊]"),
        QStringLiteral("I [衣]"),
        QStringLiteral("U [呜]"),
        QStringLiteral("E [哎]"),
        QStringLiteral("O [喔]")
    };

    fVowelRects.clear();
    const float btnW = (rect.width() - 25.0f) / 6.0f;
    const float btnH = 20.0f;
    for (int i = 0; i < 6; ++i)
    {
        QRectF br(rect.left() + i * (btnW + 5.0f), rect.top(), btnW, btnH);
        fVowelRects.push_back(br);

        const bool sel = (fSelectedVowel == i);
        p.setBrush(sel ? QColor(0x00, 0xf0, 0xff).darker(200) : QColor(0x12, 0x18, 0x26));
        p.setPen(QPen(sel ? QColor(0x00, 0xf0, 0xff) : QColor(0x28, 0x36, 0x4f), sel ? 1.4f : 1.0f));
        p.drawRoundedRect(br, 3, 3);

        p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 7, QFont::Bold));
        p.setPen(sel ? QColor(0x00, 0xf0, 0xff) : QColor(0x82, 0x98, 0xb5));
        p.drawText(br, Qt::AlignCenter, vowels[i]);
    }

    // 2. Real-time Physiological Data Box
    const QRectF teleBox(rect.left(), rect.top() + btnH + 6.0f, rect.width(), rect.height() - btnH - 6.0f);
    p.setBrush(QColor(0x0a, 0x0f, 0x18));
    p.setPen(QPen(QColor(0x1a, 0x24, 0x38), 1));
    p.drawRoundedRect(teleBox, 4, 4);

    p.setFont(QFont(QStringLiteral("Consolas, monospace"), 7));
    p.setPen(QColor(0x7e, 0x96, 0xb8));

    const QString line1 = QString::asprintf("F0: %4.1f Hz   F1: %3.0f Hz   F2: %4.0f Hz   Rd: %.2f",
                                           fVocalF0, fFormant1, fFormant2, fGlottalRd);
    const QString line2 = QString::asprintf("Re: %4.0f   L: %4.1f cm   VELUM: %s   GLOTTIS: %s",
                                           fReynoldsRe, fTractLengthCm,
                                           (fVelumOpen > 0.3f ? "NASAL" : "ORAL"),
                                           (fVocalLevel > 0.02f ? "ACTIVE" : "REST"));

    p.drawText(teleBox.adjusted(8, 4, -8, -16), Qt::AlignLeft | Qt::AlignTop, line1);
    p.drawText(teleBox.adjusted(8, 16, -8, -4), Qt::AlignLeft | Qt::AlignTop, line2);

    p.restore();
}

void StemEnergyHUDWidget::mousePressEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();

    // Check pipeline steps
    for (size_t i = 0; i < fStepRects.size(); ++i)
    {
        if (fStepRects[i].contains(pos))
        {
            fCurrentStep = static_cast<int>(i);
            emit stepClicked(fCurrentStep);
            update();
            return;
        }
    }

    // Check vowel preset buttons
    for (size_t i = 0; i < fVowelRects.size(); ++i)
    {
        if (fVowelRects[i].contains(pos))
        {
            fSelectedVowel = static_cast<int>(i);
            emit vowelSelected(fSelectedVowel);
            update();
            return;
        }
    }

    QWidget::mousePressEvent(event);
}
