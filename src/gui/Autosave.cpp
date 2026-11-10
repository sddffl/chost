#include "Autosave.h"

#include "History.h"
#include "core/Session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

static constexpr int kSlots = 8;
static constexpr int kKeepBackups = 10;
static const QString kStamp = QStringLiteral("yyyy-MM-dd_HHmmss");

// a project on the side, in its slot folder
static std::optional<Autosave::Found> readSlot(const QString& dir)
{
    const QString session = dir + QStringLiteral("/project.chost");
    if (!QFileInfo::exists(session))
        return std::nullopt;
    Autosave::Found found{session, {}, QFileInfo(session).lastModified()};
    QFile info(dir + QStringLiteral("/original.txt"));
    if (info.open(QIODevice::ReadOnly))
        found.original = QString::fromUtf8(info.readAll()).trimmed();
    return found;
}

Autosave::Autosave(chost::Session& session, History& history, std::function<QString()> projectPath, QObject* parent)
    : QObject(parent)
    , fSession(session)
    , fHistory(history)
    , fProjectPath(std::move(projectPath))
    , fFolder(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/autosave"))
{
    bool ok = false;
    const int seconds = qEnvironmentVariableIntValue("CHOST_AUTOSAVE_SECONDS", &ok); // tests
    fTimer.setInterval((ok && seconds > 0 ? seconds : 120) * 1000);
    connect(&fTimer, &QTimer::timeout, this, &Autosave::tick);
}

Autosave::~Autosave()
{
    discard();
    fLocks.clear();
}

QString Autosave::slotDir(int slot) const
{
    return fFolder + QStringLiteral("/slot-%1").arg(slot);
}

std::optional<Autosave::Found> Autosave::claim()
{
    QDir().mkpath(fFolder);
    std::optional<Found> newest;
    fLocks.resize(kSlots);
    for (int slot = 0; slot < kSlots; ++slot)
    {
        auto lock = std::make_unique<QLockFile>(fFolder + QStringLiteral("/slot-%1.lock").arg(slot));
        lock->setStaleLockTime(0); // only a chost that is gone leaves its slot, however long the other one runs
        if (!lock->tryLock(0))
            continue; // a chost running now
        // a crash in the middle of saving leaves the new copy next to the slot rather than in it
        const QString dir = slotDir(slot);
        if (!QFileInfo::exists(dir) && QFileInfo::exists(dir + QStringLiteral(".new")))
            QDir().rename(dir + QStringLiteral(".new"), dir);
        if (auto found = readSlot(dir); found && (!newest || found->time > newest->time))
        {
            newest = found;
            fFoundSlot = slot;
        }
        fLocks[slot] = std::move(lock);
    }
    return newest;
}

void Autosave::settle(Outcome outcome)
{
    const bool recovered = outcome == Outcome::Recovered;
    if (fFoundSlot >= 0 && outcome != Outcome::Left)
    {
        fSlot = fFoundSlot;
        if (!recovered)
            discard();
    }
    else
    {
        // a free slot, empty if there is one: another crashed chost's project stays for the next start
        for (int slot = 0; slot < kSlots && fSlot < 0; ++slot)
            if (fLocks[slot] && !readSlot(slotDir(slot)))
                fSlot = slot;
        for (int slot = 0; slot < kSlots && fSlot < 0; ++slot)
            if (fLocks[slot])
                fSlot = slot;
    }
    for (int slot = 0; slot < kSlots; ++slot)
        if (slot != fSlot)
            fLocks[slot].reset();
    fSavedRevision = recovered ? fHistory.revision() : -1;
    if (fSlot >= 0)
        fTimer.start();
}

void Autosave::tick()
{
    if (!fHistory.isModified())
    {
        // saved, or every change undone: what is on the side is older than the project
        discard();
        return;
    }
    if (fHistory.revision() != fSavedRevision)
        saveNow();
}

bool Autosave::saveNow()
{
    if (fSlot < 0 || !fSession.isReady() || fSession.tracks().empty())
        return false;
    const QString dir = slotDir(fSlot);
    const QString next = dir + QStringLiteral(".new");
    QDir(next).removeRecursively();
    QDir().mkpath(next);
    if (!fSession.save((next + QStringLiteral("/project.chost")).toStdString()))
        return false;
    QFile info(next + QStringLiteral("/original.txt"));
    if (info.open(QIODevice::WriteOnly))
        info.write(fProjectPath().toUtf8());
    info.close();
    QDir(dir).removeRecursively();
    if (!QDir().rename(next, dir))
        return false;
    fSavedRevision = fHistory.revision();
    return true;
}

void Autosave::saved()
{
    discard();
    fSavedRevision = fHistory.revision();
}

void Autosave::discard()
{
    if (fSlot < 0)
        return;
    QDir(slotDir(fSlot)).removeRecursively();
    QDir(slotDir(fSlot) + QStringLiteral(".new")).removeRecursively();
}

// ---------------------------------------------------------------------------------------------------------------------
// the versions a project had before it was saved over

QString Autosave::backupFolder(const QString& projectFile)
{
    const QFileInfo info(projectFile);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral("_备份");
}

void Autosave::backUp(const QString& projectFile)
{
    const QFileInfo info(projectFile);
    if (!info.exists())
        return;
    const QString folder = backupFolder(projectFile);
    const QString carla = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".carxp");
    // saved again without a change since: the newest copy is this already
    auto same = [](const QString& a, const QString& b) {
        QFile fa(a), fb(b);
        if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly))
            return !QFileInfo::exists(a) && !QFileInfo::exists(b);
        return fa.readAll() == fb.readAll();
    };
    if (const std::vector<Found> kept = backups(projectFile); !kept.empty())
    {
        const QString newest = QFileInfo(kept.front().session).absolutePath() + QLatin1Char('/');
        if (same(projectFile, newest + info.fileName()) && same(carla, newest + QFileInfo(carla).fileName()))
            return;
    }
    const QString dir = folder + QLatin1Char('/') + QDateTime::currentDateTime().toString(kStamp);
    QDir().mkpath(dir);
    // the project is two files: chost's own and carla's with the plugins in it
    QFile::remove(dir + QLatin1Char('/') + info.fileName());
    QFile::copy(projectFile, dir + QLatin1Char('/') + info.fileName());
    if (QFileInfo::exists(carla))
    {
        QFile::remove(dir + QLatin1Char('/') + QFileInfo(carla).fileName());
        QFile::copy(carla, dir + QLatin1Char('/') + QFileInfo(carla).fileName());
    }

    // only the folders made here, by their names
    static const QRegularExpression stamp(QStringLiteral("^\\d{4}-\\d\\d-\\d\\d_\\d{6}$"));
    QStringList kept = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    kept.erase(std::remove_if(kept.begin(), kept.end(), [](const QString& name) { return !stamp.match(name).hasMatch(); }), kept.end());
    for (int i = kKeepBackups; i < kept.size(); ++i)
        QDir(folder + QLatin1Char('/') + kept[i]).removeRecursively();
}

std::vector<Autosave::Found> Autosave::backups(const QString& projectFile)
{
    std::vector<Found> found;
    if (projectFile.isEmpty())
        return found;
    const QFileInfo info(projectFile);
    const QString folder = backupFolder(projectFile);
    for (const QString& name : QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
    {
        const QString session = folder + QLatin1Char('/') + name + QLatin1Char('/') + info.fileName();
        const QDateTime time = QDateTime::fromString(name, kStamp);
        if (time.isValid() && QFileInfo::exists(session))
            found.push_back({session, projectFile, time});
    }
    return found;
}
