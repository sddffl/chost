#include "ClaudeConnect.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>

namespace claude {

static const QString kServer = QStringLiteral("chost");

QString mcpProgram()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("chost-mcp.exe");
#else
    const QString name = QStringLiteral("chost-mcp");
#endif
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    return QFileInfo::exists(path) ? QDir::toNativeSeparators(path) : QString();
}

QStringList desktopConfigFiles()
{
    QStringList files;
    const QString name = QStringLiteral("/Claude/claude_desktop_config.json");
#if defined(Q_OS_WIN)
    files << QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA")) + name;
    // the Microsoft Store kind of Claude Desktop keeps its folder inside its package
    const QDir packages(QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA")) + QStringLiteral("/Packages"));
    for (const QString& package : packages.entryList({QStringLiteral("Claude_*"), QStringLiteral("AnthropicPBC.Claude*")}, QDir::Dirs))
        files << packages.filePath(package) + QStringLiteral("/LocalCache/Roaming") + name;
#elif defined(Q_OS_MACOS)
    files << QDir::homePath() + QStringLiteral("/Library/Application Support") + name;
#else
    files << QDir::homePath() + QStringLiteral("/.config") + name;
#endif
    return files;
}

static bool readConfig(const QString& file, QJsonObject& config, QString& error)
{
    QFile in(file);
    if (!in.exists())
        return true;
    if (!in.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("读不了 %1").arg(QDir::toNativeSeparators(file));
        return false;
    }
    const QByteArray text = in.readAll();
    if (text.trimmed().isEmpty())
        return true;
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
    {
        // somebody's own settings: never written over
        error = QStringLiteral("%1 的内容不是有效的 JSON（%2），chost 没有改动它。").arg(QDir::toNativeSeparators(file), parse.errorString());
        return false;
    }
    config = doc.object();
    return true;
}

bool isInConfig(const QString& file)
{
    QJsonObject config;
    QString error;
    return readConfig(file, config, error) && config.value(QStringLiteral("mcpServers")).toObject().contains(kServer);
}

bool addToConfig(const QString& file, const QString& program, QString& error)
{
    QJsonObject config;
    if (!readConfig(file, config, error))
        return false;
    if (QFileInfo::exists(file))
    {
        QFile::remove(file + QStringLiteral(".bak"));
        QFile::copy(file, file + QStringLiteral(".bak"));
    }
    QJsonObject servers = config.value(QStringLiteral("mcpServers")).toObject();
    servers.insert(kServer, QJsonObject{{QStringLiteral("command"), program}, {QStringLiteral("args"), QJsonArray{}}});
    config.insert(QStringLiteral("mcpServers"), servers);

    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(config).toJson(QJsonDocument::Indented)) < 0 || !out.commit())
    {
        error = QStringLiteral("写不了 %1").arg(QDir::toNativeSeparators(file));
        return false;
    }
    return true;
}

void showDialog(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("连接 Claude"));
    dialog.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dialog);

    auto* intro = new QLabel(QStringLiteral(
        "<b>让 Claude 直接操控 chost</b><br><br>"
        "连接以后，在 Claude 桌面版里跟 Claude 说“把鼓的音量调小一点”“给人声加个压缩”“把这首歌分轨”，"
        "Claude 就会在 chost 里动手做。你在 chost 窗口里能看到每一步，不满意按 Ctrl+Z 撤销。<br><br>"
        "需要 Claude 桌面版（从 claude.ai/download 下载）。"));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    layout->addWidget(intro);

    const QString program = mcpProgram();
    auto* status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);
    auto refresh = [status, program] {
        if (program.isEmpty())
        {
            status->setText(QStringLiteral("找不到 chost-mcp：chost 安装得不完整，请重新下载。"));
            return;
        }
        bool any = false;
        for (const QString& file : desktopConfigFiles())
            any = any || isInConfig(file);
        status->setText(any ? QStringLiteral("✔ 已经连接好了。如果 Claude 里还看不到 chost，请完全退出 Claude 桌面版再打开。")
                            : QStringLiteral("还没有连接。"));
    };
    refresh();

    auto* connectButton = new QPushButton(QStringLiteral("一键连接 Claude 桌面版"));
    connectButton->setEnabled(!program.isEmpty());
    layout->addWidget(connectButton);
    QObject::connect(connectButton, &QPushButton::clicked, &dialog, [&dialog, status, program, refresh] {
        QStringList errors;
        int written = 0;
        const QStringList files = desktopConfigFiles();
        for (int i = 0; i < files.size(); ++i)
        {
            // the usual place always; the Store package's only where that package is
            QString error;
            if (addToConfig(files[i], program, error))
                ++written;
            else
                errors << error;
        }
        refresh();
        if (written > 0)
            status->setText(QStringLiteral("✔ 连接好了！\n\n现在请完全退出 Claude 桌面版（屏幕右下角托盘里的 Claude 图标，右键 → 退出），"
                                           "再重新打开。之后在对话框里就能用 chost 的工具了，试试说：“看看 chost 里现在有哪些轨道”。"));
        if (!errors.isEmpty())
            status->setText(status->text() + QStringLiteral("\n\n") + errors.join(QLatin1Char('\n')));
        dialog.adjustSize();
    });

    // Claude Code: one command in a terminal
    auto* codeLabel = new QLabel(QStringLiteral("用 Claude Code（命令行版）的话，在终端里运行这一行："));
    codeLabel->setWordWrap(true);
    layout->addSpacing(8);
    layout->addWidget(codeLabel);
    auto* row = new QHBoxLayout;
    auto* command = new QLineEdit(QStringLiteral("claude mcp add chost -- \"%1\"").arg(program));
    command->setReadOnly(true);
    command->setCursorPosition(0);
    auto* copy = new QPushButton(QStringLiteral("复制"));
    QObject::connect(copy, &QPushButton::clicked, command, [command] { QApplication::clipboard()->setText(command->text()); });
    row->addWidget(command, 1);
    row->addWidget(copy);
    layout->addLayout(row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

} // namespace claude
