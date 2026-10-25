#include "TuneDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

static const char* const kNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static constexpr double kPixelsPerSecond = 90.0;
static constexpr int kViewHeight = 230;

// ---------------------------------------------------------------------------------------------------------------------
// PitchView

PitchView::PitchView(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(kViewHeight);
    setToolTip(QStringLiteral("灰线：原来唱的音高；橙线：修音后的音高；横条：每个音修正到的位置。"
                              "亮一些的横格是调内音。"));
}

void PitchView::setScale(const QList<int>& pitchClasses)
{
    fScale = pitchClasses;
    update();
}

void PitchView::setNotes(const std::vector<Note>& notes)
{
    fNotes = notes;
    fit();
}

void PitchView::setCurves(double hop, const std::vector<float>& sung, const std::vector<float>& tuned)
{
    fHop = hop > 0.0 ? hop : 0.01;
    fSung = sung;
    fTuned = tuned;
    fit();
}

void PitchView::clear()
{
    fNotes.clear();
    fSung.clear();
    fTuned.clear();
    fit();
}

void PitchView::fit()
{
    double low = 1e9, high = -1e9, end = 0.0;
    for (const Note& n : fNotes)
    {
        low = std::min({low, n.sung, n.corrected});
        high = std::max({high, n.sung, n.corrected});
        end = std::max(end, n.end);
    }
    end = std::max(end, fSung.size() * fHop);
    if (low > high)
        low = 60.0, high = 72.0;
    // a little room around the lowest and highest notes, at least an octave and a half in view
    const double middle = (low + high) / 2.0;
    const double half = std::max(9.0, (high - low) / 2.0 + 2.5);
    fLow = std::floor(middle - half);
    fHigh = std::ceil(middle + half);
    setMinimumWidth(std::max(560, static_cast<int>(end * kPixelsPerSecond) + 50));
    resize(minimumWidth(), height());
    update();
}

void PitchView::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x1b, 0x1e, 0x22));
    const double left = 40.0, top = 6.0, bottom = height() - 18.0;
    const double row = (bottom - top) / (fHigh - fLow);
    auto x = [&](double seconds) { return left + seconds * kPixelsPerSecond; };
    auto y = [&](double pitch) { return bottom - (pitch - fLow) * row; };

    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.8);
    p.setFont(small);
    const QRectF visible = QRectF(event->rect());
    // the notes of the key a shade lighter, so you see what the voice was pulled to
    for (int note = static_cast<int>(fLow); note < static_cast<int>(fHigh); ++note)
    {
        const bool inKey = fScale.isEmpty() || fScale.contains(note % 12);
        const QRectF band(left, y(note + 0.5), width() - left, row);
        p.fillRect(band, inKey ? QColor(0x24, 0x28, 0x2e) : QColor(0x1b, 0x1e, 0x22));
        if (note % 12 == 0 || (note % 12 == fScale.value(0, 0) && row > 7))
        {
            p.setPen(QColor(0x8b, 0x90, 0x96));
            p.drawText(QRectF(visible.left(), y(note) - 7, left - 4, 14), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1%2").arg(QLatin1String(kNames[note % 12])).arg(note / 12 - 1));
        }
    }
    p.setPen(QColor(0x3a, 0x3f, 0x46));
    const double seconds = (width() - left) / kPixelsPerSecond;
    for (int s = 0; s <= seconds; ++s)
    {
        if (s % 5)
            continue;
        p.drawLine(QPointF(x(s), top), QPointF(x(s), bottom));
        p.setPen(QColor(0x8b, 0x90, 0x96));
        p.drawText(QRectF(x(s) - 20, bottom + 2, 40, 14), Qt::AlignCenter, QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0')));
        p.setPen(QColor(0x3a, 0x3f, 0x46));
    }

    for (const Note& n : fNotes)
    {
        const QRectF bar(x(n.start), y(n.corrected) - row * 0.4, (n.end - n.start) * kPixelsPerSecond, row * 0.8);
        if (!bar.intersects(visible))
            continue;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe0, 0xa4, 0x3a, 50));
        p.drawRoundedRect(bar, 3, 3);
    }

    auto curve = [&](const std::vector<float>& values, const QColor& colour, double widthPx) {
        QPainterPath path;
        bool drawing = false;
        for (size_t i = 0; i < values.size(); ++i)
        {
            const double t = i * fHop;
            if (values[i] <= 0.0f || x(t) < visible.left() - 10 || x(t) > visible.right() + 10)
            {
                drawing = false;
                continue;
            }
            const QPointF pt(x(t), y(values[i]));
            if (drawing)
                path.lineTo(pt);
            else
                path.moveTo(pt);
            drawing = true;
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(colour, widthPx));
        p.drawPath(path);
    };
    curve(fSung, QColor(0x9a, 0xa0, 0xa8), 1.2);
    curve(fTuned, QColor(0xe0, 0xa4, 0x3a), 1.8);

    if (fSung.empty())
    {
        p.setPen(QColor(0x8b, 0x90, 0x96));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("修音完成后，这里显示每个音修正前后的音高"));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// TuneDialog

namespace {
struct Preset {
    const char* name;
    int strength; // percent
    double drift;
    int speed;    // ms
    bool hard;
};
// what a vocal producer would reach for
const Preset kPresets[] = {
    {"自然（推荐）：修准每个音，保留颤音和滑音", 100, 0.3, 40, false},
    {"轻微：只往准里推一些，听不出修过", 70, 0.0, 60, false},
    {"精准：录音室级别，音内的漂移也拉直", 100, 0.7, 25, false},
    {"电音效果（Auto-Tune）：音高直接跳到音上", 100, 1.0, 0, true},
};
} // namespace

QString TuneDialog::tunedFileFor(const QString& original)
{
    const QFileInfo info(original);
    QString folder = info.absolutePath();
    if (!QFileInfo(folder).isWritable())
        folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/tune");
    return QDir(folder).filePath(info.completeBaseName() + QStringLiteral("_修音.wav"));
}

TuneDialog::TuneDialog(const std::vector<Source>& sources, int selected, const QString& key, QWidget* parent)
    : QDialog(parent)
    , fSources(sources)
    , fProjectKey(key)
{
    setWindowTitle(QStringLiteral("AI 修音"));
    resize(720, 0);
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "把唱歌（或独奏乐器）里跑调的音逐个修准：每个音整体移到调里最近的音上，颤音、滑音这些表情都保留，"
        "声音的音色不变。修好的声音直接替换这条轨道，插件效果和音量都不变，随时可以切回原声对比。"
        "效果最好的是单独的人声：如果是整首歌，先用“AI 自动分轨”把人声分出来。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    fTrack = new QComboBox;
    int first = -1;
    for (size_t i = 0; i < fSources.size(); ++i)
    {
        fTrack->addItem(fSources[i].name);
        if (first < 0 && fSources[i].vocal)
            first = static_cast<int>(i);
    }
    fTrack->setCurrentIndex(selected >= 0 ? selected : std::max(first, 0));
    form->addRow(QStringLiteral("轨道"), fTrack);

    fKey = new QComboBox;
    const QStringList projectKey = fProjectKey.split(QLatin1Char(' '));
    if (projectKey.size() == 2)
        fKey->addItem(QStringLiteral("跟工程一样：%1").arg(keyName(fProjectKey)), fProjectKey);
    fKey->addItem(QStringLiteral("自动识别：听这段人声是什么调"), QStringLiteral("auto"));
    fKey->addItem(QStringLiteral("不按调性：每个音对齐到最近的半音"), QString());
    for (const char* mode : {"major", "minor"})
        for (const char* tonic : kNames)
        {
            const QString key = QStringLiteral("%1 %2").arg(QLatin1String(tonic), QLatin1String(mode));
            fKey->addItem(keyName(key), key);
        }
    form->addRow(QStringLiteral("调性"), fKey);

    fStrict = new QCheckBox(QStringLiteral("只用调内音（唱得很准的调外音也拉回调里）"));
    fStrict->setToolTip(QStringLiteral("不勾：唱得清清楚楚的调外音（经过音、布鲁斯音）保持不动，只修模棱两可的。"));
    form->addRow(QString(), fStrict);

    fPreset = new QComboBox;
    for (const Preset& preset : kPresets)
        fPreset->addItem(QString::fromUtf8(preset.name));
    form->addRow(QStringLiteral("修法"), fPreset);

    auto* strengthRow = new QHBoxLayout;
    fStrength = new QSlider(Qt::Horizontal);
    fStrength->setRange(0, 100);
    strengthRow->addWidget(fStrength, 1);
    fStrengthLabel = new QLabel;
    fStrengthLabel->setMinimumWidth(44);
    strengthRow->addWidget(fStrengthLabel);
    form->addRow(QStringLiteral("力度"), strengthRow);
    layout->addLayout(form);

    fStatus = new QLabel;
    fStatus->setObjectName(QStringLiteral("status")); // these and the busy property: what the remote control reports
    fStatus->setWordWrap(true);
    layout->addWidget(fStatus);
    fProgress = new QProgressBar;
    fProgress->setObjectName(QStringLiteral("progress"));
    fProgress->setRange(0, 100);
    layout->addWidget(fProgress);
    fResult = new QLabel;
    fResult->setWordWrap(true);
    layout->addWidget(fResult);

    fView = new PitchView;
    fScroll = new QScrollArea;
    fScroll->setWidget(fView);
    fScroll->setWidgetResizable(false);
    fScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    fScroll->setFixedHeight(kViewHeight + fScroll->horizontalScrollBar()->sizeHint().height() + 4);
    layout->addWidget(fScroll);

    auto* compare = new QHBoxLayout;
    fAB = new QPushButton;
    fAB->setCheckable(true);
    fAB->setToolTip(QStringLiteral("在修音后和原声之间切换，插件效果都一样"));
    compare->addWidget(fAB);
    fPlay = new QPushButton(QStringLiteral("▶ 播放 / 暂停"));
    compare->addWidget(fPlay);
    compare->addStretch();
    layout->addLayout(compare);

    auto* buttons = new QHBoxLayout;
    fOpenFolder = new QPushButton(QStringLiteral("打开所在文件夹"));
    buttons->addWidget(fOpenFolder);
    buttons->addStretch();
    fCancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(fCancel);
    fStart = new QPushButton(QStringLiteral("开始修音"));
    fStart->setDefault(true);
    buttons->addWidget(fStart);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(fPreset, &QComboBox::currentIndexChanged, this, &TuneDialog::applyPreset);
    connect(fStrength, &QSlider::valueChanged, this, [this](int value) { fStrengthLabel->setText(QStringLiteral("%1%").arg(value)); });
    connect(fKey, &QComboBox::currentIndexChanged, this, [this] { fView->setScale(scaleOf(chosenKey())); });
    connect(fTrack, &QComboBox::currentIndexChanged, this, [this] {
        // another track: the last result is not about it
        fDone = false;
        fView->clear();
        fResult->clear();
        fProgress->setValue(0);
        fStatus->clear();
        setBusy(false);
    });
    connect(fStart, &QPushButton::clicked, this, &TuneDialog::start);
    connect(fCancel, &QPushButton::clicked, this, [this] { fScript.cancel(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(fOpenFolder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(fOutput).absolutePath()));
    });
    connect(fAB, &QPushButton::clicked, this, [this](bool tuned) {
        setListening(tuned);
        emit listen(fOriginal, tuned);
    });
    connect(fPlay, &QPushButton::clicked, this, &TuneDialog::playToggled);

    connect(&fScript, &ScriptJob::progress, this, [this](int percent, const QString& message) {
        if (message.startsWith(QLatin1String("listening")))
            fStatus->setText(QStringLiteral("正在听每个音唱得多高…"));
        else if (message.startsWith(QLatin1String("finding")))
            fStatus->setText(QStringLiteral("正在把音高分成一个个音…"));
        else if (message.startsWith(QLatin1String("moving")))
            fStatus->setText(QStringLiteral("正在修正音高…"));
        fProgress->setValue(percent);
    });
    connect(&fScript, &ScriptJob::result, this, &TuneDialog::showResult);
    connect(&fScript, &ScriptJob::finished, this, &TuneDialog::finish);

    fView->setScale(scaleOf(chosenKey()));
    applyPreset();
    setListening(true);
    setBusy(false);
    if (fSources.empty())
        fStatus->setText(QStringLiteral("工程里还没有音频轨道。先导入一段人声（或用 AI 自动分轨把人声分出来）。"));
}

void TuneDialog::applyPreset()
{
    const Preset& preset = kPresets[std::clamp(fPreset->currentIndex(), 0, static_cast<int>(std::size(kPresets)) - 1)];
    fStrength->setValue(preset.strength);
    fStrength->setEnabled(!preset.hard && !fBusy);
    fStrengthLabel->setText(QStringLiteral("%1%").arg(preset.strength));
}

QString TuneDialog::keyName(const QString& key)
{
    const QString tonic = key.section(QLatin1Char(' '), 0, 0);
    return QStringLiteral("%1 %2").arg(tonic, key.endsWith(QLatin1String("minor")) ? QStringLiteral("小调") : QStringLiteral("大调"));
}

QString TuneDialog::chosenKey() const
{
    return fKey->currentData().toString();
}

QList<int> TuneDialog::scaleOf(const QString& key) const
{
    const QStringList parts = key.split(QLatin1Char(' '));
    if (parts.size() != 2)
        return {};
    int tonic = -1;
    for (int i = 0; i < 12; ++i)
        if (parts[0] == QLatin1String(kNames[i]))
            tonic = i;
    if (tonic < 0)
        return {};
    // the minor scale with its raised 7th, as tools/chost_tune.py tunes to
    const QList<int> steps = parts[1] == QLatin1String("minor") ? QList<int>{0, 2, 3, 5, 7, 8, 10, 11} : QList<int>{0, 2, 4, 5, 7, 9, 11};
    QList<int> scale;
    for (int s : steps)
        scale << (tonic + s) % 12;
    return scale;
}

void TuneDialog::setAutoStart(bool autoStart)
{
    if (autoStart)
        QTimer::singleShot(0, this, &TuneDialog::start);
}

void TuneDialog::setListening(bool tuned)
{
    fAB->setChecked(tuned);
    fAB->setText(tuned ? QStringLiteral("A/B：正在听修音后") : QStringLiteral("A/B：正在听原声"));
}

void TuneDialog::start()
{
    if (fBusy || fSources.empty())
        return;
    const Source& source = fSources[std::clamp(fTrack->currentIndex(), 0, static_cast<int>(fSources.size()) - 1)];
    if (!QFileInfo::exists(source.file))
    {
        fStatus->setText(QStringLiteral("找不到这条轨道的音频文件：%1").arg(QDir::toNativeSeparators(source.file)));
        return;
    }
    fOriginal = source.file;
    fOutput = tunedFileFor(source.file);
    QDir().mkpath(QFileInfo(fOutput).absolutePath());
    // written next to it first: the last good result stays until this one is complete
    fOutputPartial = fOutput + QStringLiteral(".part.wav");
    const Preset& preset = kPresets[std::clamp(fPreset->currentIndex(), 0, static_cast<int>(std::size(kPresets)) - 1)];
    QStringList args{QStringLiteral("--input"), fOriginal, QStringLiteral("--output"), fOutputPartial,
                     QStringLiteral("--strength"), QString::number(fStrength->value() / 100.0),
                     QStringLiteral("--drift"), QString::number(preset.drift),
                     QStringLiteral("--speed"), QString::number(preset.speed),
                     QStringLiteral("--scale"), fStrict->isChecked() ? QStringLiteral("strict") : QStringLiteral("smart")};
    if (!chosenKey().isEmpty())
        args << QStringLiteral("--key") << chosenKey();
    if (preset.hard)
        args << QStringLiteral("--hard");

    fNotes.clear();
    fSungCurve.clear();
    fFoundKey.clear();
    fNotSolo = false;
    fDone = false;
    fView->clear();
    fView->setScale(scaleOf(chosenKey()));
    fResult->clear();
    fProgress->setValue(0);
    fStatus->setText(QStringLiteral("正在准备（第一次会慢一点）…"));
    fBusy = true;
    setBusy(true);
    if (!fScript.start(QStringLiteral("chost_tune.py"), args))
        fBusy = false;
}

void TuneDialog::showResult(const QString& key, const QString& value)
{
    const QStringList v = value.split(QLatin1Char('\t'));
    auto curve = [](const QString& text) {
        std::vector<float> values;
        const QStringList items = text.split(QLatin1Char(','));
        values.reserve(items.size());
        for (const QString& item : items)
            values.push_back(item.toFloat());
        return values;
    };
    if (key == QLatin1String("NOTE") && v.size() >= 4)
        fNotes.push_back({v[0].toDouble(), v[1].toDouble(), v[2].toDouble(), v[3].toDouble()});
    else if (key == QLatin1String("CURVE"))
    {
        fSungCurve = curve(v.value(1));
        fView->setNotes(fNotes);
        fView->setCurves(v.value(0).toDouble(), fSungCurve, {});
    }
    else if (key == QLatin1String("TUNED"))
        fView->setCurves(v.value(0).toDouble(), fSungCurve, curve(v.value(1)));
    else if (key == QLatin1String("KEY"))
    {
        fFoundKey = value;
        fView->setScale(scaleOf(value));
    }
    else if (key == QLatin1String("WARN") && value == QLatin1String("notsolo"))
        fNotSolo = true;
    else if (key == QLatin1String("SUMMARY") && v.size() >= 4)
    {
        QString text;
        if (!fFoundKey.isEmpty())
            text = QStringLiteral("听出来是 %1。").arg(keyName(fFoundKey));
        text += QStringLiteral("找到 %1 个音，修正了 %2 个。平均跑调：%3 音分 → %4 音分（100 音分是一个半音，"
                                      "一般人能听出 15 音分以上的跑调）。")
                           .arg(v[0], v[1], v[2], v[3]);
        if (fNotSolo)
            text += QStringLiteral("\n⚠ 这条轨道听起来不像是单独的人声（有和弦、鼓或者是整首歌），修音效果可能不好。"
                                   "建议先用“AI 自动分轨”把人声分出来，再对人声轨修音。");
        fResult->setText(text);
        // the summary is a few lines more: the window grows rather than squeezing the pitch view
        if (height() < sizeHint().height())
            resize(width(), sizeHint().height());
    }
}

void TuneDialog::finish(bool ok, const QString& message)
{
    fBusy = false;
    if (ok)
    {
        // the track lets go of the previous tuned take, then the new one takes its place
        emit aboutToStart(fOriginal);
        QFile::remove(fOutput);
        if (!QFile::rename(fOutputPartial, fOutput))
        {
            ok = false;
            fStatus->setText(QStringLiteral("修音结果写不进去（文件可能被别的软件打开着）：%1").arg(QDir::toNativeSeparators(fOutput)));
        }
    }
    if (ok)
    {
        fDone = true;
        fProgress->setValue(100);
        fStatus->setText(QStringLiteral("✓ 修音完成，轨道已经换成修好的声音：%1").arg(QDir::toNativeSeparators(fOutput)));
        setListening(true);
        emit tuned(fOriginal, fOutput);
    }
    else if (!message.isEmpty())
    {
        QFile::remove(fOutputPartial);
        QString reason = message;
        if (message.contains(QLatin1String("no singing")))
            reason = QStringLiteral("这条轨道里没听到有音高的声音（是不是鼓或者噪音？）");
        else if (message.contains(QLatin1String("too short or silent")))
            reason = QStringLiteral("这条轨道太短或者没有声音");
        fStatus->setText(QStringLiteral("修音没有完成：%1").arg(reason));
    }
    setBusy(false);
}

void TuneDialog::setBusy(bool busy)
{
    setProperty("busy", busy);
    fStart->setEnabled(!busy && !fSources.empty());
    fStart->setText(fDone ? QStringLiteral("重新修音") : QStringLiteral("开始修音"));
    fCancel->setEnabled(busy);
    fAB->setEnabled(fDone && !busy);
    fPlay->setEnabled(fDone);
    fOpenFolder->setEnabled(fDone);
    for (QWidget* w : std::initializer_list<QWidget*>{fTrack, fKey, fStrict, fPreset})
        w->setEnabled(!busy);
    const bool hard = kPresets[std::clamp(fPreset->currentIndex(), 0, static_cast<int>(std::size(kPresets)) - 1)].hard;
    fStrength->setEnabled(!busy && !hard);
}

void TuneDialog::closeEvent(QCloseEvent* event)
{
    if (fBusy && QMessageBox::question(this, windowTitle(), QStringLiteral("还在修音，要取消吗？")) != QMessageBox::Yes)
    {
        event->ignore();
        return;
    }
    fScript.cancel();
    QDialog::closeEvent(event);
}
