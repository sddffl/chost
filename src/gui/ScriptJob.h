// Runs one of chost's Python helpers (tools/*.py) with the AI environment's Python and splits what it
// prints into progress, result lines ("TEMPO ...", "KEY ...") and the final DONE or ERROR.

#pragma once

#include <QObject>
#include <QProcess>

class ScriptJob : public QObject {
    Q_OBJECT

public:
    explicit ScriptJob(QObject* parent = nullptr);
    ~ScriptJob() override;

    // false (and finished() with the reason) when the AI components or the script are missing
    bool start(const QString& script, const QStringList& args);
    void cancel();
    bool isRunning() const { return fProcess.state() != QProcess::NotRunning; }

signals:
    void progress(int percent, const QString& message);
    void result(const QString& key, const QString& value); // every other line, e.g. "TEMPO", "120.0\t0.04"
    void finished(bool ok, const QString& message);        // message: DONE's text, or the error

private:
    void readOutput();

    QProcess fProcess;
    QByteArray fPending;
    QString fDone;
    QString fError;
    bool fCancelled = false;
};
