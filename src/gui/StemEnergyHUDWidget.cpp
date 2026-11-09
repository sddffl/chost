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

    update();
}

QSize StemEnergyHUDWidget::sizeHint() const
{
    return QSize(350, 420);
}

QSize StemEnergyHUDWidget::minimumSizeHint() const
{
    return QSize(300, 360);
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
    bgGrad.setColorAt(0.5, QColor(0x0e, 0x13, 0x20));
    bgGrad.setColorAt(1.0, QColor(0x08, 0x0a, 0x10));
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
    const float centerY = 115.0f;
    const float centerX = w / 2.0f;
    const float radius = 48.0f;
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
        const float rayLen = (radius * 0.2f) + (avgPeak * 25.0f) + (wave * 4.0f);
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
        const float amp = (avgPeak * 16.0f + 2.0f) * (1.0f - normX * normX);
        const float y = std::sin(x * 0.18f + fAnimPhase * 3.0f) * amp;
        if (x == -radius * 0.85f)
            wavePath.moveTo(x, y);
        else
            wavePath.lineTo(x, y);
    }
    p.drawPath(wavePath);
    p.restore();

    // 4. Telemetry Badges (Key, BPM, Chord, Time)
    const float tagY = 178.0f;
    const QRectF teleRect(12, tagY, w - 24, 26);
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
    const float stemStartY = 216.0f;
    p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 8, QFont::Bold));
    p.setPen(QColor(0x9d, 0xb0, 0xcc));
    p.drawText(14, stemStartY, QStringLiteral("REAL-TIME 5-STEM ENERGY (实音轨输出联动)"));

    const float barH = 18.0f;
    const float barGap = 12.0f;
    const float labelW = 90.0f;
    const float barMaxW = w - 24.0f - labelW - 45.0f;

    for (int i = 0; i < 5; ++i)
    {
        const float curY = stemStartY + 14.0f + i * (barH + barGap);
        const auto& stem = fStems[i];

        // Track Name & Role
        p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 8, QFont::Bold));
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
        p.drawText(QRectF(12 + labelW + barMaxW + 6, curY, 40, barH), Qt::AlignLeft | Qt::AlignVCenter, dbStr);
    }
}

void StemEnergyHUDWidget::mousePressEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
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
    QWidget::mousePressEvent(event);
}
