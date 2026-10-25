// File > Export mixdown: renders the project, as it sounds, to a WAV file faster than real time with
// chost-render, and optionally every track on its own (stems for another studio or a mix engineer).

#pragma once

#include "RenderJob.h"

#include <QDialog>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

class ExportDialog : public QDialog {
    Q_OBJECT

public:
    struct Track {
        int index;
        QString name;
    };
    ExportDialog(const QString& defaultPath, const std::vector<Track>& tracks, SnapshotFunction snapshot, QWidget* parent = nullptr);

    void setAutoStart(bool autoStart); // start right away (tests)

signals:
    void exported(bool ok, const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Job {
        QString output;
        QString name;
        QList<int> solo;
    };

    void browse();
    void start();
    void next();
    void setBusy(bool busy);
    int bits() const;

    std::vector<Track> fTracks;
    SnapshotFunction fSnapshot;
    QString fSession;
    std::vector<Job> fJobs;
    size_t fCurrent = 0;
    QStringList fReport;
    bool fFailed = false;
    RenderJob fRender;

    QLineEdit* fPath = nullptr;
    QComboBox* fFormat = nullptr;
    QCheckBox* fStems = nullptr;
    QLabel* fStatus = nullptr;
    QProgressBar* fProgress = nullptr;
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
    QPushButton* fOpenFolder = nullptr;
};
