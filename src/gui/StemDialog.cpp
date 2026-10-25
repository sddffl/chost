#include "StemDialog.h"

#include "Resources.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QVBoxLayout>

StemDialog::StemDialog(const QString& trackName, const QString& audioFile, QWidget* parent)
    : QDialog(parent)
    , fAudioFile(audioFile)
{
    setWindowTitle(QStringLiteral("AI 自动分轨 · %1").arg(trackName));
    resize(640, 520);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "把 <b>%1</b> 分离成单独的乐器，每个乐器会变成一条新音轨，原来的音轨自动静音。<br>"
        "第一次使用要先安装 AI 组件，并下载模型（约 1 GB），之后就不用再下载了。")
                                 .arg(QFileInfo(audioFile).fileName().toHtmlEscaped()));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // ---- python
    auto* pythonRow = new QHBoxLayout;
    pythonRow->addWidget(new QLabel(QStringLiteral("Python：")));
    fPython = new QLineEdit;
    fPython->setPlaceholderText(QStringLiteral("python.exe 的路径"));
    pythonRow->addWidget(fPython, 1);
    auto* browse = new QPushButton(QStringLiteral("浏览…"));
    pythonRow->addWidget(browse);
    auto* check = new QPushButton(QStringLiteral("检测"));
    pythonRow->addWidget(check);
    layout->addLayout(pythonRow);

    auto* statusRow = new QHBoxLayout;
    fStatus = new QLabel;
    fStatus->setObjectName(QStringLiteral("status")); // these and the busy property: what the remote control reports
    fStatus->setWordWrap(true);
    statusRow->addWidget(fStatus, 1);
    fInstall = new QPushButton(QStringLiteral("安装 AI 组件"));
    fInstall->setToolTip(QStringLiteral("在 chost 自己的文件夹里建一个独立的 Python 环境并安装 audio-separator，不影响你电脑上的其他软件"));
    statusRow->addWidget(fInstall);
    layout->addLayout(statusRow);

    // ---- level
    auto* levelBox = new QVBoxLayout;
    levelBox->addWidget(new QLabel(QStringLiteral("分多细：")));
    fLevel = new QButtonGroup(this);
    const struct {
        const char* text;
        const char* id;
    } levels[] = {
        {"4 轨：人声 · 鼓 · 贝斯 · 其他（最快）", "basic"},
        {"6 轨：再分出 吉他 · 钢琴", "fine"},
        {"细分（推荐）：6 轨，鼓再拆成 底鼓 · 军鼓 · 通鼓 · 踩镲 · 叮叮镲 · 吊镲，人声再拆成 主唱 · 和声", "finest"},
    };
    int index = 0;
    for (const auto& level : levels)
    {
        auto* radio = new QRadioButton(QString::fromUtf8(level.text));
        radio->setProperty("level", QString::fromLatin1(level.id));
        fLevel->addButton(radio, index++);
        levelBox->addWidget(radio);
    }
    fLevel->button(2)->setChecked(true);
    layout->addLayout(levelBox);

    // ---- progress
    fProgress = new QProgressBar;
    fProgress->setObjectName(QStringLiteral("progress"));
    fProgress->setRange(0, 100);
    fProgress->setValue(0);
    layout->addWidget(fProgress);
    fLog = new QPlainTextEdit;
    fLog->setObjectName(QStringLiteral("log"));
    fLog->setReadOnly(true);
    fLog->setMaximumBlockCount(2000);
    fLog->setStyleSheet(QStringLiteral("font-family: Consolas, 'DejaVu Sans Mono', monospace; font-size: 11px;"));
    layout->addWidget(fLog, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    fCancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(fCancel);
    fStart = new QPushButton(QStringLiteral("开始分轨"));
    fStart->setDefault(true);
    buttons->addWidget(fStart);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 python.exe"), fPython->text());
        if (!path.isEmpty())
        {
            fPython->setText(QDir::toNativeSeparators(path));
            checkPython();
        }
    });
    connect(check, &QPushButton::clicked, this, &StemDialog::checkPython);
    connect(fInstall, &QPushButton::clicked, this, &StemDialog::install);
    connect(fStart, &QPushButton::clicked, this, &StemDialog::startSeparation);
    connect(fCancel, &QPushButton::clicked, this, &StemDialog::cancel);
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    connect(&fProcess, &QProcess::readyReadStandardOutput, this, &StemDialog::readOutput);
    connect(&fProcess, &QProcess::readyReadStandardError, this, [this] {
        for (const QByteArray& line : fProcess.readAllStandardError().split('\n'))
            if (!line.trimmed().isEmpty())
                log(QString::fromUtf8(line).trimmed());
    });
    connect(&fProcess, &QProcess::finished, this, &StemDialog::onFinished);
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            log(QStringLiteral("无法运行：%1").arg(fProcess.program()));
            onFinished(-1, QProcess::CrashExit);
        }
    });

    setBusy(false);
    detectPython();
}

StemDialog::~StemDialog()
{
    fProcess.disconnect(this);
    if (fProcess.state() != QProcess::NotRunning)
    {
        fProcess.kill();
        fProcess.waitForFinished(3000);
    }
}

QString StemDialog::scriptPath()
{
    return resources::toolScript(QStringLiteral("chost_separate.py"));
}

QString StemDialog::envFolder() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/ai-env");
}

QString StemDialog::envPython() const
{
#ifdef Q_OS_WIN
    return envFolder() + QStringLiteral("/Scripts/python.exe");
#else
    return envFolder() + QStringLiteral("/bin/python");
#endif
}

QString StemDialog::outputFolder() const
{
    // next to the song, so the stems are easy to find; the app's own folder when that's read-only
    const QFileInfo info(fAudioFile);
    const QString name = info.completeBaseName() + QStringLiteral("_stems");
    QDir dir = info.absoluteDir();
    if (dir.mkpath(name) && QFileInfo(dir.filePath(name)).isWritable())
        return dir.filePath(name);
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/stems/") +
                             name + QDateTime::currentDateTime().toString(QStringLiteral("_yyyyMMdd_hhmmss"));
    QDir().mkpath(fallback);
    return fallback;
}

void StemDialog::detectPython()
{
    QString python = resources::aiPython();
    if (python.isEmpty() && QFileInfo::exists(envPython()))
        python = envPython();
    if (python.isEmpty())
    {
#ifdef Q_OS_WIN
        for (const char* name : {"py", "python"})
#else
        for (const char* name : {"python3", "python"})
#endif
        {
            const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
            // the Microsoft Store placeholder only opens the store
            if (!found.isEmpty() && !found.contains(QStringLiteral("WindowsApps"), Qt::CaseInsensitive))
            {
                python = found;
                break;
            }
        }
    }
    fPython->setText(QDir::toNativeSeparators(python));
    if (python.isEmpty())
        fStatus->setText(QStringLiteral("没找到 Python。请先从 python.org 安装 Python 3.10 以上版本（安装时勾选 “Add python.exe to PATH”），再点“检测”。"));
    else
        checkPython();
}

void StemDialog::checkPython()
{
    fReady = false;
    setBusy(false);
    if (fPython->text().trimmed().isEmpty())
        return;
    fStatus->setText(QStringLiteral("正在检测…"));
    QStringList args{scriptPath(), QStringLiteral("--check")};
    if (qEnvironmentVariableIsSet("CHOST_STEMS_SELFTEST"))
        args << QStringLiteral("--selftest");
    run(Job::Check, fPython->text().trimmed(), args);
}

void StemDialog::install()
{
    const QString base = fPython->text().trimmed();
    if (base.isEmpty())
    {
        fStatus->setText(QStringLiteral("先选好 Python 再安装。"));
        return;
    }
    const QString env = QDir::toNativeSeparators(envFolder());
    const QString py = QDir::toNativeSeparators(envPython());

    // an NVIDIA card makes separation many times faster, it needs the CUDA build of PyTorch
    const bool nvidia = !QStandardPaths::findExecutable(QStringLiteral("nvidia-smi")).isEmpty()
#ifdef Q_OS_WIN
                        || QFileInfo::exists(QStringLiteral("C:/Windows/System32/nvidia-smi.exe"))
#endif
        ;

    fInstallSteps.clear();
    if (QFileInfo(base).absoluteFilePath() != QFileInfo(envPython()).absoluteFilePath())
        fInstallSteps.push_back({base, QStringLiteral("-m"), QStringLiteral("venv"), env});
    fInstallSteps.push_back({py, QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--upgrade"), QStringLiteral("pip")});
    if (nvidia)
        fInstallSteps.push_back({py, QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("torch"),
                                 QStringLiteral("torchaudio"), QStringLiteral("--index-url"), QStringLiteral("https://download.pytorch.org/whl/cu124")});
    fInstallSteps.push_back({py, QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                             nvidia ? QStringLiteral("audio-separator[gpu]") : QStringLiteral("audio-separator[cpu]"),
                             QStringLiteral("librosa"), QStringLiteral("scipy")}); // librosa and scipy: audio to MIDI

    log(nvidia ? QStringLiteral("检测到 NVIDIA 显卡，安装 GPU 版本（下载约 3 GB）。") : QStringLiteral("没检测到 NVIDIA 显卡，安装 CPU 版本（分轨会慢一些）。"));
    fStatus->setText(QStringLiteral("正在安装 AI 组件，需要几分钟到十几分钟…"));
    const QStringList step = fInstallSteps.front();
    fInstallSteps.erase(fInstallSteps.begin());
    run(Job::Install, step.front(), step.mid(1));
}

void StemDialog::setAutoStart(bool autoStart)
{
    fAutoStart = autoStart;
}

void StemDialog::startSeparation()
{
    if (!fReady || fJob != Job::None)
        return;
    fAutoStart = false;
    fStems = 0;
    fProgress->setValue(0);
    const QString level = fLevel->checkedButton()->property("level").toString();
    QStringList args{scriptPath(), QStringLiteral("--input"), fAudioFile, QStringLiteral("--out"), outputFolder(),
                     QStringLiteral("--level"), level};
    if (qEnvironmentVariableIsSet("CHOST_STEMS_SELFTEST"))
        args << QStringLiteral("--selftest");
    const QString models = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/models");
    QDir().mkpath(models);
    args << QStringLiteral("--model-dir") << models;
    fStatus->setText(QStringLiteral("正在分轨，一首歌用显卡大约 1 分钟，用 CPU 可能要 10 分钟以上…"));
    run(Job::Separate, fPython->text().trimmed(), args);
}

void StemDialog::cancel()
{
    if (fProcess.state() == QProcess::NotRunning)
        return;
    fInstallSteps.clear();
    fProcess.kill();
    log(QStringLiteral("已取消。"));
}

void StemDialog::run(Job job, const QString& program, const QStringList& args)
{
    fJob = job;
    fPending.clear();
    log(QStringLiteral("> %1 %2").arg(program, args.join(QLatin1Char(' '))));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    fProcess.setProcessEnvironment(env);
    setBusy(true);
    fProcess.start(program, args);
}

void StemDialog::readOutput()
{
    fPending += fProcess.readAllStandardOutput();
    int newline;
    while ((newline = fPending.indexOf('\n')) >= 0)
    {
        const QString line = QString::fromUtf8(fPending.left(newline)).trimmed();
        fPending.remove(0, newline + 1);
        if (line.isEmpty())
            continue;

        if (fJob == Job::Check)
        {
            if (line.startsWith(QLatin1String("OK ")))
            {
                const QStringList parts = line.split(QLatin1Char(' '));
                const QString device = parts.value(2);
                fReady = true;
                resources::setAiPython(fPython->text().trimmed());
                fStatus->setText(QStringLiteral("✓ AI 组件已就绪（audio-separator %1，%2）")
                                     .arg(parts.value(1), device.startsWith(QLatin1String("cuda"))
                                                              ? QStringLiteral("用显卡 %1").arg(device.mid(5))
                                                              : QStringLiteral("用 CPU")));
            }
            else
            {
                fStatus->setText(QStringLiteral("这个 Python 还没有装 AI 组件，点右边“安装 AI 组件”。"));
            }
            continue;
        }

        if (line.startsWith(QLatin1String("PROGRESS ")))
        {
            const QString rest = line.mid(9);
            fProgress->setValue(rest.section(QLatin1Char(' '), 0, 0).toInt());
            log(rest.section(QLatin1Char(' '), 1));
        }
        else if (line.startsWith(QLatin1String("STEM ")))
        {
            const QStringList parts = line.mid(5).split(QLatin1Char('\t'));
            if (parts.size() == 3)
            {
                ++fStems;
                log(QStringLiteral("✓ %1").arg(parts[1]));
                emit stemReady(parts[0], parts[1], parts[2]);
            }
        }
        else if (line.startsWith(QLatin1String("ERROR ")))
        {
            log(QStringLiteral("错误：%1").arg(line.mid(6)));
        }
        else if (line != QLatin1String("DONE"))
        {
            log(line);
        }
    }
}

void StemDialog::onFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput();
    const bool ok = status == QProcess::NormalExit && exitCode == 0;
    const Job job = fJob;
    fJob = Job::None;
    setBusy(false);

    switch (job)
    {
    case Job::Check:
        if (!ok && !fReady)
            fStatus->setText(QStringLiteral("这个 Python 还没有装 AI 组件，点右边“安装 AI 组件”。"));
        if (fReady && fAutoStart)
            startSeparation();
        break;
    case Job::Install:
        if (!ok)
        {
            fInstallSteps.clear();
            fStatus->setText(QStringLiteral("安装失败，详细信息见下面的记录。网络不好时可以开代理后再试一次。"));
            break;
        }
        if (!fInstallSteps.empty())
        {
            const QStringList step = fInstallSteps.front();
            fInstallSteps.erase(fInstallSteps.begin());
            run(Job::Install, step.front(), step.mid(1));
            return;
        }
        fPython->setText(QDir::toNativeSeparators(envPython()));
        checkPython();
        return;
    case Job::Separate:
        fStatus->setText(ok ? QStringLiteral("✓ 分轨完成，加了 %1 条音轨。").arg(fStems)
                            : QStringLiteral("分轨没有完成，详细信息见下面的记录。第一次下载模型失败时可以开代理后再试。"));
        if (ok)
            fProgress->setValue(100);
        emit separationFinished(ok);
        break;
    case Job::None:
        break;
    }
}

void StemDialog::setBusy(bool busy)
{
    setProperty("busy", busy);
    fStart->setEnabled(!busy && fReady);
    fInstall->setEnabled(!busy);
    fCancel->setEnabled(busy);
    fPython->setEnabled(!busy);
}

void StemDialog::log(const QString& line)
{
    fLog->appendPlainText(line);
}

void StemDialog::closeEvent(QCloseEvent* event)
{
    if (fProcess.state() != QProcess::NotRunning && fJob != Job::Check)
    {
        // keep working in the background, the stems still arrive as tracks
        hide();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}
