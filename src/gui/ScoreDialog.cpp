#include "ScoreDialog.h"

#include "ScoreView.h"
#include "core/Scales.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

ScoreDialog::ScoreDialog(const QString& title, const QString& key, double tempo, const QString& folder, QWidget* parent)
    : QDialog(parent)
    , fSongKey(key)
    , fTempo(tempo)
    , fFolder(folder)
{
    setWindowTitle(QStringLiteral("出谱（五线谱）"));
    resize(1180, 820);
    auto* layout = new QHBoxLayout(this);

    auto* side = new QVBoxLayout;
    auto* form = new QFormLayout;
    fList = new QListWidget;
    fList->setMaximumHeight(150);
    fList->setToolTip(QStringLiteral("勾上的 MIDI 轨道写进同一份谱子，一轨一行（钢琴这类音域宽的会用大谱表）；鼓轨写成打击乐谱"));
    form->addRow(QStringLiteral("轨道"), fList);
    fTitle = new QLineEdit(title);
    form->addRow(QStringLiteral("标题"), fTitle);
    fKey = new QComboBox;
    form->addRow(QStringLiteral("调号"), fKey);
    fStaff = new QComboBox;
    fStaff->addItem(QStringLiteral("自动"), static_cast<int>(chost::StaffMode::Auto));
    fStaff->addItem(QStringLiteral("高音谱号"), static_cast<int>(chost::StaffMode::Treble));
    fStaff->addItem(QStringLiteral("低音谱号"), static_cast<int>(chost::StaffMode::Bass));
    fStaff->addItem(QStringLiteral("大谱表（钢琴）"), static_cast<int>(chost::StaffMode::Grand));
    fStaff->setToolTip(QStringLiteral("自动：音域跨两个谱号的用大谱表，其余按音高选高音或低音谱号"));
    form->addRow(QStringLiteral("谱表"), fStaff);
    fGrid = new QComboBox;
    fGrid->addItem(QStringLiteral("十六分音符"), 1);
    fGrid->addItem(QStringLiteral("八分音符（更好读）"), 2);
    fGrid->setToolTip(QStringLiteral("音符对齐的最小单位。转 MIDI 得来的音有快有慢，选八分音符谱子会干净很多"));
    form->addRow(QStringLiteral("最短音符"), fGrid);
    fMeter = new QComboBox;
    for (const char* meter : {"4/4", "3/4", "2/4", "6/8", "12/8", "9/8", "3/8"})
        fMeter->addItem(QString::fromLatin1(meter), QString::fromLatin1(meter));
    fMeter->setToolTip(QStringLiteral("一小节几拍：流行歌多是 4/4，华尔兹 3/4，进行曲 2/4，摇摆的 6/8、12/8"));
    form->addRow(QStringLiteral("拍号"), fMeter);
    fTriplets = new QCheckBox(QStringLiteral("识别三连音"));
    fTriplets->setChecked(true);
    fTriplets->setToolTip(QStringLiteral("一拍里弹了三个均匀的音，就写成三连音；关掉就都按最短音符对齐"));
    form->addRow(QString(), fTriplets);
    fZoom = new QComboBox;
    for (int percent : {50, 75, 100, 125, 150})
        fZoom->addItem(QStringLiteral("%1%").arg(percent), percent / 100.0);
    fZoom->setCurrentIndex(2);
    form->addRow(QStringLiteral("缩放"), fZoom);
    side->addLayout(form);

    fPdf = new QPushButton(QStringLiteral("导出 PDF…"));
    fPdf->setToolTip(QStringLiteral("A4 的 PDF，可以直接打印或发给别人"));
    fXml = new QPushButton(QStringLiteral("导出 MusicXML…"));
    fXml->setToolTip(QStringLiteral("MuseScore、Sibelius、Finale、Dorico 都能打开，在那里继续加歌词、表情记号、改排版"));
    side->addWidget(fPdf);
    side->addWidget(fXml);
    fStatus = new QLabel;
    fStatus->setWordWrap(true);
    fStatus->setTextFormat(Qt::RichText);
    fStatus->setOpenExternalLinks(true);
    fStatus->setStyleSheet(QStringLiteral("color: #8b9096;"));
    side->addWidget(fStatus);
    auto* note = new QLabel(QStringLiteral("每行谱一个声部，重叠的音会在下一个音开始的地方截断。要更精细的排版，导出 MusicXML 到 MuseScore 里改。"));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #8b9096;"));
    side->addStretch(1);
    side->addWidget(note);
    auto* sideWidget = new QWidget;
    sideWidget->setLayout(side);
    sideWidget->setFixedWidth(300);
    layout->addWidget(sideWidget);

    auto* scroll = new QScrollArea;
    scroll->setAlignment(Qt::AlignHCenter);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: #3a3e44; border: none; }"));
    fView = new ScoreView;
    scroll->setWidget(fView);
    layout->addWidget(scroll, 1);

    setSong(key, tempo);

    fRebuild.setSingleShot(true);
    fRebuild.setInterval(150);
    connect(&fRebuild, &QTimer::timeout, this, &ScoreDialog::rebuild);
    auto later = [this] { fRebuild.start(); };
    connect(fList, &QListWidget::itemChanged, this, later);
    connect(fTitle, &QLineEdit::textChanged, this, later);
    connect(fKey, &QComboBox::currentIndexChanged, this, later);
    connect(fStaff, &QComboBox::currentIndexChanged, this, later);
    connect(fGrid, &QComboBox::currentIndexChanged, this, later);
    connect(fMeter, &QComboBox::currentIndexChanged, this, later);
    connect(fTriplets, &QCheckBox::toggled, this, later);
    connect(fZoom, &QComboBox::currentIndexChanged, this, [this] { fView->setZoom(fZoom->currentData().toDouble()); });
    connect(fPdf, &QPushButton::clicked, this, [this] { exportAs(true); });
    connect(fXml, &QPushButton::clicked, this, [this] { exportAs(false); });
}

void ScoreDialog::setSong(const QString& key, double tempo)
{
    fSongKey = key;
    fTempo = tempo;
    const QString current = fKey->currentData().toString();
    const QSignalBlocker block(fKey);
    fKey->clear();
    const chost::Key song = chost::parseKey(key.toStdString());
    fKey->addItem(song.valid ? QStringLiteral("歌曲的调（%1）").arg(QString::fromStdString(chost::keyName(song))) : QStringLiteral("按音符猜"),
                  QString());
    for (int minor = 0; minor < 2; ++minor)
        for (int tonic = 0; tonic < 12; ++tonic)
        {
            const chost::Key k{tonic, minor == 1, true};
            const QString name = QString::fromStdString(chost::keyName(k));
            fKey->addItem(name.section(QLatin1Char(' '), 0, 0) + (minor ? QStringLiteral(" 小调") : QStringLiteral(" 大调")), name);
        }
    fKey->setCurrentIndex(std::max(0, fKey->findData(current)));
    fRebuild.start();
}

void ScoreDialog::setTracks(const std::vector<Track>& tracks, int selected)
{
    // keep what was ticked, by file
    QStringList ticked;
    for (int i = 0; i < fList->count(); ++i)
        if (fList->item(i)->checkState() == Qt::Checked)
            ticked << fList->item(i)->data(Qt::UserRole + 1).toString();
    const bool first = fTracks.empty() && fList->count() == 0;
    fTracks = tracks;

    const QSignalBlocker block(fList);
    fList->clear();
    const bool selectedIsMidi = std::any_of(tracks.begin(), tracks.end(), [&](const Track& t) { return t.index == selected; });
    for (const Track& t : tracks)
    {
        auto* item = new QListWidgetItem(t.name, fList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setData(Qt::UserRole, t.index);
        item->setData(Qt::UserRole + 1, t.file);
        const bool on = first ? (!selectedIsMidi || t.index == selected) : ticked.contains(t.file);
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    fRebuild.start();
}

void ScoreDialog::setChosen(const std::vector<int>& tracks)
{
    const QSignalBlocker block(fList);
    for (int i = 0; i < fList->count(); ++i)
    {
        const int index = fList->item(i)->data(Qt::UserRole).toInt();
        fList->item(i)->setCheckState(std::find(tracks.begin(), tracks.end(), index) != tracks.end() ? Qt::Checked : Qt::Unchecked);
    }
    rebuild();
}

std::vector<ScoreDialog::Track> ScoreDialog::chosen() const
{
    std::vector<Track> out;
    for (int i = 0; i < fList->count(); ++i)
        if (fList->item(i)->checkState() == Qt::Checked)
            for (const Track& t : fTracks)
                if (t.index == fList->item(i)->data(Qt::UserRole).toInt())
                    out.push_back(t);
    return out;
}

bool ScoreDialog::setMeter(const QString& meter)
{
    const int index = fMeter->findData(meter);
    if (index < 0)
        return false;
    fMeter->setCurrentIndex(index);
    rebuild();
    return true;
}

chost::Score ScoreDialog::build(const std::vector<Track>& tracks, const chost::ScoreOptions& options, chost::StaffMode staff, std::string& error)
{
    std::vector<chost::ScoreInput> inputs;
    for (const Track& t : tracks)
    {
        chost::ScoreInput input;
        input.name = t.name.toStdString();
        input.staff = staff;
        if (!chost::readMidiFile(t.file.toStdString(), input.clip, error))
        {
            error = t.name.toStdString() + ": " + error;
            return {};
        }
        inputs.push_back(input);
    }
    if (inputs.empty())
    {
        error = "no tracks chosen";
        return {};
    }
    return chost::makeScore(inputs, options, error);
}

bool ScoreDialog::write(const chost::Score& score, const QString& path, QString& error, int* pages)
{
    if (score.parts.empty())
    {
        error = QStringLiteral("the score is empty");
        return false;
    }
    if (path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
    {
        const ScoreEngraving engraving(score);
        if (pages != nullptr)
            *pages = engraving.pages();
        return engraving.writePdf(path, error);
    }
    QFile file(path);
    const std::string xml = chost::musicXml(score);
    if (!file.open(QIODevice::WriteOnly) || file.write(xml.data(), static_cast<qint64>(xml.size())) != static_cast<qint64>(xml.size()))
    {
        error = QStringLiteral("can't write %1").arg(path);
        return false;
    }
    if (pages != nullptr)
        *pages = 0;
    return true;
}

void ScoreDialog::rebuild()
{
    chost::ScoreOptions options;
    options.title = fTitle->text().trimmed().toStdString();
    options.tempo = fTempo;
    options.grid = fGrid->currentData().toInt();
    options.meter = chost::parseMeter(fMeter->currentData().toString().toStdString());
    options.triplets = fTriplets->isChecked();
    const QString keyText = fKey->currentData().toString();
    options.key = chost::parseKey((keyText.isEmpty() ? fSongKey : keyText).toStdString());
    std::string error;
    const std::vector<Track> tracks = chosen();
    fScore = tracks.empty() ? chost::Score{} : build(tracks, options, static_cast<chost::StaffMode>(fStaff->currentData().toInt()), error);
    const bool ok = !fScore.parts.empty();
    fPdf->setEnabled(ok);
    fXml->setEnabled(ok);
    if (!ok)
    {
        fView->clear(tracks.empty() ? QStringLiteral("勾选左边要出谱的 MIDI 轨道") : QStringLiteral("出不了谱：%1").arg(QString::fromStdString(error)));
        fStatus->clear();
        return;
    }
    fView->setScore(fScore);
    fView->setZoom(fZoom->currentData().toDouble());
    fStatus->setText(QStringLiteral("%1 · %2 小节 · %3 页").arg(QString::fromStdString(chost::keyName(fScore.key))).arg(fScore.measures)
                         .arg(fView->engraving()->pages()));
}

int ScoreDialog::pages() const
{
    return fView->engraving() ? fView->engraving()->pages() : 0;
}

bool ScoreDialog::exportTo(const QString& path, QString& error)
{
    if (fRebuild.isActive())
    {
        fRebuild.stop();
        rebuild();
    }
    return write(fScore, path, error);
}

void ScoreDialog::exportAs(bool pdf)
{
    QSettings settings;
    QString folder = settings.value(QStringLiteral("score/folder")).toString();
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        folder = !fFolder.isEmpty() ? fFolder : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString name = fTitle->text().trimmed();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (name.isEmpty())
        name = QStringLiteral("乐谱");
    const QString path = QFileDialog::getSaveFileName(this, pdf ? QStringLiteral("导出 PDF") : QStringLiteral("导出 MusicXML"),
                                                      QDir(folder).filePath(name + (pdf ? QStringLiteral(".pdf") : QStringLiteral(".musicxml"))),
                                                      pdf ? QStringLiteral("PDF (*.pdf)") : QStringLiteral("MusicXML (*.musicxml *.xml)"));
    if (path.isEmpty())
        return;
    settings.setValue(QStringLiteral("score/folder"), QFileInfo(path).absolutePath());
    QString error;
    if (!exportTo(path, error))
    {
        fStatus->setText(QStringLiteral("<span style='color:#e06c6c'>导出失败：%1</span>").arg(error.toHtmlEscaped()));
        return;
    }
    fStatus->setText(QStringLiteral("已导出 <a href='%1'>%2</a>（<a href='%3'>打开文件夹</a>）")
                         .arg(QUrl::fromLocalFile(path).toString(), QFileInfo(path).fileName().toHtmlEscaped(),
                              QUrl::fromLocalFile(QFileInfo(path).absolutePath()).toString()));
}
