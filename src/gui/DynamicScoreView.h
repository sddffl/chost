#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QString>
#include <vector>

class DynamicScoreContent;

class DynamicScoreView : public QWidget
{
    Q_OBJECT

public:
    explicit DynamicScoreView(QWidget* parent = nullptr);
    ~DynamicScoreView() override = default;

    void setPlayhead(double seconds, bool isPlaying);
    void setSongInfo(double bpm, const QString& key, const QString& chords);
    void setHasMidi(bool hasMidi);

signals:
    void seekRequested(double seconds);
    void autoTranscribeRequested();
    void exportPdfRequested();

private:
    DynamicScoreContent* fContent = nullptr;
    QScrollArea* fScroll = nullptr;
};

class DynamicScoreContent : public QWidget
{
    Q_OBJECT

public:
    explicit DynamicScoreContent(QWidget* parent = nullptr);

    void setPlayhead(double seconds, bool isPlaying);
    void setSongInfo(double bpm, const QString& key, const QString& chords);
    void setHasMidi(bool hasMidi);

signals:
    void seekRequested(double seconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    double fSeconds = 0.0;
    double fBpm = 120.0;
    QString fKey{QStringLiteral("E Major")};
    QString fChordsProgression{QStringLiteral("B | B | G#7 | C#m | A | B | E | E")};
    bool fIsPlaying = false;
    bool fHasMidi = true;

    // Layout metrics
    const int kMeasureWidth = 140;
    const int kHeaderWidth = 120;
    const int kNumMeasures = 64;
    const int kStaffHeight = 65;
    const int kStaffGap = 28;
    const int kTopMargin = 45;
};
