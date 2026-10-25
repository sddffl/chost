// Audio to MIDI: runs tools/chost_transcribe.py on a list of tracks one after the other and reports every
// MIDI file it writes, which the main window turns into an instrument track below its source.

#pragma once

#include <QDialog>
#include <QProcess>

#include <vector>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class TranscribeDialog : public QDialog {
    Q_OBJECT

public:
    struct Source {
        QString name;   // track name
        QString file;   // its audio file
        QString role;   // stem id ("bass", "kick", ...) or empty for a whole mix
        bool checked = true;
    };

    TranscribeDialog(const std::vector<Source>& sources, double tempo, QWidget* parent = nullptr);
    ~TranscribeDialog() override;

    void setAutoStart(bool autoStart); // start right away (tests)

signals:
    void midiReady(const QString& sourceFile, const QString& role, const QString& trackName, const QString& midiPath);
    void transcriptionFinished(bool ok);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void start();
    void next();
    void cancel();
    void readOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void setBusy(bool busy);
    void log(const QString& line);
    QString midiPathFor(const Source& source) const;

    std::vector<Source> fSources;
    double fTempo;
    std::vector<int> fQueue; // indexes into fSources still to do
    int fCurrent = -1;
    int fDone = 0;
    int fTotal = 0;
    bool fFailed = false;
    QString fCurrentMidi;

    QLabel* fStatus = nullptr;
    QListWidget* fList = nullptr;
    QProgressBar* fProgress = nullptr;
    QPlainTextEdit* fLog = nullptr;
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
    QProcess fProcess;
    QByteArray fPending;
};
