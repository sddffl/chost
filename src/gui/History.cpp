#include "History.h"

#include <QUndoCommand>

static constexpr int kMergeId = 1;
static constexpr qint64 kMergeMs = 2000;

class History::Step : public QUndoCommand {
public:
    Step(History& history, const QString& text, const QString& mergeKey, chost::SessionState before, chost::SessionState after)
        : QUndoCommand(text)
        , fHistory(history)
        , fMergeKey(mergeKey)
        , fBefore(std::move(before))
        , fAfter(std::move(after))
    {
        fTime.start();
    }

    // the change is already made when the step is recorded: the first redo has nothing to do
    void redo() override
    {
        if (fDone)
            fHistory.apply(fAfter);
        fDone = true;
    }
    void undo() override { fHistory.apply(fBefore); }
    int id() const override { return fMergeKey.isEmpty() ? -1 : kMergeId; }
    bool mergeWith(const QUndoCommand* other) override
    {
        const auto* next = static_cast<const Step*>(other);
        if (next->fMergeKey != fMergeKey || fTime.elapsed() > kMergeMs)
            return false;
        fAfter = next->fAfter;
        fTime.restart();
        return true;
    }

    chost::SessionState fBefore, fAfter;

private:
    History& fHistory;
    QString fMergeKey;
    QElapsedTimer fTime;
    bool fDone = false;
};

History::History(chost::Session& session, QObject* parent)
    : QObject(parent)
    , fSession(session)
{
    fStack.setUndoLimit(100);
    fSession.setKeepRemoved(true);
    connect(&fStack, &QUndoStack::cleanChanged, this, &History::modifiedChanged);
}

void History::reset(bool modified)
{
    fStack.clear();
    fModified = modified;
    fCurrent = fSession.state();
    fSession.dropUnused({&fCurrent});
    emit modifiedChanged();
}

void History::setSaved()
{
    fModified = false;
    fStack.setClean();
    emit modifiedChanged();
}

void History::record(const QString& text, const QString& mergeKey)
{
    if (fApplying || fGroups > 0)
        return;
    chost::SessionState now = fSession.state();
    if (now == fCurrent)
        return;
    chost::SessionState before = fCurrent;
    fCurrent = now;
    fStack.push(new Step(*this, text, mergeKey, std::move(before), std::move(now)));
    dropUnused();
}

void History::absorb()
{
    if (fApplying || fGroups > 0)
        return;
    fCurrent = fSession.state();
    // where the history stands: after the step just done, before the one that would be redone
    const int index = fStack.index();
    if (index > 0)
        static_cast<Step*>(const_cast<QUndoCommand*>(fStack.command(index - 1)))->fAfter = fCurrent;
    if (index < fStack.count())
        static_cast<Step*>(const_cast<QUndoCommand*>(fStack.command(index)))->fBefore = fCurrent;
    dropUnused();
}

void History::apply(const chost::SessionState& state)
{
    fApplying = true;
    fSession.restore(state);
    fCurrent = fSession.state();
    fApplying = false;
    emit restored();
}

void History::dropUnused()
{
    // what no step can go back to any more (beyond the undo limit, or undone and then overwritten)
    std::vector<const chost::SessionState*> keep{&fCurrent};
    for (int i = 0; i < fStack.count(); ++i)
    {
        const auto* step = static_cast<const Step*>(fStack.command(i));
        keep.push_back(&step->fBefore);
        keep.push_back(&step->fAfter);
    }
    fSession.dropUnused(keep);
}

History::Group::Group(History& history, const QString& text)
    : fHistory(history)
    , fText(text)
{
    ++fHistory.fGroups;
}

History::Group::~Group()
{
    if (--fHistory.fGroups == 0)
        fHistory.record(fText);
}
