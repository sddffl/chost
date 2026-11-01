// AI > AI mastering: renders the mix with chost-render, then tools/chost_master.py gives it a master's
// tone, glue and loudness, optionally matched to a reference song. The result can go on a new track for
// an A/B comparison with the mix.

#pragma once

#include "RenderJob.h"
#include "ScriptJob.h"

#include <QDialog>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

// The tone change the mastering made, over a 20 Hz - 20 kHz log scale.
class EqCurveView : public QWidget {
public:
    explicit EqCurveView(QWidget* parent = nullptr);
    void setCurve(const std::vector<std::pair<double, double>>& points); // (Hz, dB)

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<std::pair<double, double>> fPoints;
};

class MasterDialog : public QDialog {
    Q_OBJECT

public:
    MasterDialog(const QString& defaultOutput, SnapshotFunction snapshot, QWidget* parent = nullptr);

    void setAutoStart(bool autoStart); // start right away (tests)
    void setListening(bool master);    // the A/B switch, as the tracks are now

signals:
    void aboutToStart(); // the old master's track should let go of its file, it's about to be replaced
    void mastered(bool ok, const QString& path);
    void compareRequested(const QString& path); // put the master on a new track
    void listen(bool master);                   // A/B: the master track alone, or the mix without it
    void playToggled();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void start();
    void runMaster();
    void finish(bool ok, const QString& message);
    void setBusy(bool busy);
    void showResult(const QString& key, const QString& value);

    SnapshotFunction fSnapshot;
    RenderJob fRender;
    ScriptJob fScript;
    QString fMixFile;
    QString fOutput;
    double fBefore[2] = {0.0, 0.0};
    bool fBusy = false;
    bool fCompared = false;

    QLineEdit* fReference = nullptr;
    QComboBox* fStyle = nullptr;
    QComboBox* fTarget = nullptr;
    QCheckBox* fMonoBass = nullptr;
    QLineEdit* fPath = nullptr;
    QLabel* fStatus = nullptr;
    QProgressBar* fProgress = nullptr;
    QLabel* fResult = nullptr;
    EqCurveView* fCurve = nullptr;
    QPushButton* fStart = nullptr;
    QPushButton* fCancel = nullptr;
    QPushButton* fCompare = nullptr;
    QPushButton* fAB = nullptr;
    QPushButton* fPlay = nullptr;
    QPushButton* fOpenFolder = nullptr;
};
