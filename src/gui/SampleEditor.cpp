#include "SampleEditor.h"

#include "Theme.h"
#include "core/AudioConvert.h"
#include "core/ClipEdit.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kToolbarHeight = 32;
constexpr int kRulerHeight = 24;
constexpr int kKeyWidth = 46;
}

SampleEditor::SampleEditor(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(180);
    setMouseTracking(true);
    buildToolbar();
}

void SampleEditor::buildToolbar()
{
    fToolbar = new QWidget(this);
    fToolbar->setFixedHeight(kToolbarHeight);
    fToolbar->setStyleSheet(QStringLiteral(
        "QWidget { background-color: #23272d; border-bottom: 1px solid #141619; }"
        "QPushButton { background: #2f343b; color: #d6d9dc; border: 1px solid #3c424a; border-radius: 3px; padding: 2px 8px; font-size: 11px; }"
        "QPushButton:hover { background: #3b424b; border-color: #4a90d9; }"
        "QPushButton:checked { background: #4a90d9; color: white; border-color: #5aa0e9; font-weight: bold; }"
        "QComboBox { background: #2f343b; color: #d6d9dc; border: 1px solid #3c424a; border-radius: 3px; padding: 1px 4px; font-size: 11px; }"
        "QLabel { color: #8b9096; font-size: 11px; }"
    ));

    auto* layout = new QHBoxLayout(fToolbar);
    layout->setContentsMargins(8, 2, 8, 2);
    layout->setSpacing(6);

    fSelectBtn = new QPushButton(QStringLiteral("选择 (1)"), fToolbar);
    fSelectBtn->setCheckable(true);
    fSelectBtn->setChecked(true);
    layout->addWidget(fSelectBtn);

    fScissorsBtn = new QPushButton(QStringLiteral("✂️ 剪刀 (2)"), fToolbar);
    fScissorsBtn->setCheckable(true);
    layout->addWidget(fScissorsBtn);

    connect(fSelectBtn, &QPushButton::clicked, this, [this] {
        fTool = Tool::Select;
        fSelectBtn->setChecked(true);
        fScissorsBtn->setChecked(false);
        setCursor(Qt::ArrowCursor);
    });

    connect(fScissorsBtn, &QPushButton::clicked, this, [this] {
        fTool = Tool::Scissors;
        fScissorsBtn->setChecked(true);
        fSelectBtn->setChecked(false);
        setCursor(Qt::CrossCursor);
    });

    layout->addSpacing(8);

    fMuteBtn = new QPushButton(QStringLiteral("静音选区"), fToolbar);
    layout->addWidget(fMuteBtn);
    connect(fMuteBtn, &QPushButton::clicked, this, [this] {
        if (fSelectionEnd > fSelectionStart && fTrackIndex >= 0)
        {
            chost::ClipEdit edit;
            edit.kind = chost::ClipEditKind::Silence;
            edit.start = fSelectionStart;
            edit.end = fSelectionEnd;
            emit clipEditRequested(fTrackIndex, edit, QStringLiteral("静音选区"));
        }
    });

    fNormalizeBtn = new QPushButton(QStringLiteral("标准化"), fToolbar);
    layout->addWidget(fNormalizeBtn);
    connect(fNormalizeBtn, &QPushButton::clicked, this, [this] {
        if (fTrackIndex >= 0)
        {
            chost::ClipEdit edit;
            edit.kind = chost::ClipEditKind::Normalize;
            if (fSelectionEnd > fSelectionStart)
            {
                edit.start = fSelectionStart;
                edit.end = fSelectionEnd;
            }
            edit.db = -1.0;
            emit clipEditRequested(fTrackIndex, edit, QStringLiteral("标准化"));
        }
    });

    fFadeInBtn = new QPushButton(QStringLiteral("淡入"), fToolbar);
    layout->addWidget(fFadeInBtn);
    connect(fFadeInBtn, &QPushButton::clicked, this, [this] {
        if (fSelectionEnd > fSelectionStart && fTrackIndex >= 0)
        {
            chost::ClipEdit edit;
            edit.kind = chost::ClipEditKind::FadeIn;
            edit.start = fSelectionStart;
            edit.end = fSelectionEnd;
            emit clipEditRequested(fTrackIndex, edit, QStringLiteral("淡入"));
        }
    });

    fFadeOutBtn = new QPushButton(QStringLiteral("淡出"), fToolbar);
    layout->addWidget(fFadeOutBtn);
    connect(fFadeOutBtn, &QPushButton::clicked, this, [this] {
        if (fSelectionEnd > fSelectionStart && fTrackIndex >= 0)
        {
            chost::ClipEdit edit;
            edit.kind = chost::ClipEditKind::FadeOut;
            edit.start = fSelectionStart;
            edit.end = fSelectionEnd;
            emit clipEditRequested(fTrackIndex, edit, QStringLiteral("淡出"));
        }
    });

    layout->addSpacing(12);

    auto* snapLabel = new QLabel(QStringLiteral("吸附:"), fToolbar);
    layout->addWidget(snapLabel);
    fSnapCombo = new QComboBox(fToolbar);
    fSnapCombo->addItem(QStringLiteral("小节"), 1.0);
    fSnapCombo->addItem(QStringLiteral("1/2 拍"), 0.5);
    fSnapCombo->addItem(QStringLiteral("1/4 拍"), 0.25);
    fSnapCombo->addItem(QStringLiteral("1/8 拍"), 0.125);
    fSnapCombo->addItem(QStringLiteral("1/16 拍"), 0.0625);
    fSnapCombo->addItem(QStringLiteral("关闭"), 0.0);
    fSnapCombo->setCurrentIndex(0);
    layout->addWidget(fSnapCombo);

    layout->addSpacing(12);

    auto* zoomOutBtn = new QPushButton(QStringLiteral("-"), fToolbar);
    zoomOutBtn->setFixedWidth(24);
    layout->addWidget(zoomOutBtn);
    connect(zoomOutBtn, &QPushButton::clicked, this, [this] {
        fPixelsPerSecond = std::max(20.0, fPixelsPerSecond * 0.75);
        update();
    });

    auto* zoomInBtn = new QPushButton(QStringLiteral("+"), fToolbar);
    zoomInBtn->setFixedWidth(24);
    layout->addWidget(zoomInBtn);
    connect(zoomInBtn, &QPushButton::clicked, this, [this] {
        fPixelsPerSecond = std::min(1000.0, fPixelsPerSecond * 1.33);
        update();
    });

    auto* fitBtn = new QPushButton(QStringLiteral("适应"), fToolbar);
    layout->addWidget(fitBtn);
    connect(fitBtn, &QPushButton::clicked, this, [this] {
        const int waveW = std::max(100, width() - kKeyWidth);
        if (fDuration > 0.0)
            fPixelsPerSecond = std::clamp(static_cast<double>(waveW) / fDuration, 20.0, 1000.0);
        fViewStart = 0.0;
        update();
    });

    auto* ampLabel = new QLabel(QStringLiteral("波形放大:"), fToolbar);
    layout->addWidget(ampLabel);
    fAmpSlider = new QSlider(Qt::Horizontal, fToolbar);
    fAmpSlider->setRange(10, 40);
    fAmpSlider->setValue(10);
    fAmpSlider->setFixedWidth(70);
    layout->addWidget(fAmpSlider);
    connect(fAmpSlider, &QSlider::valueChanged, this, [this](int val) {
        fAmplitudeZoom = val / 10.0f;
        update();
    });

    layout->addStretch();

    fInfoLabel = new QLabel(QStringLiteral("尚未选中音频轨道"), fToolbar);
    layout->addWidget(fInfoLabel);
}

void SampleEditor::setTrack(int trackIndex, const QString& trackName, const QString& filePath, const QColor& color)
{
    fTrackIndex = trackIndex;
    fTrackName = trackName;
    fFilePath = filePath;
    fColor = color.isValid() ? color : theme::trackColor;
    fSelectionStart = 0.0;
    fSelectionEnd = 0.0;
    updatePeaks();

    if (fDuration > 0.0)
    {
        const int waveW = std::max(100, width() - kKeyWidth);
        fPixelsPerSecond = std::clamp(static_cast<double>(waveW) / fDuration, 25.0, 800.0);
        fViewStart = 0.0;
        const int min = static_cast<int>(fDuration) / 60;
        const int sec = static_cast<int>(fDuration) % 60;
        fInfoLabel->setText(QStringLiteral("%1 · %2 Hz · %3 声道 · %4:%5")
                                .arg(fTrackName)
                                .arg(fSampleRate)
                                .arg(fChannels)
                                .arg(min, 2, 10, QChar('0'))
                                .arg(sec, 2, 10, QChar('0')));
    }
    else
    {
        fInfoLabel->setText(fTrackName);
    }
    update();
}

void SampleEditor::clearTrack()
{
    fTrackIndex = -1;
    fTrackName.clear();
    fFilePath.clear();
    fDensePeaks.clear();
    fDuration = 0.0;
    fInfoLabel->setText(QStringLiteral("尚未选中音频轨道"));
    update();
}

void SampleEditor::updatePeaks()
{
    fDensePeaks.clear();
    fDuration = 0.0;
    if (fFilePath.isEmpty())
        return;

    chost::AudioFileInfo info;
    if (!chost::probeAudioFile(fFilePath.toStdString(), info))
        return;

    fSampleRate = info.sampleRate;
    fChannels = info.channels;
    if (info.sampleRate > 0)
        fDuration = static_cast<double>(info.frames) / info.sampleRate;

    constexpr size_t kDenseBuckets = 8192;
    chost::computePeaks(fFilePath.toStdString(), kDenseBuckets, fDensePeaks, info);
}

void SampleEditor::setTempo(double bpm)
{
    if (bpm > 20.0 && bpm < 400.0)
    {
        fTempo = bpm;
        update();
    }
}

void SampleEditor::setPlayhead(double seconds)
{
    if (std::abs(fPlayhead - seconds) > 0.001)
    {
        fPlayhead = seconds;
        update();
    }
}

void SampleEditor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (fToolbar)
        fToolbar->setGeometry(0, 0, width(), kToolbarHeight);
}

int SampleEditor::timeToX(double seconds, int waveLeft) const
{
    return waveLeft + static_cast<int>((seconds - fViewStart) * fPixelsPerSecond);
}

double SampleEditor::xToTime(int x, int waveLeft) const
{
    return std::max(0.0, fViewStart + static_cast<double>(x - waveLeft) / fPixelsPerSecond);
}

double SampleEditor::snapTime(double seconds) const
{
    if (!fSnapCombo)
        return seconds;
    const double snapBeat = fSnapCombo->currentData().toDouble();
    if (snapBeat <= 0.0)
        return seconds;

    const double beatSec = 60.0 / fTempo * snapBeat;
    return std::round(seconds / beatSec) * beatSec;
}

void SampleEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    // Background
    p.fillRect(rect(), QColor(0x1a, 0x1d, 0x22));

    const int waveLeft = kKeyWidth;
    const int waveTop = kToolbarHeight + kRulerHeight;
    const int waveWidth = width() - waveLeft;
    const int waveHeight = height() - waveTop;

    if (waveWidth <= 0 || waveHeight <= 0)
        return;

    const QRect rulerRect(waveLeft, kToolbarHeight, waveWidth, kRulerHeight);
    const QRect keysRect(0, waveTop, kKeyWidth, waveHeight);
    const QRect waveRect(waveLeft, waveTop, waveWidth, waveHeight);

    drawRuler(p, rulerRect);
    drawPianoKeys(p, keysRect);
    drawWaveform(p, waveRect);
}

void SampleEditor::drawRuler(QPainter& p, const QRect& rulerRect)
{
    p.fillRect(rulerRect, QColor(0x20, 0x24, 0x2a));
    p.setPen(QColor(0x14, 0x16, 0x19));
    p.drawLine(rulerRect.left(), rulerRect.bottom(), rulerRect.right(), rulerRect.bottom());

    const double beatSec = 60.0 / fTempo;
    const double barSec = beatSec * 4.0; // 4/4 meter

    const double startTime = fViewStart;
    const double endTime = xToTime(rulerRect.right(), rulerRect.left());

    const int startBar = static_cast<int>(std::floor(startTime / barSec));
    const int endBar = static_cast<int>(std::ceil(endTime / barSec)) + 1;

    p.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Bold));

    for (int bar = startBar; bar <= endBar; ++bar)
    {
        const double barTime = bar * barSec;
        const int barX = timeToX(barTime, rulerRect.left());
        if (barX >= rulerRect.left() && barX <= rulerRect.right())
        {
            // Bar tick
            p.setPen(QColor(0x4a, 0x90, 0xd9));
            p.drawLine(barX, rulerRect.top() + 6, barX, rulerRect.bottom());
            p.setPen(QColor(0xd6, 0xd9, 0xdc));
            p.drawText(barX + 4, rulerRect.top() + 15, QString::number(bar + 1));
        }

        // Sub-beats (1, 2, 3, 4)
        for (int beat = 1; beat < 4; ++beat)
        {
            const double beatTime = barTime + beat * beatSec;
            const int beatX = timeToX(beatTime, rulerRect.left());
            if (beatX >= rulerRect.left() && beatX <= rulerRect.right())
            {
                p.setPen(QColor(0x35, 0x3a, 0x42));
                p.drawLine(beatX, rulerRect.top() + 14, beatX, rulerRect.bottom());
            }
        }
    }
}

void SampleEditor::drawPianoKeys(QPainter& p, const QRect& keysRect)
{
    p.fillRect(keysRect, QColor(0x18, 0x1a, 0x1f));
    p.setPen(QColor(0x14, 0x16, 0x19));
    p.drawLine(keysRect.right(), keysRect.top(), keysRect.right(), keysRect.bottom());

    constexpr int kWhiteKeyHeight = 16;
    const int keyCount = keysRect.height() / kWhiteKeyHeight + 2;

    p.setFont(QFont(QStringLiteral("Segoe UI"), 7));

    // Draw keys simulating piano keys with C3 highlighted
    for (int i = 0; i < keyCount; ++i)
    {
        const int y = keysRect.top() + i * kWhiteKeyHeight;
        const int noteIdx = 72 - i; // C4 downwards
        const int semitone = (noteIdx % 12 + 12) % 12;
        const bool isBlack = (semitone == 1 || semitone == 3 || semitone == 6 || semitone == 8 || semitone == 10);
        const bool isC = (semitone == 0);

        QRect keyBox(keysRect.left(), y, keysRect.width() - (isBlack ? 14 : 0), kWhiteKeyHeight);

        if (isBlack)
        {
            p.fillRect(keyBox, QColor(0x14, 0x16, 0x18));
            p.setPen(QColor(0x0a, 0x0c, 0x0e));
            p.drawRect(keyBox);
        }
        else
        {
            p.fillRect(keyBox, isC ? QColor(0x2c, 0x34, 0x3e) : QColor(0x24, 0x28, 0x2e));
            p.setPen(QColor(0x18, 0x1b, 0x20));
            p.drawRect(keyBox);

            if (isC)
            {
                const int octave = noteIdx / 12 - 1;
                p.setPen(octave == 3 ? QColor(0x4a, 0x90, 0xd9) : QColor(0x8b, 0x90, 0x96));
                p.drawText(keyBox.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("C%1").arg(octave));
            }
        }
    }
}

void SampleEditor::drawWaveform(QPainter& p, const QRect& waveRect)
{
    p.fillRect(waveRect, QColor(0x12, 0x14, 0x17));

    // Horizontal guidelines
    p.setPen(QPen(QColor(0x1c, 0x20, 0x26), 1, Qt::DotLine));
    const int midY = waveRect.center().y();
    p.drawLine(waveRect.left(), midY, waveRect.right(), midY);
    p.drawLine(waveRect.left(), midY - waveRect.height() / 4, waveRect.right(), midY - waveRect.height() / 4);
    p.drawLine(waveRect.left(), midY + waveRect.height() / 4, waveRect.right(), midY + waveRect.height() / 4);

    // Bar and Beat vertical grid lines
    const double beatSec = 60.0 / fTempo;
    const double barSec = beatSec * 4.0;
    const double startTime = fViewStart;
    const double endTime = xToTime(waveRect.right(), waveRect.left());
    const int startBar = static_cast<int>(std::floor(startTime / barSec));
    const int endBar = static_cast<int>(std::ceil(endTime / barSec)) + 1;

    for (int bar = startBar; bar <= endBar; ++bar)
    {
        const double barTime = bar * barSec;
        const int barX = timeToX(barTime, waveRect.left());
        if (barX >= waveRect.left() && barX <= waveRect.right())
        {
            p.setPen(QColor(0x23, 0x2a, 0x33));
            p.drawLine(barX, waveRect.top(), barX, waveRect.bottom());
        }
        for (int beat = 1; beat < 4; ++beat)
        {
            const int beatX = timeToX(barTime + beat * beatSec, waveRect.left());
            if (beatX >= waveRect.left() && beatX <= waveRect.right())
            {
                p.setPen(QColor(0x18, 0x1c, 0x22));
                p.drawLine(beatX, waveRect.top(), beatX, waveRect.bottom());
            }
        }
    }

    // Draw audio clip background
    const int clipStartPix = timeToX(0.0, waveRect.left());
    const int clipEndPix = timeToX(fDuration, waveRect.left());
    const int drawLeft = std::max(waveRect.left(), clipStartPix);
    const int drawRight = std::min(waveRect.right(), clipEndPix);

    if (drawRight > drawLeft)
    {
        QRect clipBg(drawLeft, waveRect.top(), drawRight - drawLeft, waveRect.height());
        p.fillRect(clipBg, QColor(0x15, 0x1b, 0x22));
        p.setPen(QColor(0x2a, 0x33, 0x40));
        p.drawRect(clipBg);
    }

    // Dense waveform rendering
    if (!fDensePeaks.empty() && fDuration > 0.0 && drawRight > drawLeft)
    {
        const size_t peakCount = fDensePeaks.size();
        const double halfH = (waveRect.height() / 2.0 - 4.0) * fAmplitudeZoom;
        const QColor waveCol = fColor.isValid() ? fColor : QColor(0x4a, 0x90, 0xd9);

        p.setPen(waveCol);

        for (int x = drawLeft; x <= drawRight; ++x)
        {
            const double t0 = xToTime(x, waveRect.left());
            const double t1 = xToTime(x + 1, waveRect.left());
            const size_t idx0 = static_cast<size_t>(std::clamp(t0 / fDuration * peakCount, 0.0, static_cast<double>(peakCount - 1)));
            const size_t idx1 = static_cast<size_t>(std::clamp(t1 / fDuration * peakCount, 0.0, static_cast<double>(peakCount - 1)));

            float peak = 0.0f;
            for (size_t k = idx0; k <= idx1 && k < peakCount; ++k)
                peak = std::max(peak, fDensePeaks[k]);

            const int ampPix = static_cast<int>(std::min(1.0f, peak) * halfH);
            if (ampPix > 0)
                p.drawLine(x, midY - ampPix, x, midY + ampPix);
        }
    }

    // Selection highlight
    if (fSelectionEnd > fSelectionStart)
    {
        const int selX0 = timeToX(fSelectionStart, waveRect.left());
        const int selX1 = timeToX(fSelectionEnd, waveRect.left());
        const int sLeft = std::max(waveRect.left(), std::min(selX0, selX1));
        const int sRight = std::min(waveRect.right(), std::max(selX0, selX1));
        if (sRight > sLeft)
        {
            p.fillRect(QRect(sLeft, waveRect.top(), sRight - sLeft, waveRect.height()), QColor(0x4a, 0x90, 0xd9, 50));
            p.setPen(QColor(0x5a, 0xa0, 0xe9));
            p.drawLine(sLeft, waveRect.top(), sLeft, waveRect.bottom());
            p.drawLine(sRight, waveRect.top(), sRight, waveRect.bottom());
        }
    }

    // Playhead cursor
    const int playheadX = timeToX(fPlayhead, waveRect.left());
    if (playheadX >= waveRect.left() && playheadX <= waveRect.right())
    {
        p.setPen(QPen(QColor(0xf0, 0xf0, 0xf0), 2));
        p.drawLine(playheadX, kToolbarHeight, playheadX, waveRect.bottom());

        // Playhead triangle in ruler
        QPolygon triangle;
        triangle << QPoint(playheadX - 6, kToolbarHeight)
                 << QPoint(playheadX + 6, kToolbarHeight)
                 << QPoint(playheadX, kToolbarHeight + 10);
        p.setBrush(QColor(0xf0, 0xf0, 0xf0));
        p.setPen(Qt::NoPen);
        p.drawPolygon(triangle);
    }
}

void SampleEditor::mousePressEvent(QMouseEvent* event)
{
    const int waveLeft = kKeyWidth;
    const double clickTime = xToTime(event->pos().x(), waveLeft);

    if (event->button() == Qt::LeftButton)
    {
        const int playX = timeToX(fPlayhead, waveLeft);
        if (std::abs(event->pos().x() - playX) <= 8 || event->pos().y() <= kToolbarHeight + kRulerHeight)
        {
            fScrubbing = true;
            const double t = snapTime(std::max(0.0, clickTime));
            emit seekRequested(t);
            setPlayhead(t);
            update();
            return;
        }

        if (fTool == Tool::Scissors)
        {
            // Scissors tool -> split clip
            if (fTrackIndex >= 0 && clickTime > 0.0 && clickTime < fDuration)
            {
                const double splitSec = snapTime(clickTime);
                chost::ClipEdit edit;
                edit.kind = chost::ClipEditKind::Move; // or split
                edit.seconds = splitSec;
                emit clipEditRequested(fTrackIndex, edit, QStringLiteral("剪刀切割片段"));
            }
        }
        else
        {
            // Select tool -> begin range selection
            fSelecting = true;
            fSelectionStart = snapTime(clickTime);
            fSelectionEnd = fSelectionStart;
            emit seekRequested(fSelectionStart);
            update();
        }
    }
}

void SampleEditor::mouseMoveEvent(QMouseEvent* event)
{
    const int waveLeft = kKeyWidth;
    if (fScrubbing)
    {
        const double t = snapTime(std::max(0.0, xToTime(event->pos().x(), waveLeft)));
        emit seekRequested(t);
        setPlayhead(t);
        update();
        return;
    }
    if (fSelecting)
    {
        fSelectionEnd = snapTime(xToTime(event->pos().x(), waveLeft));
        emit rangeChanged(std::min(fSelectionStart, fSelectionEnd), std::max(fSelectionStart, fSelectionEnd));
        update();
        return;
    }
    const int playX = timeToX(fPlayhead, waveLeft);
    if (std::abs(event->pos().x() - playX) <= 6 || event->pos().y() <= kToolbarHeight + kRulerHeight)
        setCursor(Qt::SizeHorCursor);
    else if (fTool == Tool::Scissors)
        setCursor(Qt::CrossCursor);
    else
        setCursor(Qt::ArrowCursor);
}

void SampleEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (fScrubbing)
        {
            fScrubbing = false;
            return;
        }
        if (fSelecting)
        {
            fSelecting = false;
            if (std::abs(fSelectionEnd - fSelectionStart) < 0.05)
            {
                fSelectionStart = 0.0;
                fSelectionEnd = 0.0;
                emit rangeChanged(0.0, 0.0);
            }
            update();
        }
    }
}

void SampleEditor::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (event->modifiers() & Qt::ControlModifier)
    {
        // Horizontal zoom centered on mouse
        const double factor = delta > 0 ? 1.25 : 0.8;
        const int waveLeft = kKeyWidth;
        const double mouseTime = xToTime(event->position().toPoint().x(), waveLeft);

        fPixelsPerSecond = std::clamp(fPixelsPerSecond * factor, 15.0, 1500.0);
        fViewStart = std::max(0.0, mouseTime - (event->position().toPoint().x() - waveLeft) / fPixelsPerSecond);
        update();
    }
    else
    {
        // Horizontal scroll
        const double scrollSec = -delta / 120.0 * 2.0;
        fViewStart = std::clamp(fViewStart + scrollSec, 0.0, std::max(0.0, fDuration - 1.0));
        update();
    }
}
