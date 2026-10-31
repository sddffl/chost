#pragma once

#include <QWidget>
#include <QStackedWidget>
#include <QTabBar>
#include <QPushButton>

class SampleEditor;
class PianoRoll;

class LowerZone : public QWidget {
    Q_OBJECT

public:
    enum TabIndex {
        TabMixConsole = 0,
        TabEditor = 1,
        TabPianoRoll = 2
    };

    explicit LowerZone(QWidget* mixConsoleWidget, QWidget* parent = nullptr);

    SampleEditor* sampleEditor() const { return fSampleEditor; }
    PianoRoll* pianoRoll() const { return fPianoRoll; }

    void setCurrentTab(TabIndex index);
    TabIndex currentTab() const;

signals:
    void tabChanged(int index);

private:
    QTabBar* fTabBar = nullptr;
    QStackedWidget* fStack = nullptr;
    SampleEditor* fSampleEditor = nullptr;
    PianoRoll* fPianoRoll = nullptr;
};
