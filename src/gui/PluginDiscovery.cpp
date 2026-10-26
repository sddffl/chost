#include "PluginDiscovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

static QString cachePath()
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(folder);
    return QDir(folder).filePath(QStringLiteral("plugin-cache.json"));
}

// a plugin that was updated or reinstalled is discovered again
static QString stampOf(const chost::PluginEntry& file)
{
    QString path = QString::fromStdString(file.spec).section(QLatin1Char(':'), 1);
    QFileInfo info(path);
    if (info.isDir() && file.format == "VST3")
        info = QFileInfo(path + QStringLiteral("/Contents"));
    return QStringLiteral("%1/%2").arg(info.size()).arg(info.lastModified().toSecsSinceEpoch());
}

static QJsonObject toJson(const chost::PluginEntry& e)
{
    return QJsonObject{{QStringLiteral("spec"), QString::fromStdString(e.spec)},
                       {QStringLiteral("name"), QString::fromStdString(e.name)},
                       {QStringLiteral("maker"), QString::fromStdString(e.maker)},
                       {QStringLiteral("category"), QString::fromStdString(e.category)},
                       {QStringLiteral("synth"), e.isSynth},
                       {QStringLiteral("audioIns"), static_cast<int>(e.audioIns)},
                       {QStringLiteral("audioOuts"), static_cast<int>(e.audioOuts)},
                       {QStringLiteral("midiIns"), static_cast<int>(e.midiIns)}};
}

static chost::PluginEntry fromJson(const QJsonObject& o, const chost::PluginEntry& file)
{
    chost::PluginEntry e = file;
    e.spec = o.value(QStringLiteral("spec")).toString().toStdString();
    e.name = o.value(QStringLiteral("name")).toString().toStdString();
    e.maker = o.value(QStringLiteral("maker")).toString().toStdString();
    e.category = o.value(QStringLiteral("category")).toString().toStdString();
    e.isSynth = o.value(QStringLiteral("synth")).toBool();
    e.audioIns = static_cast<uint32_t>(o.value(QStringLiteral("audioIns")).toInt());
    e.audioOuts = static_cast<uint32_t>(o.value(QStringLiteral("audioOuts")).toInt());
    e.midiIns = static_cast<uint32_t>(o.value(QStringLiteral("midiIns")).toInt());
    return e;
}

PluginDiscovery::PluginDiscovery(QObject* parent)
    : QObject(parent)
{
    QFile file(cachePath());
    if (file.open(QIODevice::ReadOnly))
        fCache = QJsonDocument::fromJson(file.readAll()).object();

    fTimeout = new QTimer(this);
    fTimeout->setSingleShot(true);
    // some plugins (big sample libraries, shells) take a while, a hung one is given up on
    fTimeout->setInterval(90000);
    connect(fTimeout, &QTimer::timeout, this, [this] { fProcess.kill(); });
    connect(&fProcess, &QProcess::finished, this, &PluginDiscovery::onFinished);
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onFinished();
    });
}

PluginDiscovery::~PluginDiscovery()
{
    fProcess.disconnect(this);
    if (fProcess.state() != QProcess::NotRunning)
    {
        fProcess.kill();
        fProcess.waitForFinished(3000);
    }
}

QString PluginDiscovery::keyOf(const chost::PluginEntry& file) const
{
    return QString::fromStdString(file.spec);
}

bool PluginDiscovery::known(const chost::PluginEntry& file) const
{
    const QJsonObject o = fCache.value(keyOf(file)).toObject();
    return !o.isEmpty() && o.value(QStringLiteral("stamp")).toString() == stampOf(file);
}

std::vector<chost::PluginEntry> PluginDiscovery::resolve(const std::vector<chost::PluginEntry>& files) const
{
    std::vector<chost::PluginEntry> out;
    for (const chost::PluginEntry& file : files)
    {
        const QJsonArray plugins = fCache.value(keyOf(file)).toObject().value(QStringLiteral("plugins")).toArray();
        if (!known(file) || plugins.isEmpty())
        {
            out.push_back(file); // not known yet, or discovery failed: keep the file name
            continue;
        }
        for (const QJsonValue& p : plugins)
            out.push_back(fromJson(p.toObject(), file));
    }
    std::sort(out.begin(), out.end(), [](const chost::PluginEntry& a, const chost::PluginEntry& b) { return a.name < b.name; });
    return out;
}

void PluginDiscovery::start(const std::vector<chost::PluginEntry>& files)
{
    if (isRunning() || chost::discoveryTool().empty())
        return;
    fQueue.clear();
    for (const chost::PluginEntry& file : files)
        if ((file.format == "VST2" || file.format == "VST3") && !known(file))
            fQueue.push_back(file);
    fTotal = static_cast<int>(fQueue.size());
    if (fQueue.empty())
        return;
    fCurrent = 0;
    fWin32 = false;
    next();
}

void PluginDiscovery::next()
{
    const chost::PluginEntry& file = fQueue[static_cast<size_t>(fCurrent)];
    emit progress(fCurrent, fTotal, QString::fromStdString(file.name));
    QStringList args;
    for (const std::string& a : chost::discoveryArguments(file))
        args << QString::fromStdString(a);
    fProcess.setProcessChannelMode(QProcess::MergedChannels);
    fProcess.start(QString::fromStdString(chost::discoveryTool(fWin32)), args);
    fTimeout->start();
}

void PluginDiscovery::onFinished()
{
    fTimeout->stop();
    if (fCurrent < 0)
        return;
    const chost::PluginEntry file = fQueue[static_cast<size_t>(fCurrent)];
    const std::string output = fProcess.readAll().toStdString();
    const std::vector<chost::PluginEntry> found = chost::parseDiscovery(output, file, fWin32);

    // nothing from the 64-bit tool: maybe an old 32-bit VST2, which carla can run through its bridge
    if (found.empty() && !fWin32 && file.format == "VST2" && !chost::discoveryTool(true).empty())
    {
        fWin32 = true;
        next();
        return;
    }

    QJsonArray plugins;
    for (const chost::PluginEntry& e : found)
        plugins.append(toJson(e));
    fCache.insert(keyOf(file), QJsonObject{{QStringLiteral("stamp"), stampOf(file)}, {QStringLiteral("plugins"), plugins}});
    save();

    fWin32 = false;
    if (++fCurrent < fTotal)
    {
        next();
        return;
    }
    fCurrent = -1;
    fQueue.clear();
    emit progress(fTotal, fTotal, QString());
    emit finished();
}

void PluginDiscovery::save() const
{
    QFile file(cachePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(fCache).toJson(QJsonDocument::Compact));
}
