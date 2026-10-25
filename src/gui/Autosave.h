// Nothing is lost when chost or the computer goes down: every couple of minutes a project with unsaved
// changes is saved on the side (in chost's own folder, never over the user's file). A chost that closes
// normally removes it again; the next chost after a crash finds it and offers to bring the project back.
// Several chosts at once each have their own slot, held by a lock file while that chost runs.
//
// Saving a project also keeps the version it overwrites, in <project>_备份/<time>/ next to it.

#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class History;
class QLockFile;
namespace chost {
class Session;
}

class Autosave : public QObject {
    Q_OBJECT

public:
    struct Found {
        QString session;  // the project as it was saved on the side
        QString original; // the project file it belongs to, empty if it was never saved
        QDateTime time;
    };

    // projectPath: the project file as it is now (empty while it has none)
    Autosave(chost::Session& session, History& history, std::function<QString()> projectPath, QObject* parent = nullptr);
    ~Autosave() override; // a normal end: what was saved on the side goes

    // takes a slot; what a chost that crashed left behind, the newest if there are several
    std::optional<Found> claim();
    enum class Outcome {
        Recovered, // the project was opened from it: it stays until the next save on the side replaces it
        Discarded, // the user did not want it back
        Left,      // nobody was asked (a test run): it stays for the next chost
    };
    // after claim(), with what became of what it found; then saving on the side starts
    void settle(Outcome outcome);

    void saved(); // the project was saved by the user: nothing on the side is needed any more
    bool saveNow(); // (tests) at once rather than on the timer

    // a project about to be overwritten: a copy in <project>_备份/<time>/, the 10 newest kept
    static void backUp(const QString& projectFile);
    // the copies kept of a project, newest first
    static std::vector<Found> backups(const QString& projectFile);
    static QString backupFolder(const QString& projectFile);

private:
    QString slotDir(int slot) const;
    void tick();
    void discard();

    chost::Session& fSession;
    History& fHistory;
    std::function<QString()> fProjectPath;
    QString fFolder;
    QTimer fTimer;
    std::vector<std::unique_ptr<QLockFile>> fLocks; // index = slot, null where another chost has it
    int fSlot = -1;
    int fFoundSlot = -1;
    int fSavedRevision = -1;
};
