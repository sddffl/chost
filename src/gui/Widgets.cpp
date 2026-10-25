#include "Widgets.h"

#include "Theme.h"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterMaxDb = 6.0f;
constexpr float kMaxVolume = 1.27f; // carla's volume range is 0 .. 1.27 (+2 dB)
constexpr int kFaderSteps = 1000;

float toDb(float linear)
{
    return linear > 0.0f ? 20.0f * std::log10(linear) : -200.0f;
}

// fader travel is cubic so that 0 dB sits near the top, like a console
float faderToVolume(int pos)
{
    const float p = static_cast<float>(pos) / kFaderSteps;
    return kMaxVolume * p * p * p;
}

int volumeToFader(float volume)
{
    return static_cast<int>(std::cbrt(std::clamp(volume, 0.0f, kMaxVolume) / kMaxVolume) * kFaderSteps + 0.5f);
}

QString formatTime(double seconds)
{
    const int total = static_cast<int>(seconds);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

LevelMeter::LevelMeter(QWidget* parent)
    : QWidget(parent)
{
    setMinimumWidth(14);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void LevelMeter::setLevels(float left, float right)
{
    const float levels[2] = {left, right};
    for (int i = 0; i < 2; ++i)
    {
        // fast attack, slow release, and a hold marker that falls slowly
        fLevel[i] = levels[i] >= fLevel[i] ? levels[i] : fLevel[i] * 0.85f;
        fHold[i] = levels[i] >= fHold[i] ? levels[i] : fHold[i] * 0.97f;
    }
    update();
}

void LevelMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), theme::lcd);

    const int barWidth = (width() - 3) / 2;
    const int h = height() - 2;
    auto dbToY = [&](float db) {
        const float t = (std::clamp(db, kMeterMinDb, kMeterMaxDb) - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
        return 1 + static_cast<int>((1.0f - t) * h);
    };

    QLinearGradient grad(0, dbToY(kMeterMaxDb), 0, dbToY(kMeterMinDb));
    grad.setColorAt(0.0, theme::meterHigh);
    grad.setColorAt((kMeterMaxDb - 0.0f) / (kMeterMaxDb - kMeterMinDb), theme::meterHigh);
    grad.setColorAt((kMeterMaxDb + 6.0f) / (kMeterMaxDb - kMeterMinDb), theme::meterMid);
    grad.setColorAt((kMeterMaxDb + 18.0f) / (kMeterMaxDb - kMeterMinDb), theme::meterLow);
    grad.setColorAt(1.0, theme::meterLow.darker(140));

    for (int i = 0; i < 2; ++i)
    {
        const int x = 1 + i * (barWidth + 1);
        const int top = dbToY(toDb(fLevel[i]));
        p.fillRect(QRect(x, top, barWidth, height() - 1 - top), grad);
        if (fHold[i] > 0.001f)
        {
            const int y = dbToY(toDb(fHold[i]));
            p.fillRect(QRect(x, y, barWidth, 2), fHold[i] >= 1.0f ? theme::meterHigh : theme::text);
        }
    }

    // 0 dB tick
    p.setPen(QColor(255, 255, 255, 60));
    p.drawLine(0, dbToY(0.0f), width(), dbToY(0.0f));
}

// ---------------------------------------------------------------------------------------------------------------------

ArrangeView::ArrangeView(QWidget* parent)
    : QWidget(parent)
{
    setAcceptDrops(true);
    setMinimumHeight(kRulerHeight + kLaneHeight + 30);
    fScroll = new QScrollBar(Qt::Horizontal, this);
    connect(fScroll, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    updateScrollRange();
}

void ArrangeView::setTrack(const QString& trackName, const QString& clipName, const std::vector<float>& peaks, double seconds)
{
    fHasTrack = true;
    fTrackName = trackName;
    fClipName = clipName;
    fPeaks = peaks;
    fSeconds = seconds;
    updateScrollRange();
    update();
}

void ArrangeView::clearTrack()
{
    fHasTrack = false;
    fPeaks.clear();
    fSeconds = 0.0;
    updateScrollRange();
    update();
}

void ArrangeView::setPlayhead(double seconds, bool followPlayback)
{
    if (std::abs(seconds - fPlayhead) < 1e-4)
        return;
    fPlayhead = seconds;

    // page the view along with the cursor, as Cubase does with "auto-scroll"
    if (followPlayback)
    {
        const int x = secondsToX(seconds);
        if (x > width() - 20 || x < kHeaderWidth)
            fScroll->setValue(static_cast<int>(seconds * fPixelsPerSecond) - 40);
    }
    update();
}

void ArrangeView::setMuted(bool muted)
{
    fMuted = muted;
    update();
}

void ArrangeView::setTempo(double bpm)
{
    fBpm = bpm > 0.0 ? bpm : 120.0;
    update();
}

double ArrangeView::xToSeconds(int x) const
{
    return std::max(0.0, (x - kHeaderWidth + fScroll->value()) / fPixelsPerSecond);
}

int ArrangeView::secondsToX(double seconds) const
{
    return kHeaderWidth + static_cast<int>(seconds * fPixelsPerSecond) - fScroll->value();
}

QRect ArrangeView::laneRect() const
{
    return {0, kRulerHeight, width(), kLaneHeight};
}

QRect ArrangeView::muteRect() const
{
    return {kHeaderWidth - 34, kRulerHeight + 10, 24, 20};
}

void ArrangeView::updateScrollRange()
{
    const double visible = std::max(1, width() - kHeaderWidth) / fPixelsPerSecond;
    const double total = std::max(fSeconds + 10.0, 60.0);
    fScroll->setRange(0, std::max(0, static_cast<int>((total - visible) * fPixelsPerSecond)));
    fScroll->setPageStep(std::max(1, width() - kHeaderWidth));
    fScroll->setSingleStep(20);
}

void ArrangeView::resizeEvent(QResizeEvent*)
{
    const int h = fScroll->sizeHint().height();
    fScroll->setGeometry(kHeaderWidth, height() - h, width() - kHeaderWidth, h);
    updateScrollRange();
}

void ArrangeView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int scrollH = fScroll->height();
    const QRect content(kHeaderWidth, kRulerHeight, width() - kHeaderWidth, height() - kRulerHeight - scrollH);

    p.fillRect(rect(), theme::window);

    // ---- grid and ruler: bars and beats at the project tempo, 4/4
    const double beat = 60.0 / fBpm;
    const double bar = beat * 4.0;
    p.fillRect(QRect(kHeaderWidth, 0, width() - kHeaderWidth, kRulerHeight), theme::panelLight);
    p.setClipRect(QRect(kHeaderWidth, 0, width() - kHeaderWidth, height() - scrollH));

    const double firstSecond = xToSeconds(kHeaderWidth);
    const double lastSecond = xToSeconds(width());
    const bool showBeats = beat * fPixelsPerSecond > 14.0;
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);

    for (int b = static_cast<int>(firstSecond / beat); b * beat <= lastSecond + beat; ++b)
    {
        const int x = secondsToX(b * beat);
        const bool isBar = b % 4 == 0;
        if (!isBar && !showBeats)
            continue;
        p.setPen(isBar ? QColor(0x4a, 0x50, 0x57) : QColor(0x30, 0x34, 0x39));
        p.drawLine(x, kRulerHeight, x, content.bottom());
        p.setPen(isBar ? theme::text : theme::textDim);
        p.drawLine(x, isBar ? 12 : 22, x, kRulerHeight - 1);
        if (isBar && bar * fPixelsPerSecond > 26.0)
            p.drawText(x + 4, 13, QString::number(b / 4 + 1));
    }
    p.setPen(theme::textDim);
    for (int s = static_cast<int>(firstSecond / 10.0) * 10; s <= lastSecond; s += 10)
        p.drawText(secondsToX(s) + 4, kRulerHeight - 4, formatTime(s));

    // ---- the audio lane
    const QRect lane = laneRect();
    p.fillRect(QRect(kHeaderWidth, lane.top(), width(), lane.height()), QColor(0x26, 0x2a, 0x2f));
    p.setPen(theme::border);
    p.drawLine(kHeaderWidth, lane.bottom(), width(), lane.bottom());

    if (fHasTrack && fSeconds > 0.0)
    {
        const int x0 = secondsToX(0.0);
        const int x1 = secondsToX(fSeconds);
        const QRect clip(x0, lane.top() + 2, x1 - x0, lane.height() - 4);
        const QColor body = fMuted ? theme::event.darker(170) : theme::event;

        p.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath shape;
        shape.addRoundedRect(clip, 4, 4);
        p.fillPath(shape, body);
        p.fillRect(QRect(clip.left(), clip.top(), clip.width(), 16), body.darker(125));
        p.setRenderHint(QPainter::Antialiasing, false);

        p.setPen(Qt::white);
        p.drawText(QRect(clip.left() + 6, clip.top(), clip.width() - 8, 16), Qt::AlignVCenter, fClipName);

        // waveform: one vertical line per pixel column from the peak buckets
        if (!fPeaks.empty())
        {
            const QRect wave(clip.left(), clip.top() + 18, clip.width(), clip.height() - 20);
            const int mid = wave.center().y();
            const double half = wave.height() / 2.0 - 1.0;
            p.setPen(fMuted ? theme::waveform.lighter(130) : theme::waveform);
            const int from = std::max(wave.left(), kHeaderWidth);
            const int to = std::min(wave.right(), width());
            for (int x = from; x <= to; ++x)
            {
                const size_t a = static_cast<size_t>(double(x - wave.left()) / wave.width() * fPeaks.size());
                const size_t b = std::max(a + 1, static_cast<size_t>(double(x + 1 - wave.left()) / wave.width() * fPeaks.size()));
                float peak = 0.0f;
                for (size_t i = a; i < b && i < fPeaks.size(); ++i)
                    peak = std::max(peak, fPeaks[i]);
                const int dy = static_cast<int>(std::min(1.0f, peak) * half);
                p.drawLine(x, mid - dy, x, mid + dy);
            }
            p.setPen(QColor(0x14, 0x2c, 0x44, 90));
            p.drawLine(wave.left(), mid, wave.right(), mid);
        }
    }

    // ---- project cursor
    const int cx = secondsToX(fPlayhead);
    if (cx >= kHeaderWidth)
    {
        p.setPen(QPen(theme::cursor, 1));
        p.drawLine(cx, 0, cx, content.bottom());
        QPolygon tri;
        tri << QPoint(cx - 6, 0) << QPoint(cx + 6, 0) << QPoint(cx, 8);
        p.setBrush(theme::cursor);
        p.drawPolygon(tri);
    }

    // ---- track header column (drawn last so it covers the scrolled content)
    p.setClipping(false);
    p.fillRect(QRect(0, 0, kHeaderWidth, height()), theme::panel);
    p.setPen(theme::textDim);
    p.drawText(QRect(10, 0, kHeaderWidth - 10, kRulerHeight), Qt::AlignVCenter, QStringLiteral("轨道"));
    p.setPen(theme::border);
    p.drawLine(kHeaderWidth - 1, 0, kHeaderWidth - 1, height());
    p.drawLine(0, kRulerHeight - 1, width(), kRulerHeight - 1);

    if (fHasTrack)
    {
        const QRect header(0, lane.top(), kHeaderWidth - 1, lane.height());
        p.fillRect(header, theme::panelLight);
        p.fillRect(QRect(0, lane.top(), 6, lane.height()), theme::trackColor);
        p.setPen(theme::border);
        p.drawLine(0, lane.bottom(), kHeaderWidth, lane.bottom());

        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(theme::text);
        p.drawText(QRect(14, lane.top() + 8, kHeaderWidth - 60, 22), Qt::AlignVCenter, fTrackName);
        p.setFont(small);
        p.setPen(theme::textDim);
        p.drawText(QRect(14, lane.top() + 32, kHeaderWidth - 20, 18), Qt::AlignVCenter,
                   QStringLiteral("音频 · %1").arg(formatTime(fSeconds)));

        const QRect m = muteRect();
        p.fillRect(m, fMuted ? theme::mute : theme::panel);
        p.setBrush(Qt::NoBrush);
        p.setPen(fMuted ? Qt::black : theme::text);
        p.drawRect(m.adjusted(0, 0, -1, -1));
        p.drawText(m, Qt::AlignCenter, QStringLiteral("M"));
    }
    else
    {
        p.setPen(theme::textDim);
        p.drawText(QRect(kHeaderWidth, lane.top(), width() - kHeaderWidth, lane.height()), Qt::AlignCenter,
                   QStringLiteral("把音频文件拖到这里，或用 文件 → 打开音频文件"));
    }
}

void ArrangeView::mousePressEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    if (fHasTrack && muteRect().contains(pos))
    {
        fMuted = !fMuted;
        emit muteToggled(fMuted);
        update();
        return;
    }
    if (pos.x() >= kHeaderWidth && pos.y() < height() - fScroll->height())
        emit seekRequested(xToSeconds(pos.x()));
}

void ArrangeView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
    if (event->modifiers() & Qt::ControlModifier)
    {
        // zoom around the mouse position
        const int mx = static_cast<int>(event->position().x());
        const double anchor = xToSeconds(mx);
        fPixelsPerSecond = std::clamp(fPixelsPerSecond * (delta > 0 ? 1.25 : 0.8), 5.0, 2000.0);
        updateScrollRange();
        fScroll->setValue(static_cast<int>(anchor * fPixelsPerSecond) - (mx - kHeaderWidth));
        update();
    }
    else
    {
        fScroll->setValue(fScroll->value() - delta);
    }
    event->accept();
}

void ArrangeView::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void ArrangeView::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile())
        emit fileDropped(urls.first().toLocalFile());
}

// ---------------------------------------------------------------------------------------------------------------------

InsertRack::InsertRack(int slotCount, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    for (int i = 0; i < slotCount; ++i)
    {
        Row row;
        auto* line = new QHBoxLayout;
        line->setSpacing(2);

        row.power = new QToolButton;
        row.power->setObjectName(QStringLiteral("insertPower"));
        row.power->setCheckable(true);
        row.power->setFixedSize(14, 20);
        row.power->setToolTip(QStringLiteral("启用 / 旁路"));
        line->addWidget(row.power);

        row.name = new QPushButton;
        row.name->setObjectName(QStringLiteral("insertSlot"));
        row.name->setFixedHeight(20);
        row.name->setContextMenuPolicy(Qt::CustomContextMenu);
        line->addWidget(row.name, 1);
        layout->addLayout(line);

        connect(row.power, &QToolButton::toggled, this, [this, i](bool on) { emit activeToggled(i, on); });
        connect(row.name, &QPushButton::clicked, this, [this, i] {
            if (i < fFilled)
                emit editRequested(i);
            else
                emit addRequested();
        });
        connect(row.name, &QPushButton::customContextMenuRequested, this, [this, i](const QPoint& at) {
            if (i >= fFilled)
                return;
            QMenu menu;
            menu.addAction(QStringLiteral("编辑…"), this, [this, i] { emit editRequested(i); });
            menu.addAction(QStringLiteral("上移"), this, [this, i] { emit moveRequested(i, -1); })->setEnabled(i > 0);
            menu.addAction(QStringLiteral("下移"), this, [this, i] { emit moveRequested(i, +1); })->setEnabled(i + 1 < fFilled);
            menu.addSeparator();
            menu.addAction(QStringLiteral("移除"), this, [this, i] { emit removeRequested(i); });
            menu.exec(fRows[i].name->mapToGlobal(at));
        });
        fRows.push_back(row);
    }
    setSlots({});
}

void InsertRack::setSlots(const std::vector<Slot>& items)
{
    fFilled = static_cast<int>(std::min(items.size(), fRows.size()));
    for (int i = 0; i < static_cast<int>(fRows.size()); ++i)
    {
        Row& row = fRows[i];
        const bool filled = i < fFilled;
        const QSignalBlocker blocker(row.power);
        row.power->setEnabled(filled);
        row.power->setChecked(filled && items[i].active);
        row.name->setText(filled ? items[i].name : (i == fFilled ? QStringLiteral("＋ 添加插件") : QString()));
        row.name->setEnabled(i <= fFilled);
        row.name->setToolTip(filled ? QStringLiteral("单击编辑，右键更多操作") : QString());
        row.name->setProperty("filled", filled);
        row.name->setProperty("bypassed", filled && !items[i].active);
        row.name->style()->unpolish(row.name);
        row.name->style()->polish(row.name);
    }
}

// ---------------------------------------------------------------------------------------------------------------------

ChannelStrip::ChannelStrip(const QString& name, bool isOutput, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("channelStrip"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(isOutput ? 110 : 170);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(5);

    if (!isOutput)
    {
        auto* title = new QLabel(QStringLiteral("插入效果"));
        title->setStyleSheet(QStringLiteral("color: #8b9096;"));
        layout->addWidget(title);
        fInserts = new InsertRack(8, this);
        layout->addWidget(fInserts);
    }

    fMute = new QToolButton;
    fMute->setObjectName(QStringLiteral("mute"));
    fMute->setText(QStringLiteral("M"));
    fMute->setCheckable(true);
    fMute->setToolTip(QStringLiteral("静音"));
    fMute->setVisible(!isOutput);
    layout->addWidget(fMute, 0, Qt::AlignHCenter);
    connect(fMute, &QToolButton::toggled, this, &ChannelStrip::muteToggled);

    auto* faderRow = new QHBoxLayout;
    faderRow->addStretch();
    fFader = new QSlider(Qt::Vertical);
    fFader->setRange(0, kFaderSteps);
    fFader->setMinimumHeight(100);
    fFader->setToolTip(QStringLiteral("音量"));
    faderRow->addWidget(fFader);
    fMeter = new LevelMeter;
    faderRow->addWidget(fMeter);
    faderRow->addStretch();
    layout->addLayout(faderRow, 1);
    connect(fFader, &QSlider::valueChanged, this, [this](int pos) {
        const float v = faderToVolume(pos);
        updateVolumeLabel(v);
        emit volumeChanged(v);
    });

    fVolumeLabel = new QLabel;
    fVolumeLabel->setAlignment(Qt::AlignCenter);
    fVolumeLabel->setObjectName(QStringLiteral("lcdSmall"));
    layout->addWidget(fVolumeLabel);

    fName = new QLabel(name);
    fName->setObjectName(isOutput ? QStringLiteral("channelNameOut") : QStringLiteral("channelName"));
    fName->setAlignment(Qt::AlignCenter);
    layout->addWidget(fName);

    if (isOutput)
    {
        // the rack has no master bus to scale, the output strip only meters
        fFader->setVisible(false);
        fVolumeLabel->setText(QStringLiteral("输出"));
    }
    else
    {
        setVolume(1.0f);
    }
}

void ChannelStrip::setLevels(float left, float right)
{
    fMeter->setLevels(left, right);
}

void ChannelStrip::setVolume(float volume)
{
    const QSignalBlocker blocker(fFader);
    fFader->setValue(volumeToFader(volume));
    updateVolumeLabel(volume);
}

void ChannelStrip::setMuted(bool muted)
{
    const QSignalBlocker blocker(fMute);
    fMute->setChecked(muted);
}

void ChannelStrip::setName(const QString& name)
{
    fName->setText(name);
}

void ChannelStrip::setFaderEnabled(bool enabled)
{
    fFader->setEnabled(enabled);
    fMute->setEnabled(enabled);
}

void ChannelStrip::updateVolumeLabel(float volume)
{
    const float db = toDb(volume);
    fVolumeLabel->setText(db <= -100.0f ? QStringLiteral("-∞") : QString::number(db, 'f', 1));
}
