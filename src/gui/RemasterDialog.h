// AI > Autonomous Remastering: runs tools/chost_workflow_remaster.py to analyze audio,
// separate 14 stems using GPU, transcribe accompaniment to MIDI, tune vocals with smart pitch correction,
// multitrack mix sum and balance, master bus compression/EQ/limiting, and export to Desktop.

#pragma once

#include <QDialog>
#include <QProcess>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;

class RemasterDialog : public QDialog {
    Q_OBJECT

public:
    RemasterDialog(const QString& trackName, const QString& audioFile, QWidget* parent = nullptr);
    ~RemasterDialog() override;

    void setAutoStart(bool autoStart);
    QString masterPath() const { return fMasterPath; }
    QString audioFile() const { return fAudioFile; }

signals:
    void songAnalyzed(double bpm, const QString& key);
    void stemReady(const QString& id, const QString& label, const QString& path);
    void vocalTuned(const QString& role, const QString& path);
    void midiReady(const QString& role, const QString& path);
    void trackBalanced(const QString& role, float volumeDb, float pan);
    void masterReady(const QString& masterPath);
    void progressUpdated(int percent, const QString& message);
    void importRequested(const QString& masterPath, const QString& workDir);
    void remasterFinished(bool ok);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void startRemaster();
    void cancel();
    void readOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void setBusy(bool busy);
    void log(const QString& line);
    QString desktopFolder() const;

    QString fTrackName;
    QString fAudioFile;
    QString fMasterPath;
    QString fWorkDir;

    QLineEdit* fAudioInput = nullptr;
    QLineEdit* fExportDir = nullptr;
    QLabel* fStatus = nullptr;
    QProgressBar* fProgress = nullptr;
    QPlainTextEdit* fLog = nullptr;
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
    QPushButton* fOpenDesktop = nullptr;
    QPushButton* fImportStems = nullptr;

    QProcess fProcess;
    QByteArray fPending;
    bool fBusy = false;
    bool fAutoStart = false;
    QList<QPair<QString, QString>> fDiscoveredStems; // label, path
    QList<QPair<QString, QString>> fDiscoveredMidis; // role, path
};
