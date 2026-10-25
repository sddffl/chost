#include "RenderJob.h"

#include "Resources.h"

RenderJob::RenderJob(QObject* parent)
    : QObject(parent)
{
    fProcess.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&fProcess, &QProcess::readyReadStandardOutput, this, &RenderJob::readOutput);
    connect(&fProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        const bool ok = status == QProcess::NormalExit && code == 0 && !fResult.isEmpty();
        if (!ok && fError.isEmpty())
            fError = QStringLiteral("渲染中断了（可能是某个插件崩溃）");
        emit finished(ok, fOutput, ok ? fResult : fError);
    });
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            emit finished(false, fOutput, QStringLiteral("找不到 chost-render，请重新解压 chost"));
    });
}

RenderJob::~RenderJob()
{
    fProcess.disconnect(this);
    if (fProcess.state() != QProcess::NotRunning)
    {
        fProcess.kill();
        fProcess.waitForFinished(3000);
    }
}

void RenderJob::start(const QString& session, const QString& output, int bits, const QList<int>& solo,
                      const QList<int>& mute)
{
    fOutput = output;
    fResult.clear();
    fError.clear();
    fPending.clear();
    QStringList args{QStringLiteral("--session"), session, QStringLiteral("--output"), output,
                     QStringLiteral("--bits"), QString::number(bits)};
    for (int track : solo)
        args << QStringLiteral("--solo") << QString::number(track);
    for (int track : mute)
        args << QStringLiteral("--mute") << QString::number(track);
    const QString tool = resources::renderTool();
    if (tool.isEmpty())
    {
        emit finished(false, output, QStringLiteral("找不到 chost-render，请重新解压 chost"));
        return;
    }
    fProcess.start(tool, args);
}

void RenderJob::cancel()
{
    if (fProcess.state() != QProcess::NotRunning)
    {
        fError = QStringLiteral("已取消");
        fProcess.kill();
    }
}

void RenderJob::readOutput()
{
    fPending += fProcess.readAllStandardOutput();
    int newline;
    while ((newline = fPending.indexOf('\n')) >= 0)
    {
        const QString line = QString::fromUtf8(fPending.left(newline)).trimmed();
        fPending.remove(0, newline + 1);
        if (line.startsWith(QLatin1String("PROGRESS ")))
            emit progress(line.mid(9).toInt());
        else if (line.startsWith(QLatin1String("DONE ")))
            fResult = line.mid(5).section(QLatin1Char('\t'), 1);
        else if (line.startsWith(QLatin1String("ERROR ")))
            fError = line.mid(6);
    }
}
