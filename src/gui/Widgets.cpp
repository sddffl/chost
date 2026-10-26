#include "Widgets.h"

#include "Theme.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
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

PanControl::PanControl(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("pan"));
    setFixedHeight(15);
    setMinimumWidth(44);
    setCursor(Qt::SizeHorCursor);
    setToolTip(QStringLiteral("声像：左右拖动，双击回到中间"));
}

QString PanControl::text(float pan)
{
    const int amount = static_cast<int>(std::lround(std::abs(pan) * 100.0f));
    if (amount == 0)
        return QStringLiteral("C");
    return (pan < 0.0f ? QStringLiteral("L%1") : QStringLiteral("R%1")).arg(amount);
}

void PanControl::setPan(float pan)
{
    fPan = std::clamp(pan, -1.0f, 1.0f);
    update();
}

void PanControl::change(float pan)
{
    pan = std::round(std::clamp(pan, -1.0f, 1.0f) * 100.0f) / 100.0f;
    if (pan == fPan)
        return;
    fPan = pan;
    update();
    emit panChanged(fPan);
}

void PanControl::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), theme::lcd);
    const int mid = width() / 2;
    const int x = mid + static_cast<int>(std::lround(fPan * (width() / 2 - 1)));
    p.fillRect(QRect(std::min(mid, x), 2, std::abs(x - mid) + 1, height() - 4), theme::accent.darker(130));
    p.fillRect(QRect(mid, 0, 1, height()), theme::textDim);
    p.fillRect(QRect(std::clamp(x - 1, 0, width() - 2), 0, 2, height()), theme::text);
    // the value on a dark patch of its own, readable wherever the marker is
    QFont font = p.font();
    font.setPixelSize(10);
    p.setFont(font);
    const QString label = text(fPan);
    const int w = p.fontMetrics().horizontalAdvance(label) + 6;
    const QRect box((width() - w) / 2, 2, w, height() - 4);
    p.fillRect(box, QColor(theme::lcd.red(), theme::lcd.green(), theme::lcd.blue(), 200));
    p.setPen(theme::text);
    p.drawText(box, Qt::AlignCenter, label);
}

void PanControl::mousePressEvent(QMouseEvent* event)
{
    fDragX = static_cast<int>(event->position().x());
    fDragPan = fPan;
}

void PanControl::mouseMoveEvent(QMouseEvent* event)
{
    // the whole width is the whole way, finer with Shift
    const float scale = (event->modifiers() & Qt::ShiftModifier) ? 0.25f : 1.0f;
    change(fDragPan + scale * 2.0f * (static_cast<float>(event->position().x()) - fDragX) / std::max(1, width()));
}

void PanControl::mouseDoubleClickEvent(QMouseEvent*)
{
    change(0.0f);
}

void PanControl::wheelEvent(QWheelEvent* event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0)
        change(fPan + 0.05f * steps);
    event->accept();
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
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
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
    fRows.clear();
    fRowCount = 0;
    for (const Lane& lane : fLanes)
        fRows.push_back(lane.hidden ? -1 : fRowCount++);
    fLongest = 0.0;
    for (const Lane& lane : fLanes)
        fLongest = std::max(fLongest, lane.seconds);
    if (fSelected >= static_cast<int>(fLanes.size()))
        fSelected = static_cast<int>(fLanes.size()) - 1;
    updateScrollRange();
    update();
}

void ArrangeView::setMarkers(const std::vector<Marker>& markers)
{
    fMarkers = markers;
    update();
}

void ArrangeView::setRange(double start, double end)
{
    fRangeStart = std::max(0.0, std::min(start, end));
    fRangeEnd = std::max(0.0, std::max(start, end));
    update();
}

double ArrangeView::snap(double seconds, Qt::KeyboardModifiers modifiers) const
{
    if (modifiers & Qt::ShiftModifier)
        return seconds;
    const double beat = 60.0 / fBpm;
    const double step = beat * fPixelsPerSecond >= 8.0 ? beat : beat * 4.0;
    return std::round(seconds / step) * step;
}

QRect ArrangeView::markerRect(int marker) const
{
    const Marker& m = fMarkers[static_cast<size_t>(marker)];
    const int x = secondsToX(m.seconds);
    const int w = fontMetrics().horizontalAdvance(m.name) * 85 / 100 + 12;
    return {x, 1, w, kMarkerBand - 2};
}

int ArrangeView::markerAt(const QPoint& pos) const
{
    if (pos.y() >= kMarkerBand || pos.x() < kHeaderWidth)
        return -1;
    for (int i = static_cast<int>(fMarkers.size()) - 1; i >= 0; --i)
        if (markerRect(i).adjusted(-3, 0, 0, 0).contains(pos))
            return i;
    return -1;
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
        const int top = std::max(0, fRows[static_cast<size_t>(lane)]) * kLaneHeight;
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
    const int row = (y - kRulerHeight + fVScroll->value()) / kLaneHeight;
    const auto it = std::find(fRows.begin(), fRows.end(), row);
    return it == fRows.end() ? -1 : static_cast<int>(it - fRows.begin());
}

QRect ArrangeView::laneRect(int lane) const
{
    const int row = lane >= 0 && lane < static_cast<int>(fRows.size()) ? fRows[static_cast<size_t>(lane)] : -1;
    if (row < 0)
        return {0, -10 * kLaneHeight, width(), kLaneHeight}; // folded away: off the screen
    return {0, kRulerHeight + row * kLaneHeight - fVScroll->value(), width(), kLaneHeight};
}

QRect ArrangeView::foldRect(int lane) const
{
    const QRect r = laneRect(lane);
    return {12 + fLanes[static_cast<size_t>(lane)].depth * 14, r.top() + 8, 16, 18};
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
    const int content = (fRowCount + 1) * kLaneHeight;
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
        if (fLanes[static_cast<size_t>(i)].hidden)
            continue;
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
    {
        if (fLanes[static_cast<size_t>(i)].hidden)
            continue;
        if (fDrag == Drag::Clip && fDragging && i == fDragLane)
        {
            // the clip where it was, faint, and where it goes
            p.setOpacity(0.3);
            paintLane(p, i);
            p.setOpacity(1.0);
            p.save();
            p.translate(fDragDelta * fPixelsPerSecond, 0.0);
            paintLane(p, i);
            p.restore();
            continue;
        }
        paintLane(p, i);
    }

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
        p.drawLine(x, kMarkerBand + (isBar ? 12 : 22), x, kRulerHeight - 1);
        if (isBar && bar * fPixelsPerSecond > 26.0)
            p.drawText(x + 4, kMarkerBand + 13, QString::number(b / 4 + 1));
    }
    p.setPen(theme::textDim);
    for (int s = static_cast<int>(firstSecond / 10.0) * 10; s <= lastSecond; s += 10)
        p.drawText(secondsToX(s) + 4, kRulerHeight - 4, formatTime(s));

    // ---- the range: on the ruler, and over the lanes (the selected one most)
    if (hasRange())
    {
        const int r0 = secondsToX(fRangeStart), r1 = secondsToX(fRangeEnd);
        p.fillRect(QRect(r0, kMarkerBand, r1 - r0, kRulerHeight - kMarkerBand), QColor(0x4a, 0x90, 0xd9, 110));
        p.setClipRect(content);
        p.fillRect(QRect(r0, kRulerHeight, r1 - r0, content.height()), QColor(0x4a, 0x90, 0xd9, 28));
        if (fSelected >= 0)
            p.fillRect(QRect(r0, laneRect(fSelected).top(), r1 - r0, kLaneHeight), QColor(0x4a, 0x90, 0xd9, 60));
        p.setPen(QColor(0x6f, 0xa8, 0xe6));
        p.drawLine(r0, kMarkerBand, r0, content.bottom());
        p.drawLine(r1, kMarkerBand, r1, content.bottom());
        p.setClipRect(QRect(kHeaderWidth, 0, right - kHeaderWidth, height() - scrollH));
    }

    // ---- markers: a flag on the band above the bars, a line down through the tracks
    p.fillRect(QRect(kHeaderWidth, 0, right - kHeaderWidth, kMarkerBand), theme::panel);
    for (int i = 0; i < static_cast<int>(fMarkers.size()); ++i)
    {
        const double at = fDrag == Drag::Marker && fDragging && i == fDragMarker ? fMarkers[i].seconds + fDragDelta : fMarkers[i].seconds;
        const int x = secondsToX(at);
        p.setPen(QPen(QColor(0xe8, 0xa8, 0x38, 150), 1, Qt::DashLine));
        p.drawLine(x, kMarkerBand, x, content.bottom());
        QRect flag = markerRect(i);
        flag.moveLeft(x);
        p.fillRect(flag, QColor(0xe8, 0xa8, 0x38));
        p.setPen(Qt::black);
        p.drawText(flag.adjusted(5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, fMarkers[i].name);
    }

    // ---- project cursor
    const int cx = secondsToX(fPlayhead);
    if (cx >= kHeaderWidth)
    {
        p.setPen(QPen(theme::cursor, 1));
        p.drawLine(cx, 0, cx, content.bottom());
        QPolygon tri;
        tri << QPoint(cx - 6, kMarkerBand) << QPoint(cx + 6, kMarkerBand) << QPoint(cx, kMarkerBand + 8);
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
        if (l.hidden)
            continue;
        const QRect lane = laneRect(i);
        const QRect header(0, lane.top(), kHeaderWidth - 1, lane.height());
        p.fillRect(header, i == fSelected ? theme::selection : theme::panelLight);
        p.fillRect(QRect(0, lane.top(), 6, lane.height()), l.colour);
        // a track in a group: a line in the group's colour down the indent
        const int indent = l.depth * 14;
        for (int d = 0; d < l.depth; ++d)
            p.fillRect(QRect(12 + d * 14 + 6, lane.top(), 2, lane.height()), theme::border.lighter(140));
        p.setPen(theme::border);
        p.drawLine(0, lane.bottom(), kHeaderWidth, lane.bottom());

        int left = 14 + indent;
        if (l.folder)
        {
            // the fold arrow
            const QRect f = foldRect(i);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(theme::text);
            QPolygonF arrow;
            const QPointF c = QRectF(f).center();
            if (l.folded)
                arrow << c + QPointF(-3, -5) << c + QPointF(4, 0) << c + QPointF(-3, 5);
            else
                arrow << c + QPointF(-5, -3) << c + QPointF(5, -3) << c + QPointF(0, 4);
            p.drawPolygon(arrow);
            p.setBrush(Qt::NoBrush);
            p.setRenderHint(QPainter::Antialiasing, false);
            left += 18;
        }
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(theme::text);
        p.drawText(QRect(left, lane.top() + 6, kHeaderWidth - 66 - left, 22), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(l.name, Qt::ElideRight, kHeaderWidth - 66 - left));
        p.setFont(small);
        p.setPen(theme::textDim);
        const QString kind = l.kindText.isEmpty() ? QStringLiteral("音频") : l.kindText;
        const QString info = l.folder ? kind
                             : l.clipName.isEmpty() ? kind + QStringLiteral(" · 空")
                                                    : QStringLiteral("%1 · %2").arg(kind, formatTime(l.seconds));
        p.drawText(QRect(14 + indent, lane.top() + 30, kHeaderWidth - 20 - indent, 18), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(info, Qt::ElideMiddle, kHeaderWidth - 24 - indent));

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
    if (l.folder)
    {
        // a group: a band as long as its tracks, in its colour
        if (l.seconds <= 0.0)
            return;
        const QRect lane = laneRect(i);
        const int x0 = secondsToX(0.0);
        const int x1 = secondsToX(l.seconds);
        QColor band = l.colour;
        band.setAlpha(l.muted ? 40 : 90);
        p.fillRect(QRect(x0, lane.top() + 10, x1 - x0, lane.height() - 21), band);
        p.setPen(l.colour);
        p.drawRect(QRect(x0, lane.top() + 10, x1 - x0, lane.height() - 21).adjusted(0, 0, -1, -1));
        return;
    }
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

    if (l.midi)
    {
        // a small piano roll: the clip's pitch range fills the clip's height
        if (l.notes.empty())
            return;
        const QRect area(clip.left(), clip.top() + 17, clip.width(), clip.height() - 19);
        int low = 127, high = 0;
        for (const NoteBar& n : l.notes)
        {
            low = std::min(low, n.pitch);
            high = std::max(high, n.pitch);
        }
        const int span = std::max(12, high - low + 1);
        low -= (span - (high - low + 1)) / 2;
        const double rowHeight = static_cast<double>(area.height()) / span;
        const int barHeight = std::max(2, static_cast<int>(rowHeight));
        for (const NoteBar& n : l.notes)
        {
            const int x0 = std::max(secondsToX(n.start), kHeaderWidth);
            const int x1 = std::max(x0 + 2, secondsToX(n.end));
            if (x1 < kHeaderWidth || x0 > width())
                continue;
            const int y = area.bottom() - static_cast<int>((n.pitch - low + 1) * rowHeight);
            p.fillRect(QRect(x0, y, x1 - x0, barHeight), wave);
        }
        return;
    }

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
        if (l.folder && foldRect(lane).adjusted(-4, -4, 4, 4).contains(pos))
        {
            emit foldToggled(lane, !l.folded);
            return;
        }
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
        if (event->button() != Qt::LeftButton)
            return;
        fPressPos = pos;
        fPressSeconds = xToSeconds(pos.x());
        fDragging = false;
        fDragDelta = 0.0;
        const int marker = markerAt(pos);
        if (marker >= 0)
        {
            fDrag = Drag::Marker;
            fDragMarker = marker;
            emit seekRequested(fMarkers[static_cast<size_t>(marker)].seconds);
            return;
        }
        if (pos.y() < kRulerHeight)
        {
            fDrag = Drag::Range; // a click seeks, a drag marks a range
            emit seekRequested(fPressSeconds);
            return;
        }
        if (lane >= 0 && lane != fSelected)
        {
            fSelected = lane;
            emit trackSelected(lane);
        }
        // on a clip: a drag moves it
        if (lane >= 0 && !fLanes[lane].clipName.isEmpty() && fPressSeconds <= fLanes[lane].seconds)
        {
            fDrag = Drag::Clip;
            fDragLane = lane;
        }
        emit seekRequested(fPressSeconds);
    }
}

void ArrangeView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    if (fDrag == Drag::None)
    {
        setCursor(markerAt(pos) >= 0 ? Qt::SizeHorCursor : pos.y() < kRulerHeight && pos.x() >= kHeaderWidth ? Qt::IBeamCursor : Qt::ArrowCursor);
        return;
    }
    if (!fDragging && std::abs(pos.x() - fPressPos.x()) < 4)
        return;
    fDragging = true;
    const double at = xToSeconds(pos.x());
    switch (fDrag)
    {
    case Drag::Range:
        setRange(snap(fPressSeconds, event->modifiers()), snap(at, event->modifiers()));
        break;
    case Drag::Clip:
    {
        // whole beats, so the music stays on the grid; never all the way out of the song
        const Lane& l = fLanes[fDragLane];
        fDragDelta = std::max(snap(at - fPressSeconds, event->modifiers()), -std::max(0.0, l.seconds - 0.05));
        break;
    }
    case Drag::Marker:
        fDragDelta = snap(fMarkers[static_cast<size_t>(fDragMarker)].seconds + at - fPressSeconds, event->modifiers()) -
                     fMarkers[static_cast<size_t>(fDragMarker)].seconds;
        fDragDelta = std::max(fDragDelta, -fMarkers[static_cast<size_t>(fDragMarker)].seconds);
        break;
    default:
        break;
    }
    update();
}

void ArrangeView::mouseReleaseEvent(QMouseEvent*)
{
    const Drag drag = fDrag;
    fDrag = Drag::None;
    if (fDragging)
    {
        if (drag == Drag::Range)
            emit rangeChanged(fRangeStart, fRangeEnd);
        else if (drag == Drag::Clip && std::abs(fDragDelta) > 1e-6)
            emit clipMoveRequested(fDragLane, fDragDelta);
        else if (drag == Drag::Marker && std::abs(fDragDelta) > 1e-6)
            emit markerMoved(fDragMarker, fMarkers[static_cast<size_t>(fDragMarker)].seconds + fDragDelta);
    }
    fDragging = false;
    fDragDelta = 0.0;
    update();
}

void ArrangeView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && hasRange())
    {
        setRange(0.0, 0.0);
        emit rangeChanged(0.0, 0.0);
        return;
    }
    QWidget::keyPressEvent(event);
}

void ArrangeView::mouseDoubleClickEvent(QMouseEvent* event)
{
    // double click below the tracks adds one, as in Cubase
    const QPoint pos = event->position().toPoint();
    if (const int marker = markerAt(pos); marker >= 0)
    {
        emit markerRenameRequested(marker);
        return;
    }
    const int lane = pos.y() > kRulerHeight ? laneAt(pos.y()) : -2;
    if (lane == -1)
        emit trackMenuRequested(-1, mapToGlobal(pos));
    else if (lane >= 0 && pos.x() >= kHeaderWidth)
        emit clipDoubleClicked(lane); // opens the MIDI editor, as in Cubase
}

void ArrangeView::contextMenuEvent(QContextMenuEvent* event)
{
    if (const int marker = markerAt(event->pos()); marker >= 0)
    {
        emit markerMenuRequested(marker, event->globalPos());
        return;
    }
    const int lane = event->pos().y() > kRulerHeight ? laneAt(event->pos().y()) : -2;
    if (lane == -2)
    {
        if (event->pos().x() >= kHeaderWidth)
            emit markerMenuRequested(-1, event->globalPos()); // the ruler: add a marker, the range
        return;
    }
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
        fInserts = new InsertRack(4, this, 17);
        layout->addWidget(fInserts);

        // sends, after the fader, to effect buses (a reverb, a delay)
        auto* sendTitle = new QLabel(QStringLiteral("发送"));
        sendTitle->setStyleSheet(QStringLiteral("color: #8b9096;"));
        layout->addWidget(sendTitle);
        for (int i = 0; i < kSendSlots; ++i)
        {
            SendRow row;
            auto* line = new QHBoxLayout;
            line->setSpacing(3);
            row.name = new QToolButton;
            row.name->setObjectName(QStringLiteral("sendSlot"));
            row.name->setFixedHeight(17);
            row.name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            line->addWidget(row.name, 1);
            row.level = new QSlider(Qt::Horizontal);
            row.level->setRange(0, kFaderSteps);
            row.level->setFixedWidth(52);
            line->addWidget(row.level);
            layout->addLayout(line);
            connect(row.name, &QToolButton::clicked, this, [this, i] {
                emit sendMenuRequested(i, fSendRows[static_cast<size_t>(i)].name->mapToGlobal(QPoint(0, fSendRows[static_cast<size_t>(i)].name->height())));
            });
            connect(row.level, &QSlider::valueChanged, this, [this, i](int pos) {
                const float v = faderToVolume(pos);
                fSendRows[static_cast<size_t>(i)].level->setToolTip(QStringLiteral("发送量 %1 dB").arg(toDb(v), 0, 'f', 1));
                emit sendLevelChanged(i, v);
            });
            fSendRows.push_back(row);
        }
        setSends({});
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
    if (!isOutput)
    {
        fPan = new PanControl;
        layout->addWidget(fPan);
        connect(fPan, &PanControl::panChanged, this, &ChannelStrip::panChanged);
    }
    layout->addLayout(buttons);
    connect(fMute, &QToolButton::toggled, this, &ChannelStrip::muteToggled);
    connect(fSolo, &QToolButton::toggled, this, &ChannelStrip::soloToggled);

    auto* faderRow = new QHBoxLayout;
    faderRow->addStretch();
    fFader = new QSlider(Qt::Vertical);
    fFader->setRange(0, kFaderSteps);
    fFader->setMinimumHeight(isOutput ? 100 : 60);
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

    if (!isOutput)
    {
        fOutput = new QToolButton;
        fOutput->setObjectName(QStringLiteral("channelOutput"));
        fOutput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        fOutput->setToolTip(QStringLiteral("输出到哪里：总输出，或一个编组"));
        layout->addWidget(fOutput);
        connect(fOutput, &QToolButton::clicked, this, [this] { emit outputMenuRequested(fOutput->mapToGlobal(QPoint(0, fOutput->height()))); });
        setOutput(QStringLiteral("Stereo Out"));
    }

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

void ChannelStrip::setSends(const std::vector<SendSlot>& sends)
{
    fSendCount = static_cast<int>(std::min<size_t>(sends.size(), fSendRows.size()));
    for (int i = 0; i < static_cast<int>(fSendRows.size()); ++i)
    {
        SendRow& row = fSendRows[static_cast<size_t>(i)];
        const bool filled = i < fSendCount;
        row.name->setText(filled ? sends[static_cast<size_t>(i)].bus : i == fSendCount ? QStringLiteral("＋ 发送") : QString());
        row.name->setEnabled(i <= fSendCount);
        row.name->setToolTip(filled ? QStringLiteral("单击换一个或去掉") : QStringLiteral("把这条轨的声音送一份到效果轨（混响、延迟）"));
        row.name->setProperty("filled", filled);
        row.name->style()->unpolish(row.name);
        row.name->style()->polish(row.name);
        const QSignalBlocker blocker(row.level);
        row.level->setVisible(filled);
        row.level->setValue(filled ? volumeToFader(sends[static_cast<size_t>(i)].level) : 0);
        row.level->setToolTip(filled ? QStringLiteral("发送量 %1 dB").arg(toDb(sends[static_cast<size_t>(i)].level), 0, 'f', 1) : QString());
    }
}

void ChannelStrip::setOutput(const QString& name)
{
    if (fOutput)
        fOutput->setText(QStringLiteral("→ %1").arg(name));
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

void ChannelStrip::setPan(float pan)
{
    if (fPan)
        fPan->setPan(pan);
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
