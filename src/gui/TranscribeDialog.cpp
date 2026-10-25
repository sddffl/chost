#include "TranscribeDialog.h"

#include "Resources.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

static QString roleText(const QString& role)
{
    if (role.isEmpty() || role == QLatin1String("mix"))
        return QStringLiteral("整首（多音）");
    static const QHash<QString, QString> texts{
        {QStringLiteral("vocals"), QStringLiteral("人声旋律")}, {QStringLiteral("lead"), QStringLiteral("主唱旋律")},
        {QStringLiteral("backing"), QStringLiteral("和声")},     {QStringLiteral("bass"), QStringLiteral("贝斯")},
        {QStringLiteral("guitar"), QStringLiteral("吉他")},      {QStringLiteral("piano"), QStringLiteral("钢琴")},
        {QStringLiteral("other"), QStringLiteral("其他乐器")},   {QStringLiteral("drums"), QStringLiteral("整套鼓")},
        {QStringLiteral("kick"), QStringLiteral("底鼓")},        {QStringLiteral("snare"), QStringLiteral("军鼓")},
        {QStringLiteral("toms"), QStringLiteral("通鼓")},        {QStringLiteral("hh"), QStringLiteral("踩镲")},
        {QStringLiteral("ride"), QStringLiteral("叮叮镲")},      {QStringLiteral("crash"), QStringLiteral("吊镲")},
    };
    return texts.value(role, role);
}

TranscribeDialog::TranscribeDialog(const std::vector<Source>& sources, double tempo, QWidget* parent)
    : QDialog(parent)
    , fSources(sources)
    , fTempo(tempo)
{
    setWindowTitle(QStringLiteral("AI 转 MIDI"));
    resize(600, 560);
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "把勾选的音轨转成 MIDI，每条生成一条乐器轨，放在原音轨下面，先用自带的 GM 音色播放，之后可以换成你自己的乐器。"
        "分好轨再转最准：人声、贝斯按单音旋律转，钢琴、吉他按和弦转，鼓的每个部件转成对应的鼓音符。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    fList = new QListWidget;
    for (const Source& s : fSources)
    {
        auto* item = new QListWidgetItem(QStringLiteral("%1  ·  %2").arg(s.name, roleText(s.role)));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(s.checked ? Qt::Checked : Qt::Unchecked);
        fList->addItem(item);
    }
    layout->addWidget(fList, 1);

    fStatus = new QLabel;
    fStatus->setWordWrap(true);
    layout->addWidget(fStatus);
    fProgress = new QProgressBar;
    fProgress->setRange(0, 100);
    layout->addWidget(fProgress);
    fLog = new QPlainTextEdit;
    fLog->setReadOnly(true);
    fLog->setMaximumBlockCount(2000);
    fLog->setStyleSheet(QStringLiteral("font-family: Consolas, 'DejaVu Sans Mono', monospace; font-size: 11px;"));
    layout->addWidget(fLog, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    fCancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(fCancel);
    fStart = new QPushButton(QStringLiteral("开始转换"));
    fStart->setDefault(true);
    buttons->addWidget(fStart);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(fStart, &QPushButton::clicked, this, &TranscribeDialog::start);
    connect(fCancel, &QPushButton::clicked, this, &TranscribeDialog::cancel);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(&fProcess, &QProcess::readyReadStandardOutput, this, &TranscribeDialog::readOutput);
    connect(&fProcess, &QProcess::readyReadStandardError, this, [this] {
        for (const QByteArray& line : fProcess.readAllStandardError().split('\n'))
            if (!line.trimmed().isEmpty())
                log(QString::fromUtf8(line).trimmed());
    });
    connect(&fProcess, &QProcess::finished, this, &TranscribeDialog::onFinished);
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            log(QStringLiteral("无法运行：%1").arg(fProcess.program()));
            onFinished(-1, QProcess::CrashExit);
        }
    });

    if (resources::aiPython().isEmpty())
        fStatus->setText(QStringLiteral("还没有安装 AI 组件：请先打开“AI 分轨”窗口，点“安装 AI 组件”。"));
    else if (resources::toolScript(QStringLiteral("chost_transcribe.py")).isEmpty())
        fStatus->setText(QStringLiteral("找不到 tools/chost_transcribe.py，请重新解压 chost。"));
    setBusy(false);
}

TranscribeDialog::~TranscribeDialog()
{
    fProcess.disconnect(this);
    if (fProcess.state() != QProcess::NotRunning)
    {
        fProcess.kill();
        fProcess.waitForFinished(3000);
    }
}

void TranscribeDialog::setAutoStart(bool autoStart)
{
    if (autoStart)
        QTimer::singleShot(0, this, &TranscribeDialog::start);
}

QString TranscribeDialog::midiPathFor(const Source& source) const
{
    const QFileInfo info(source.file);
    const QString name = info.completeBaseName() + QStringLiteral(".mid");
    if (QFileInfo(info.absolutePath()).isWritable())
        return info.absoluteDir().filePath(name);
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/midi");
    QDir().mkpath(fallback);
    return QDir(fallback).filePath(name);
}

void TranscribeDialog::start()
{
    if (fProcess.state() != QProcess::NotRunning || resources::aiPython().isEmpty())
        return;
    fQueue.clear();
    for (int i = 0; i < fList->count(); ++i)
        if (fList->item(i)->checkState() == Qt::Checked)
            fQueue.push_back(i);
    if (fQueue.empty())
    {
        fStatus->setText(QStringLiteral("先勾选要转换的音轨。"));
        return;
    }
    fTotal = static_cast<int>(fQueue.size());
    fDone = 0;
    fFailed = false;
    fProgress->setValue(0);
    setBusy(true);
    next();
}

void TranscribeDialog::next()
{
    if (fQueue.empty())
    {
        fCurrent = -1;
        setBusy(false);
        fProgress->setValue(100);
        fStatus->setText(fFailed ? QStringLiteral("有音轨没转成功，详细信息见下面的记录。")
                                 : QStringLiteral("✓ 转换完成，加了 %1 条 MIDI 轨。").arg(fDone));
        emit transcriptionFinished(!fFailed);
        return;
    }
    fCurrent = fQueue.front();
    fQueue.erase(fQueue.begin());
    const Source& s = fSources[static_cast<size_t>(fCurrent)];
    fCurrentMidi = midiPathFor(s);
    fStatus->setText(QStringLiteral("正在转换：%1（%2/%3）").arg(s.name).arg(fTotal - static_cast<int>(fQueue.size())).arg(fTotal));

    QStringList args{resources::toolScript(QStringLiteral("chost_transcribe.py")),
                     QStringLiteral("--input"), s.file,
                     QStringLiteral("--output"), fCurrentMidi,
                     QStringLiteral("--role"), s.role.isEmpty() ? QStringLiteral("mix") : s.role,
                     QStringLiteral("--tempo"), QString::number(fTempo),
                     QStringLiteral("--name"), s.name};
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    fProcess.setProcessEnvironment(env);
    fPending.clear();
    log(QStringLiteral("— %1").arg(s.name));
    fProcess.start(resources::aiPython(), args);
}

void TranscribeDialog::cancel()
{
    fQueue.clear();
    if (fProcess.state() != QProcess::NotRunning)
    {
        fFailed = true;
        fProcess.kill();
        log(QStringLiteral("已取消。"));
    }
}

void TranscribeDialog::readOutput()
{
    fPending += fProcess.readAllStandardOutput();
    int newline;
    while ((newline = fPending.indexOf('\n')) >= 0)
    {
        const QString line = QString::fromUtf8(fPending.left(newline)).trimmed();
        fPending.remove(0, newline + 1);
        if (line.startsWith(QLatin1String("PROGRESS ")))
        {
            // overall progress: finished tracks plus this one's share
            const int pct = line.mid(9).section(QLatin1Char(' '), 0, 0).toInt();
            const int finished = fTotal - static_cast<int>(fQueue.size()) - 1;
            fProgress->setValue((finished * 100 + pct) / std::max(1, fTotal));
        }
        else if (line.startsWith(QLatin1String("MIDI ")) && fCurrent >= 0)
        {
            const QStringList parts = line.mid(5).split(QLatin1Char('\t'));
            const Source& s = fSources[static_cast<size_t>(fCurrent)];
            log(QStringLiteral("✓ %1：%2 个音符").arg(s.name, parts.value(1)));
            ++fDone;
            emit midiReady(s.file, s.role, s.name, parts.value(0));
        }
        else if (line.startsWith(QLatin1String("ERROR ")))
        {
            log(QStringLiteral("错误：%1").arg(line.mid(6)));
        }
        else if (!line.isEmpty() && line != QLatin1String("DONE"))
        {
            log(line);
        }
    }
}

void TranscribeDialog::onFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput();
    if (status != QProcess::NormalExit || exitCode != 0)
        fFailed = true;
    if (fCurrent >= 0)
        next();
}

void TranscribeDialog::setBusy(bool busy)
{
    const bool usable = !resources::aiPython().isEmpty() && !resources::toolScript(QStringLiteral("chost_transcribe.py")).isEmpty();
    fStart->setEnabled(!busy && usable);
    fCancel->setEnabled(busy);
    fList->setEnabled(!busy);
}

void TranscribeDialog::log(const QString& line)
{
    fLog->appendPlainText(line);
}

void TranscribeDialog::closeEvent(QCloseEvent* event)
{
    if (fProcess.state() != QProcess::NotRunning)
    {
        // keep converting in the background, the MIDI tracks still arrive
        hide();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}
