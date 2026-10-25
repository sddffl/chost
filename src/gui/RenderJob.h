// Runs chost-render on a saved session: the mix, or one track alone, to a WAV file.

#pragma once

#include <QList>
#include <QObject>
#include <QProcess>

#include <functional>

// The project as it is right now, saved where chost-render can read it.
struct RenderSnapshot {
    QString session;    // the .chost file, empty on error
    QString error;
    QList<int> masters; // tracks holding masters made earlier, left out of a mix that is to be mastered
};
using SnapshotFunction = std::function<RenderSnapshot()>;

class RenderJob : public QObject {
    Q_OBJECT

public:
    explicit RenderJob(QObject* parent = nullptr);
    ~RenderJob() override;

    // solo: the only tracks to render, empty for the mix as it sounds; mute: tracks to leave out
    void start(const QString& session, const QString& output, int bits = 24, const QList<int>& solo = {},
               const QList<int>& mute = {});
    void cancel();
    bool isRunning() const { return fProcess.state() != QProcess::NotRunning; }

signals:
    void progress(int percent);
    void finished(bool ok, const QString& output, const QString& message); // message: an error, or "<seconds>\t<peak dB>"

private:
    void readOutput();

    QProcess fProcess;
    QByteArray fPending;
    QString fOutput;
    QString fResult;
    QString fError;
};
