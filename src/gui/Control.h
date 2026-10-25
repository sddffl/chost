// Remote control: an AI assistant (through chost-mcp, chost's MCP server) works the project the way the
// user does in the window, and every change it makes is seen there, recorded in the undo history and
// saved like any other. chost listens on a local socket of the user's own (src/control/Tools.h has the
// protocol and the tools); errors come back to the assistant instead of popping up.

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <vector>

class MainWindow;
class QLocalServer;
class QLocalSocket;

class Control : public QObject {
    Q_OBJECT

public:
    explicit Control(MainWindow& window);
    ~Control() override;

    bool listen(); // false: another chost already has the user's remote control

    using Reply = std::function<void(bool ok, const QJsonValue& result, const QString& error)>;
    // one tool call; the reply may come later (wait_for_jobs). The same as a call from the socket (tests).
    void call(const QString& tool, const QJsonObject& args, Reply reply);
    bool knows(const QString& tool) const;

private:
    struct Job {
        int id = 0;
        QString kind;           // "stems", "transcribe", ...
        QString what;           // e.g. the track's name
        QPointer<QWidget> dialog; // null for the analysis, which has no window
        QDateTime started;
        bool seenBusy = false;
        bool done = false;
        int percent = 0;
        QString message;
        QString log;
        QString file; // what it made, where it says so
    };
    struct Wait {
        QDateTime deadline;
        Reply reply;
    };

    void onConnection();
    void onReadyRead(QLocalSocket* socket);
    void pollJobs();
    QJsonArray jobsJson() const;
    int startJob(const QString& kind, const QString& what, QWidget* dialog);
    void setJobFile(int id, const QString& file);
    bool anyJobRunning() const;

    MainWindow& fWindow;
    QLocalServer* fServer = nullptr;
    std::vector<Job> fJobs;
    std::vector<Wait> fWaits;
    QTimer fPoll;
    int fNextJob = 1;
};
