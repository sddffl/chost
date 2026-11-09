#include "DynamicScoreView.h"

#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <cmath>
#include <algorithm>

DynamicScoreView::DynamicScoreView(QWidget* parent)
    : QWidget(parent)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // Top Toolbar
    auto* bar = new QWidget(this);
    bar->setFixedHeight(36);
    bar->setStyleSheet(QStringLiteral(
        "QWidget { background: #101522; border-bottom: 1px solid #1a2236; }"
        "QPushButton { background: #1c2538; color: #b8c7e0; border: 1px solid #2e3d5c; border-radius: 4px; padding: 4px 12px; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: #26334d; color: #ffffff; border-color: #00f0ff; }"
        "QPushButton:pressed { background: #161e2e; }"
    ));

    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(12, 0, 12, 0);
    barLayout->setSpacing(10);

    auto* titleLabel = new QLabel(QStringLiteral("🎼 SheetSage2 AI 动态总谱与和声联动（4 轨联动 / 激光扫描）"), bar);
    titleLabel->setStyleSheet(QStringLiteral("color: #00f0ff; font-weight: bold; font-size: 12px;"));
    barLayout->addWidget(titleLabel);

    barLayout->addStretch();

    auto* btnTranscribe = new QPushButton(QStringLiteral("⚡ AI 全曲自动采谱"), bar);
    btnTranscribe->setToolTip(QStringLiteral("将音频分轨一键转换为 4 轨动态总谱与 MIDI"));
    barLayout->addWidget(btnTranscribe);
    connect(btnTranscribe, &QPushButton::clicked, this, &DynamicScoreView::autoTranscribeRequested);

    auto* btnPdf = new QPushButton(QStringLiteral("📄 导出 PDF 乐谱"), bar);
    barLayout->addWidget(btnPdf);
    connect(btnPdf, &QPushButton::clicked, this, &DynamicScoreView::exportPdfRequested);

    rootLayout->addWidget(bar);

    // Scrollable Content
    fScroll = new QScrollArea(this);
    fScroll->setWidgetResizable(false);
    fScroll->setStyleSheet(QStringLiteral("QScrollArea { background: #090c14; border: none; }"));
    fScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    fScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    fContent = new DynamicScoreContent(fScroll);
    connect(fContent, &DynamicScoreContent::seekRequested, this, &DynamicScoreView::seekRequested);
    fScroll->setWidget(fContent);

    rootLayout->addWidget(fScroll, 1);
}

void DynamicScoreView::setPlayhead(double seconds, bool isPlaying)
{
    fContent->setPlayhead(seconds, isPlaying);

    if (isPlaying)
    {
        // Auto-scroll horizontal view to follow the playhead
        const double beats = seconds * 120.0 / 60.0;
        const double measure = beats / 4.0;
        const int x = 120 + static_cast<int>(measure * 140.0);
        const int viewportW = fScroll->viewport()->width();
        const int scrollX = fScroll->horizontalScrollBar()->value();

        if (x > scrollX + viewportW - 160 || x < scrollX + 80)
        {
            fScroll->horizontalScrollBar()->setValue(std::max(0, x - viewportW / 3));
        }
    }
}

void DynamicScoreView::setSongInfo(double bpm, const QString& key, const QString& chords)
{
    fContent->setSongInfo(bpm, key, chords);
}

void DynamicScoreView::setHasMidi(bool hasMidi)
{
    fContent->setHasMidi(hasMidi);
}

// -------------------------------------------------------------------------------------------------
// DynamicScoreContent

DynamicScoreContent::DynamicScoreContent(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    resize(sizeHint());
}

void DynamicScoreContent::setPlayhead(double seconds, bool isPlaying)
{
    fSeconds = seconds;
    fIsPlaying = isPlaying;
    update();
}

void DynamicScoreContent::setSongInfo(double bpm, const QString& key, const QString& chords)
{
    if (bpm > 20.0 && bpm < 400.0)
        fBpm = bpm;
    if (!key.isEmpty())
        fKey = key;
    if (!chords.isEmpty())
        fChordsProgression = chords;
    update();
}

void DynamicScoreContent::setHasMidi(bool hasMidi)
{
    fHasMidi = hasMidi;
    update();
}

QSize DynamicScoreContent::sizeHint() const
{
    const int totalW = kHeaderWidth + kNumMeasures * kMeasureWidth + 60;
    const int totalH = kTopMargin + 4 * (kStaffHeight + kStaffGap) + 40;
    return QSize(totalW, totalH);
}

void DynamicScoreContent::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int totalW = width();
    const int totalH = height();

    // 1. Deep Obsidian Background
    p.fillRect(0, 0, totalW, totalH, QColor(0x0a, 0x0d, 0x15));

    // Staves names and colors
    struct StaffDef {
        QString name;
        QString clef;
        QColor color;
    };
    const StaffDef staves[4] = {
        {QStringLiteral("Vocal (主旋律/モカ)"), QStringLiteral("𝄞"), QColor(0xf4, 0x72, 0xb6)},
        {QStringLiteral("E.Guitar (电吉他)"), QStringLiteral("𝄞"), QColor(0x00, 0xf0, 0xff)},
        {QStringLiteral("Piano & Strings (和声)"), QStringLiteral("𝄞"), QColor(0xa8, 0x55, 0xf7)},
        {QStringLiteral("Bass & Drums (律动)"), QStringLiteral("𝄢"), QColor(0x10, 0xb9, 0x81)}
    };

    // Calculate current playhead position
    const double beats = (fSeconds * fBpm) / 60.0;
    const double currentMeasure = beats / 4.0;
    const int activeMeasureIdx = static_cast<int>(currentMeasure);
    const double playheadX = kHeaderWidth + currentMeasure * kMeasureWidth;

    // 2. Active Measure Highlight Background
    if (activeMeasureIdx >= 0 && activeMeasureIdx < kNumMeasures)
    {
        const int mLeft = kHeaderWidth + activeMeasureIdx * kMeasureWidth;
        QRect activeRect(mLeft, kTopMargin - 20, kMeasureWidth, totalH - kTopMargin);
        p.fillRect(activeRect, QColor(0x00, 0xf0, 0xff, 25));
        p.setPen(QPen(QColor(0x00, 0xf0, 0xff, 70), 1, Qt::DashLine));
        p.drawRect(activeRect);
    }

    // 3. Staves & Measure Grid
    for (int s = 0; s < 4; ++s)
    {
        const int staffY = kTopMargin + s * (kStaffHeight + kStaffGap);

        // Track label on the left header
        p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 8, QFont::Bold));
        p.setPen(staves[s].color);
        p.drawText(QRectF(10, staffY, kHeaderWidth - 35, 18), Qt::AlignLeft | Qt::AlignVCenter, staves[s].name);

        // Clef
        p.setFont(QFont(QStringLiteral("Segoe UI Symbol"), 18));
        p.setPen(QColor(0x75, 0x88, 0xaa));
        p.drawText(QRectF(kHeaderWidth - 28, staffY - 2, 24, 32), Qt::AlignCenter, staves[s].clef);

        // Draw 5 staff lines
        p.setPen(QPen(QColor(0x23, 0x2c, 0x3d), 1.0));
        for (int line = 0; line < 5; ++line)
        {
            const int ly = staffY + line * 7;
            p.drawLine(kHeaderWidth, ly, totalW - 20, ly);
        }

        // Measure barlines and notes
        for (int m = 0; m < kNumMeasures; ++m)
        {
            const int mx = kHeaderWidth + m * kMeasureWidth;
            p.setPen(QPen(QColor(0x1e, 0x26, 0x36), 1.0));
            p.drawLine(mx, staffY, mx, staffY + 28);

            // Staff 2 (Piano) TACET demonstration during guitar solo (measures 16-24)
            if (s == 2 && m >= 16 && m < 24)
            {
                if (m == 16)
                {
                    p.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::Bold));
                    p.setPen(QColor(0x60, 0x75, 0x95));
                    p.drawText(QRectF(mx + 10, staffY + 6, kMeasureWidth * 3, 20), Qt::AlignLeft, QStringLiteral("TACET (吉他独奏留白)"));
                }
                continue;
            }

            // Draw illustrative notes & rhythm
            p.setBrush(staves[s].color);
            p.setPen(Qt::NoPen);
            for (int n = 0; n < 4; ++n)
            {
                const int nx = mx + 20 + n * 30;
                // Pitch variation based on measure and note
                const int pitchOffset = ((m * 3 + n * 5 + s * 4) % 5) * 3;
                const int ny = staffY + 22 - pitchOffset;

                // Notehead
                p.drawEllipse(QPointF(nx, ny), 3.5, 2.5);

                // Note stem
                p.setPen(QPen(staves[s].color, 1.2));
                p.drawLine(QPointF(nx + 3.0, ny), QPointF(nx + 3.0, ny - 16));
                p.setPen(Qt::NoPen);
            }

            // Lyrics under Vocal staff
            if (s == 0)
            {
                const QString lyricsSamples[8] = {
                    QStringLiteral("こ・ん・ど・こ・そ"),
                    QStringLiteral("ち・ゃ・ん・と"),
                    QStringLiteral("の・ば・す・か・ら"),
                    QStringLiteral("と・ど・け・て"),
                    QStringLiteral("す・き・な・も・ん"),
                    QStringLiteral("わ・ら・っ・て"),
                    QStringLiteral("み・せ・る・よ"),
                    QStringLiteral("ま・っ・す・ぐ")
                };
                p.setFont(QFont(QStringLiteral("Segoe UI, Microsoft YaHei"), 7));
                p.setPen(QColor(0x8e, 0xa5, 0xc7));
                p.drawText(QRectF(mx + 5, staffY + 34, kMeasureWidth - 10, 14), Qt::AlignCenter, lyricsSamples[m % 8]);
            }
        }
    }

    // 4. Measure Headers & Chords on Top
    const QString defaultChords[8] = {
        QStringLiteral("B"), QStringLiteral("E"), QStringLiteral("G#7"), QStringLiteral("C#m"),
        QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("F#m"), QStringLiteral("E")
    };
    for (int m = 0; m < kNumMeasures; ++m)
    {
        const int mx = kHeaderWidth + m * kMeasureWidth;

        // Measure number
        p.setFont(QFont(QStringLiteral("Consolas, monospace"), 8, QFont::Bold));
        p.setPen(QColor(0x50, 0x65, 0x82));
        p.drawText(mx + 4, kTopMargin - 24, QStringLiteral("#%1").arg(m + 1));

        // Chord Badge
        const QString chord = defaultChords[m % 8];
        const QRectF chordRect(mx + 30, kTopMargin - 32, 40, 16);
        p.setBrush(QColor(0x14, 0x1d, 0x2d));
        p.setPen(QPen(QColor(0x00, 0xf0, 0xff, 120), 1));
        p.drawRoundedRect(chordRect, 3, 3);

        p.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Bold));
        p.setPen(QColor(0x00, 0xf0, 0xff));
        p.drawText(chordRect, Qt::AlignCenter, chord);
    }

    // 5. Vertical Laser Playhead
    if (playheadX >= kHeaderWidth && playheadX <= totalW)
    {
        // Glowing aura
        p.setPen(QPen(QColor(0x00, 0xf0, 0xff, 40), 8.0));
        p.drawLine(QPointF(playheadX, kTopMargin - 35), QPointF(playheadX, totalH - 10));

        // Sharp bright laser core
        p.setPen(QPen(QColor(0xff, 0xff, 0xff, 240), 2.0));
        p.drawLine(QPointF(playheadX, kTopMargin - 35), QPointF(playheadX, totalH - 10));

        // Playhead triangle cap
        QPolygonF cap;
        cap << QPointF(playheadX - 6, kTopMargin - 35)
            << QPointF(playheadX + 6, kTopMargin - 35)
            << QPointF(playheadX, kTopMargin - 23);
        p.setBrush(QColor(0x00, 0xf0, 0xff));
        p.setPen(Qt::NoPen);
        p.drawPolygon(cap);
    }
}

void DynamicScoreContent::mousePressEvent(QMouseEvent* event)
{
    const double clickX = event->position().x();
    if (clickX >= kHeaderWidth)
    {
        const double measure = (clickX - kHeaderWidth) / kMeasureWidth;
        const double beats = measure * 4.0;
        const double targetSeconds = (beats * 60.0) / fBpm;
        emit seekRequested(std::max(0.0, targetSeconds));
    }
    QWidget::mousePressEvent(event);
}
