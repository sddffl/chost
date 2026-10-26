#include "ArrangeDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

struct Style {
    const char* id;
    const char* text;
    std::vector<const char*> parts;
};

const std::vector<Style>& styles()
{
    static const std::vector<Style> list{
        {"pop", "流行（鼓、贝斯、钢琴、木吉他扫弦、弦乐）", {"drums", "bass", "keys", "guitar", "strings"}},
        {"rock", "摇滚（鼓、贝斯、失真吉他，副歌加弦乐）", {"drums", "bass", "guitar", "strings"}},
        {"ballad", "抒情（轻鼓、贝斯、钢琴分解和弦、弦乐）", {"drums", "bass", "keys", "strings"}},
        {"folk", "民谣（沙锤和轻鼓、贝斯、木吉他指弹）", {"drums", "bass", "guitar"}},
        {"electronic", "电子（四拍底鼓、合成贝斯、琶音、铺底）", {"drums", "bass", "keys", "strings"}},
        {"chinese", "中国风（古筝分解和弦、弦乐、轻鼓、贝斯）", {"drums", "bass", "keys", "strings"}},
    };
    return list;
}

const std::vector<std::pair<QString, QString>>& partNames()
{
    static const std::vector<std::pair<QString, QString>> list{
        {QStringLiteral("drums"), QStringLiteral("鼓")},     {QStringLiteral("bass"), QStringLiteral("贝斯")},
        {QStringLiteral("keys"), QStringLiteral("键盘")},    {QStringLiteral("guitar"), QStringLiteral("吉他")},
        {QStringLiteral("strings"), QStringLiteral("弦乐/铺底")},
    };
    return list;
}

} // namespace

ArrangeDialog::ArrangeDialog(const std::vector<Source>& sources, int selected, double tempo, const QString& outputFolder,
                             QWidget* parent)
    : QDialog(parent)
    , fSources(sources)
    , fTempo(tempo)
    , fOutputFolder(outputFolder)
{
    setWindowTitle(QStringLiteral("AI 自动编曲"));
    resize(620, 0);
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "按选好的风格自动写出鼓、贝斯、键盘、吉他和弦乐，每样一条 MIDI 乐器轨，用你在“乐器设置”里选的音源播放。"
        "前奏和尾奏轻、主歌中等、副歌最满，换段落的地方有加花和镲。生成的音符都可以在钢琴卷帘里改。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    fFromTrack = new QRadioButton(QStringLiteral("跟着这条轨道（自动听出和弦和段落）"));
    fTrack = new QComboBox;
    for (const Source& s : fSources)
        fTrack->addItem(QStringLiteral("%1（%2）").arg(s.name, s.midi ? QStringLiteral("MIDI") : QStringLiteral("音频")));
    if (selected >= 0 && selected < fTrack->count())
        fTrack->setCurrentIndex(selected);
    fFromChords = new QRadioButton(QStringLiteral("用我写的和弦"));
    fChords = new QLineEdit;
    fChords->setPlaceholderText(QStringLiteral("例如：C G | Am F    （竖线分小节，一小节可以写两个和弦）"));
    auto* sourceGroup = new QButtonGroup(this);
    sourceGroup->addButton(fFromTrack);
    sourceGroup->addButton(fFromChords);
    (fSources.empty() ? fFromChords : fFromTrack)->setChecked(true);
    fFromTrack->setEnabled(!fSources.empty());
    form->addRow(fFromTrack);
    form->addRow(QString(), fTrack);
    form->addRow(fFromChords);
    form->addRow(QString(), fChords);

    fStyle = new QComboBox;
    for (const Style& s : styles())
        fStyle->addItem(QString::fromUtf8(s.text), QString::fromLatin1(s.id));
    form->addRow(QStringLiteral("风格"), fStyle);

    fForm = new QComboBox;
    fForm->addItem(QStringLiteral("完整歌曲：前奏、主歌、副歌、主歌、副歌、尾奏"), QStringLiteral("song"));
    fForm->addItem(QStringLiteral("只循环和弦，力度不变（先试听用）"), QStringLiteral("loop"));
    form->addRow(QStringLiteral("段落"), fForm);

    auto* partsRow = new QHBoxLayout;
    for (const auto& [id, name] : partNames())
    {
        auto* box = new QCheckBox(name);
        fPartBoxes[id] = box;
        partsRow->addWidget(box);
    }
    partsRow->addStretch();
    form->addRow(QStringLiteral("乐器"), partsRow);
    form->addRow(QStringLiteral("速度"), new QLabel(QStringLiteral("%1 BPM（在顶部走带栏里改）").arg(fTempo, 0, 'f', 1)));
    fReplace = new QCheckBox(QStringLiteral("替换上一次生成的编曲轨道"));
    fReplace->setChecked(true);
    form->addRow(QString(), fReplace);
    layout->addLayout(form);

    fStatus = new QLabel;
    fStatus->setObjectName(QStringLiteral("status")); // these and the busy property: what the remote control reports
    fStatus->setWordWrap(true);
    layout->addWidget(fStatus);
    fProgress = new QProgressBar;
    fProgress->setObjectName(QStringLiteral("progress"));
    fProgress->setRange(0, 100);
    layout->addWidget(fProgress);
    fChordView = new QLabel;
    fChordView->setWordWrap(true);
    fChordView->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fChordView->setStyleSheet(QStringLiteral("font-family: Consolas, 'DejaVu Sans Mono', monospace;"));
    layout->addWidget(fChordView);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    fCancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(fCancel);
    fStart = new QPushButton(QStringLiteral("生成编曲"));
    fStart->setDefault(true);
    buttons->addWidget(fStart);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(fStyle, &QComboBox::currentIndexChanged, this, &ArrangeDialog::styleChanged);
    connect(fFromTrack, &QRadioButton::toggled, this, &ArrangeDialog::sourceChanged);
    connect(fChords, &QLineEdit::textEdited, this, [this] { fFromChords->setChecked(true); });
    connect(fStart, &QPushButton::clicked, this, &ArrangeDialog::start);
    connect(fCancel, &QPushButton::clicked, &fJob, &ScriptJob::cancel);
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    connect(&fJob, &ScriptJob::progress, this, [this](int percent, const QString&) { fProgress->setValue(percent); });
    connect(&fJob, &ScriptJob::result, this, [this](const QString& key, const QString& value) {
        if (key == QLatin1String("CHORDS"))
        {
            // four bars a line, a line repeating right after itself said once
            const QStringList bars = value.split(QStringLiteral(" | "));
            QStringList lines;
            for (int i = 0; i < bars.size();)
            {
                const QString line = bars.mid(i, 4).join(QStringLiteral(" | "));
                int repeats = 1;
                while (i + 4 * repeats < bars.size() && bars.mid(i + 4 * repeats, 4).join(QStringLiteral(" | ")) == line)
                    ++repeats;
                lines << QStringLiteral("%1  %2%3").arg(i + 1, 3).arg(line, repeats > 1 ? QStringLiteral("    × %1").arg(repeats) : QString());
                i += 4 * repeats;
            }
            fChordView->setText(QStringLiteral("和弦（每行 4 小节，第一列是小节号）：\n") + lines.join(QLatin1Char('\n')));
        }
        else if (key == QLatin1String("MIDI"))
        {
            const QStringList v = value.split(QLatin1Char('\t'));
            fParts.push_back({v.value(0), v.value(1), v.value(2)});
        }
    });
    connect(&fJob, &ScriptJob::finished, this, [this](bool ok, const QString& message) {
        fBusy = false;
        setBusy(false);
        if (!ok)
        {
            QString reason = message;
            if (message.startsWith(QLatin1String("unknown chord")))
                reason = QStringLiteral("看不懂这个和弦：%1（可以写 C、Am、F#m7、Bb、Gsus4、C/E 这样的）").arg(message.section(QLatin1Char(':'), 1).trimmed());
            fStatus->setText(QStringLiteral("编曲没有完成：%1").arg(reason));
            return;
        }
        fProgress->setValue(100);
        fStatus->setText(QStringLiteral("✓ 编曲完成，加了 %1 条乐器轨。").arg(fParts.size()));
        emit arranged(fParts, fReplace->isChecked());
    });

    styleChanged();
    sourceChanged();
    setBusy(false);
}

void ArrangeDialog::setChords(const QString& chords)
{
    fChords->setText(chords);
    fFromChords->setChecked(true);
}

void ArrangeDialog::setAutoStart(bool autoStart)
{
    if (autoStart)
        QTimer::singleShot(0, this, &ArrangeDialog::start);
}

void ArrangeDialog::styleChanged()
{
    const Style& style = styles()[static_cast<size_t>(std::max(0, fStyle->currentIndex()))];
    for (auto& [id, box] : fPartBoxes)
    {
        bool on = false;
        for (const char* p : style.parts)
            on = on || id == QLatin1String(p);
        box->setChecked(on);
    }
}

void ArrangeDialog::sourceChanged()
{
    // a song brings its own sections
    fForm->setItemText(0, fFromTrack->isChecked() ? QStringLiteral("跟着原曲的段落（安静的地方轻，热闹的地方满）")
                                                  : QStringLiteral("完整歌曲：前奏、主歌、副歌、主歌、副歌、尾奏"));
}

void ArrangeDialog::start()
{
    if (fBusy)
        return;
    QStringList parts;
    for (const auto& [id, name] : partNames())
        if (fPartBoxes[id]->isChecked())
            parts << id;
    if (parts.isEmpty())
    {
        fStatus->setText(QStringLiteral("至少勾选一样乐器。"));
        return;
    }
    // every run in a folder of its own: the tracks of the last one may still have their files open
    QDir base(fOutputFolder);
    int run = 1;
    while (base.exists(QString::number(run)))
        ++run;
    const QString folder = base.filePath(QString::number(run));
    if (!QDir().mkpath(folder))
    {
        fStatus->setText(QStringLiteral("没法写入 %1").arg(QDir::toNativeSeparators(folder)));
        return;
    }

    QStringList args{QStringLiteral("--output-dir"), folder,
                     QStringLiteral("--tempo"), QString::number(fTempo),
                     QStringLiteral("--style"), fStyle->currentData().toString(),
                     QStringLiteral("--parts"), parts.join(QLatin1Char(',')),
                     QStringLiteral("--name"), QStringLiteral("arrangement"),
                     QStringLiteral("--seed"), QString::number(run)};
    const bool loop = fForm->currentData().toString() == QLatin1String("loop");
    if (fFromTrack->isChecked() && fTrack->currentIndex() >= 0)
    {
        const Source& s = fSources[static_cast<size_t>(fTrack->currentIndex())];
        args << (s.midi ? QStringLiteral("--midi") : QStringLiteral("--audio")) << s.file
             << QStringLiteral("--structure") << (loop ? QStringLiteral("loop") : QStringLiteral("follow"));
    }
    else
    {
        if (fChords->text().trimmed().isEmpty())
        {
            fStatus->setText(QStringLiteral("先写上和弦，例如：C G | Am F"));
            return;
        }
        args << QStringLiteral("--chords") << fChords->text().trimmed()
             << QStringLiteral("--structure") << (loop ? QStringLiteral("loop") : QStringLiteral("song"));
    }
    fParts.clear();
    fChordView->clear();
    fProgress->setValue(0);
    fStatus->setText(fFromTrack->isChecked() ? QStringLiteral("正在听和弦、编曲…") : QStringLiteral("正在编曲…"));
    fBusy = true;
    setBusy(true);
    if (!fJob.start(QStringLiteral("chost_arrange.py"), args))
        return; // finished() said why
}

void ArrangeDialog::setBusy(bool busy)
{
    setProperty("busy", busy);
    fStart->setEnabled(!busy);
    fCancel->setEnabled(busy);
}

void ArrangeDialog::closeEvent(QCloseEvent* event)
{
    fJob.cancel();
    QDialog::closeEvent(event);
}
