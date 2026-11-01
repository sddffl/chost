#include "ScriptJob.h"

#include "Resources.h"

ScriptJob::ScriptJob(QObject* parent)
    : QObject(parent)
{
    fProcess.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    fProcess.setProcessEnvironment(env);
    connect(&fProcess, &QProcess::readyReadStandardOutput, this, &ScriptJob::readOutput);
    connect(&fProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        const bool ok = !fCancelled && status == QProcess::NormalExit && code == 0 && fError.isEmpty();
        if (!ok && fError.isEmpty())
            fError = fCancelled ? QStringLiteral("已取消") : QStringLiteral("处理中断了");
        emit finished(ok, ok ? fDone : fError);
    });
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            emit finished(false, QStringLiteral("AI 组件的 Python 启动不了，请在“AI 分轨”窗口里重新检测或安装"));
    });
}

ScriptJob::~ScriptJob()
{
    fProcess.disconnect(this);
    if (fProcess.state() != QProcess::NotRunning)
    {
        fProcess.kill();
        fProcess.waitForFinished(3000);
    }
}

bool ScriptJob::start(const QString& script, const QStringList& args)
{
    fPending.clear();
    fDone.clear();
    fError.clear();
    fCancelled = false;
    const QString python = resources::aiPython();
    const QString path = resources::toolScript(script);
    if (python.isEmpty())
    {
        emit finished(false, QStringLiteral("还没有安装 AI 组件：请先打开“AI 分轨”窗口，点“安装 AI 组件”"));
        return false;
    }
    if (path.isEmpty())
    {
        emit finished(false, QStringLiteral("找不到 %1，请重新解压 chost").arg(script));
        return false;
    }
    fProcess.start(python, QStringList{path} + args);
    return true;
}

void ScriptJob::cancel()
{
    if (fProcess.state() != QProcess::NotRunning)
    {
        fCancelled = true;
        fProcess.kill();
    }
}

void ScriptJob::readOutput()
{
    fPending += fProcess.readAllStandardOutput();
    int newline;
    while ((newline = fPending.indexOf('\n')) >= 0)
    {
        const QString line = QString::fromUtf8(fPending.left(newline)).trimmed();
        fPending.remove(0, newline + 1);
        const QString key = line.section(QLatin1Char(' '), 0, 0);
        const QString value = line.section(QLatin1Char(' '), 1);
        if (key == QLatin1String("PROGRESS"))
            emit progress(value.section(QLatin1Char(' '), 0, 0).toInt(), value.section(QLatin1Char(' '), 1));
        else if (key == QLatin1String("DONE"))
            fDone = value;
        else if (key == QLatin1String("ERROR"))
            fError = value;
        else if (!key.isEmpty())
            emit result(key, value);
    }
}
