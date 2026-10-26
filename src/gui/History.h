// Undo and redo for the whole project. Every change a user makes (tracks, files, faders, mute and solo,
// inserts, instruments, tempo and key, and everything the AI tools put in) is recorded afterwards as the
// session's state before and after; undo puts the session back into the earlier state. Removed tracks
// and plugins are only set aside by the session meanwhile, so they come back with every setting intact.

#pragma once

#include "core/Session.h"

#include <QElapsedTimer>
#include <QObject>
#include <QUndoStack>

class History : public QObject {
    Q_OBJECT

public:
    History(chost::Session& session, QObject* parent = nullptr);

    QUndoStack* stack() { return &fStack; }

    // a new or opened project: no history, nothing unsaved (modified: the project was changed and not
    // saved, e.g. reloaded for another audio driver)
    void reset(bool modified = false);
    // after a change: one step named text, if anything changed. Steps with the same mergeKey right after
    // one another become one (a fader dragged is one step, not a hundred).
    void record(const QString& text, const QString& mergeKey = {});
    // a change that is no step of its own (an A/B switch): it becomes part of where the history stands now
    void absorb();

    bool isModified() const { return fModified || !fStack.isClean(); }
    int revision() const { return fRevision; } // goes up with every change, undo and redo
    void setSaved();

    // while one is alive, record() collects instead of recording: the whole of an AI job is one step
    class Group {
    public:
        Group(History& history, const QString& text);
        ~Group();

    private:
        History& fHistory;
        QString fText;
    };

signals:
    void restored(); // the session was put back into another state: every view has to follow
    void modifiedChanged();

private:
    class Step;
    void apply(const chost::SessionState& state);
    void dropUnused();

    chost::Session& fSession;
    QUndoStack fStack;
    chost::SessionState fCurrent;
    int fGroups = 0;
    bool fModified = false;
    bool fApplying = false;
    int fRevision = 0;
};
