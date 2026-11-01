#include "TapeDeckWidget.h"

#include <QPainter>
#include <QGradient>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFontDatabase>
#include <cmath>
#include <algorithm>

TapeDeckWidget::TapeDeckWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("tapeDeckWidget"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setMouseTracking(true);

    fAnimTimer = new QTimer(this);
    connect(fAnimTimer, &QTimer::timeout, this, &TapeDeckWidget::updateAnimation);
    fAnimTimer->start(25); // 40 fps smooth needle and reel animation
}

QSize TapeDeckWidget::sizeHint() const
{
    return QSize(340, 380);
}

QSize TapeDeckWidget::minimumSizeHint() const
{
    return QSize(240, 270);
}

float TapeDeckWidget::peakToAngle(float peak) const
{
    if (peak <= 0.0001f)
        return -38.0f;

    const float db = 20.0f * std::log10(peak);
    if (db <= -24.0f)
        return -38.0f;
    if (db <= -10.0f)
        return -38.0f + (db + 24.0f) / 14.0f * 18.0f; // -38° to -20°
    if (db <= 0.0f)
        return -20.0f + (db + 10.0f) / 10.0f * 24.0f; // -20° to +4°
    return +4.0f + std::clamp(db / 6.0f, 0.0f, 1.0f) * 32.0f; // +4° to +36°
}

void TapeDeckWidget::setLevels(float leftPeak, float rightPeak)
{
    fLeftPeak = leftPeak;
    fRightPeak = rightPeak;
    fLeftTargetAngle = peakToAngle(leftPeak);
    fRightTargetAngle = peakToAngle(rightPeak);
}

void TapeDeckWidget::setTime(double seconds)
{
    fCurrentSeconds = std::max(0.0, seconds);
}

void TapeDeckWidget::setPlaying(bool playing)
{
    fIsPlaying = playing;
}

void TapeDeckWidget::setTrackName(const QString& name)
{
    fTrackName = name.isEmpty() ? QStringLiteral("MASTER BUS") : name.toUpper();
}

void TapeDeckWidget::updateAnimation()
{
    // Needle ballistics: fast attack, smooth analog decay
    auto updateBallistic = [](float& current, float target) {
        if (target > current)
            current += (target - current) * 0.35f; // Fast attack ~30ms
        else
            current += (target - current) * 0.10f; // Inertia decay ~250ms
    };

    updateBallistic(fLeftAngle, fLeftTargetAngle);
    updateBallistic(fRightAngle, fRightTargetAngle);

    // Continuous reel rotation when playing
    if (fIsPlaying)
    {
        fReelAngle += 3.8f;
        if (fReelAngle >= 360.0f)
            fReelAngle -= 360.0f;
    }

    update();
}

void TapeDeckWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const double sx = static_cast<double>(width()) / 340.0;
    const double sy = static_cast<double>(height()) / 380.0;
    const double scale = std::min(sx, sy);
    const double tx = (width() - 340.0 * scale) / 2.0;
    const double ty = (height() - 380.0 * scale) / 2.0;

    // Background surrounding letterboxing
    p.fillRect(rect(), QColor(0x10, 0x12, 0x15));

    p.translate(tx, ty);
    p.scale(scale, scale);

    // =========================================================================
    // 1. Brushed Aluminum Chassis (Outer Shell)
    // =========================================================================
    const QRectF chassis(4, 4, 332, 372);
    QLinearGradient alum(0, 0, 0, 380);
    alum.setColorAt(0.0, QColor(0xf2, 0xf4, 0xf7));
    alum.setColorAt(0.04, QColor(0xd8, 0xdc, 0xe2));
    alum.setColorAt(0.5, QColor(0xb4, 0xbc, 0xc6));
    alum.setColorAt(0.96, QColor(0xa2, 0xaa, 0xb4));
    alum.setColorAt(1.0, QColor(0x8e, 0x96, 0xa0));

    p.setPen(QPen(QColor(0x40, 0x46, 0x50), 1.5));
    p.setBrush(alum);
    p.drawRoundedRect(chassis, 10, 10);

    // Outer subtle metallic highlight bevel
    p.setPen(QPen(QColor(0xff, 0xff, 0xff, 140), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(chassis.adjusted(1, 1, -1, -1), 9, 9);

    // 4 Corner Hardware Screws
    auto drawScrew = [&](double cx, double cy) {
        p.setPen(QPen(QColor(0x48, 0x4e, 0x56), 1.0));
        QRadialGradient sgrad(cx - 1, cy - 1, 4.5);
        sgrad.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
        sgrad.setColorAt(0.7, QColor(0xa8, 0xb0, 0xb8));
        sgrad.setColorAt(1.0, QColor(0x50, 0x58, 0x62));
        p.setBrush(sgrad);
        p.drawEllipse(QPointF(cx, cy), 3.5, 3.5);
        // Screw slot
        p.setPen(QPen(QColor(0x38, 0x3d, 0x44), 1.0));
        p.drawLine(QPointF(cx - 2.2, cy - 1.2), QPointF(cx + 2.2, cy + 1.2));
    };
    drawScrew(14, 14);
    drawScrew(326, 14);
    drawScrew(14, 366);
    drawScrew(326, 366);

    // =========================================================================
    // 2. Top Dark Recessed Bay (Reels + LED + VU Meters)
    // =========================================================================
    const QRectF topBay(16, 16, 308, 196);
    QLinearGradient bayGrad(0, topBay.top(), 0, topBay.bottom());
    bayGrad.setColorAt(0.0, QColor(0x15, 0x17, 0x1b));
    bayGrad.setColorAt(0.5, QColor(0x1a, 0x1d, 0x22));
    bayGrad.setColorAt(1.0, QColor(0x10, 0x12, 0x15));

    p.setPen(QPen(QColor(0x28, 0x2e, 0x38), 1.5));
    p.setBrush(bayGrad);
    p.drawRoundedRect(topBay, 8, 8);

    // Subtle inner shadow
    p.setPen(QPen(QColor(0x05, 0x06, 0x08, 180), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(topBay.adjusted(1, 1, -1, -1), 7, 7);

    // -------------------------------------------------------------------------
    // 2A. Dual Tape Reels (Left & Right)
    // -------------------------------------------------------------------------
    auto drawReel = [&](double cx, double cy, float angle) {
        // Outer reel rim
        QRadialGradient rimGrad(cx, cy, 38);
        rimGrad.setColorAt(0.0, QColor(0x25, 0x28, 0x2f));
        rimGrad.setColorAt(0.7, QColor(0x1c, 0x1f, 0x24));
        rimGrad.setColorAt(0.92, QColor(0x32, 0x36, 0x40));
        rimGrad.setColorAt(1.0, QColor(0x10, 0x11, 0x14));

        p.setPen(QPen(QColor(0x40, 0x46, 0x52), 1.2));
        p.setBrush(rimGrad);
        p.drawEllipse(QPointF(cx, cy), 35, 35);

        // Inner tape layer ring
        p.setPen(QPen(QColor(0x2a, 0x2e, 0x36), 1.0));
        p.setBrush(QColor(0x14, 0x16, 0x1a));
        p.drawEllipse(QPointF(cx, cy), 28, 28);

        // Rotating spokes (6 spokes at 60 degrees)
        p.save();
        p.translate(cx, cy);
        p.rotate(angle);
        for (int deg = 0; deg < 360; deg += 60)
        {
            p.save();
            p.rotate(deg);
            p.setPen(QPen(QColor(0x5a, 0x62, 0x70), 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(0, -9), QPointF(0, -26));
            p.setPen(QPen(QColor(0x8a, 0x94, 0xa4, 180), 0.8));
            p.drawLine(QPointF(0, -9), QPointF(0, -26));
            p.restore();
        }
        p.restore();

        // Center hub cap & spindle pin
        QRadialGradient hub(cx, cy, 10);
        hub.setColorAt(0.0, QColor(0x9a, 0xa4, 0xb2));
        hub.setColorAt(0.5, QColor(0x50, 0x58, 0x66));
        hub.setColorAt(1.0, QColor(0x28, 0x2d, 0x34));
        p.setPen(QPen(QColor(0x18, 0x1a, 0x1e), 1.0));
        p.setBrush(hub);
        p.drawEllipse(QPointF(cx, cy), 8, 8);

        // Center chrome pin
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xff, 0xff, 0xff));
        p.drawEllipse(QPointF(cx, cy), 2.5, 2.5);
    };

    drawReel(64, 60, fReelAngle);
    drawReel(276, 60, -fReelAngle);

    // -------------------------------------------------------------------------
    // 2B. Center Digital Red LED Time Counter
    // -------------------------------------------------------------------------
    const QRectF ledBox(126, 36, 88, 38);
    p.setPen(QPen(QColor(0x66, 0x12, 0x18), 1.5));
    p.setBrush(QColor(0x12, 0x03, 0x05));
    p.drawRoundedRect(ledBox, 4, 4);

    // Glowing red LED digits
    const int totalSec = static_cast<int>(fCurrentSeconds);
    const int mins = (totalSec / 60) % 100;
    const int secs = totalSec % 60;
    const QString timeStr = QStringLiteral("%1:%2")
                                .arg(mins, 2, 10, QLatin1Char('0'))
                                .arg(secs, 2, 10, QLatin1Char('0'));

    QFont ledFont = font();
    ledFont.setBold(true);
    ledFont.setItalic(true);
    ledFont.setPointSizeF(16.0);
    ledFont.setFamily(QStringLiteral("Consolas, Courier New, monospace"));
    p.setFont(ledFont);

    // LED Glow shadow
    p.setPen(QPen(QColor(0xff, 0x22, 0x33, 80), 3.0));
    p.drawText(ledBox, Qt::AlignCenter, timeStr);
    // Sharp LED core
    p.setPen(QColor(0xff, 0x3b, 0x48));
    p.drawText(ledBox, Qt::AlignCenter, timeStr);

    // Mini indicator bar beneath LED digits
    const QRectF barUnder(138, 79, 64, 3);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x28, 0x08, 0x0c));
    p.drawRoundedRect(barUnder, 1.5, 1.5);
    const double prog = std::fmod(fCurrentSeconds, 60.0) / 60.0;
    QRectF barActive = barUnder;
    barActive.setWidth(std::max(4.0, barUnder.width() * prog));
    p.setBrush(QColor(0xff, 0x2d, 0x40));
    p.drawRoundedRect(barActive, 1.5, 1.5);

    // -------------------------------------------------------------------------
    // 2C. Dual Analog VU Meters (Left & Right Channel)
    // -------------------------------------------------------------------------
    auto drawVUMeter = [&](double mx, double my, double w, double h, float angle, const QString& channelLabel) {
        const QRectF meterRect(mx, my, w, h);

        // Meter background plate (arched dial face)
        QPainterPath dialPath;
        dialPath.moveTo(mx, my + h);
        dialPath.lineTo(mx, my + h * 0.35);
        dialPath.arcTo(mx, my, w, h * 0.7, 180, -180);
        dialPath.lineTo(mx + w, my + h);
        dialPath.closeSubpath();

        QLinearGradient dialGrad(mx, my, mx, my + h);
        dialGrad.setColorAt(0.0, QColor(0xf6, 0xf8, 0xfa));
        dialGrad.setColorAt(0.7, QColor(0xe4, 0xe8, 0xee));
        dialGrad.setColorAt(1.0, QColor(0xd2, 0xd7, 0xdf));

        p.setPen(QPen(QColor(0x48, 0x50, 0x5c), 1.2));
        p.setBrush(dialGrad);
        p.drawPath(dialPath);

        // Subtle top shadow
        p.setPen(QPen(QColor(0x8a, 0x90, 0x98, 120), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(dialPath);

        // Scale arc ticks and numbers
        const double cx = mx + w / 2.0;
        const double cy = my + h + 2.0; // Pivot point at bottom
        const double rTicks = h * 0.88;

        // Arched line
        p.setPen(QPen(QColor(0x50, 0x58, 0x64), 1.0));
        p.drawArc(QRectF(cx - rTicks, cy - rTicks, rTicks * 2.0, rTicks * 2.0), 52 * 16, 76 * 16);

        // Markings: -20, -10, -5, 0, +3
        struct TickDef { float db; const char* txt; bool red; };
        const TickDef ticks[] = {
            {-20.0f, "-20", false},
            {-10.0f, "-10", false},
            {-5.0f,  "-5",  false},
            {0.0f,   "0",   false},
            {+3.0f,  "+3",  true}
        };

        QFont scaleFont = font();
        scaleFont.setBold(true);
        scaleFont.setPointSizeF(5.8);
        p.setFont(scaleFont);

        for (const auto& tk : ticks)
        {
            const float a = peakToAngle(std::pow(10.0f, tk.db / 20.0f));
            const double rad = (90.0 - a) * (3.14159265358979323846 / 180.0);
            const double cosA = std::cos(rad);
            const double sinA = -std::sin(rad);

            const QPointF p1(cx + cosA * (rTicks - 1.0), cy + sinA * (rTicks - 1.0));
            const QPointF p2(cx + cosA * (rTicks + 5.0), cy + sinA * (rTicks + 5.0));

            p.setPen(QPen(tk.red ? QColor(0xe6, 0x22, 0x22) : QColor(0x28, 0x2d, 0x34), 1.0));
            p.drawLine(p1, p2);

            const QPointF pt(cx + cosA * (rTicks - 7.5), cy + sinA * (rTicks - 7.5));
            p.drawText(QRectF(pt.x() - 10, pt.y() - 5, 20, 10), Qt::AlignCenter, QString::fromLatin1(tk.txt));
        }

        // "VU" text centered
        QFont vuFont = font();
        vuFont.setBold(true);
        vuFont.setPointSizeF(6.5);
        p.setFont(vuFont);
        p.setPen(QColor(0x60, 0x68, 0x74));
        p.drawText(QRectF(cx - 15, cy - 22, 30, 10), Qt::AlignCenter, QStringLiteral("VU"));

        // ---------------------------------------------------------------------
        // The Analog Needle
        // ---------------------------------------------------------------------
        p.save();
        p.translate(cx, cy);
        p.rotate(angle);

        // Needle shadow
        p.setPen(QPen(QColor(0x00, 0x00, 0x00, 50), 1.5));
        p.drawLine(QPointF(1.0, 0), QPointF(1.0, -rTicks - 3.0));

        // Vivid Red Needle
        p.setPen(QPen(QColor(0xee, 0x18, 0x22), 1.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(0, 4.0), QPointF(0, -rTicks - 2.0));
        p.restore();

        // Needle Pivot Hub
        QRadialGradient pivot(cx, cy, 8);
        pivot.setColorAt(0.0, QColor(0x60, 0x66, 0x72));
        pivot.setColorAt(0.7, QColor(0x26, 0x2a, 0x30));
        pivot.setColorAt(1.0, QColor(0x14, 0x16, 0x1a));
        p.setPen(QPen(QColor(0x40, 0x48, 0x54), 1.0));
        p.setBrush(pivot);
        p.drawEllipse(QPointF(cx, cy), 6.5, 6.5);

        // Chrome center dot
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xd0, 0xd6, 0xe0));
        p.drawEllipse(QPointF(cx, cy), 2.0, 2.0);

        // Channel Label below meter
        QFont labelFont = font();
        labelFont.setBold(true);
        labelFont.setPointSizeF(5.8);
        p.setFont(labelFont);
        p.setPen(QColor(0x8a, 0x92, 0xa0));
        p.drawText(QRectF(mx, my + h + 2, w, 12), Qt::AlignCenter, channelLabel);
    };

    drawVUMeter(32, 114, 128, 76, fLeftAngle, QStringLiteral("LEFT CHANNEL"));
    drawVUMeter(180, 114, 128, 76, fRightAngle, QStringLiteral("RIGHT CHANNEL"));

    // =========================================================================
    // 3. Middle Status Panel (Signal Process & Info)
    // =========================================================================
    const QRectF sigCard(16, 220, 172, 42);
    QLinearGradient sigGrad(0, sigCard.top(), 0, sigCard.bottom());
    sigGrad.setColorAt(0.0, QColor(0x1a, 0x1c, 0x20));
    sigGrad.setColorAt(1.0, QColor(0x0e, 0x10, 0x12));
    p.setPen(QPen(QColor(0x36, 0x3c, 0x46), 1.2));
    p.setBrush(sigGrad);
    p.drawRoundedRect(sigCard, 5, 5);

    QFont subFont = font();
    subFont.setBold(true);
    subFont.setPointSizeF(5.8);
    p.setFont(subFont);
    p.setPen(QColor(0x76, 0x80, 0x90));
    p.drawText(QRectF(sigCard.left() + 8, sigCard.top() + 4, sigCard.width() - 16, 12),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SIGNAL PROCESS"));

    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(8.5);
    p.setFont(titleFont);
    p.setPen(QColor(0xee, 0xf2, 0xf8));
    p.drawText(QRectF(sigCard.left() + 8, sigCard.top() + 16, sigCard.width() - 16, 15),
               Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(fTrackName, Qt::ElideRight, static_cast<int>(sigCard.width() - 16)));

    p.setFont(subFont);
    p.setPen(QColor(0x60, 0x6a, 0x78));
    p.drawText(QRectF(sigCard.left() + 8, sigCard.top() + 29, sigCard.width() - 16, 10),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("ARTIST REF: AI STUDIO"));

    // Right Metallic Selection Pill
    const QRectF pillCard(198, 222, 126, 38);
    QLinearGradient pillGrad(0, pillCard.top(), 0, pillCard.bottom());
    pillGrad.setColorAt(0.0, QColor(0xd2, 0xd6, 0xde));
    pillGrad.setColorAt(0.5, QColor(0xb6, 0xbe, 0xc8));
    pillGrad.setColorAt(1.0, QColor(0x9aa0aa));
    p.setPen(QPen(QColor(0x40, 0x46, 0x50), 1.2));
    p.setBrush(pillGrad);
    p.drawRoundedRect(pillCard, 4, 4);

    QFont pillFont = font();
    pillFont.setBold(true);
    pillFont.setPointSizeF(7.0);
    p.setFont(pillFont);
    p.setPen(QColor(0x20, 0x24, 0x2c));
    p.drawText(pillCard.adjusted(8, 0, -18, 0), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("FM-01X TAPE"));

    // Down arrow indicator
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x40, 0x48, 0x54));
    QPolygonF arr;
    const double ax = pillCard.right() - 10;
    const double ay = pillCard.center().y();
    arr << QPointF(ax - 3.5, ay - 2) << QPointF(ax + 3.5, ay - 2) << QPointF(ax, ay + 3);
    p.drawPolygon(arr);

    // =========================================================================
    // 4. Vintage Aluminum Transport Buttons
    // =========================================================================
    const QRectF panelBtns(16, 268, 308, 52);
    QLinearGradient btnPlt(0, panelBtns.top(), 0, panelBtns.bottom());
    btnPlt.setColorAt(0.0, QColor(0xcc, 0xd2, 0xdc));
    btnPlt.setColorAt(1.0, QColor(0xb0, 0xb8, 0xc4));
    p.setPen(QPen(QColor(0x70, 0x78, 0x86), 1.0));
    p.setBrush(btnPlt);
    p.drawRoundedRect(panelBtns, 6, 6);

    auto drawBtn = [&](const QRectF& r, int idx, bool isOrange, const QString& glyph, bool isRecord = false) {
        const bool hovered = (fHoveredBtn == idx);
        const bool pressed = (fPressedBtn == idx);

        QRectF drawR = r;
        if (pressed)
            drawR.translate(0, 1.5);

        // Button shadow
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x40, 0x46, 0x50, 120));
        p.drawRoundedRect(r.adjusted(0, 2, 0, 2), 5, 5);

        if (isOrange)
        {
            // Illuminated Play/Pause button
            QLinearGradient oGrad(0, drawR.top(), 0, drawR.bottom());
            if (pressed)
            {
                oGrad.setColorAt(0.0, QColor(0xe0, 0x55, 0x00));
                oGrad.setColorAt(1.0, QColor(0xff, 0x7a, 0x18));
            }
            else if (hovered)
            {
                oGrad.setColorAt(0.0, QColor(0xff, 0x8a, 0x22));
                oGrad.setColorAt(1.0, QColor(0xed, 0x62, 0x00));
            }
            else
            {
                oGrad.setColorAt(0.0, QColor(0xff, 0x77, 0x00));
                oGrad.setColorAt(1.0, QColor(0xe6, 0x58, 0x00));
            }
            p.setPen(QPen(QColor(0xa0, 0x3d, 0x00), 1.5));
            p.setBrush(oGrad);
            p.drawRoundedRect(drawR, 6, 6);

            // Icon
            p.setFont(QFont(QStringLiteral("Segoe UI Symbol, Arial"), 13, QFont::Bold));
            p.setPen(Qt::white);
            p.drawText(drawR, Qt::AlignCenter, glyph);
        }
        else
        {
            // Brushed metallic button
            QLinearGradient mGrad(0, drawR.top(), 0, drawR.bottom());
            if (pressed)
            {
                mGrad.setColorAt(0.0, QColor(0xa0, 0xa6, 0xb0));
                mGrad.setColorAt(1.0, QColor(0xd0, 0xd6, 0xe0));
            }
            else if (hovered)
            {
                mGrad.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
                mGrad.setColorAt(1.0, QColor(0xc6, 0xcc, 0xd6));
            }
            else
            {
                mGrad.setColorAt(0.0, QColor(0xee, 0xf2, 0xf6));
                mGrad.setColorAt(1.0, QColor(0xbe, 0xc4, 0xce));
            }
            p.setPen(QPen(QColor(0x60, 0x68, 0x74), 1.2));
            p.setBrush(mGrad);
            p.drawRoundedRect(drawR, 5, 5);

            if (isRecord)
            {
                // Red record dot
                p.setPen(QPen(QColor(0x99, 0x0a, 0x10), 1.0));
                p.setBrush(QColor(0xee, 0x1a, 0x24));
                p.drawEllipse(drawR.center(), 5.0, 5.0);
            }
            else
            {
                p.setFont(QFont(QStringLiteral("Segoe UI Symbol, Arial"), 10, QFont::Bold));
                p.setPen(QColor(0x30, 0x34, 0x3c));
                p.drawText(drawR, Qt::AlignCenter, glyph);
            }
        }
    };

    drawBtn(fRewindBtn, 0, false, QStringLiteral("◀◀"));
    drawBtn(fRecordBtn, 1, false, QString(), true);
    drawBtn(fPlayBtn, 2, true, fIsPlaying ? QStringLiteral("❚❚") : QStringLiteral("▶"));
    drawBtn(fStopBtn, 3, false, QStringLiteral("■"));
    drawBtn(fFwdBtn, 4, false, QStringLiteral("▶▶"));

    // =========================================================================
    // 5. Silkscreen Legend & Typography
    // =========================================================================
    QFont brandFont = font();
    brandFont.setBold(true);
    brandFont.setItalic(true);
    brandFont.setPointSizeF(10.0);
    brandFont.setFamily(QStringLiteral("Arial, Helvetica, sans-serif"));
    p.setFont(brandFont);
    p.setPen(QColor(0x30, 0x36, 0x40));
    p.drawText(QRectF(16, 328, 140, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("PRODUCER"));

    QFont modelFont = font();
    modelFont.setBold(true);
    modelFont.setPointSizeF(5.4);
    p.setFont(modelFont);
    p.setPen(QColor(0x60, 0x68, 0x76));
    p.drawText(QRectF(16, 344, 180, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("ALUMINUM SERIES  -  FM-01X"));

    p.drawText(QRectF(150, 334, 174, 12), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("PRECISION AUDIO"));
    p.drawText(QRectF(150, 346, 174, 12), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("HI-FIDELITY METAL CASSETTE"));
}

void TapeDeckWidget::mousePressEvent(QMouseEvent* event)
{
    const double sx = static_cast<double>(width()) / 340.0;
    const double sy = static_cast<double>(height()) / 380.0;
    const double scale = std::min(sx, sy);
    const double tx = (width() - 340.0 * scale) / 2.0;
    const double ty = (height() - 380.0 * scale) / 2.0;

    const QPointF pt = (event->position() - QPointF(tx, ty)) / scale;

    if (fRewindBtn.contains(pt))   fPressedBtn = 0;
    else if (fRecordBtn.contains(pt)) fPressedBtn = 1;
    else if (fPlayBtn.contains(pt))   fPressedBtn = 2;
    else if (fStopBtn.contains(pt))   fPressedBtn = 3;
    else if (fFwdBtn.contains(pt))    fPressedBtn = 4;
    else fPressedBtn = -1;

    update();
}

void TapeDeckWidget::mouseMoveEvent(QMouseEvent* event)
{
    const double sx = static_cast<double>(width()) / 340.0;
    const double sy = static_cast<double>(height()) / 380.0;
    const double scale = std::min(sx, sy);
    const double tx = (width() - 340.0 * scale) / 2.0;
    const double ty = (height() - 380.0 * scale) / 2.0;

    const QPointF pt = (event->position() - QPointF(tx, ty)) / scale;

    const int oldHover = fHoveredBtn;
    if (fRewindBtn.contains(pt))   fHoveredBtn = 0;
    else if (fRecordBtn.contains(pt)) fHoveredBtn = 1;
    else if (fPlayBtn.contains(pt))   fHoveredBtn = 2;
    else if (fStopBtn.contains(pt))   fHoveredBtn = 3;
    else if (fFwdBtn.contains(pt))    fHoveredBtn = 4;
    else fHoveredBtn = -1;

    if (fHoveredBtn != -1)
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();

    if (oldHover != fHoveredBtn)
        update();
}

void TapeDeckWidget::mouseReleaseEvent(QMouseEvent* event)
{
    const int btn = fPressedBtn;
    fPressedBtn = -1;
    update();

    const double sx = static_cast<double>(width()) / 340.0;
    const double sy = static_cast<double>(height()) / 380.0;
    const double scale = std::min(sx, sy);
    const double tx = (width() - 340.0 * scale) / 2.0;
    const double ty = (height() - 380.0 * scale) / 2.0;
    const QPointF pt = (event->position() - QPointF(tx, ty)) / scale;

    if (btn == 0 && fRewindBtn.contains(pt))
        emit rewindRequested();
    else if (btn == 1 && fRecordBtn.contains(pt))
        emit recordRequested();
    else if (btn == 2 && fPlayBtn.contains(pt))
    {
        if (fIsPlaying)
            emit pauseRequested();
        else
            emit playRequested();
    }
    else if (btn == 3 && fStopBtn.contains(pt))
        emit stopRequested();
    else if (btn == 4 && fFwdBtn.contains(pt))
        emit fastForwardRequested();
}
