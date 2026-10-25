// chost-mcp: chost's MCP server. An AI assistant (Claude Desktop, Claude Code, ...) starts it and talks
// MCP over stdin and stdout, one JSON-RPC message per line; each tool call goes on to the chost window
// that runs on this computer (one is started when none runs), which does the work and answers.
//
//   chost-mcp                 serve (what the assistant runs)
//   chost-mcp --chost <exe>   the chost to start, by default the one next to chost-mcp

#include "control/Tools.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>

#include <cstdio>
#include <iostream>
#include <string>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr int kStartSeconds = 60;   // for a chost started here to come up
constexpr int kCallSeconds = 130;   // a call: wait_for_jobs takes up to 110

// the running chost, over its local socket
class Link {
public:
    explicit Link(QString chost)
        : fChost(std::move(chost))
    {
    }

    // the answer of chost: {"ok": true, "result": ...} or {"ok": false, "error": "..."}
    QJsonObject call(const QString& tool, const QJsonObject& args)
    {
        QString error;
        if (!connected(error))
            return failure(error);
        const int id = ++fNextId;
        const QJsonObject request{{QStringLiteral("id"), id}, {QStringLiteral("tool"), tool}, {QStringLiteral("args"), args}};
        fSocket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        fSocket.flush();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kCallSeconds * 1000)
        {
            while (fSocket.canReadLine())
            {
                const QJsonObject response = QJsonDocument::fromJson(fSocket.readLine()).object();
                if (response.value(QStringLiteral("id")).toInt() == id)
                    return response;
            }
            if (fSocket.state() != QLocalSocket::ConnectedState)
                return failure(QStringLiteral("chost was closed while it worked on %1").arg(tool));
            fSocket.waitForReadyRead(200);
        }
        fSocket.abort(); // the answer may still come: a fresh connection keeps answers from getting mixed up
        return failure(QStringLiteral("chost did not answer %1 within %2 seconds").arg(tool).arg(kCallSeconds));
    }

private:
    static QJsonObject failure(const QString& error)
    {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    }

    bool tryConnect()
    {
        fSocket.abort();
        fSocket.connectToServer(control::serverName());
        return fSocket.waitForConnected(500);
    }

    bool connected(QString& error)
    {
        if (fSocket.state() == QLocalSocket::ConnectedState || tryConnect())
            return true;
        // no chost running: start one, the user sees it come up
        if (!QFileInfo::exists(fChost))
        {
            error = QStringLiteral("chost is not running, and it is not at %1 to start it").arg(QDir::toNativeSeparators(fChost));
            return false;
        }
        // with stdin and stdout of its own: what it prints must never get into the MCP messages on ours
        QProcess process;
        process.setProgram(fChost);
        process.setWorkingDirectory(QFileInfo(fChost).absolutePath());
        process.setStandardInputFile(QProcess::nullDevice());
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        if (!process.startDetached())
        {
            error = QStringLiteral("chost is not running and could not be started (%1)").arg(QDir::toNativeSeparators(fChost));
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kStartSeconds * 1000)
        {
            if (tryConnect())
                return true;
            QThread::msleep(300);
        }
        error = QStringLiteral("chost was started but did not come up within %1 seconds; is it waiting for an answer in its window?").arg(kStartSeconds);
        return false;
    }

    QString fChost;
    QLocalSocket fSocket;
    int fNextId = 0;
};

void send(const QJsonObject& message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

void reply(const QJsonValue& id, const QJsonValue& result)
{
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}});
}

void replyError(const QJsonValue& id, int code, const QString& message)
{
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), id},
          {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}});
}

QJsonObject textResult(const QString& text, bool isError)
{
    return {{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}}},
            {QStringLiteral("isError"), isError}};
}

bool knownTool(const QString& name)
{
    for (const QJsonValue& tool : control::tools())
        if (tool.toObject().value(QStringLiteral("name")).toString() == name)
            return true;
    return false;
}

void handle(Link& link, const QJsonObject& message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    if (!message.contains(QStringLiteral("id")))
        return; // a notification ("notifications/initialized", "notifications/cancelled"): nothing to answer

    if (method == QLatin1String("initialize"))
    {
        // the client's protocol version where it is one we know, else our newest
        static const QStringList known{QStringLiteral("2024-11-05"), QStringLiteral("2025-03-26"), QStringLiteral("2025-06-18"),
                                       QStringLiteral("2025-11-25")};
        const QString asked = params.value(QStringLiteral("protocolVersion")).toString();
        reply(id, QJsonObject{{QStringLiteral("protocolVersion"), known.contains(asked) ? asked : QStringLiteral("2025-06-18")},
                              {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                              {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("chost")},
                                                                         {QStringLiteral("title"), QStringLiteral("chost")},
                                                                         {QStringLiteral("version"), QStringLiteral(CHOST_VERSION)}}},
                              {QStringLiteral("instructions"), control::instructions()}});
    }
    else if (method == QLatin1String("ping"))
        reply(id, QJsonObject{});
    else if (method == QLatin1String("tools/list"))
        reply(id, QJsonObject{{QStringLiteral("tools"), control::tools()}});
    else if (method == QLatin1String("tools/call"))
    {
        const QString name = params.value(QStringLiteral("name")).toString();
        if (!knownTool(name))
        {
            replyError(id, -32602, QStringLiteral("unknown tool: %1").arg(name));
            return;
        }
        const QJsonObject answer = link.call(name, params.value(QStringLiteral("arguments")).toObject());
        if (answer.value(QStringLiteral("ok")).toBool())
        {
            const QJsonValue result = answer.value(QStringLiteral("result"));
            const QByteArray text = result.isObject() ? QJsonDocument(result.toObject()).toJson(QJsonDocument::Indented)
                                    : result.isArray() ? QJsonDocument(result.toArray()).toJson(QJsonDocument::Indented)
                                                       : result.toVariant().toString().toUtf8();
            reply(id, textResult(QString::fromUtf8(text), false));
        }
        else
            reply(id, textResult(answer.value(QStringLiteral("error")).toString(), true));
    }
    else if (method == QLatin1String("resources/list"))
        reply(id, QJsonObject{{QStringLiteral("resources"), QJsonArray{}}});
    else if (method == QLatin1String("prompts/list"))
        reply(id, QJsonObject{{QStringLiteral("prompts"), QJsonArray{}}});
    else
        replyError(id, -32601, QStringLiteral("method not found: %1").arg(method));
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef Q_OS_WIN
    // JSON lines, byte for byte: no \r\n turning up in them
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("chost-mcp"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CHOST_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("chost's MCP server: lets an AI assistant work in chost. It talks MCP on stdin and stdout."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption chostOpt(QStringLiteral("chost"), QStringLiteral("The chost program to start when none runs."), QStringLiteral("exe"));
    parser.addOption(chostOpt);
    parser.process(app);

#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("chost.exe");
#else
    const QString exe = QStringLiteral("chost");
#endif
    Link link(parser.isSet(chostOpt) ? parser.value(chostOpt) : QDir(QCoreApplication::applicationDirPath()).filePath(exe));

    std::string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
        {
            replyError(QJsonValue(), -32700, QStringLiteral("parse error: %1").arg(error.errorString()));
            continue;
        }
        handle(link, doc.object());
    }
    return 0;
}
