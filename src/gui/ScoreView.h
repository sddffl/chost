// Sheet music on the page: a chost::Score engraved on A4 pages with the Bravura music font (SMuFL), on
// screen and into PDF. Systems hold as many bars as fit and are spread to the page width; staves get the
// room their notes need.

#pragma once

#include "core/Score.h"

#include <QWidget>

#include <memory>
#include <vector>

class QPainter;

class ScoreEngraving {
public:
    explicit ScoreEngraving(const chost::Score& score);

    int pages() const { return static_cast<int>(fPages.size()); }
    static QSizeF pageSize() { return {210.0, 297.0}; } // millimetres
    void paint(QPainter& p, int page) const;            // the painter in millimetres, the page at 0,0

    bool writePdf(const QString& path, QString& error) const;

    struct Column {
        int start = 0;      // sixteenths in the bar
        double x = 0.0;     // from the bar's start, millimetres, once the system is spread
        double fixed = 0.0; // room for accidentals and moved noteheads, which doesn't stretch
        double flex = 0.0;  // room for the notes' length, which does
    };
    struct Measure {
        std::vector<Column> columns;
        double fixed = 0.0;
        double flex = 0.0;
        double x = 0.0;     // on the page, once in a system
        double width = 0.0;
    };
    struct Staff {
        int part = 0;
        int index = 0; // in its part
    };
    struct System {
        int first = 0; // bars [first, last)
        int last = 0;
        double indent = 0.0;
        double top = 0.0;            // on its page
        std::vector<double> middles; // each staff's middle line, from top
        double height = 0.0;
        int page = 0;
    };

private:
    const chost::ScoreEvent* eventAt(int staff, int measure, int start) const;
    bool stemUp(int staff, int measure, const chost::ScoreEvent& e) const;
    void paintSystem(QPainter& p, const System& s) const;
    void paintStaffMeasure(QPainter& p, const System& s, int staff, int measure, double middle) const;
    double headerWidth(bool first) const;

    chost::Score fScore;
    std::vector<Staff> fStaves;
    std::vector<Measure> fMeasures;
    std::vector<System> fSystems;
    std::vector<std::vector<int>> fPages; // systems on each page
};

// The pages one under the other, at a zoom.
class ScoreView : public QWidget {
    Q_OBJECT

public:
    explicit ScoreView(QWidget* parent = nullptr);

    void setScore(const chost::Score& score);
    void clear(const QString& message);
    void setZoom(double zoom); // 1: a page as wide as 780 pixels
    const ScoreEngraving* engraving() const { return fEngraving.get(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void resizeToPages();

    std::unique_ptr<ScoreEngraving> fEngraving;
    QString fMessage;
    double fZoom = 1.0;
};
