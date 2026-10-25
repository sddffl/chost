#include "Widgets.h"

#include "Theme.h"

#include <QContextMenuEvent>
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
    fVScroll = new QScrollBar(Qt::Vertical, this);
    connect(fScroll, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    connect(fVScroll, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    updateScrollRange();
}

void ArrangeView::setLanes(const std::vector<Lane>& lanes)
{
    fLanes = lanes;
    fLongest = 0.0;
    for (const Lane& lane : fLanes)
        fLongest = std::max(fLongest, lane.seconds);
    if (fSelected >= static_cast<int>(fLanes.size()))
        fSelected = static_cast<int>(fLanes.size()) - 1;
    updateScrollRange();
    update();
}

void ArrangeView::setLaneState(int lane, bool muted, bool solo)
{
    if (lane < 0 || lane >= static_cast<int>(fLanes.size()))
        return;
    fLanes[lane].muted = muted;
    fLanes[lane].solo = solo;
    update();
}

void ArrangeView::setSelected(int lane)
{
    fSelected = lane;
    // keep the selected track in view
    if (lane >= 0)
    {
        const int top = lane * kLaneHeight;
        const int visible = height() - kRulerHeight - fScroll->height();
        if (top < fVScroll->value())
            fVScroll->setValue(top);
        else if (top + kLaneHeight > fVScroll->value() + visible)
            fVScroll->setValue(top + kLaneHeight - visible);
    }
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
        if (x > width() - fVScroll->width() - 20 || x < kHeaderWidth)
            fScroll->setValue(static_cast<int>(seconds * fPixelsPerSecond) - 40);
    }
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

int ArrangeView::laneAt(int y) const
{
    if (y < kRulerHeight || y >= height() - fScroll->height())
        return -1;
    const int lane = (y - kRulerHeight + fVScroll->value()) / kLaneHeight;
    return lane < static_cast<int>(fLanes.size()) ? lane : -1;
}

QRect ArrangeView::laneRect(int lane) const
{
    return {0, kRulerHeight + lane * kLaneHeight - fVScroll->value(), width(), kLaneHeight};
}

QRect ArrangeView::muteRect(int lane) const
{
    const QRect r = laneRect(lane);
    return {kHeaderWidth - 58, r.top() + 8, 22, 18};
}

QRect ArrangeView::soloRect(int lane) const
{
    const QRect r = laneRect(lane);
    return {kHeaderWidth - 32, r.top() + 8, 22, 18};
}

void ArrangeView::updateScrollRange()
{
    const int areaWidth = std::max(1, width() - kHeaderWidth - fVScroll->sizeHint().width());
    const double visible = areaWidth / fPixelsPerSecond;
    const double total = std::max(fLongest + 10.0, 60.0);
    fScroll->setRange(0, std::max(0, static_cast<int>((total - visible) * fPixelsPerSecond)));
    fScroll->setPageStep(areaWidth);
    fScroll->setSingleStep(20);

    // one empty lane's worth of room below the last track, to drop files on
    const int areaHeight = std::max(1, height() - kRulerHeight - fScroll->sizeHint().height());
    const int content = (static_cast<int>(fLanes.size()) + 1) * kLaneHeight;
    fVScroll->setRange(0, std::max(0, content - areaHeight));
    fVScroll->setPageStep(areaHeight);
    fVScroll->setSingleStep(kLaneHeight / 3);
}

void ArrangeView::resizeEvent(QResizeEvent*)
{
    const int h = fScroll->sizeHint().height();
    const int w = fVScroll->sizeHint().width();
    fScroll->setGeometry(kHeaderWidth, height() - h, width() - kHeaderWidth - w, h);
    fVScroll->setGeometry(width() - w, kRulerHeight, w, height() - kRulerHeight - h);
    updateScrollRange();
}

void ArrangeView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int scrollH = fScroll->height();
    const int right = width() - fVScroll->width();
    const QRect content(kHeaderWidth, kRulerHeight, right - kHeaderWidth, height() - kRulerHeight - scrollH);

    p.fillRect(rect(), theme::window);

    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);

    // ---- lane backgrounds, then grid lines over them
    p.setClipRect(content);
    for (int i = 0; i < static_cast<int>(fLanes.size()); ++i)
    {
        const QRect lane = laneRect(i);
        p.fillRect(QRect(kHeaderWidth, lane.top(), right, lane.height()),
                   i == fSelected ? QColor(0x2c, 0x31, 0x37) : QColor(0x26, 0x2a, 0x2f));
        p.setPen(theme::border);
        p.drawLine(kHeaderWidth, lane.bottom(), right, lane.bottom());
    }

    const double beat = 60.0 / fBpm;
    const double bar = beat * 4.0;
    const double firstSecond = xToSeconds(kHeaderWidth);
    const double lastSecond = xToSeconds(right);
    const bool showBeats = beat * fPixelsPerSecond > 14.0;
    for (int b = static_cast<int>(firstSecond / beat); b * beat <= lastSecond + beat; ++b)
    {
        const bool isBar = b % 4 == 0;
        if (!isBar && !showBeats)
            continue;
        const int x = secondsToX(b * beat);
        p.setPen(isBar ? QColor(0x4a, 0x50, 0x57, 160) : QColor(0x3a, 0x3f, 0x45, 110));
        p.drawLine(x, kRulerHeight, x, content.bottom());
    }

    for (int i = 0; i < static_cast<int>(fLanes.size()); ++i)
        paintLane(p, i);

    if (fLanes.empty())
    {
        p.setPen(theme::textDim);
        p.drawText(content, Qt::AlignCenter, QStringLiteral("把音频文件拖到这里（可以一次拖多个），每个文件会成为一条音轨"));
    }

    // ---- ruler: bars and beats at the project tempo, 4/4
    p.setClipRect(QRect(kHeaderWidth, 0, right - kHeaderWidth, height() - scrollH));
    p.fillRect(QRect(kHeaderWidth, 0, right - kHeaderWidth, kRulerHeight), theme::panelLight);
    for (int b = static_cast<int>(firstSecond / beat); b * beat <= lastSecond + beat; ++b)
    {
        const bool isBar = b % 4 == 0;
        if (!isBar && !showBeats)
            continue;
        const int x = secondsToX(b * beat);
        p.setPen(isBar ? theme::text : theme::textDim);
        p.drawLine(x, isBar ? 12 : 22, x, kRulerHeight - 1);
        if (isBar && bar * fPixelsPerSecond > 26.0)
            p.drawText(x + 4, 13, QString::number(b / 4 + 1));
    }
    p.setPen(theme::textDim);
    for (int s = static_cast<int>(firstSecond / 10.0) * 10; s <= lastSecond; s += 10)
        p.drawText(secondsToX(s) + 4, kRulerHeight - 4, formatTime(s));

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
        p.setBrush(Qt::NoBrush);
    }

    // ---- track header column (drawn last so it covers the scrolled content)
    p.setClipRect(QRect(0, 0, kHeaderWidth, height()));
    p.fillRect(QRect(0, 0, kHeaderWidth, height()), theme::panel);
    p.setClipRect(QRect(0, kRulerHeight, kHeaderWidth, height() - kRulerHeight));
    for (int i = 0; i < static_cast<int>(fLanes.size()); ++i)
    {
        const Lane& l = fLanes[i];
        const QRect lane = laneRect(i);
        const QRect header(0, lane.top(), kHeaderWidth - 1, lane.height());
        p.fillRect(header, i == fSelected ? theme::selection : theme::panelLight);
        p.fillRect(QRect(0, lane.top(), 6, lane.height()), l.colour);
        p.setPen(theme::border);
        p.drawLine(0, lane.bottom(), kHeaderWidth, lane.bottom());

        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(theme::text);
        p.drawText(QRect(14, lane.top() + 6, kHeaderWidth - 80, 22), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(l.name, Qt::ElideRight, kHeaderWidth - 80));
        p.setFont(small);
        p.setPen(theme::textDim);
        p.drawText(QRect(14, lane.top() + 30, kHeaderWidth - 20, 18), Qt::AlignVCenter,
                   l.clipName.isEmpty() ? QStringLiteral("音频 · 空") : QStringLiteral("音频 · %1").arg(formatTime(l.seconds)));

        auto button = [&](const QRect& r, bool on, const QColor& colour, const QString& text) {
            p.fillRect(r, on ? colour : theme::panel);
            p.setPen(on ? Qt::black : theme::text);
            p.drawRect(r.adjusted(0, 0, -1, -1));
            p.drawText(r, Qt::AlignCenter, text);
        };
        button(muteRect(i), l.muted, theme::mute, QStringLiteral("M"));
        button(soloRect(i), l.solo, theme::solo, QStringLiteral("S"));
    }

    p.setClipping(false);
    p.fillRect(QRect(0, 0, kHeaderWidth, kRulerHeight), theme::panel);
    p.setPen(theme::textDim);
    p.drawText(QRect(10, 0, kHeaderWidth - 10, kRulerHeight), Qt::AlignVCenter,
               QStringLiteral("轨道（%1）").arg(fLanes.size()));
    p.setPen(theme::border);
    p.drawLine(kHeaderWidth - 1, 0, kHeaderWidth - 1, height());
    p.drawLine(0, kRulerHeight - 1, right, kRulerHeight - 1);
}

void ArrangeView::paintLane(QPainter& p, int i)
{
    const Lane& l = fLanes[i];
    if (l.clipName.isEmpty() || l.seconds <= 0.0)
        return;

    const QRect lane = laneRect(i);
    const bool silent = l.muted || (!l.solo && std::any_of(fLanes.begin(), fLanes.end(), [](const Lane& o) { return o.solo; }));
    const int x0 = secondsToX(0.0);
    const int x1 = secondsToX(l.seconds);
    const QRect clip(x0, lane.top() + 2, x1 - x0, lane.height() - 5);
    const QColor body = silent ? l.colour.darker(220) : l.colour.lighter(135);
    const QColor wave = silent ? l.colour.darker(140) : l.colour.darker(230);

    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath shape;
    shape.addRoundedRect(clip, 4, 4);
    p.fillPath(shape, body);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRect(clip.left(), clip.top(), clip.width(), 15), body.darker(125));

    p.setPen(Qt::white);
    const int labelLeft = std::max(clip.left(), kHeaderWidth) + 6;
    p.drawText(QRect(labelLeft, clip.top(), std::max(0, clip.right() - labelLeft - 4), 15), Qt::AlignVCenter, l.clipName);

    // waveform: one vertical line per pixel column from the peak buckets
    if (l.peaks.empty())
        return;
    const QRect area(clip.left(), clip.top() + 16, clip.width(), clip.height() - 17);
    const int mid = area.center().y();
    const double half = area.height() / 2.0 - 1.0;
    p.setPen(wave);
    const int from = std::max(area.left(), kHeaderWidth);
    const int to = std::min(area.right(), width());
    const size_t count = l.peaks.size();
    for (int x = from; x <= to; ++x)
    {
        const size_t a = static_cast<size_t>(double(x - area.left()) / area.width() * count);
        const size_t b = std::max(a + 1, static_cast<size_t>(double(x + 1 - area.left()) / area.width() * count));
        float peak = 0.0f;
        for (size_t k = a; k < b && k < count; ++k)
            peak = std::max(peak, l.peaks[k]);
        const int dy = static_cast<int>(std::min(1.0f, peak) * half);
        p.drawLine(x, mid - dy, x, mid + dy);
    }
}

void ArrangeView::mousePressEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    const int lane = laneAt(pos.y());
    if (lane >= 0 && pos.x() < kHeaderWidth)
    {
        Lane& l = fLanes[lane];
        if (muteRect(lane).contains(pos))
        {
            l.muted = !l.muted;
            emit muteToggled(lane, l.muted);
        }
        else if (soloRect(lane).contains(pos))
        {
            l.solo = !l.solo;
            emit soloToggled(lane, l.solo);
        }
        fSelected = lane;
        emit trackSelected(lane);
        update();
        return;
    }
    if (pos.x() >= kHeaderWidth && pos.y() < height() - fScroll->height())
    {
        if (lane >= 0 && lane != fSelected && event->button() == Qt::LeftButton && pos.y() >= kRulerHeight)
        {
            fSelected = lane;
            emit trackSelected(lane);
        }
        if (event->button() == Qt::LeftButton)
            emit seekRequested(xToSeconds(pos.x()));
    }
}

void ArrangeView::mouseDoubleClickEvent(QMouseEvent* event)
{
    // double click below the tracks adds one, as in Cubase
    const QPoint pos = event->position().toPoint();
    if (pos.y() > kRulerHeight && laneAt(pos.y()) < 0)
        emit trackMenuRequested(-1, mapToGlobal(pos));
}

void ArrangeView::contextMenuEvent(QContextMenuEvent* event)
{
    const int lane = event->pos().y() > kRulerHeight ? laneAt(event->pos().y()) : -2;
    if (lane == -2)
        return;
    if (lane >= 0)
    {
        fSelected = lane;
        emit trackSelected(lane);
        update();
    }
    emit trackMenuRequested(lane, event->globalPos());
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
    else if (event->modifiers() & Qt::ShiftModifier)
    {
        fScroll->setValue(fScroll->value() - delta);
    }
    else
    {
        fVScroll->setValue(fVScroll->value() - delta / 2);
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
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (paths.isEmpty())
        return;
    // one file on a track replaces its audio, anything else makes new tracks
    const int lane = paths.size() == 1 ? laneAt(event->position().toPoint().y()) : -1;
    emit filesDropped(paths, lane);
    event->acceptProposedAction();
}

// ---------------------------------------------------------------------------------------------------------------------

InsertRack::InsertRack(int slotCount, QWidget* parent, int rowHeight)
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
        row.power->setFixedSize(14, rowHeight);
        row.power->setToolTip(QStringLiteral("启用 / 旁路"));
        line->addWidget(row.power);

        row.name = new QPushButton;
        row.name->setObjectName(QStringLiteral("insertSlot"));
        row.name->setFixedHeight(rowHeight);
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
    setToolTip(items.size() > fRows.size() ? QStringLiteral("还有 %1 个插件，在左边的检查器里能看到全部").arg(items.size() - fRows.size())
                                           : QString());
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
    setFixedWidth(isOutput ? 110 : 150);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(5);

    if (!isOutput)
    {
        auto* title = new QLabel(QStringLiteral("插入效果"));
        title->setStyleSheet(QStringLiteral("color: #8b9096;"));
        layout->addWidget(title);
        fInserts = new InsertRack(6, this, 17);
        layout->addWidget(fInserts);
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(4);
    buttons->addStretch();
    fMute = new QToolButton;
    fMute->setObjectName(QStringLiteral("mute"));
    fMute->setText(QStringLiteral("M"));
    fMute->setCheckable(true);
    fMute->setToolTip(QStringLiteral("静音"));
    buttons->addWidget(fMute);
    fSolo = new QToolButton;
    fSolo->setObjectName(QStringLiteral("solo"));
    fSolo->setText(QStringLiteral("S"));
    fSolo->setCheckable(true);
    fSolo->setToolTip(QStringLiteral("独奏"));
    buttons->addWidget(fSolo);
    buttons->addStretch();
    fMute->setVisible(!isOutput);
    fSolo->setVisible(!isOutput);
    layout->addLayout(buttons);
    connect(fMute, &QToolButton::toggled, this, &ChannelStrip::muteToggled);
    connect(fSolo, &QToolButton::toggled, this, &ChannelStrip::soloToggled);

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

    fName = new QToolButton;
    fName->setObjectName(QStringLiteral("channelName"));
    fName->setText(name);
    fName->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(fName);
    connect(fName, &QToolButton::clicked, this, &ChannelStrip::selected);
    if (isOutput)
        setColour(QColor(0x6b, 0x4f, 0xa0));

    setVolume(1.0f);
}

void ChannelStrip::mousePressEvent(QMouseEvent* event)
{
    emit selected();
    QWidget::mousePressEvent(event);
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

void ChannelStrip::setSolo(bool solo)
{
    const QSignalBlocker blocker(fSolo);
    fSolo->setChecked(solo);
}

void ChannelStrip::setName(const QString& name)
{
    fName->setText(name);
}

void ChannelStrip::setColour(const QColor& colour)
{
    fName->setStyleSheet(QStringLiteral("background: %1;").arg(colour.name()));
}

void ChannelStrip::setSelected(bool selected)
{
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
}

void ChannelStrip::updateVolumeLabel(float volume)
{
    const float db = toDb(volume);
    fVolumeLabel->setText(db <= -100.0f ? QStringLiteral("-∞") : QString::number(db, 'f', 1));
}
