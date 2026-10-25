// Finds out what every VST file really is (name, maker, instrument or effect) with carla's discovery tool,
// one file at a time in the background, and remembers the answers so the next start is instant.

#pragma once

#include "core/HostEngine.h"

#include <QJsonObject>
#include <QObject>
#include <QProcess>

class QTimer;

#include <vector>

class PluginDiscovery : public QObject {
    Q_OBJECT

public:
    explicit PluginDiscovery(QObject* parent = nullptr);
    ~PluginDiscovery() override;

    // The scanned files with every file already known replaced by what it holds.
    std::vector<chost::PluginEntry> resolve(const std::vector<chost::PluginEntry>& files) const;
    // Discovers the files not known yet; progress() for each, finished() at the end.
    void start(const std::vector<chost::PluginEntry>& files);
    bool isRunning() const { return fCurrent >= 0; }

signals:
    void progress(int done, int total, const QString& name);
    void finished();

private:
    void next();
    void onFinished();
    QString keyOf(const chost::PluginEntry& file) const;
    bool known(const chost::PluginEntry& file) const;
    void save() const;

    QJsonObject fCache; // file spec -> {stamp, plugins: [...]}
    std::vector<chost::PluginEntry> fQueue;
    int fCurrent = -1;
    int fTotal = 0;
    bool fWin32 = false; // second try with the 32-bit tool
    QProcess fProcess;
    QTimer* fTimeout = nullptr;
};
