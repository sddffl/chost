// AI > Auto mix balance: renders every playing track on its own (after its inserts and fader), measures
// how loud it is, and sets the faders to a starting balance (core/MixBalance). Then the whole mix is
// rendered once more, and if it peaks too close to 0 dB every balanced fader comes down together.
// One undo step.

#pragma once

#include "RenderJob.h"
#include "core/MixBalance.h"
#include "core/Session.h"

#include <QDialog>

#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;

class BalanceDialog : public QDialog {
    Q_OBJECT

public:
    struct Track {
        chost::Uid uid = 0;
        int index = -1; // in the project when the balance starts, what chost-render's --solo takes
        QString name;
        QString role;
        float volume = 1.0f;
    };
    // the tracks to balance, as they are when it starts; set: the new faders, by uid, as one undo step
    using TrackFunction = std::function<std::vector<Track>()>;
    using ApplyFunction = std::function<void(const std::vector<std::pair<chost::Uid, float>>& volumes)>;
    // a track's part changed by hand, to keep with the track
    using RoleFunction = std::function<void(chost::Uid uid, const QString& role)>;
    BalanceDialog(TrackFunction tracks, SnapshotFunction snapshot, ApplyFunction apply, RoleFunction role, QWidget* parent = nullptr);

    void setAutoStart(bool autoStart);
    void setVocalStyle(chost::VocalStyle style);

signals:
    void balanced(bool ok);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void start();
    void next();
    void finish();
    void setBusy(bool busy);
    void roleChosen(size_t track, const QString& role);
    QString renderPath(int n) const;

    TrackFunction fTrackList;
    SnapshotFunction fSnapshot;
    ApplyFunction fApply;
    RoleFunction fRole;
    RenderJob fRender;
    std::vector<Track> fTracks;
    std::vector<chost::BalancePart> fParts;
    std::vector<float> fNew;
    size_t fCurrent = 0;
    bool fCheckingMix = false; // the last render: the whole mix after the balance
    QString fSession;

    QComboBox* fStyle = nullptr;
    QLabel* fStatus = nullptr;
    QProgressBar* fProgress = nullptr;
    QTreeWidget* fResults = nullptr;
    std::vector<QComboBox*> fRoles; // the "当作" column: which part each track is
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
};
