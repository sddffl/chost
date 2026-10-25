#include "PianoRoll.h"

#include "Theme.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

static constexpr int kRulerHeight = 22;
static constexpr int kEndHandle = 5; // pixels at a note's right end that resize it

static bool isBlackKey(int pitch)
{
    const int k = pitch % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

static QString drumName(int pitch)
{
    switch (pitch)
    {
    case 35: return QStringLiteral("底鼓 2");
    case 36: return QStringLiteral("底鼓");
    case 37: return QStringLiteral("鼓边");
    case 38: return QStringLiteral("军鼓");
    case 39: return QStringLiteral("拍手");
    case 40: return QStringLiteral("军鼓 2");
    case 41: return QStringLiteral("落地通鼓 2");
    case 42: return QStringLiteral("踩镲 闭");
    case 43: return QStringLiteral("落地通鼓");
    case 44: return QStringLiteral("踩镲 踏");
    case 45: return QStringLiteral("低通鼓");
    case 46: return QStringLiteral("踩镲 开");
    case 47: return QStringLiteral("中通鼓");
    case 48: return QStringLiteral("高通鼓 2");
    case 49: return QStringLiteral("吊镲");
    case 50: return QStringLiteral("高通鼓");
    case 51: return QStringLiteral("叮叮镲");
    case 52: return QStringLiteral("中国镲");
    case 53: return QStringLiteral("叮叮镲 帽");
    case 54: return QStringLiteral("铃鼓");
    case 55: return QStringLiteral("水镲");
    case 56: return QStringLiteral("牛铃");
    case 57: return QStringLiteral("吊镲 2");
    case 59: return QStringLiteral("叮叮镲 2");
    default: return {};
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// PianoRoll

PianoRoll::PianoRoll(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(400, 240);
    fHScroll = new QScrollBar(Qt::Horizontal, this);
    fVScroll = new QScrollBar(Qt::Vertical, this);
    connect(fHScroll, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    connect(fVScroll, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
}

void PianoRoll::setClip(const chost::MidiClip& clip)
{
    fClip = clip;
    fSelected.assign(fClip.notes.size(), false);
    fUndo.clear();
    fRedo.clear();
    updateScrollBars();
    update();
}

void PianoRoll::setGrid(double beats)
{
    fGrid = beats;
    update();
}

void PianoRoll::setPlayhead(double seconds)
{
    if (std::abs(seconds - fPlayhead) < 1e-4)
        return;
    fPlayhead = seconds;
    update();
}

int PianoRoll::keysWidth() const
{
    return fClip.drums ? 96 : 56;
}

double PianoRoll::snap(double seconds) const
{
    const double g = gridSeconds();
    return g > 0.0 ? std::round(seconds / g) * g : seconds;
}

double PianoRoll::xToSeconds(int x) const
{
    return (x - keysWidth() + fHScroll->value()) / fPixelsPerSecond;
}

int PianoRoll::secondsToX(double seconds) const
{
    return keysWidth() + static_cast<int>(std::lround(seconds * fPixelsPerSecond)) - fHScroll->value();
}

int PianoRoll::pitchAt(int y) const
{
    return std::clamp(127 - (y - kRulerHeight + fVScroll->value()) / fRowHeight, 0, 127);
}

int PianoRoll::pitchToY(int pitch) const
{
    return kRulerHeight + (127 - pitch) * fRowHeight - fVScroll->value();
}

QRect PianoRoll::noteRect(const chost::MidiNote& n) const
{
    const int x0 = secondsToX(n.start);
    const int x1 = std::max(x0 + 3, secondsToX(n.end));
    return QRect(x0, pitchToY(n.pitch) + 1, x1 - x0, fRowHeight - 1);
}

int PianoRoll::noteAt(const QPoint& pos, bool* onEnd) const
{
    // the topmost (last drawn) note wins
    for (int i = static_cast<int>(fClip.notes.size()) - 1; i >= 0; --i)
    {
        const QRect r = noteRect(fClip.notes[static_cast<size_t>(i)]);
        if (r.contains(pos))
        {
            if (onEnd != nullptr)
                *onEnd = pos.x() >= r.right() - kEndHandle && r.width() > 2 * kEndHandle;
            return i;
        }
    }
    return -1;
}

double PianoRoll::length() const
{
    // some room after the last note to add more, at least 8 bars
    return std::max(fClip.seconds, 32.0 * secondsPerBeat()) + 8.0 * secondsPerBeat();
}

void PianoRoll::updateScrollBars()
{
    const int sh = fHScroll->sizeHint().height();
    const int sw = fVScroll->sizeHint().width();
    fHScroll->setGeometry(keysWidth(), height() - sh, width() - keysWidth() - sw, sh);
    fVScroll->setGeometry(width() - sw, kRulerHeight, sw, height() - kRulerHeight - sh);

    const int contentWidth = static_cast<int>(length() * fPixelsPerSecond);
    const int viewWidth = width() - keysWidth() - sw;
    fHScroll->setRange(0, std::max(0, contentWidth - viewWidth));
    fHScroll->setPageStep(viewWidth);
    fHScroll->setSingleStep(20);

    const int contentHeight = 128 * fRowHeight;
    const int viewHeight = height() - kRulerHeight - sh;
    fVScroll->setRange(0, std::max(0, contentHeight - viewHeight));
    fVScroll->setPageStep(viewHeight);
    fVScroll->setSingleStep(fRowHeight);
}

void PianoRoll::scrollToNotes()
{
    if (fClip.notes.empty())
    {
        fVScroll->setValue((127 - 72) * fRowHeight);
        return;
    }
    int lo = 127, hi = 0;
    for (const chost::MidiNote& n : fClip.notes)
    {
        lo = std::min<int>(lo, n.pitch);
        hi = std::max<int>(hi, n.pitch);
    }
    const int middle = (127 - (lo + hi) / 2) * fRowHeight;
    fVScroll->setValue(middle - fVScroll->pageStep() / 2);
}

void PianoRoll::resizeEvent(QResizeEvent*)
{
    updateScrollBars();
}

void PianoRoll::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const int keys = keysWidth();
    const int right = fVScroll->x();
    const int bottom = fHScroll->y();
    p.fillRect(rect(), theme::window);

    // rows
    p.setClipRect(keys, kRulerHeight, right - keys, bottom - kRulerHeight);
    for (int pitch = 0; pitch < 128; ++pitch)
    {
        const int y = pitchToY(pitch);
        if (y + fRowHeight < kRulerHeight || y > bottom)
            continue;
        const bool named = fClip.drums && !drumName(pitch).isEmpty();
        QColor row = isBlackKey(pitch) && !fClip.drums ? QColor(0x23, 0x26, 0x2a) : QColor(0x2b, 0x2f, 0x34);
        if (named)
            row = QColor(0x30, 0x35, 0x3b);
        p.fillRect(keys, y, right - keys, fRowHeight, row);
        p.setPen(pitch % 12 == 0 ? QColor(0x40, 0x45, 0x4c) : QColor(0x26, 0x29, 0x2d));
        p.drawLine(keys, y + fRowHeight - 1, right, y + fRowHeight - 1);
    }

    // grid, beats and bars
    const double spb = secondsPerBeat();
    const double first = std::max(0.0, xToSeconds(keys));
    const double last = xToSeconds(right);
    const double g = gridSeconds();
    if (g * fPixelsPerSecond >= 6.0)
    {
        p.setPen(QColor(0x31, 0x35, 0x3a));
        for (double t = std::floor(first / g) * g; t <= last; t += g)
            p.drawLine(secondsToX(t), kRulerHeight, secondsToX(t), bottom);
    }
    for (int beat = static_cast<int>(first / spb); beat * spb <= last; ++beat)
    {
        p.setPen(beat % 4 == 0 ? QColor(0x55, 0x5b, 0x63) : QColor(0x3b, 0x40, 0x46));
        const int x = secondsToX(beat * spb);
        p.drawLine(x, kRulerHeight, x, bottom);
    }

    // notes, louder ones stronger
    for (size_t i = 0; i < fClip.notes.size(); ++i)
    {
        const chost::MidiNote& n = fClip.notes[i];
        const QRect r = noteRect(n);
        if (r.right() < keys || r.left() > right)
            continue;
        QColor c = fSelected[i] ? fColour.lighter(150) : fColour;
        c.setAlpha(110 + n.velocity);
        p.setPen(fSelected[i] ? QPen(Qt::white, 1) : QPen(fColour.darker(170), 1));
        p.setBrush(c);
        p.drawRoundedRect(r.adjusted(0, 0, -1, -1), 2, 2);
    }

    // rubber band
    if (fDrag == Drag::Select)
    {
        p.setPen(QPen(theme::accent, 1, Qt::DashLine));
        p.setBrush(QColor(0x4a, 0x90, 0xd9, 40));
        p.drawRect(QRect(fPressPos, fMousePos).normalized());
    }

    // playhead
    const int px = secondsToX(fPlayhead);
    if (px >= keys && px <= right)
    {
        p.setPen(QPen(theme::cursor, 1));
        p.drawLine(px, kRulerHeight, px, bottom);
    }
    p.setClipping(false);

    // ruler: bar numbers
    p.fillRect(keys, 0, right - keys, kRulerHeight, theme::panelLight);
    p.setClipRect(keys, 0, right - keys, kRulerHeight);
    p.setPen(theme::textDim);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);
    for (int beat = static_cast<int>(first / spb); beat * spb <= last; ++beat)
    {
        const int x = secondsToX(beat * spb);
        if (beat % 4 == 0)
        {
            p.drawLine(x, kRulerHeight - 8, x, kRulerHeight);
            p.drawText(x + 3, kRulerHeight - 7, QString::number(beat / 4 + 1));
        }
        else
        {
            p.drawLine(x, kRulerHeight - 4, x, kRulerHeight);
        }
    }
    p.setClipping(false);

    // keyboard, or the drum names
    p.setClipRect(0, kRulerHeight, keys, bottom - kRulerHeight);
    for (int pitch = 0; pitch < 128; ++pitch)
    {
        const int y = pitchToY(pitch);
        if (y + fRowHeight < kRulerHeight || y > bottom)
            continue;
        const QRect key(0, y, keys - 1, fRowHeight);
        const bool black = isBlackKey(pitch) && !fClip.drums;
        p.fillRect(key, black ? QColor(0x1a, 0x1b, 0x1d) : QColor(0xd8, 0xda, 0xdc));
        p.setPen(QColor(0x80, 0x84, 0x88));
        p.drawLine(0, y + fRowHeight - 1, keys - 1, y + fRowHeight - 1);
        const QString name = rowName(pitch);
        if (!name.isEmpty())
        {
            p.setPen(QColor(0x30, 0x33, 0x36));
            p.drawText(key.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignRight, name);
        }
    }
    p.setClipping(false);
    p.fillRect(0, 0, keys, kRulerHeight, theme::panel);
    p.fillRect(right, bottom, width() - right, height() - bottom, theme::panel);
}

QString PianoRoll::rowName(int pitch) const
{
    if (fClip.drums)
        return drumName(pitch);
    // Cubase names middle C (60) C3
    return pitch % 12 == 0 ? QStringLiteral("C%1").arg(pitch / 12 - 2) : QString();
}

void PianoRoll::checkpoint()
{
    fUndo.push_back(fClip.notes);
    if (fUndo.size() > 200)
        fUndo.erase(fUndo.begin());
    fRedo.clear();
}

void PianoRoll::changed()
{
    std::vector<std::pair<chost::MidiNote, bool>> both;
    for (size_t i = 0; i < fClip.notes.size(); ++i)
        both.emplace_back(fClip.notes[i], fSelected[i]);
    std::stable_sort(both.begin(), both.end(), [](const auto& a, const auto& b) { return a.first.start < b.first.start; });
    fClip.seconds = 0.0;
    for (size_t i = 0; i < both.size(); ++i)
    {
        fClip.notes[i] = both[i].first;
        fSelected[i] = both[i].second;
        fClip.seconds = std::max(fClip.seconds, fClip.notes[i].end);
    }
    updateScrollBars();
    update();
    emit edited();
}

void PianoRoll::undo()
{
    if (fUndo.empty())
        return;
    fRedo.push_back(fClip.notes);
    fClip.notes = fUndo.back();
    fUndo.pop_back();
    fSelected.assign(fClip.notes.size(), false);
    changed();
}

void PianoRoll::redo()
{
    if (fRedo.empty())
        return;
    fUndo.push_back(fClip.notes);
    fClip.notes = fRedo.back();
    fRedo.pop_back();
    fSelected.assign(fClip.notes.size(), false);
    changed();
}

void PianoRoll::quantize()
{
    const bool any = std::find(fSelected.begin(), fSelected.end(), true) != fSelected.end();
    checkpoint();
    for (size_t i = 0; i < fClip.notes.size(); ++i)
    {
        if (any && !fSelected[i])
            continue;
        chost::MidiNote& n = fClip.notes[i];
        const double len = n.end - n.start;
        n.start = std::max(0.0, snap(n.start));
        n.end = n.start + std::max(gridSeconds(), snap(len));
    }
    changed();
}

void PianoRoll::removeSelected()
{
    if (std::find(fSelected.begin(), fSelected.end(), true) == fSelected.end())
        return;
    checkpoint();
    std::vector<chost::MidiNote> kept;
    for (size_t i = 0; i < fClip.notes.size(); ++i)
        if (!fSelected[i])
            kept.push_back(fClip.notes[i]);
    fClip.notes = kept;
    fSelected.assign(fClip.notes.size(), false);
    changed();
}

void PianoRoll::shiftSelected(double seconds, int pitch)
{
    if (std::find(fSelected.begin(), fSelected.end(), true) == fSelected.end())
        return;
    checkpoint();
    for (size_t i = 0; i < fClip.notes.size(); ++i)
    {
        if (!fSelected[i])
            continue;
        chost::MidiNote& n = fClip.notes[i];
        const double len = n.end - n.start;
        n.start = std::max(0.0, n.start + seconds);
        n.end = n.start + len;
        n.pitch = static_cast<uint8_t>(std::clamp(n.pitch + pitch, 0, 127));
        if (pitch != 0)
            emit audition(n.pitch, n.channel);
    }
    changed();
}

void PianoRoll::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    const QPoint pos = event->position().toPoint();
    if (pos.y() < kRulerHeight || pos.x() >= fVScroll->x() || pos.y() >= fHScroll->y())
        return;
    if (pos.x() < keysWidth())
    {
        emit audition(pitchAt(pos.y()), fClip.drums ? 9 : 0); // the keyboard plays
        return;
    }

    const bool add = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    bool onEnd = false;
    const int hit = noteAt(pos, &onEnd);

    if (event->button() == Qt::RightButton)
    {
        if (hit >= 0)
        {
            if (!fSelected[static_cast<size_t>(hit)])
                fSelected.assign(fSelected.size(), false);
            fSelected[static_cast<size_t>(hit)] = true;
            removeSelected();
        }
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    fPressPos = fMousePos = pos;
    fDragChanged = false;
    if (hit >= 0)
    {
        if (add)
            fSelected[static_cast<size_t>(hit)] = !fSelected[static_cast<size_t>(hit)];
        else if (!fSelected[static_cast<size_t>(hit)])
        {
            fSelected.assign(fSelected.size(), false);
            fSelected[static_cast<size_t>(hit)] = true;
        }
        fPressNote = hit;
        fPressNotes = fClip.notes;
        fDrag = onEnd ? Drag::Resize : Drag::Move;
        const chost::MidiNote& n = fClip.notes[static_cast<size_t>(hit)];
        emit audition(n.pitch, n.channel);
    }
    else if (add)
    {
        fDrag = Drag::Select;
    }
    else
    {
        // a new note of one grid step at the click, dragging right makes it longer
        checkpoint();
        chost::MidiNote n;
        n.start = std::max(0.0, std::floor(xToSeconds(pos.x()) / gridSeconds()) * gridSeconds());
        n.end = n.start + gridSeconds();
        n.pitch = static_cast<uint8_t>(pitchAt(pos.y()));
        n.velocity = 100;
        n.channel = fClip.drums ? 9 : (fClip.notes.empty() ? 0 : fClip.notes.front().channel);
        fClip.notes.push_back(n);
        fSelected.assign(fClip.notes.size(), false);
        fSelected.back() = true;
        fPressNote = static_cast<int>(fClip.notes.size()) - 1;
        fPressNotes = fClip.notes;
        fDrag = Drag::Resize;
        fDragChanged = true;
        emit audition(n.pitch, n.channel);
        update();
    }
}

void PianoRoll::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    fMousePos = pos;
    if (fDrag == Drag::None)
    {
        bool onEnd = false;
        const int hit = noteAt(pos, &onEnd);
        setCursor(hit >= 0 ? (onEnd ? Qt::SizeHorCursor : Qt::SizeAllCursor) : Qt::ArrowCursor);
        return;
    }
    if (fDrag == Drag::Select)
    {
        update();
        return;
    }

    const double g = gridSeconds();
    const double dt = std::round((pos.x() - fPressPos.x()) / fPixelsPerSecond / g) * g;
    if (fDrag == Drag::Move)
    {
        const int dp = pitchAt(pos.y()) - pitchAt(fPressPos.y());
        if (!fDragChanged && dt == 0.0 && dp == 0)
            return;
        if (!fDragChanged)
        {
            fUndo.push_back(fPressNotes);
            fRedo.clear();
            fDragChanged = true;
        }
        const int oldPitch = fClip.notes[static_cast<size_t>(fPressNote)].pitch;
        for (size_t i = 0; i < fClip.notes.size(); ++i)
        {
            if (!fSelected[i])
                continue;
            const chost::MidiNote& from = fPressNotes[i];
            chost::MidiNote& n = fClip.notes[i];
            const double len = from.end - from.start;
            n.start = std::max(0.0, from.start + dt);
            n.end = n.start + len;
            n.pitch = static_cast<uint8_t>(std::clamp(from.pitch + dp, 0, 127));
        }
        const chost::MidiNote& lead = fClip.notes[static_cast<size_t>(fPressNote)];
        if (lead.pitch != oldPitch)
            emit audition(lead.pitch, lead.channel);
    }
    else if (fDrag == Drag::Resize)
    {
        // the grabbed note's end follows the mouse, the other selected notes change by as much
        const chost::MidiNote& grabbed = fPressNotes[static_cast<size_t>(fPressNote)];
        const double delta = std::max(grabbed.start + g, snap(xToSeconds(pos.x()))) - grabbed.end;
        if (!fDragChanged && std::abs(delta) < 1e-9)
            return;
        if (!fDragChanged)
        {
            fUndo.push_back(fPressNotes);
            fRedo.clear();
            fDragChanged = true;
        }
        for (size_t i = 0; i < fClip.notes.size(); ++i)
        {
            if (!fSelected[i])
                continue;
            const chost::MidiNote& from = fPressNotes[i];
            fClip.notes[i].end = std::max(from.start + g, from.end + delta);
        }
    }
    update();
}

void PianoRoll::mouseReleaseEvent(QMouseEvent*)
{
    if (fDrag == Drag::Select)
    {
        const QRect band = QRect(fPressPos, fMousePos).normalized();
        for (size_t i = 0; i < fClip.notes.size(); ++i)
            if (band.intersects(noteRect(fClip.notes[i])))
                fSelected[i] = true;
        update();
    }
    else if (fDragChanged)
    {
        changed();
    }
    fDrag = Drag::None;
    fPressNote = -1;
    fDragChanged = false;
}

void PianoRoll::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int hit = noteAt(event->position().toPoint());
    if (hit < 0)
        return;
    // the first click of the double click selected it
    fSelected.assign(fSelected.size(), false);
    fSelected[static_cast<size_t>(hit)] = true;
    removeSelected();
}

void PianoRoll::keyPressEvent(QKeyEvent* event)
{
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    switch (event->key())
    {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        removeSelected();
        return;
    case Qt::Key_A:
        if (ctrl)
        {
            fSelected.assign(fSelected.size(), true);
            update();
            return;
        }
        break;
    case Qt::Key_Z:
        if (ctrl)
        {
            shift ? redo() : undo();
            return;
        }
        break;
    case Qt::Key_Y:
        if (ctrl)
        {
            redo();
            return;
        }
        break;
    case Qt::Key_Up:
        shiftSelected(0.0, shift ? 12 : 1);
        return;
    case Qt::Key_Down:
        shiftSelected(0.0, shift ? -12 : -1);
        return;
    case Qt::Key_Left:
        shiftSelected(-gridSeconds(), 0);
        return;
    case Qt::Key_Right:
        shiftSelected(gridSeconds(), 0);
        return;
    case Qt::Key_Escape:
        fSelected.assign(fSelected.size(), false);
        update();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void PianoRoll::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (event->modifiers() & Qt::ControlModifier)
    {
        // zoom around the mouse
        const int x = static_cast<int>(event->position().x());
        const double at = xToSeconds(x);
        fPixelsPerSecond = std::clamp(fPixelsPerSecond * (delta > 0 ? 1.25 : 0.8), 20.0, 2000.0);
        updateScrollBars();
        fHScroll->setValue(static_cast<int>(at * fPixelsPerSecond) - (x - keysWidth()));
    }
    else if (event->modifiers() & Qt::ShiftModifier)
    {
        fHScroll->setValue(fHScroll->value() - delta);
    }
    else
    {
        fVScroll->setValue(fVScroll->value() - delta / 2);
    }
    update();
}

// ---------------------------------------------------------------------------------------------------------------------
// PianoRollDialog

PianoRollDialog::PianoRollDialog(const QString& trackName, const QString& midiPath, const QColor& colour, QWidget* parent)
    : QDialog(parent)
    , fPath(midiPath)
{
    setWindowTitle(QStringLiteral("编辑音符 — %1").arg(trackName));
    resize(1100, 620);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("网格")));
    fGrid = new QComboBox;
    fGrid->addItem(QStringLiteral("1/4（一拍）"), 1.0);
    fGrid->addItem(QStringLiteral("1/8"), 0.5);
    fGrid->addItem(QStringLiteral("1/16"), 0.25);
    fGrid->addItem(QStringLiteral("1/32"), 0.125);
    fGrid->addItem(QStringLiteral("1/8 三连音"), 1.0 / 3.0);
    fGrid->addItem(QStringLiteral("1/16 三连音"), 1.0 / 6.0);
    fGrid->setCurrentIndex(2);
    fGrid->setToolTip(QStringLiteral("音符对齐的单位，也是新加音符的长度"));
    bar->addWidget(fGrid);
    auto* quantize = new QPushButton(QStringLiteral("量化"));
    quantize->setToolTip(QStringLiteral("把选中的音符（没选就是全部）对齐到网格"));
    bar->addWidget(quantize);
    auto* undo = new QPushButton(QStringLiteral("撤销"));
    auto* redo = new QPushButton(QStringLiteral("重做"));
    bar->addWidget(undo);
    bar->addWidget(redo);
    auto* help = new QLabel(QStringLiteral("单击空白处加音符 · 拖动移动 · 拖右边缘改长短 · 右键或 Delete 删除 · Ctrl+拖动框选 · 方向键移动 · Ctrl+滚轮缩放"));
    help->setStyleSheet(QStringLiteral("color: #8b9096;"));
    bar->addWidget(help, 1);
    fStatus = new QLabel;
    fStatus->setStyleSheet(QStringLiteral("color: #8b9096;"));
    bar->addWidget(fStatus);
    layout->addLayout(bar);

    fRoll = new PianoRoll;
    fRoll->setColour(colour);
    layout->addWidget(fRoll, 1);

    chost::MidiClip clip;
    std::string error;
    if (!chost::readMidiFile(midiPath.toStdString(), clip, error))
        fStatus->setText(QStringLiteral("读不了这个 MIDI 文件：%1").arg(QString::fromStdString(error)));
    fRoll->setClip(clip);
    fRoll->setGrid(fGrid->currentData().toDouble());
    QTimer::singleShot(0, fRoll, &PianoRoll::scrollToNotes);

    fSaveTimer = new QTimer(this);
    fSaveTimer->setSingleShot(true);
    fSaveTimer->setInterval(250);
    connect(fSaveTimer, &QTimer::timeout, this, &PianoRollDialog::save);
    connect(fRoll, &PianoRoll::edited, this, [this] {
        fStatus->setText(QStringLiteral("…"));
        fSaveTimer->start();
    });
    connect(fRoll, &PianoRoll::audition, this, [this](int pitch, int channel) { emit audition(fPath, pitch, channel); });
    connect(fGrid, &QComboBox::currentIndexChanged, this, [this] { fRoll->setGrid(fGrid->currentData().toDouble()); });
    connect(quantize, &QPushButton::clicked, fRoll, &PianoRoll::quantize);
    connect(undo, &QPushButton::clicked, fRoll, &PianoRoll::undo);
    connect(redo, &QPushButton::clicked, fRoll, &PianoRoll::redo);
    fRoll->setFocus();
}

void PianoRollDialog::save()
{
    // edits never overwrite a file chost didn't make: the first one goes to "<name>_edit.mid"
    const QString old = fPath;
    const QFileInfo info(fPath);
    if (!info.completeBaseName().endsWith(QLatin1String("_edit")))
    {
        const QString name = info.completeBaseName() + QStringLiteral("_edit.mid");
        QString folder = info.absolutePath();
        if (!QFileInfo(folder).isWritable())
        {
            folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/midi");
            QDir().mkpath(folder);
        }
        fPath = QDir(folder).filePath(name);
    }
    std::string error;
    if (!chost::writeMidiFile(fPath.toStdString(), fRoll->clip(), error))
    {
        fPath = old;
        fStatus->setText(QStringLiteral("保存失败：%1").arg(QString::fromStdString(error)));
        return;
    }
    fStatus->setText(QStringLiteral("已保存 %1").arg(QFileInfo(fPath).fileName()));
    emit saved(old, fPath);
}
