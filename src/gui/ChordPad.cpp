#include "ChordPad.h"

#include "Theme.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>

namespace {

const char* const kPadKeys = "1234567QWERTYU"; // a key for every pad, row by row

int padOfKey(int key)
{
    for (int i = 0; kPadKeys[i]; ++i)
        if (key == kPadKeys[i])
            return i;
    return -1;
}

// "A minor" -> "A 小调"
QString keyLabel(const QString& key)
{
    if (key.isEmpty())
        return QStringLiteral("?");
    return key.section(QLatin1Char(' '), 0, 0) + (key.endsWith(QLatin1String("minor")) ? QStringLiteral(" 小调") : QStringLiteral(" 大调"));
}

QString beatsLabel(double beats)
{
    if (std::abs(beats - 4.0) < 1e-6)
        return QStringLiteral("1 小节");
    if (std::fmod(beats, 4.0) < 1e-6)
        return QStringLiteral("%1 小节").arg(beats / 4.0);
    if (std::abs(beats - 2.0) < 1e-6)
        return QStringLiteral("半小节");
    return QStringLiteral("%1 拍").arg(QString::number(beats, 'g', 3));
}

constexpr int kChordRole = Qt::UserRole;
constexpr int kBeatsRole = Qt::UserRole + 1;

} // namespace

// One pad: the chord big, its numeral, its key; lit up as much as it fits after the chord before.
class ChordPadButton : public QAbstractButton {
public:
    ChordPadButton(QWidget* parent)
        : QAbstractButton(parent)
    {
        setMinimumSize(78, 64);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setFocusPolicy(Qt::NoFocus);
    }

    void set(const QString& chord, const QString& numeral, QChar key)
    {
        fChord = chord;
        fNumeral = numeral;
        fKey = key;
        update();
    }
    void setScore(float score, bool star)
    {
        fScore = score;
        fStar = star;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
        QColor fill = isDown() ? theme::accent.darker(130) : theme::panelLight;
        if (!isDown() && fStar)
            fill = QColor(0x3a, 0x4a, 0x5c);
        p.setPen(QPen(fStar ? theme::accent : theme::border, fStar ? 2.0 : 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(r, 6, 6);
        // how well it would follow: a bar along the bottom
        if (fScore > 0.f)
        {
            QColor bar = theme::accent;
            bar.setAlphaF(0.25 + 0.75 * fScore);
            p.setPen(Qt::NoPen);
            p.setBrush(bar);
            p.drawRoundedRect(QRectF(r.left() + 6, r.bottom() - 7, (r.width() - 12) * fScore, 4), 2, 2);
        }
        QFont big = font();
        big.setPointSizeF(big.pointSizeF() * 1.45);
        big.setBold(true);
        p.setFont(big);
        p.setPen(theme::text);
        p.drawText(r.adjusted(0, -6, 0, -10), Qt::AlignCenter, fChord);
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        p.setFont(small);
        p.setPen(theme::textDim);
        p.drawText(r.adjusted(6, 4, -6, -10), Qt::AlignLeft | Qt::AlignTop, fNumeral);
        p.drawText(r.adjusted(6, 4, -6, -10), Qt::AlignRight | Qt::AlignTop, fKey);
        if (fStar)
        {
            p.setPen(theme::solo);
            p.drawText(r.adjusted(6, 4, -6, -12), Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("★"));
        }
    }

private:
    QString fChord, fNumeral;
    QChar fKey;
    float fScore = 0.f;
    bool fStar = false;
};

ChordPadDialog::ChordPadDialog(const QString& key, double tempo, const QString& outputFolder, QWidget* parent)
    : QDialog(parent)
    , fSongKey(key)
    , fTempo(tempo)
    , fOutputFolder(outputFolder)
{
    setWindowTitle(QStringLiteral("和弦板"));
    resize(760, 0);
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "点一下和弦就能听（也可以按键盘 1–7 和 Q–U）。亮起来、带 ★ 的是接在刚才那个和弦后面最顺的。"
        "点过的和弦会排成下面的和弦进行，可以写成一条 MIDI 轨，或者交给 AI 自动编曲。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("调性")));
    fKey = new QComboBox;
    top->addWidget(fKey);
    fSevenths = new QCheckBox(QStringLiteral("七和弦"));
    top->addWidget(fSevenths);
    top->addSpacing(16);
    top->addWidget(new QLabel(QStringLiteral("用这条轨道的乐器听")));
    fTarget = new QComboBox;
    fTarget->setMinimumWidth(160);
    top->addWidget(fTarget, 1);
    auto* newTrack = new QPushButton(QStringLiteral("新建钢琴轨"));
    top->addWidget(newTrack);
    layout->addLayout(top);
    connect(fSevenths, &QCheckBox::toggled, this, &ChordPadDialog::rebuildPads);
    connect(fKey, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        rebuildPads();
        if (!fKey->currentData().toString().isEmpty())
            fStatus->setText(QStringLiteral("和弦板换成了 %1。工程的调性没变（修音、编曲用工程的调性）。").arg(keyLabel(fKey->currentData().toString())));
    });
    connect(newTrack, &QPushButton::clicked, this, &ChordPadDialog::newTrackRequested);

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    auto* rowOne = new QLabel(QStringLiteral("本调和弦"));
    auto* rowTwo = new QLabel(QStringLiteral("色彩和弦"));
    for (QLabel* label : {rowOne, rowTwo})
        label->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textDim.name()));
    grid->addWidget(rowOne, 0, 0);
    grid->addWidget(rowTwo, 1, 0);
    for (int i = 0; i < 14; ++i)
    {
        auto* button = new ChordPadButton(this);
        grid->addWidget(button, i / 7, 1 + i % 7);
        connect(button, &QAbstractButton::pressed, this, [this, i] { press(i); });
        connect(button, &QAbstractButton::released, this, [this, i] { release(i); });
        fButtons.push_back(button);
    }
    layout->addLayout(grid);
    fSuggestion = new QLabel;
    fSuggestion->setWordWrap(true);
    layout->addWidget(fSuggestion);

    // the progression
    auto* progressionRow = new QHBoxLayout;
    auto* progressionLabel = new QLabel(QStringLiteral("<b>和弦进行</b>"));
    progressionRow->addWidget(progressionLabel);
    fRecord = new QCheckBox(QStringLiteral("点和弦时加进来"));
    fRecord->setChecked(true);
    progressionRow->addWidget(fRecord);
    progressionRow->addStretch();
    fPlay = new QPushButton(QStringLiteral("▶ 试听进行"));
    auto* undoLast = new QPushButton(QStringLiteral("删掉最后一个"));
    auto* clear = new QPushButton(QStringLiteral("清空"));
    for (QPushButton* b : {fPlay, undoLast, clear})
    {
        b->setFocusPolicy(Qt::NoFocus);
        progressionRow->addWidget(b);
    }
    layout->addLayout(progressionRow);

    fList = new QListWidget;
    fList->setFlow(QListView::LeftToRight);
    fList->setWrapping(true);
    fList->setResizeMode(QListView::Adjust);
    fList->setSpacing(3);
    fList->setGridSize(QSize(76, 46));
    fList->setDragDropMode(QAbstractItemView::InternalMove);
    fList->setDefaultDropAction(Qt::MoveAction);
    fList->setMinimumHeight(104);
    fList->setFocusPolicy(Qt::NoFocus);
    fList->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(fList);
    connect(fList, &QWidget::customContextMenuRequested, this, &ChordPadDialog::chordMenu);
    // dragged into another order: the progression follows the list
    connect(fList->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        std::vector<chost::ProgressionChord> chords;
        for (int i = 0; i < fList->count(); ++i)
            chords.push_back({chost::parseChord(fList->item(i)->data(kChordRole).toString().toStdString()), fList->item(i)->data(kBeatsRole).toDouble()});
        fProgression = chords;
    });
    connect(fList, &QListWidget::itemPressed, this, [this](QListWidgetItem* item) {
        // a chord of the progression clicked: it sounds, and the pads say what could follow it
        const chost::Chord chord = chost::parseChord(item->data(kChordRole).toString().toStdString());
        if (!chord.valid())
            return;
        const std::vector<int> voicing = chost::voiceChord(chord, fVoicing);
        fVoicing = voicing;
        std::vector<int> pitches{chost::bassNote(chord)};
        pitches.insert(pitches.end(), voicing.begin(), voicing.end());
        if (target() >= 0)
        {
            emit notes(target(), pitches, true);
            QTimer::singleShot(600, this, [this, pitches, t = target()] { emit notes(t, pitches, false); });
        }
        suggest(&chord);
    });
    connect(fPlay, &QPushButton::clicked, this, [this] { fPreview.isActive() ? stopPreview() : playProgression(); });
    connect(undoLast, &QPushButton::clicked, this, [this] {
        if (!fProgression.empty())
            fProgression.pop_back();
        refreshProgression();
        suggest(fProgression.empty() ? nullptr : &fProgression.back().chord);
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        stopPreview();
        fProgression.clear();
        fStart = 0.0;
        refreshProgression();
        suggest(nullptr);
    });

    // recognising a song's chords
    auto* recogniseRow = new QHBoxLayout;
    recogniseRow->addWidget(new QLabel(QStringLiteral("从歌曲里听出和弦")));
    fSource = new QComboBox;
    recogniseRow->addWidget(fSource, 1);
    fRecognise = new QPushButton(QStringLiteral("识别和弦"));
    recogniseRow->addWidget(fRecognise);
    layout->addLayout(recogniseRow);
    connect(fRecognise, &QPushButton::clicked, this, [this] { recognise(fSource->currentIndex()); });

    // the progression goes on
    auto* outRow = new QHBoxLayout;
    outRow->addWidget(new QLabel(QStringLiteral("弹法")));
    fPattern = new QComboBox;
    fPattern->addItem(QStringLiteral("柱式和弦（一直按着）"), static_cast<int>(chost::ChordPattern::Block));
    fPattern->addItem(QStringLiteral("分解和弦（八分音符）"), static_cast<int>(chost::ChordPattern::Arpeggio));
    fPattern->addItem(QStringLiteral("流行钢琴节奏"), static_cast<int>(chost::ChordPattern::Rhythm));
    outRow->addWidget(fPattern);
    auto* writeButton = new QPushButton(QStringLiteral("写成 MIDI 轨"));
    auto* arrangeButton = new QPushButton(QStringLiteral("交给 AI 自动编曲…"));
    outRow->addStretch();
    outRow->addWidget(writeButton);
    outRow->addWidget(arrangeButton);
    layout->addLayout(outRow);
    connect(writeButton, &QPushButton::clicked, this, &ChordPadDialog::write);
    connect(arrangeButton, &QPushButton::clicked, this, [this] {
        if (fProgression.empty())
        {
            fStatus->setText(QStringLiteral("先点几个和弦，排出一段和弦进行。"));
            return;
        }
        emit arrangeRequested(progression());
    });

    fStatus = new QLabel;
    fStatus->setObjectName(QStringLiteral("status")); // these and the busy property: what the remote control reports
    fStatus->setWordWrap(true);
    layout->addWidget(fStatus);

    fPreview.setSingleShot(true);
    connect(&fPreview, &QTimer::timeout, this, &ChordPadDialog::previewStep);

    connect(&fRecognition, &ScriptJob::progress, this, [this](int percent, const QString&) {
        fStatus->setText(QStringLiteral("正在听和弦… %1%").arg(percent));
    });
    connect(&fRecognition, &ScriptJob::result, this, [this](const QString& key, const QString& value) {
        if (key == QLatin1String("START"))
            fStart = std::max(0.0, value.toDouble());
        else if (key == QLatin1String("CHORDS"))
            setProgression(value, fStart);
    });
    connect(&fRecognition, &ScriptJob::finished, this, [this](bool ok, const QString& message) {
        setProperty("busy", false);
        fRecognise->setEnabled(true);
        if (ok)
            fStatus->setText(QStringLiteral("✓ 识别完成：%1 个小节的和弦。播放歌曲时，和弦板会跟着显示现在的和弦，并提示接下来可以用什么。")
                                 .arg(std::ceil(std::accumulate(fProgression.begin(), fProgression.end(), 0.0,
                                                                [](double sum, const chost::ProgressionChord& c) { return sum + c.beats; }) / 4.0)));
        else
            fStatus->setText(QStringLiteral("识别失败：%1").arg(message));
    });

    // the pad keys work whatever has the focus in here
    for (QWidget* child : findChildren<QWidget*>())
        child->installEventFilter(this);

    setSong(key, tempo);
    refreshProgression();
}

ChordPadDialog::~ChordPadDialog()
{
    fRecognition.cancel();
}

void ChordPadDialog::setSong(const QString& key, double tempo)
{
    fSongKey = key;
    fTempo = tempo > 0.0 ? tempo : 120.0;
    const QString chosen = fKey->currentData().toString();
    const QSignalBlocker blocker(fKey);
    fKey->clear();
    fKey->addItem(fSongKey.isEmpty() ? QStringLiteral("跟工程（还不知道，先用 C 大调）") : QStringLiteral("跟工程：%1").arg(keyLabel(fSongKey)), QString());
    static const char* const names[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    for (const char* mode : {"major", "minor"})
        for (const char* name : names)
        {
            const QString k = QStringLiteral("%1 %2").arg(QLatin1String(name), QLatin1String(mode));
            fKey->addItem(keyLabel(k), k);
        }
    fKey->setCurrentIndex(std::max(0, fKey->findData(chosen)));
    rebuildPads();
}

chost::Key ChordPadDialog::key() const
{
    const QString chosen = fKey->currentData().toString();
    chost::Key k = chost::parseKey((chosen.isEmpty() ? fSongKey : chosen).toStdString());
    if (!k.valid)
        k = chost::Key{0, false, true};
    return k;
}

void ChordPadDialog::setTracks(const std::vector<Track>& instruments, const std::vector<Track>& audio, int selected)
{
    const int was = fTarget->currentData().toInt();
    const bool had = fTarget->count() > 0 && fTarget->currentData().isValid();
    fTarget->clear();
    for (const Track& t : instruments)
        fTarget->addItem(t.name, t.index);
    if (instruments.empty())
        fTarget->addItem(QStringLiteral("（没有乐器轨：点右边新建）"));
    // the selected instrument track, else the one used so far, else the first
    int index = fTarget->findData(selected);
    if (index < 0 && had)
        index = fTarget->findData(was);
    fTarget->setCurrentIndex(std::max(0, index));

    fAudio = audio;
    const QString source = fSource->currentText();
    fSource->clear();
    for (const Track& t : audio)
        fSource->addItem(t.name);
    fSource->setCurrentIndex(std::max(0, fSource->findText(source)));
    fRecognise->setEnabled(!audio.empty() && !fRecognition.isRunning());
}

int ChordPadDialog::target() const
{
    const QVariant data = fTarget->currentData();
    return data.isValid() ? data.toInt() : -1;
}

void ChordPadDialog::rebuildPads()
{
    const chost::Key k = key();
    fPads = chost::chordPads(k, fSevenths->isChecked());
    for (size_t i = 0; i < fButtons.size() && i < fPads.size(); ++i)
        fButtons[i]->set(QString::fromStdString(fPads[i].chord.name(k)), QString::fromStdString(fPads[i].numeral), QLatin1Char(kPadKeys[i]));
    suggest(fProgression.empty() ? nullptr : &fProgression.back().chord);
    refreshProgression(); // names spelt for the key
}

void ChordPadDialog::suggest(const chost::Chord* previous)
{
    const chost::Key k = key();
    const std::vector<float> scores = chost::nextChordScores(k, fPads, previous);
    std::vector<size_t> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return scores[a] > scores[b]; });
    std::vector<size_t> best;
    for (size_t i : order)
    {
        // one pad per chord: the same chord on both rows counts once
        const bool same = std::any_of(best.begin(), best.end(), [&](size_t b) { return fPads[b].chord.name() == fPads[i].chord.name(); });
        if (best.size() < 3 && scores[i] >= 0.45f && !same)
            best.push_back(i);
    }
    QStringList names;
    for (size_t i = 0; i < fButtons.size() && i < scores.size(); ++i)
        fButtons[i]->setScore(scores[i], std::find(best.begin(), best.end(), i) != best.end());
    for (size_t i : best)
        names << QStringLiteral("%1（%2）").arg(QString::fromStdString(fPads[i].chord.name(k)), QString::fromStdString(fPads[i].numeral));
    if (previous)
        fSuggestion->setText(QStringLiteral("<b>%1</b> 后面可以接：%2").arg(QString::fromStdString(previous->name(k)), names.join(QStringLiteral("、"))));
    else
        fSuggestion->setText(QStringLiteral("一般从这几个开始：%1").arg(names.join(QStringLiteral("、"))));
}

void ChordPadDialog::press(int pad)
{
    if (pad < 0 || pad >= static_cast<int>(fPads.size()))
        return;
    const chost::Chord& chord = fPads[static_cast<size_t>(pad)].chord;
    const std::vector<int> voicing = chost::voiceChord(chord, fVoicing);
    fVoicing = voicing;
    if (!fSounding.empty())
        emit notes(fSoundingTrack, fSounding, false);
    fSounding = {chost::bassNote(chord)};
    fSounding.insert(fSounding.end(), voicing.begin(), voicing.end());
    fSoundingTrack = target();
    if (fSoundingTrack >= 0)
        emit notes(fSoundingTrack, fSounding, true);
    else
        fStatus->setText(QStringLiteral("还没有乐器轨，听不到声音：点“新建钢琴轨”。"));
    if (fRecord->isChecked())
        addToProgression(chord);
    suggest(&chord);
}

void ChordPadDialog::release(int)
{
    if (!fSounding.empty() && fSoundingTrack >= 0)
        emit notes(fSoundingTrack, fSounding, false);
    fSounding.clear();
}

void ChordPadDialog::addToProgression(const chost::Chord& chord)
{
    fProgression.push_back({chord, 4.0});
    refreshProgression();
    fList->scrollToBottom();
}

void ChordPadDialog::refreshProgression()
{
    if (!fList)
        return;
    const chost::Key k = key();
    const QSignalBlocker blocker(fList);
    fList->clear();
    for (size_t i = 0; i < fProgression.size(); ++i)
    {
        const chost::ProgressionChord& c = fProgression[i];
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(QString::fromStdString(c.chord.name(k)), beatsLabel(c.beats)));
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(kChordRole, QString::fromStdString(c.chord.name(k)));
        item->setData(kBeatsRole, c.beats);
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
        if (static_cast<int>(i) == fPlaying || static_cast<int>(i) == fPreviewIndex)
        {
            item->setBackground(theme::accent.darker(150));
            item->setForeground(theme::text);
        }
        fList->addItem(item);
    }
    if (fProgression.empty())
        fStatus->setText(fStatus->text().isEmpty() ? QStringLiteral("还没有和弦进行：点上面的和弦，或者从歌曲里识别。") : fStatus->text());
}

void ChordPadDialog::setProgression(const QString& text, double startSeconds)
{
    std::string error;
    const std::vector<chost::ProgressionChord> chords = chost::parseProgression(text.toStdString(), error);
    if (chords.empty())
    {
        fStatus->setText(QString::fromStdString(error));
        return;
    }
    // a chord that goes on bar after bar is one long chord
    fProgression.clear();
    for (const chost::ProgressionChord& c : chords)
    {
        if (!fProgression.empty() && fProgression.back().chord.name() == c.chord.name())
            fProgression.back().beats += c.beats;
        else
            fProgression.push_back(c);
    }
    fStart = startSeconds;
    refreshProgression();
    suggest(&fProgression.back().chord);
}

QString ChordPadDialog::progression() const
{
    return QString::fromStdString(chost::progressionText(fProgression, key()));
}

void ChordPadDialog::chordMenu(const QPoint& pos)
{
    QListWidgetItem* item = fList->itemAt(pos);
    if (!item)
        return;
    const int row = fList->row(item);
    QMenu menu(this);
    for (double beats : {2.0, 4.0, 8.0, 16.0})
        menu.addAction(beatsLabel(beats), this, [this, row, beats] {
            fProgression[static_cast<size_t>(row)].beats = beats;
            refreshProgression();
        });
    menu.addSeparator();
    menu.addAction(QStringLiteral("删除"), this, [this, row] {
        fProgression.erase(fProgression.begin() + row);
        refreshProgression();
    });
    menu.exec(fList->viewport()->mapToGlobal(pos));
}

void ChordPadDialog::playProgression()
{
    if (fProgression.empty())
    {
        fStatus->setText(QStringLiteral("先点几个和弦，排出一段和弦进行。"));
        return;
    }
    if (target() < 0)
    {
        fStatus->setText(QStringLiteral("还没有乐器轨，听不到声音：点“新建钢琴轨”。"));
        return;
    }
    fPreviewIndex = -1;
    fVoicing.clear();
    fPlay->setText(QStringLiteral("■ 停止"));
    previewStep();
}

void ChordPadDialog::previewStep()
{
    if (!fSounding.empty())
        emit notes(fSoundingTrack, fSounding, false);
    fSounding.clear();
    ++fPreviewIndex;
    if (fPreviewIndex >= static_cast<int>(fProgression.size()))
    {
        stopPreview();
        return;
    }
    const chost::ProgressionChord& c = fProgression[static_cast<size_t>(fPreviewIndex)];
    const std::vector<int> voicing = chost::voiceChord(c.chord, fVoicing);
    fVoicing = voicing;
    fSounding = {chost::bassNote(c.chord)};
    fSounding.insert(fSounding.end(), voicing.begin(), voicing.end());
    fSoundingTrack = target();
    emit notes(fSoundingTrack, fSounding, true);
    refreshProgression();
    suggest(&c.chord);
    fPreview.start(static_cast<int>(c.beats * 60000.0 / fTempo));
}

void ChordPadDialog::stopPreview()
{
    fPreview.stop();
    if (!fSounding.empty())
        emit notes(fSoundingTrack, fSounding, false);
    fSounding.clear();
    fPreviewIndex = -1;
    fPlay->setText(QStringLiteral("▶ 试听进行"));
    refreshProgression();
}

void ChordPadDialog::setPlayhead(double seconds, bool playing)
{
    int index = -1;
    if (playing && !fPreview.isActive() && !fProgression.empty())
    {
        const double beat = (seconds - fStart) * fTempo / 60.0;
        double at = 0.0;
        for (size_t i = 0; i < fProgression.size() && beat >= 0.0; ++i)
        {
            if (beat < at + fProgression[i].beats)
            {
                index = static_cast<int>(i);
                break;
            }
            at += fProgression[i].beats;
        }
    }
    if (index == fPlaying)
        return;
    fPlaying = index;
    refreshProgression();
    if (index >= 0)
    {
        fList->scrollToItem(fList->item(index));
        suggest(&fProgression[static_cast<size_t>(index)].chord);
    }
}

void ChordPadDialog::recognise(int audioTrack)
{
    if (audioTrack < 0 || audioTrack >= static_cast<int>(fAudio.size()) || fRecognition.isRunning())
        return;
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/chost-chords");
    setProperty("busy", true);
    fRecognise->setEnabled(false);
    fStatus->setText(QStringLiteral("正在听 %1 的和弦…").arg(fAudio[static_cast<size_t>(audioTrack)].name));
    if (!fRecognition.start(QStringLiteral("chost_arrange.py"),
                            {QStringLiteral("--chords-only"), QStringLiteral("--output-dir"), folder, QStringLiteral("--tempo"),
                             QString::number(fTempo), QStringLiteral("--audio"), fAudio[static_cast<size_t>(audioTrack)].file}))
    {
        setProperty("busy", false);
        fRecognise->setEnabled(true);
    }
}

void ChordPadDialog::write()
{
    if (fProgression.empty())
    {
        fStatus->setText(QStringLiteral("先点几个和弦，排出一段和弦进行。"));
        return;
    }
    QDir().mkpath(fOutputFolder);
    QString path;
    for (int n = 1; path.isEmpty() || QFileInfo::exists(path); ++n)
        path = QDir(fOutputFolder).filePath(QStringLiteral("和弦_%1.mid").arg(n));
    const auto pattern = static_cast<chost::ChordPattern>(fPattern->currentData().toInt());
    const chost::MidiClip clip = chost::chordClip(fProgression, fTempo, pattern, fStart);
    std::string error;
    if (!chost::writeMidiFile(path.toStdString(), clip, error))
    {
        fStatus->setText(QStringLiteral("写不了 MIDI 文件：%1").arg(QString::fromStdString(error)));
        return;
    }
    fStatus->setText(QStringLiteral("✓ 写好了：%1").arg(QDir::toNativeSeparators(path)));
    emit written(target(), path);
}

void ChordPadDialog::keyPressEvent(QKeyEvent* event)
{
    const int pad = padOfKey(event->key());
    if (pad < 0 || event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
    {
        QDialog::keyPressEvent(event);
        return;
    }
    if (!event->isAutoRepeat())
    {
        fButtons[static_cast<size_t>(pad)]->setDown(true);
        press(pad);
    }
}

void ChordPadDialog::keyReleaseEvent(QKeyEvent* event)
{
    const int pad = padOfKey(event->key());
    if (pad < 0 || event->isAutoRepeat())
    {
        QDialog::keyReleaseEvent(event);
        return;
    }
    fButtons[static_cast<size_t>(pad)]->setDown(false);
    release(pad);
}

bool ChordPadDialog::eventFilter(QObject* watched, QEvent* event)
{
    // the pad keys, before a list or a box takes them for itself
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if (padOfKey(key->key()) >= 0 && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier)))
        {
            event->type() == QEvent::KeyPress ? keyPressEvent(key) : keyReleaseEvent(key);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ChordPadDialog::closeEvent(QCloseEvent* event)
{
    stopPreview();
    release(-1);
    fRecognition.cancel();
    QDialog::closeEvent(event);
}
