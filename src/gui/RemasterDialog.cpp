#include "RemasterDialog.h"

#include "Resources.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

RemasterDialog::RemasterDialog(const QString& trackName, const QString& audioFile, QWidget* parent)
    : QDialog(parent)
    , fTrackName(trackName)
    , fAudioFile(audioFile)
{
    setWindowTitle(QStringLiteral("AI 全自动音乐重制与顶级母带 · %1").arg(trackName));
    resize(720, 560);

    auto* layout = new QVBoxLayout(this);

    auto* banner = new QLabel(QStringLiteral(
        "<b>一键全自动顶级音乐制作工作流：</b><br>"
        "① 音频分析（自动测速定调、节拍侦测）<br>"
        "② RTX 3090 GPU 14 轨细分极速分轨（主唱/和声/底鼓/军鼓/通鼓/踩镲/吊镲/贝斯/吉他/钢琴等）<br>"
        "③ 伴奏分轨高精度转谱 MIDI<br>"
        "④ 人声与和声智能修音与跑调纠偏（保持自然音准与颤音）<br>"
        "⑤ 空间声像排布与多轨响度声学混音平衡<br>"
        "⑥ 商业级总线母带雕琢与 True-Peak 限制（-14 LUFS 行业标准）<br>"
        "⑦ 导出 24-bit 顶级母带 WAV 直达系统桌面。"));
    banner->setWordWrap(true);
    banner->setStyleSheet(QStringLiteral("QLabel { background-color: #23272e; padding: 10px; border-radius: 6px; color: #abb2bf; }"));
    layout->addWidget(banner);

    // Audio input row
    auto* audioRow = new QHBoxLayout;
    audioRow->addWidget(new QLabel(QStringLiteral("输入音频：")));
    fAudioInput = new QLineEdit(audioFile);
    audioRow->addWidget(fAudioInput, 1);
    auto* browseAudio = new QPushButton(QStringLiteral("浏览…"));
    audioRow->addWidget(browseAudio);
    connect(browseAudio, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getOpenFileName(this, QStringLiteral("选择输入音频"), fAudioInput->text(),
                                                           QStringLiteral("音频文件 (*.wav *.flac *.mp3 *.m4a *.aif)"));
        if (!picked.isEmpty())
            fAudioInput->setText(QDir::toNativeSeparators(picked));
    });
    layout->addLayout(audioRow);

    // Desktop export row
    auto* exportRow = new QHBoxLayout;
    exportRow->addWidget(new QLabel(QStringLiteral("导出目录：")));
    fExportDir = new QLineEdit(desktopFolder());
    exportRow->addWidget(fExportDir, 1);
    auto* browseExport = new QPushButton(QStringLiteral("更改…"));
    exportRow->addWidget(browseExport);
    connect(browseExport, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择导出文件夹"), fExportDir->text());
        if (!dir.isEmpty())
            fExportDir->setText(QDir::toNativeSeparators(dir));
    });
    layout->addLayout(exportRow);

    // Status label
    fStatus = new QLabel(QStringLiteral("准备就绪，点击“开始全自动重制与顶级母带”即可启动全流程。"));
    fStatus->setObjectName(QStringLiteral("status"));
    fStatus->setWordWrap(true);
    fStatus->setStyleSheet(QStringLiteral("font-weight: bold; color: #61afef; margin-top: 4px;"));
    layout->addWidget(fStatus);

    // Progress bar
    fProgress = new QProgressBar;
    fProgress->setObjectName(QStringLiteral("progress"));
    fProgress->setRange(0, 100);
    fProgress->setValue(0);
    fProgress->setTextVisible(true);
    layout->addWidget(fProgress);

    // Log viewer
    fLog = new QPlainTextEdit;
    fLog->setObjectName(QStringLiteral("log"));
    fLog->setReadOnly(true);
    fLog->setMaximumBlockCount(3000);
    fLog->setStyleSheet(QStringLiteral("background: #181a1f; color: #98c379; font-family: Consolas, 'DejaVu Sans Mono', monospace; font-size: 11px;"));
    layout->addWidget(fLog, 1);

    // Buttons
    auto* buttons = new QHBoxLayout;
    fOpenDesktop = new QPushButton(QStringLiteral("📂 在桌面查看母带音频"));
    fOpenDesktop->setEnabled(false);
    buttons->addWidget(fOpenDesktop);

    fImportStems = new QPushButton(QStringLiteral("🎵 载入 14 轨分轨与 MIDI 到当前工程"));
    fImportStems->setEnabled(false);
    buttons->addWidget(fImportStems);

    buttons->addStretch();

    fCancel = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(fCancel);

    fStart = new QPushButton(QStringLiteral("⚡ 开始全自动重制与顶级母带"));
    fStart->setDefault(true);
    fStart->setStyleSheet(QStringLiteral("QPushButton { background-color: #98c379; color: #1e1e1e; font-weight: bold; padding: 6px 14px; } QPushButton:hover { background-color: #aadb86; }"));
    buttons->addWidget(fStart);

    layout->addLayout(buttons);

    // Connect signals
    connect(fStart, &QPushButton::clicked, this, &RemasterDialog::startRemaster);
    connect(fCancel, &QPushButton::clicked, this, [this] {
        if (fBusy)
            cancel();
        else
            close();
    });
    connect(fOpenDesktop, &QPushButton::clicked, this, [this] {
        if (fMasterPath.isEmpty() || !QFileInfo::exists(fMasterPath))
            return;
#ifdef Q_OS_WIN
        QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(fMasterPath)});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(fMasterPath).absolutePath()));
#endif
    });
    connect(fImportStems, &QPushButton::clicked, this, [this] {
        emit importRequested(fMasterPath, fWorkDir);
        fImportStems->setEnabled(false);
        fImportStems->setText(QStringLiteral("✓ 已载入工程"));
    });

    connect(&fProcess, &QProcess::readyReadStandardOutput, this, &RemasterDialog::readOutput);
    connect(&fProcess, &QProcess::readyReadStandardError, this, &RemasterDialog::readOutput);
    connect(&fProcess, &QProcess::finished, this, &RemasterDialog::onFinished);

    setBusy(false);
}

RemasterDialog::~RemasterDialog()
{
    fProcess.disconnect(this);
    if (fProcess.state() != QProcess::NotRunning)
    {
        fProcess.kill();
        fProcess.waitForFinished(3000);
    }
}

QString RemasterDialog::desktopFolder() const
{
    QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty() || !QDir(desktop).exists())
    {
        desktop = QDir::homePath() + QStringLiteral("/Desktop");
    }
    return QDir::toNativeSeparators(desktop);
}

void RemasterDialog::setAutoStart(bool autoStart)
{
    fAutoStart = autoStart;
    if (fAutoStart && !fBusy)
        startRemaster();
}

void RemasterDialog::setBusy(bool busy)
{
    fBusy = busy;
    setProperty("busy", busy);
    fStart->setEnabled(!busy);
    fCancel->setText(busy ? QStringLiteral("取消") : QStringLiteral("关闭"));
    fAudioInput->setEnabled(!busy);
    fExportDir->setEnabled(!busy);
}

void RemasterDialog::log(const QString& line)
{
    fLog->appendPlainText(line);
}

void RemasterDialog::startRemaster()
{
    if (fBusy)
        return;

    const QString audioPath = fAudioInput->text().trimmed();
    if (audioPath.isEmpty() || !QFileInfo::exists(audioPath))
    {
        fStatus->setText(QStringLiteral("请先指定有效的输入音频文件。"));
        return;
    }

    const QString python = resources::aiPython();
    const QString script = resources::toolScript(QStringLiteral("chost_workflow_remaster.py"));

    if (python.isEmpty())
    {
        fStatus->setText(QStringLiteral("未找到 Python 环境，请先在“AI 自动分轨”中完成检测或安装。"));
        return;
    }
    if (script.isEmpty())
    {
        fStatus->setText(QStringLiteral("未找到工作流脚本 chost_workflow_remaster.py，请检查 chost 安装目录下的 tools/ 文件夹。"));
        return;
    }

    fMasterPath.clear();
    fDiscoveredStems.clear();
    fDiscoveredMidis.clear();

    const QFileInfo fileInfo(audioPath);
    fWorkDir = QDir::toNativeSeparators(fileInfo.absoluteDir().filePath(fileInfo.completeBaseName() + QStringLiteral("_remaster_work")));

    QStringList args;
    args << script;
    args << QStringLiteral("--input") << audioPath;
    const QString exportDir = fExportDir->text().trimmed();
    if (!exportDir.isEmpty())
        args << QStringLiteral("--export-dir") << exportDir;

    fPending.clear();
    fProgress->setValue(0);
    fLog->clear();
    log(QStringLiteral("> %1 %2").arg(python, args.join(QLatin1Char(' '))));

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    if (QDir(QStringLiteral("D:/AI_Models/audio_separator")).exists())
        env.insert(QStringLiteral("AUDIO_SEPARATOR_MODEL_DIR"), QStringLiteral("D:/AI_Models/audio_separator"));
    fProcess.setProcessEnvironment(env);

    setBusy(true);
    fStatus->setText(QStringLiteral("正在初始化全自动制作工作流..."));
    fProcess.start(python, args);
}

void RemasterDialog::cancel()
{
    if (fProcess.state() == QProcess::NotRunning)
        return;
    fProcess.kill();
    log(QStringLiteral("已取消全自动母带流程。"));
    setBusy(false);
    fStatus->setText(QStringLiteral("处理已被用户取消。"));
    emit remasterFinished(false);
}

void RemasterDialog::readOutput()
{
    fPending += fProcess.readAllStandardOutput();
    fPending += fProcess.readAllStandardError();
    int newline;
    while ((newline = fPending.indexOf('\n')) >= 0)
    {
        const QString line = QString::fromUtf8(fPending.left(newline)).trimmed();
        fPending.remove(0, newline + 1);
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("PROGRESS ")))
        {
            const QString rest = line.mid(9).trimmed();
            const int space = rest.indexOf(QLatin1Char(' '));
            if (space > 0)
            {
                const int pct = rest.left(space).toInt();
                const QString msg = rest.mid(space + 1).trimmed();
                fProgress->setValue(pct);
                fStatus->setText(msg);
                emit progressUpdated(pct, msg);
            }
        }
        else if (line.startsWith(QLatin1String("ANALYZE ")))
        {
            const QStringList parts = line.mid(8).trimmed().split(QLatin1Char('\t'));
            if (!parts.isEmpty())
            {
                const double bpm = parts.value(0).toDouble();
                const QString key = parts.value(1);
                emit songAnalyzed(bpm, key);
            }
        }
        else if (line.startsWith(QLatin1String("STEM ")))
        {
            const QStringList parts = line.mid(5).trimmed().split(QLatin1Char('\t'));
            if (parts.size() >= 3)
            {
                const QString id = parts[0];
                const QString label = parts[1];
                const QString path = parts[2];
                fDiscoveredStems.append(qMakePair(label, path));
                emit stemReady(id, label, path);
            }
        }
        else if (line.startsWith(QLatin1String("TUNED ")))
        {
            const QStringList parts = line.mid(6).trimmed().split(QLatin1Char('\t'));
            if (parts.size() >= 2)
            {
                emit vocalTuned(parts[0], parts[1]);
            }
        }
        else if (line.startsWith(QLatin1String("MIDI ")))
        {
            const QStringList parts = line.mid(5).trimmed().split(QLatin1Char('\t'));
            QString role;
            QString midiPath;
            if (parts.size() >= 2)
            {
                if (parts[0].endsWith(QLatin1String(".mid"), Qt::CaseInsensitive))
                {
                    midiPath = parts[0];
                    role = QFileInfo(midiPath).baseName();
                }
                else if (parts[1].endsWith(QLatin1String(".mid"), Qt::CaseInsensitive))
                {
                    role = parts[0];
                    midiPath = parts[1];
                }
            }
            else if (parts.size() == 1 && parts[0].endsWith(QLatin1String(".mid"), Qt::CaseInsensitive))
            {
                midiPath = parts[0];
                role = QFileInfo(midiPath).baseName();
            }

            if (!midiPath.isEmpty() && QFileInfo::exists(midiPath))
            {
                bool alreadyDiscovered = false;
                for (const auto& pair : fDiscoveredMidis)
                {
                    if (pair.second == midiPath)
                    {
                        alreadyDiscovered = true;
                        break;
                    }
                }
                if (!alreadyDiscovered)
                {
                    fDiscoveredMidis.append(qMakePair(role, midiPath));
                    emit midiReady(role, midiPath);
                }
            }
        }
        else if (line.startsWith(QLatin1String("BALANCE ")))
        {
            const QStringList parts = line.mid(8).trimmed().split(QLatin1Char('\t'));
            if (parts.size() >= 3)
            {
                emit trackBalanced(parts[0], parts[1].toFloat(), parts[2].toFloat());
            }
        }
        else if (line.startsWith(QLatin1String("MASTER ")))
        {
            fMasterPath = line.mid(7).trimmed();
            emit masterReady(fMasterPath);
        }

        log(line);
    }
}

void RemasterDialog::onFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput();
    const bool ok = (status == QProcess::NormalExit && exitCode == 0);
    setBusy(false);

    if (ok)
    {
        fProgress->setValue(100);
        fStatus->setText(QStringLiteral("✓ 全套重制与顶级母带制作完成！已保存至桌面。"));
        fOpenDesktop->setEnabled(!fMasterPath.isEmpty() && QFileInfo::exists(fMasterPath));
        fImportStems->setEnabled(!fWorkDir.isEmpty() && QDir(fWorkDir).exists());
        emit masterReady(fMasterPath);
        emit remasterFinished(true);
    }
    else
    {
        fStatus->setText(QStringLiteral("全自动处理未能正常结束，详情请参阅上方日志。"));
        emit remasterFinished(false);
    }
}

void RemasterDialog::closeEvent(QCloseEvent* event)
{
    if (fBusy)
    {
        cancel();
    }
    event->accept();
}
