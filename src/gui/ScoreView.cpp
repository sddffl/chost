#include "ScoreView.h"

#include <QFontDatabase>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPdfWriter>

#include <algorithm>
#include <cmath>
#include <map>

namespace {

// sizes in staff spaces (Bravura's engraving defaults), a space 1.75 mm: a 7 mm staff
constexpr double kSpace = 1.75;
constexpr double kStaffLine = 0.13;
constexpr double kStem = 0.12;
constexpr double kLedger = 0.16;
constexpr double kThinBar = 0.16;
constexpr double kThickBar = 0.5;
constexpr double kBeam = 0.5;
constexpr double kBeamGap = 0.25;
constexpr double kHead = 1.18;  // a black or half notehead's width
constexpr double kWhole = 1.69; // a whole note's

constexpr double kMargin = 15.0;   // millimetres around the page
constexpr double kTitleRoom = 22.0; // above the first system of page 1
constexpr double kFooter = 8.0;

// SMuFL code points
constexpr char32_t kGClef = 0xE050, kFClef = 0xE062, kPercussionClef = 0xE069, kBrace = 0xE000;
constexpr char32_t kHeadXWhole = 0xE0A7, kHeadXHalf = 0xE0A8, kHeadXBlack = 0xE0A9;
constexpr char32_t kHeadWhole = 0xE0A2, kHeadHalf = 0xE0A3, kHeadBlack = 0xE0A4;
constexpr char32_t kFlat = 0xE260, kNatural = 0xE261, kSharp = 0xE262;
constexpr char32_t kRestWhole = 0xE4E3, kRestHalf = 0xE4E4, kRestQuarter = 0xE4E5, kRest8th = 0xE4E6, kRest16th = 0xE4E7;
constexpr char32_t kFlag8thUp = 0xE240, kFlag8thDown = 0xE241, kFlag16thUp = 0xE242, kFlag16thDown = 0xE243;
constexpr char32_t kDot = 0xE1E7, kTimeSig0 = 0xE080, kQuarterNoteUp = 0xE1D5, kTuplet0 = 0xE880;

QString musicFamily()
{
    static const QString family = [] {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Bravura.otf"));
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QStringLiteral("Bravura") : families.first();
    }();
    return family;
}

// a glyph of the music font at x, y (its origin), sized for the staff (scale 1: an em is four spaces)
void glyph(QPainter& p, char32_t code, double x, double y, double scale = 1.0, double stretch = 1.0)
{
    static QFont font = [] {
        QFont f(musicFamily());
        f.setPixelSize(100);
        f.setStyleStrategy(QFont::StyleStrategy(QFont::NoFontMerging | QFont::PreferAntialias));
        f.setHintingPreference(QFont::PreferNoHinting);
        return f;
    }();
    p.save();
    p.translate(x, y);
    const double s = 4.0 * kSpace * scale / 100.0;
    p.scale(s, s * stretch);
    p.setFont(font);
    p.drawText(QPointF(0.0, 0.0), QString::fromUcs4(&code, 1));
    p.restore();
}

// text sized in millimetres (fonts take whole pixels, too coarse at a page's scale)
void text(QPainter& p, const QRectF& rect, int flags, const QString& string, double size, bool bold = false, bool italic = false)
{
    QFont font;
    font.setPixelSize(100);
    font.setBold(bold);
    font.setItalic(italic);
    const double s = size / 100.0;
    p.save();
    p.translate(rect.topLeft());
    p.scale(s, s);
    p.setFont(font);
    p.setPen(Qt::black);
    p.drawText(QRectF(0.0, 0.0, rect.width() / s, rect.height() / s), flags, string);
    p.restore();
}

void line(QPainter& p, double x1, double y1, double x2, double y2, double thickness)
{
    p.setPen(QPen(Qt::black, thickness * kSpace, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
}

// a tie from x1 to x2 at y, curving up or down
void tie(QPainter& p, double x1, double x2, double y, bool up)
{
    const double length = x2 - x1;
    if (length < 0.6 * kSpace)
        return;
    const double sign = up ? -1.0 : 1.0;
    const double height = std::min(0.55 * kSpace + length * 0.06, 1.6 * kSpace) * sign;
    const double thick = 0.2 * kSpace * sign;
    QPainterPath path;
    path.moveTo(x1, y);
    path.cubicTo(x1 + length * 0.2, y + height, x2 - length * 0.2, y + height, x2, y);
    path.cubicTo(x2 - length * 0.2, y + height + thick, x1 + length * 0.2, y + height + thick, x1, y);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    p.drawPath(path);
}

// a note's place on its staff: half spaces above the middle line
int position(const chost::ScoreNote& n, chost::Clef clef)
{
    return n.octave * 7 + n.step - (clef == chost::Clef::Bass ? 22 : 34); // drums sit as on a treble staff
}

// room for a note's length (ticks): longer notes get more, but far less than in proportion
double spacing(int ticks)
{
    return kSpace * (2.2 + 1.05 * std::log2(std::max(1.0, ticks / 3.0)));
}

// a time signature's number as the music font's digits, centred on x
void timeDigits(QPainter& p, int number, double x, double y)
{
    const QString digits = QString::number(number);
    const double width = 1.8 * kSpace; // a digit's
    double at = x - width * digits.size() / 2.0;
    for (QChar c : digits)
    {
        glyph(p, kTimeSig0 + static_cast<char32_t>(c.digitValue()), at, y);
        at += width;
    }
}

// the columns the accidentals of a chord need, top down: one, or more where they would collide
std::vector<int> accidentalColumns(const chost::ScoreEvent& e, chost::Clef clef, int* count)
{
    std::vector<int> column(e.notes.size(), -1);
    std::vector<std::pair<int, int>> placed; // position, column
    int used = 0;
    for (int i = static_cast<int>(e.notes.size()) - 1; i >= 0; --i)
    {
        if (!e.notes[static_cast<size_t>(i)].accidental)
            continue;
        const int pos = position(e.notes[static_cast<size_t>(i)], clef);
        int c = 0;
        while (std::any_of(placed.begin(), placed.end(), [&](const auto& pc) { return pc.second == c && std::abs(pc.first - pos) < 6; }))
            ++c;
        placed.push_back({pos, c});
        column[static_cast<size_t>(i)] = c;
        used = std::max(used, c + 1);
    }
    if (count != nullptr)
        *count = used;
    return column;
}

// noteheads a second apart go to the other side of the stem: which ones
std::vector<bool> displaced(const chost::ScoreEvent& e, chost::Clef clef, bool up)
{
    const size_t n = e.notes.size();
    std::vector<bool> out(n, false);
    if (up)
    {
        for (size_t i = 1; i < n; ++i)
            out[i] = position(e.notes[i], clef) - position(e.notes[i - 1], clef) == 1 && !out[i - 1];
    }
    else
    {
        for (size_t k = 1; k < n; ++k)
        {
            const size_t i = n - 1 - k;
            out[i] = position(e.notes[i + 1], clef) - position(e.notes[i], clef) == 1 && !out[i + 1];
        }
    }
    return out;
}

char32_t restGlyph(int display)
{
    return display >= 48 ? kRestWhole : display >= 24 ? kRestHalf : display >= 12 ? kRestQuarter : display >= 6 ? kRest8th : kRest16th;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// layout

ScoreEngraving::ScoreEngraving(const chost::Score& score)
    : fScore(score)
{
    for (size_t p = 0; p < fScore.parts.size(); ++p)
        for (size_t s = 0; s < fScore.parts[p].staves.size(); ++s)
            fStaves.push_back(Staff{static_cast<int>(p), static_cast<int>(s)});

    // each bar's columns: every start of a note or rest in any staff, room for what comes until the next
    fMeasures.resize(static_cast<size_t>(fScore.measures));
    for (int m = 0; m < fScore.measures; ++m)
    {
        Measure& measure = fMeasures[static_cast<size_t>(m)];
        std::map<int, Column> columns;
        for (size_t st = 0; st < fStaves.size(); ++st)
        {
            const chost::ScoreStaff& staff = fScore.parts[static_cast<size_t>(fStaves[st].part)].staves[static_cast<size_t>(fStaves[st].index)];
            for (const chost::ScoreEvent& e : staff.measures[static_cast<size_t>(m)].events)
            {
                Column& c = columns[e.start];
                c.start = e.start;
                if (e.rest)
                    continue;
                int accidentals = 0;
                accidentalColumns(e, staff.clef, &accidentals);
                const bool up = stemUp(static_cast<int>(st), m, e);
                const std::vector<bool> moved = displaced(e, staff.clef, up);
                const bool anyMoved = std::find(moved.begin(), moved.end(), true) != moved.end();
                double room = accidentals > 0 ? 0.35 * kSpace + accidentals * 1.1 * kSpace : 0.0;
                if (anyMoved && !up)
                    room += kHead * kSpace;
                c.fixed = std::max(c.fixed, room);
            }
        }
        for (auto it = columns.begin(); it != columns.end(); ++it)
        {
            auto next = std::next(it);
            Column c = it->second;
            c.flex = spacing((next == columns.end() ? fScore.meter.barTicks() : next->first) - c.start);
            measure.columns.push_back(c);
            measure.fixed += c.fixed;
            measure.flex += c.flex;
        }
        measure.fixed += 1.4 * kSpace; // after the barline before
    }

    // systems: as many bars as fit at their natural width, then spread to the line
    const double lineWidth = pageSize().width() - 2.0 * kMargin;
    double firstIndent = 0.0;
    for (const chost::ScorePart& part : fScore.parts)
        if (!part.name.empty())
            firstIndent = 18.0;
    int bar = 0;
    while (bar < fScore.measures)
    {
        System s;
        s.first = bar;
        s.indent = fSystems.empty() ? firstIndent : 0.0;
        double width = s.indent + headerWidth(fSystems.empty());
        while (bar < fScore.measures)
        {
            const Measure& m = fMeasures[static_cast<size_t>(bar)];
            if (bar > s.first && width + m.fixed + m.flex > lineWidth)
                break;
            width += m.fixed + m.flex;
            ++bar;
        }
        s.last = bar;
        fSystems.push_back(s);
    }
    for (System& s : fSystems)
    {
        double fixed = 0.0, flex = 0.0;
        for (int m = s.first; m < s.last; ++m)
        {
            fixed += fMeasures[static_cast<size_t>(m)].fixed;
            flex += fMeasures[static_cast<size_t>(m)].flex;
        }
        const double start = kMargin + s.indent + headerWidth(&s == &fSystems.front());
        const double room = kMargin + lineWidth - start;
        double stretch = flex > 0.0 ? (room - fixed) / flex : 1.0;
        // a short last line keeps its natural spacing
        if (&s == &fSystems.back() && fSystems.size() > 1 && fixed + flex < 0.7 * room)
            stretch = 1.0;
        stretch = std::max(stretch, 0.4);
        double x = start;
        for (int mi = s.first; mi < s.last; ++mi)
        {
            Measure& m = fMeasures[static_cast<size_t>(mi)];
            m.x = x;
            double cx = 1.4 * kSpace;
            for (Column& c : m.columns)
            {
                cx += c.fixed;
                c.x = cx;
                cx += c.flex * stretch;
            }
            m.width = cx;
            x += cx;
        }

        // staves: apart enough for what sticks out of them
        std::vector<double> above(fStaves.size(), 0.0), below(fStaves.size(), 0.0);
        for (size_t st = 0; st < fStaves.size(); ++st)
        {
            const chost::ScoreStaff& staff = fScore.parts[static_cast<size_t>(fStaves[st].part)].staves[static_cast<size_t>(fStaves[st].index)];
            int high = 4, low = -4;
            for (int mi = s.first; mi < s.last; ++mi)
                for (const chost::ScoreEvent& e : staff.measures[static_cast<size_t>(mi)].events)
                {
                    if (e.rest)
                        continue;
                    const bool up = stemUp(static_cast<int>(st), mi, e);
                    const int top = position(e.notes.back(), staff.clef);
                    const int bottom = position(e.notes.front(), staff.clef);
                    const int stem = chost::displayLength(e) < 48 ? 7 : 0;
                    if (e.tuplet >= 0)
                        high = std::max(high, top + (up ? stem : 0) + 4); // room for its 3
                    high = std::max({high, top + (up ? stem : 0) + 1, up ? 0 : top + 1});
                    low = std::min({low, bottom - (up ? 0 : stem) - 1, bottom - 1});
                }
            above[st] = (high - 4) * kSpace / 2.0;
            below[st] = (-4 - low) * kSpace / 2.0;
        }
        double y = above.empty() ? 0.0 : above[0] + 2.0 * kSpace;
        for (size_t st = 0; st < fStaves.size(); ++st)
        {
            if (st > 0)
            {
                const bool samePart = fStaves[st].part == fStaves[st - 1].part;
                y += 4.0 * kSpace + std::max(samePart ? 5.0 * kSpace : 5.5 * kSpace, below[st - 1] + above[st] + 1.5 * kSpace);
            }
            s.middles.push_back(y);
        }
        s.height = y + 2.0 * kSpace + (below.empty() ? 0.0 : below.back());
    }

    // pages
    const double bottom = pageSize().height() - kMargin - kFooter;
    double y = kMargin + kTitleRoom;
    fPages.push_back({});
    for (size_t i = 0; i < fSystems.size(); ++i)
    {
        System& s = fSystems[i];
        if (!fPages.back().empty() && y + s.height > bottom)
        {
            fPages.push_back({});
            y = kMargin;
        }
        s.page = static_cast<int>(fPages.size()) - 1;
        s.top = y;
        fPages.back().push_back(static_cast<int>(i));
        y += s.height + 6.0 * kSpace;
    }
    // full pages spread their systems down to the bottom (a little, not into wide gaps)
    for (size_t page = 0; page + 1 < fPages.size(); ++page)
    {
        const std::vector<int>& list = fPages[page];
        if (list.size() < 2)
            continue;
        const System& last = fSystems[static_cast<size_t>(list.back())];
        const double extra = std::min((bottom - (last.top + last.height)) / static_cast<double>(list.size() - 1), 15.0);
        for (size_t k = 1; k < list.size(); ++k)
            fSystems[static_cast<size_t>(list[k])].top += extra * static_cast<double>(k);
    }
}

double ScoreEngraving::headerWidth(bool first) const
{
    // clef, key signature, and the time signature on the first line (wider with two digits)
    const int digits = std::max(fScore.meter.beats, fScore.meter.beatType) >= 10 ? 2 : 1;
    return 4.0 * kSpace + std::abs(fScore.fifths) * 1.05 * kSpace + (first ? (0.8 + 1.8 * digits) * kSpace : 0.0);
}

const chost::ScoreEvent* ScoreEngraving::eventAt(int staff, int measure, int start) const
{
    const chost::ScoreStaff& st = fScore.parts[static_cast<size_t>(fStaves[static_cast<size_t>(staff)].part)].staves[static_cast<size_t>(fStaves[static_cast<size_t>(staff)].index)];
    for (const chost::ScoreEvent& e : st.measures[static_cast<size_t>(measure)].events)
        if (e.start == start)
            return &e;
    return nullptr;
}

bool ScoreEngraving::stemUp(int staff, int measure, const chost::ScoreEvent& e) const
{
    const chost::ScoreStaff& st = fScore.parts[static_cast<size_t>(fStaves[static_cast<size_t>(staff)].part)].staves[static_cast<size_t>(fStaves[static_cast<size_t>(staff)].index)];
    if (st.clef == chost::Clef::Percussion)
        return true; // drums: one voice, stems up
    // a beamed group goes one way, decided by its notes furthest from the middle line
    int sum = 0;
    auto add = [&](const chost::ScoreEvent& ev) {
        if (!ev.notes.empty())
            sum += position(ev.notes.front(), st.clef) + position(ev.notes.back(), st.clef);
    };
    if (e.beam >= 0)
    {
        for (const chost::ScoreEvent& ev : st.measures[static_cast<size_t>(measure)].events)
            if (ev.beam == e.beam)
                add(ev);
    }
    else
    {
        add(e);
    }
    return sum < 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// painting

void ScoreEngraving::paint(QPainter& p, int page) const
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QSizeF size = pageSize();
    if (page == 0)
    {
        if (!fScore.title.empty())
            text(p, QRectF(kMargin, kMargin - 4.0, size.width() - 2.0 * kMargin, 12.0), Qt::AlignHCenter | Qt::AlignVCenter,
                 QString::fromStdString(fScore.title), 7.0, true);
        // the tempo: a quarter note = so many a minute
        if (!fSystems.empty())
        {
            const System& s = fSystems.front();
            const double y = s.top - 1.0 * kSpace;
            const double x = kMargin + s.indent;
            p.setPen(Qt::black);
            glyph(p, kQuarterNoteUp, x, y, 0.75);
            // in 6/8 and the like the beat is a dotted quarter
            const bool dotted = fScore.meter.compound();
            if (dotted)
                glyph(p, kDot, x + 1.2 * kSpace, y - 0.3 * kSpace, 0.75);
            char tempo[32];
            std::snprintf(tempo, sizeof tempo, "= %g", std::round(dotted ? fScore.tempo / 1.5 : fScore.tempo));
            text(p, QRectF(x + (dotted ? 2.0 : 1.5) * kSpace, y - 4.0, 30.0, 5.0), Qt::AlignLeft | Qt::AlignBottom, QString::fromLatin1(tempo), 3.2);
        }
    }
    if (page < 0 || page >= pages())
        return;
    for (int i : fPages[static_cast<size_t>(page)])
        paintSystem(p, fSystems[static_cast<size_t>(i)]);

    if (pages() > 1)
    {
        text(p, QRectF(0.0, size.height() - kMargin, size.width(), 6.0), Qt::AlignHCenter | Qt::AlignTop,
             QStringLiteral("%1 / %2").arg(page + 1).arg(pages()), 3.0);
    }
}

void ScoreEngraving::paintSystem(QPainter& p, const System& s) const
{
    const bool first = &s == &fSystems.front();
    const double left = kMargin + s.indent;
    const double right = fMeasures[static_cast<size_t>(s.last - 1)].x + fMeasures[static_cast<size_t>(s.last - 1)].width;
    auto middle = [&](size_t st) { return s.top + s.middles[st]; };

    for (size_t st = 0; st < fStaves.size(); ++st)
    {
        const double m = middle(st);
        for (int l = -2; l <= 2; ++l)
            line(p, left, m + l * kSpace, right, m + l * kSpace, kStaffLine);
    }
    // the line joining the staves at the start, a brace for a grand staff, the parts' names
    if (fStaves.size() > 1)
        line(p, left, middle(0) - 2.0 * kSpace, left, middle(fStaves.size() - 1) + 2.0 * kSpace, kThinBar);
    for (size_t st = 0; st < fStaves.size(); ++st)
    {
        const chost::ScorePart& part = fScore.parts[static_cast<size_t>(fStaves[st].part)];
        if (fStaves[st].index != 0)
            continue;
        const size_t lastStaff = st + part.staves.size() - 1;
        const double top = middle(st) - 2.0 * kSpace;
        const double bottom = middle(lastStaff) + 2.0 * kSpace;
        if (part.staves.size() > 1)
        {
            p.setPen(Qt::black);
            glyph(p, kBrace, left - 1.4 * kSpace, bottom, 1.0, (bottom - top) / (4.0 * kSpace)); // only taller
        }
        if (first && !part.name.empty())
        {
            text(p, QRectF(kMargin - 2.0, top, s.indent - 2.0 * kSpace, bottom - top), Qt::AlignRight | Qt::AlignVCenter,
                 QString::fromStdString(part.name), 3.2);
        }
    }
    if (!first)
    {
        text(p, QRectF(left, middle(0) - 3.2 * kSpace - 4.0, 20.0, 4.0), Qt::AlignLeft | Qt::AlignBottom, QString::number(s.first + 1), 2.6,
             false, true);
    }

    // clefs, key signatures, the time signature
    for (size_t st = 0; st < fStaves.size(); ++st)
    {
        const chost::ScoreStaff& staff = fScore.parts[static_cast<size_t>(fStaves[st].part)].staves[static_cast<size_t>(fStaves[st].index)];
        const bool treble = staff.clef == chost::Clef::Treble;
        const bool drums = staff.clef == chost::Clef::Percussion;
        const double m = middle(st);
        double x = left + 0.8 * kSpace;
        p.setPen(Qt::black);
        if (drums)
            glyph(p, kPercussionClef, x + 0.6 * kSpace, m);
        else
            glyph(p, treble ? kGClef : kFClef, x, m + (treble ? 1.0 : -1.0) * kSpace);
        x += 3.2 * kSpace;
        static const int sharps[7] = {4, 1, 5, 2, -1, 3, 0};
        static const int flats[7] = {0, 3, -1, 2, -2, 1, -3};
        for (int i = 0; i < std::abs(fScore.fifths); ++i)
        {
            // a drum staff has no key signature, but keeps the time signature in line with the others
            const int pos = (fScore.fifths > 0 ? sharps : flats)[i] - (treble ? 0 : 2);
            if (!drums)
                glyph(p, fScore.fifths > 0 ? kSharp : kFlat, x, m - pos * kSpace / 2.0);
            x += 1.05 * kSpace;
        }
        if (first)
        {
            const int digits = std::max(fScore.meter.beats, fScore.meter.beatType) >= 10 ? 2 : 1;
            const double centre = x + 0.4 * kSpace + 0.9 * kSpace * digits;
            timeDigits(p, fScore.meter.beats, centre, m - kSpace);
            timeDigits(p, fScore.meter.beatType, centre, m + kSpace);
        }
    }

    // bars, and barlines through each part's staves
    for (int mi = s.first; mi < s.last; ++mi)
    {
        const Measure& measure = fMeasures[static_cast<size_t>(mi)];
        for (size_t st = 0; st < fStaves.size(); ++st)
            paintStaffMeasure(p, s, static_cast<int>(st), mi, middle(st));
        const double x = measure.x + measure.width;
        for (size_t st = 0; st < fStaves.size(); ++st)
        {
            if (fStaves[st].index != 0)
                continue;
            const size_t lastStaff = st + fScore.parts[static_cast<size_t>(fStaves[st].part)].staves.size() - 1;
            const double top = middle(st) - 2.0 * kSpace;
            const double bottom = middle(lastStaff) + 2.0 * kSpace;
            if (mi == fScore.measures - 1)
            {
                line(p, x - kThickBar * kSpace / 2.0, top, x - kThickBar * kSpace / 2.0, bottom, kThickBar);
                line(p, x - (kThickBar + 0.6) * kSpace, top, x - (kThickBar + 0.6) * kSpace, bottom, kThinBar);
            }
            else
            {
                line(p, x - kThinBar * kSpace / 2.0, top, x - kThinBar * kSpace / 2.0, bottom, kThinBar);
            }
        }
    }
}

void ScoreEngraving::paintStaffMeasure(QPainter& p, const System& s, int staffIndex, int mi, double middle) const
{
    const Staff& ref = fStaves[static_cast<size_t>(staffIndex)];
    const chost::ScoreStaff& staff = fScore.parts[static_cast<size_t>(ref.part)].staves[static_cast<size_t>(ref.index)];
    const chost::ScoreMeasure& measure = staff.measures[static_cast<size_t>(mi)];
    const Measure& layout = fMeasures[static_cast<size_t>(mi)];
    auto columnX = [&](int start) {
        for (const Column& c : layout.columns)
            if (c.start == start)
                return layout.x + c.x;
        return layout.x;
    };
    auto yOf = [&](int pos) { return middle - pos * kSpace / 2.0; };
    p.setPen(Qt::black);

    // beams first: they decide where the stems of their notes end
    struct Geometry {
        bool up = true;
        double stemX = 0.0;
        double stemEnd = 0.0;
        bool beamed = false;
    };
    std::vector<Geometry> geometry(measure.events.size());
    for (size_t i = 0; i < measure.events.size(); ++i)
    {
        const chost::ScoreEvent& e = measure.events[i];
        if (e.rest)
            continue;
        Geometry& g = geometry[i];
        g.up = stemUp(staffIndex, mi, e);
        const double x = columnX(e.start);
        g.stemX = g.up ? x + (kHead - kStem / 2.0) * kSpace : x + kStem / 2.0 * kSpace;
        const double top = yOf(position(e.notes.back(), staff.clef));
        const double bottom = yOf(position(e.notes.front(), staff.clef));
        const double length = (chost::displayLength(e) < 6 ? 3.75 : 3.5) * kSpace;
        g.stemEnd = g.up ? std::min(top - length, middle) : std::max(bottom + length, middle);
    }
    std::map<int, std::vector<size_t>> groups;
    for (size_t i = 0; i < measure.events.size(); ++i)
        if (measure.events[i].beam >= 0 && !measure.events[i].rest)
            groups[measure.events[i].beam].push_back(i);
    for (const auto& [beam, members] : groups)
    {
        if (members.size() < 2)
            continue;
        const bool up = geometry[members.front()].up;
        const double x1 = geometry[members.front()].stemX, x2 = geometry[members.back()].stemX;
        double y1 = geometry[members.front()].stemEnd, y2 = geometry[members.back()].stemEnd;
        // a gentle slope, then moved so that no stem gets too short
        const double slope = std::clamp(y2 - y1, -1.0 * kSpace, 1.0 * kSpace);
        y2 = y1 + slope;
        auto at = [&](double x) { return x2 > x1 ? y1 + (y2 - y1) * (x - x1) / (x2 - x1) : y1; };
        double shift = 0.0;
        for (size_t i : members)
        {
            const chost::ScoreEvent& e = measure.events[i];
            const double minimum = (chost::displayLength(e) < 6 ? 3.0 : 2.75) * kSpace;
            const double note = yOf(position(up ? e.notes.back() : e.notes.front(), staff.clef));
            const double y = at(geometry[i].stemX) + shift;
            if (up && y > note - minimum)
                shift -= y - (note - minimum);
            if (!up && y < note + minimum)
                shift += (note + minimum) - y;
        }
        y1 += shift;
        y2 += shift;
        for (size_t i : members)
        {
            geometry[i].stemEnd = at(geometry[i].stemX);
            geometry[i].beamed = true;
        }

        // the beams: one along the group, a second under each run of sixteenths, hooks for lone ones
        const double inward = up ? 1.0 : -1.0;
        auto beamSegment = [&](double xa, double xb, int level) {
            const double offset = level * (kBeam + kBeamGap) * kSpace * inward;
            QPainterPath path;
            path.moveTo(xa, at(xa) + offset);
            path.lineTo(xb, at(xb) + offset);
            path.lineTo(xb, at(xb) + offset + kBeam * kSpace * inward);
            path.lineTo(xa, at(xa) + offset + kBeam * kSpace * inward);
            path.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawPath(path);
        };
        beamSegment(x1 - kStem / 2.0 * kSpace, x2 + kStem / 2.0 * kSpace, 0);
        for (size_t k = 0; k < members.size(); ++k)
        {
            const size_t i = members[k];
            auto sixteenth = [&](size_t j) { return chost::displayLength(measure.events[j]) < 6; };
            if (!sixteenth(i))
                continue;
            const bool prev16 = k > 0 && sixteenth(members[k - 1]);
            const bool next16 = k + 1 < members.size() && sixteenth(members[k + 1]);
            if (next16)
                beamSegment(geometry[i].stemX - kStem / 2.0 * kSpace, geometry[members[k + 1]].stemX + kStem / 2.0 * kSpace, 1);
            else if (!prev16)
            {
                const double hook = 1.1 * kSpace;
                if (k == 0)
                    beamSegment(geometry[i].stemX - kStem / 2.0 * kSpace, geometry[i].stemX + hook, 1);
                else
                    beamSegment(geometry[i].stemX - hook, geometry[i].stemX + kStem / 2.0 * kSpace, 1);
            }
        }
    }

    for (size_t i = 0; i < measure.events.size(); ++i)
    {
        const chost::ScoreEvent& e = measure.events[i];
        const double x = columnX(e.start);
        p.setPen(Qt::black);
        if (e.rest)
        {
            const int display = chost::displayLength(e);
            if (e.wholeBar)
                glyph(p, kRestWhole, layout.x + layout.width / 2.0 - 0.8 * kSpace, yOf(2));
            else
            {
                glyph(p, restGlyph(display), x, yOf(display >= 48 ? 2 : 0));
                int dots = 0;
                std::string type;
                chost::noteType(display, type, dots);
                const double width = (display >= 24 ? 1.2 : display >= 12 ? 1.1 : 1.0) * kSpace;
                for (int d = 0; d < dots; ++d)
                    glyph(p, kDot, x + width + (0.3 + d * 0.6) * kSpace, yOf(1));
            }
            continue;
        }
        const Geometry& g = geometry[i];
        const std::vector<bool> moved = displaced(e, staff.clef, g.up);
        const int display = chost::displayLength(e);
        const bool whole = display >= 48;
        const double headWidth = (whole ? kWhole : kHead) * kSpace;
        const char32_t head = whole ? kHeadWhole : display >= 24 ? kHeadHalf : kHeadBlack;
        int accidentalCount = 0;
        const std::vector<int> accidentalColumn = accidentalColumns(e, staff.clef, &accidentalCount);
        const bool anyLeft = !g.up && std::find(moved.begin(), moved.end(), true) != moved.end();
        double rightmost = x + headWidth;

        for (size_t k = 0; k < e.notes.size(); ++k)
        {
            const chost::ScoreNote& n = e.notes[k];
            const int pos = position(n, staff.clef);
            const double y = yOf(pos);
            const double hx = moved[k] ? (g.up ? x + headWidth - kStem * kSpace : x - headWidth + kStem * kSpace) : x;
            rightmost = std::max(rightmost, hx + headWidth);
            // ledger lines out to the note
            for (int l = 6; l <= pos; l += 2)
                line(p, hx - 0.4 * kSpace, yOf(l), hx + headWidth + 0.4 * kSpace, yOf(l), kLedger);
            for (int l = -6; l >= pos; l -= 2)
                line(p, hx - 0.4 * kSpace, yOf(l), hx + headWidth + 0.4 * kSpace, yOf(l), kLedger);
            p.setPen(Qt::black);
            glyph(p, n.cross ? (whole ? kHeadXWhole : display >= 24 ? kHeadXHalf : kHeadXBlack) : head, hx, y);
            if (n.accidental)
            {
                const char32_t sign = n.alter > 0 ? kSharp : n.alter < 0 ? kFlat : kNatural;
                const double width = (n.alter > 0 ? 1.0 : n.alter < 0 ? 0.9 : 0.7) * kSpace;
                const double edge = x - (anyLeft ? headWidth : 0.0) - 0.25 * kSpace - accidentalColumn[k] * 1.1 * kSpace;
                glyph(p, sign, edge - width, y);
            }
        }
        // dots, in the space above a note on a line
        int dots = 0;
        std::string type;
        chost::noteType(display, type, dots);
        for (const chost::ScoreNote& n : e.notes)
        {
            const int pos = position(n, staff.clef);
            const double y = yOf(pos % 2 == 0 ? pos + 1 : pos);
            for (int d = 0; d < dots; ++d)
                glyph(p, kDot, rightmost + (0.45 + d * 0.6) * kSpace, y);
        }
        // the stem, and a flag when it isn't beamed
        if (!whole)
        {
            const double start = g.up ? yOf(position(e.notes.front(), staff.clef)) - 0.17 * kSpace
                                      : yOf(position(e.notes.back(), staff.clef)) + 0.17 * kSpace;
            line(p, g.stemX, start, g.stemX, g.stemEnd, kStem);
            if (!g.beamed && display < 12)
            {
                p.setPen(Qt::black);
                const char32_t flag = display < 6 ? (g.up ? kFlag16thUp : kFlag16thDown) : (g.up ? kFlag8thUp : kFlag8thDown);
                glyph(p, flag, g.stemX - kStem / 2.0 * kSpace, g.stemEnd);
            }
        }

        // ties: to the same note in the next event of this staff, or to the end of the line
        for (size_t k = 0; k < e.notes.size(); ++k)
        {
            const chost::ScoreNote& n = e.notes[k];
            const int pos = position(n, staff.clef);
            const bool tieUp = e.notes.size() > 1 ? k >= e.notes.size() / 2 : !g.up;
            const double y = yOf(pos) + (tieUp ? -0.55 : 0.55) * kSpace;
            if (n.tieStart)
            {
                double to = -1.0;
                if (i + 1 < measure.events.size())
                    to = columnX(measure.events[i + 1].start);
                else if (mi + 1 < s.last)
                {
                    const chost::ScoreMeasure& next = staff.measures[static_cast<size_t>(mi + 1)];
                    if (!next.events.empty())
                        to = fMeasures[static_cast<size_t>(mi + 1)].x + fMeasures[static_cast<size_t>(mi + 1)].columns.front().x;
                    for (const Column& c : fMeasures[static_cast<size_t>(mi + 1)].columns)
                        if (!next.events.empty() && c.start == next.events.front().start)
                            to = fMeasures[static_cast<size_t>(mi + 1)].x + c.x;
                }
                else
                {
                    to = layout.x + layout.width + 1.0 * kSpace; // on into the next line
                }
                if (to > 0.0)
                    tie(p, rightmost - 0.1 * kSpace + (dots > 0 ? 0.6 * kSpace : 0.0), to - 0.15 * kSpace, y, tieUp);
            }
            if (n.tieStop && i == 0 && mi == s.first)
            {
                // the end of a tie that started on the line before
                tie(p, x - 2.0 * kSpace, x - 0.15 * kSpace, y, tieUp);
            }
        }
    }

    // triplets: a 3 by the beam of a beamed group, else a bracket with the 3 over the notes
    std::map<int, std::vector<size_t>> tuplets;
    for (size_t i = 0; i < measure.events.size(); ++i)
        if (measure.events[i].tuplet >= 0)
            tuplets[measure.events[i].tuplet].push_back(i);
    p.setPen(Qt::black);
    for (const auto& [id, members] : tuplets)
    {
        const chost::ScoreEvent& front = measure.events[members.front()];
        const chost::ScoreEvent& back = measure.events[members.back()];
        const bool beamed = front.beam >= 0 && !front.rest &&
                            std::all_of(members.begin(), members.end(), [&](size_t i) { return measure.events[i].beam == front.beam; });
        if (beamed)
        {
            const Geometry& a = geometry[members.front()];
            const Geometry& b = geometry[members.back()];
            const double x = (a.stemX + b.stemX) / 2.0;
            const double y = (a.stemEnd + b.stemEnd) / 2.0;
            glyph(p, kTuplet0 + 3, x - 0.5 * kSpace, a.up ? y - 0.6 * kSpace : y + 2.0 * kSpace);
            continue;
        }
        double top = middle - 3.0 * kSpace;
        for (size_t i : members)
        {
            const chost::ScoreEvent& e = measure.events[i];
            if (e.rest)
                continue;
            top = std::min(top, yOf(position(e.notes.back(), staff.clef)) - 1.5 * kSpace);
            if (geometry[i].up)
                top = std::min(top, geometry[i].stemEnd - 1.0 * kSpace);
        }
        const double x1 = columnX(front.start);
        const double x2 = columnX(back.start) + kHead * kSpace;
        const double mid = (x1 + x2) / 2.0;
        line(p, x1, top + 0.6 * kSpace, x1, top, kStem);
        line(p, x1, top, mid - 0.9 * kSpace, top, kStem);
        line(p, mid + 0.9 * kSpace, top, x2, top, kStem);
        line(p, x2, top, x2, top + 0.6 * kSpace, kStem);
        glyph(p, kTuplet0 + 3, mid - 0.5 * kSpace, top + 0.6 * kSpace);
    }
}

bool ScoreEngraving::writePdf(const QString& path, QString& error) const
{
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(600);
    writer.setTitle(QString::fromStdString(fScore.title));
    writer.setCreator(QStringLiteral("chost"));
    QPainter p;
    if (!p.begin(&writer))
    {
        error = QStringLiteral("can't write %1").arg(path);
        return false;
    }
    const double scale = writer.resolution() / 25.4;
    for (int page = 0; page < pages(); ++page)
    {
        if (page > 0)
            writer.newPage();
        p.save();
        p.scale(scale, scale);
        paint(p, page);
        p.restore();
    }
    p.end();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// ScoreView

ScoreView::ScoreView(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    resizeToPages();
}

void ScoreView::setScore(const chost::Score& score)
{
    fEngraving = std::make_unique<ScoreEngraving>(score);
    fMessage.clear();
    resizeToPages();
    update();
}

void ScoreView::clear(const QString& message)
{
    fEngraving.reset();
    fMessage = message;
    resizeToPages();
    update();
}

void ScoreView::setZoom(double zoom)
{
    fZoom = std::clamp(zoom, 0.3, 3.0);
    resizeToPages();
    update();
}

static constexpr int kGap = 16; // pixels around and between the pages

void ScoreView::resizeToPages()
{
    const double perMm = 780.0 / ScoreEngraving::pageSize().width() * fZoom;
    const int pages = fEngraving ? std::max(1, fEngraving->pages()) : 1;
    const int w = static_cast<int>(ScoreEngraving::pageSize().width() * perMm) + 2 * kGap;
    const int h = static_cast<int>(ScoreEngraving::pageSize().height() * perMm + kGap) * pages + kGap;
    setFixedSize(w, h);
}

void ScoreView::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.fillRect(event->rect(), QColor(0x3a, 0x3e, 0x44));
    const double perMm = 780.0 / ScoreEngraving::pageSize().width() * fZoom;
    const QSizeF page = ScoreEngraving::pageSize() * perMm;
    if (!fEngraving)
    {
        p.fillRect(QRectF(kGap, kGap, page.width(), page.height()), Qt::white);
        p.setPen(QColor(0x60, 0x60, 0x60));
        p.drawText(QRectF(kGap, kGap, page.width(), page.height() / 3), Qt::AlignCenter | Qt::TextWordWrap, fMessage);
        return;
    }
    for (int i = 0; i < fEngraving->pages(); ++i)
    {
        const QRectF r(kGap, kGap + i * (page.height() + kGap), page.width(), page.height());
        if (!r.intersects(event->rect()))
            continue;
        p.fillRect(r.translated(3, 3), QColor(0, 0, 0, 60));
        p.fillRect(r, Qt::white);
        p.save();
        p.translate(r.topLeft());
        p.scale(perMm, perMm);
        fEngraving->paint(p, i);
        p.restore();
    }
}
