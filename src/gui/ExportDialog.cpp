#include "ExportDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

static QString fileSafe(QString name)
{
    static const QString bad = QStringLiteral("\\/:*?\"<>|");
    for (QChar& c : name)
        if (bad.contains(c) || c < QLatin1Char(' '))
            c = QLatin1Char('_');
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("track") : name;
}

static QString duration(double seconds)
{
    const int s = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

ExportDialog::ExportDialog(const QString& defaultPath, const std::vector<Track>& tracks, SnapshotFunction snapshot, QWidget* parent)
    : QDialog(parent)
    , fTracks(tracks)
    , fSnapshot(std::move(snapshot))
{
    setWindowTitle(QStringLiteral("导出混音"));
    resize(560, 0);
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "把整个工程按现在听到的样子（音量、静音、独奏、所有插件效果）混成一个 WAV 文件。"
        "导出不用等歌曲播完，比实时播放快得多。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    auto* pathRow = new QHBoxLayout;
    fPath = new QLineEdit(QDir::toNativeSeparators(defaultPath));
    pathRow->addWidget(fPath, 1);
    auto* browse = new QPushButton(QStringLiteral("浏览…"));
    pathRow->addWidget(browse);
    form->addRow(QStringLiteral("保存到"), pathRow);
    fFormat = new QComboBox;
    fFormat->addItem(QStringLiteral("WAV 24 位（推荐，发给混音师或母带也用这个）"), 24);
    fFormat->addItem(QStringLiteral("WAV 16 位（CD 音质，文件小一点）"), 16);
    fFormat->addItem(QStringLiteral("WAV 32 位浮点（超过 0 dB 也不会削波）"), 32);
    form->addRow(QStringLiteral("格式"), fFormat);
    fStems = new QCheckBox(QStringLiteral("同时把每条轨道单独导出（分轨文件，放在旁边的“_分轨”文件夹里）"));
    fStems->setEnabled(!fTracks.empty());
    form->addRow(QString(), fStems);
    layout->addLayout(form);

    fStatus = new QLabel;
    fStatus->setObjectName(QStringLiteral("status")); // these and the busy property: what the remote control reports
    fStatus->setWordWrap(true);
    fStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(fStatus);
    fProgress = new QProgressBar;
    fProgress->setObjectName(QStringLiteral("progress"));
    fProgress->setRange(0, 100);
    layout->addWidget(fProgress);

    auto* buttons = new QHBoxLayout;
    fOpenFolder = new QPushButton(QStringLiteral("打开所在文件夹"));
    fOpenFolder->setEnabled(false);
    buttons->addWidget(fOpenFolder);
    buttons->addStretch();
    fCancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(fCancel);
    fStart = new QPushButton(QStringLiteral("导出"));
    fStart->setDefault(true);
    buttons->addWidget(fStart);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(browse, &QPushButton::clicked, this, &ExportDialog::browse);
    connect(fStart, &QPushButton::clicked, this, &ExportDialog::start);
    connect(fCancel, &QPushButton::clicked, this, [this] {
        fJobs.clear();
        fRender.cancel();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(fOpenFolder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QDir::fromNativeSeparators(fPath->text())).absolutePath()));
    });

    connect(&fRender, &RenderJob::progress, this, [this](int percent) {
        if (fJobs.empty())
            return;
        fStatus->setText(QStringLiteral("正在导出：%1（%2/%3）").arg(fJobs[fCurrent].name).arg(fCurrent + 1).arg(fJobs.size()));
        fProgress->setValue(static_cast<int>((fCurrent * 100 + static_cast<size_t>(percent)) / fJobs.size()));
    });
    connect(&fRender, &RenderJob::finished, this, [this](bool ok, const QString& output, const QString& message) {
        if (fJobs.empty() || fCurrent >= fJobs.size())
        {
            // cancelled
            fFailed = true;
            fReport << QStringLiteral("已取消。");
            fCurrent = 0;
            fJobs.clear();
            next();
            return;
        }
        const Job& job = fJobs[fCurrent];
        if (ok)
        {
            const double seconds = message.section(QLatin1Char('\t'), 0, 0).toDouble();
            const double peak = message.section(QLatin1Char('\t'), 1, 1).toDouble();
            QString line = QStringLiteral("✓ %1：%2，最大电平 %3 dB").arg(QFileInfo(output).fileName(), duration(seconds)).arg(peak, 0, 'f', 1);
            if (peak > -0.05 && bits() != 32)
                line += QStringLiteral("（有削波：把总音量调低一点再导出，或者用 AI 母带）");
            fReport << line;
        }
        else
        {
            fFailed = true;
            fReport << QStringLiteral("✗ %1：%2").arg(job.name, message);
        }
        ++fCurrent;
        next();
    });

    setBusy(false);
}

void ExportDialog::setAutoStart(bool autoStart)
{
    if (autoStart)
        QTimer::singleShot(0, this, &ExportDialog::start);
}

int ExportDialog::bits() const
{
    return fFormat->currentData().toInt();
}

void ExportDialog::browse()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出混音"), QDir::fromNativeSeparators(fPath->text()),
                                                QStringLiteral("WAV 音频 (*.wav)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".wav");
    fPath->setText(QDir::toNativeSeparators(path));
}

void ExportDialog::start()
{
    if (fRender.isRunning())
        return;
    QString path = QDir::fromNativeSeparators(fPath->text().trimmed());
    if (path.isEmpty())
    {
        fStatus->setText(QStringLiteral("先选好保存到哪里。"));
        return;
    }
    if (QFileInfo(path).suffix().compare(QLatin1String("wav"), Qt::CaseInsensitive) != 0)
        path += QStringLiteral(".wav");
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
    {
        fStatus->setText(QStringLiteral("没法在这里保存：%1").arg(info.absolutePath()));
        return;
    }

    const RenderSnapshot snapshot = fSnapshot();
    if (snapshot.session.isEmpty())
    {
        fStatus->setText(QStringLiteral("没能准备工程：%1").arg(snapshot.error));
        return;
    }
    fSession = snapshot.session;

    fJobs.clear();
    fJobs.push_back({info.absoluteFilePath(), QStringLiteral("混音"), {}});
    if (fStems->isChecked())
    {
        const QDir stems(info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral("_分轨"));
        QDir().mkpath(stems.absolutePath());
        int n = 1;
        for (const Track& t : fTracks)
            fJobs.push_back({stems.filePath(QStringLiteral("%1 %2.wav").arg(n++, 2, 10, QLatin1Char('0')).arg(fileSafe(t.name))), t.name, {t.index}});
    }
    fCurrent = 0;
    fReport.clear();
    fFailed = false;
    fProgress->setValue(0);
    fStatus->setText(QStringLiteral("正在加载插件…"));
    setBusy(true);
    next();
}

void ExportDialog::next()
{
    if (fCurrent >= fJobs.size())
    {
        setBusy(false);
        if (!fFailed)
            fProgress->setValue(100);
        fStatus->setText((fFailed ? QStringLiteral("导出没有全部完成：\n") : QStringLiteral("导出完成：\n")) + fReport.join(QLatin1Char('\n')));
        fOpenFolder->setEnabled(!fFailed || fCurrent > 1);
        const QString mix = fJobs.empty() ? QString() : fJobs.front().output;
        fJobs.clear();
        emit exported(!fFailed, mix);
        return;
    }
    const Job& job = fJobs[fCurrent];
    fRender.start(fSession, job.output, bits(), job.solo);
}

void ExportDialog::setBusy(bool busy)
{
    setProperty("busy", busy);
    fStart->setEnabled(!busy);
    fCancel->setEnabled(busy);
    fPath->setEnabled(!busy);
    fFormat->setEnabled(!busy);
    fStems->setEnabled(!busy && !fTracks.empty());
}

void ExportDialog::closeEvent(QCloseEvent* event)
{
    if (fRender.isRunning()
        && QMessageBox::question(this, windowTitle(), QStringLiteral("还在导出，要取消吗？")) != QMessageBox::Yes)
    {
        event->ignore();
        return;
    }
    fJobs.clear();
    fRender.cancel();
    QDialog::closeEvent(event);
}
