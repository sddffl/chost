// AI stem separation: runs tools/chost_separate.py (audio-separator) on one track's audio file and
// reports every finished stem, which the main window turns into a track.

#pragma once

#include <QDialog>
#include <QProcess>

#include <vector>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class StemDialog : public QDialog {
    Q_OBJECT

public:
    StemDialog(const QString& trackName, const QString& audioFile, QWidget* parent = nullptr);
    ~StemDialog() override;

    static QString scriptPath();
    void setAutoStart(bool autoStart); // start as soon as the AI components check out (tests)

signals:
    void stemReady(const QString& stemId, const QString& label, const QString& path);
    void separationFinished(bool ok);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Job { None, Check, Install, Separate };

    void detectPython();
    void checkPython();
    void install();
    void startSeparation();
    void cancel();
    void run(Job job, const QString& program, const QStringList& args);
    void readOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void setBusy(bool busy);
    void log(const QString& line);
    QString outputFolder() const;
    QString envFolder() const;
    QString envPython() const;

    QString fAudioFile;
    QLineEdit* fPython = nullptr;
    QLabel* fStatus = nullptr;
    QPushButton* fInstall = nullptr;
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
    QButtonGroup* fLevel = nullptr;
    QProgressBar* fProgress = nullptr;
    QPlainTextEdit* fLog = nullptr;

    QProcess fProcess;
    Job fJob = Job::None;
    std::vector<QStringList> fInstallSteps; // remaining commands of the install, program first
    QByteArray fPending;
    bool fReady = false;
    bool fAutoStart = false;
    int fStems = 0;
};
