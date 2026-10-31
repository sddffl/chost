#include "LowerZone.h"

#include "SampleEditor.h"
#include "PianoRoll.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>

LowerZone::LowerZone(QWidget* mixConsoleWidget, QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    fStack = new QStackedWidget(this);

    // Page 0: MixConsole
    if (mixConsoleWidget)
        fStack->addWidget(mixConsoleWidget);
    else
        fStack->addWidget(new QWidget);

    // Page 1: Sample Editor (Cubase-style thick waveform track)
    fSampleEditor = new SampleEditor(this);
    fStack->addWidget(fSampleEditor);

    // Page 2: Piano Roll (Embedded MIDI editor)
    fPianoRoll = new PianoRoll(this);
    fStack->addWidget(fPianoRoll);

    rootLayout->addWidget(fStack, 1);

    // Bottom tab bar bar styled like Cubase Lower Zone
    auto* bottomBar = new QWidget(this);
    bottomBar->setFixedHeight(26);
    bottomBar->setStyleSheet(QStringLiteral(
        "QWidget { background-color: #1a1d22; border-top: 1px solid #141619; }"
        "QTabBar::tab { background: #23272d; color: #8b9096; padding: 4px 14px; font-size: 11px; font-weight: bold; border-right: 1px solid #141619; }"
        "QTabBar::tab:hover { background: #2b3038; color: #d6d9dc; }"
        "QTabBar::tab:selected { background: #323842; color: #ffffff; border-bottom: 2px solid #4a90d9; }"
    ));

    auto* barLayout = new QHBoxLayout(bottomBar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(0);

    fTabBar = new QTabBar(bottomBar);
    fTabBar->setDrawBase(false);
    fTabBar->addTab(QStringLiteral("MixConsole"));
    fTabBar->addTab(QStringLiteral("编辑器"));
    fTabBar->addTab(QStringLiteral("采样器控制"));
    fTabBar->addTab(QStringLiteral("调谐音频"));
    fTabBar->addTab(QStringLiteral("键盘"));

    connect(fTabBar, &QTabBar::currentChanged, this, [this](int idx) {
        int stackIdx = 0;
        if (idx == 0)
            stackIdx = TabMixConsole;
        else if (idx == 1)
            stackIdx = TabEditor;
        else if (idx >= 2)
            stackIdx = TabPianoRoll;

        fStack->setCurrentIndex(stackIdx);
        emit tabChanged(idx);
    });

    barLayout->addWidget(fTabBar);
    barLayout->addStretch();

    rootLayout->addWidget(bottomBar);
}

void LowerZone::setCurrentTab(TabIndex index)
{
    if (index == TabMixConsole)
    {
        fTabBar->setCurrentIndex(0);
        fStack->setCurrentIndex(TabMixConsole);
    }
    else if (index == TabEditor)
    {
        fTabBar->setCurrentIndex(1);
        fStack->setCurrentIndex(TabEditor);
    }
    else if (index == TabPianoRoll)
    {
        fTabBar->setCurrentIndex(4);
        fStack->setCurrentIndex(TabPianoRoll);
    }
}

LowerZone::TabIndex LowerZone::currentTab() const
{
    return static_cast<TabIndex>(fStack->currentIndex());
}
