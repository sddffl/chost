#include "MainWindow.h"

#include "PluginWindows.h"
#include "Theme.h"
#include "Widgets.h"
#include "core/AudioConvert.h"

#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using chost::HostEvent;

static const QString kTrackName = QStringLiteral("Audio 01");
static constexpr double kTempo = 120.0;

static QLabel* sectionTitle(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("chost"));
    resize(1400, 900);
    setAcceptDrops(true);

    buildMenus();
    buildTransport();

    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(buildInspector());
    fArrange = new ArrangeView;
    fArrange->setTempo(kTempo);
    top->addWidget(fArrange);
    top->addWidget(buildBrowser());
    top->setStretchFactor(1, 1);
    top->setSizes({220, 860, 320});

    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(top);
    main->addWidget(buildMixConsole());
    main->setStretchFactor(0, 1);
    main->setSizes({400, 470});
    setCentralWidget(main);

    connect(fArrange, &ArrangeView::seekRequested, this, &MainWindow::seek);
    connect(fArrange, &ArrangeView::muteToggled, this, &MainWindow::setTrackMuted);
    connect(fArrange, &ArrangeView::fileDropped, this, &MainWindow::loadAudioFile);

    auto* space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, &MainWindow::togglePlay);

    fHost.setEventCallback([this](HostEvent event, uint pluginId, int index, float value, const std::string& text) {
        // carla may call this from its own threads, widgets are only touched on the GUI thread
        const QString qtext = QString::fromStdString(text);
        QMetaObject::invokeMethod(this, [=] { onHostEvent(event, pluginId, index, value, qtext); }, Qt::QueuedConnection);
    });

    fTimer = new QTimer(this);
    connect(fTimer, &QTimer::timeout, this, &MainWindow::tick);
    fTimer->start(30);

    startEngine(QString::fromStdString(chost::defaultAudioDriver()));
    rescanPlugins();
}

MainWindow::~MainWindow()
{
    fTimer->stop();
    closeEditors();
    fHost.setEventCallback(nullptr);
    fHost.stop();
}

// ---------------------------------------------------------------------------------------------------------------------
// layout

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(QStringLiteral("文件"));
    file->addAction(QStringLiteral("打开音频文件…"), QKeySequence(QStringLiteral("Ctrl+I")), this, &MainWindow::openAudioFile);
    file->addSeparator();
    file->addAction(QStringLiteral("打开工程…"), QKeySequence::Open, this, &MainWindow::openProject);
    file->addAction(QStringLiteral("保存工程…"), QKeySequence::Save, this, &MainWindow::saveProject);
    file->addSeparator();
    file->addAction(QStringLiteral("退出"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* transport = menuBar()->addMenu(QStringLiteral("走带"));
    transport->addAction(QStringLiteral("播放 / 暂停（空格）"), this, &MainWindow::togglePlay);
    transport->addAction(QStringLiteral("停止"), this, &MainWindow::stop);
    transport->addAction(QStringLiteral("回到开头"), this, [this] { seek(0.0); });

    QMenu* plugins = menuBar()->addMenu(QStringLiteral("插件"));
    plugins->addAction(QStringLiteral("添加插入效果…"), this, &MainWindow::addInsert);
    plugins->addAction(QStringLiteral("重新扫描插件"), this, &MainWindow::rescanPlugins);
}

void MainWindow::buildTransport()
{
    auto* bar = addToolBar(QStringLiteral("走带控制"));
    bar->setMovable(false);

    auto transportButton = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("transport"));
        b->setText(text);
        b->setToolTip(tip);
        bar->addWidget(b);
        return b;
    };
    connect(transportButton(QStringLiteral("⏮"), QStringLiteral("回到开头")), &QToolButton::clicked, this, [this] { seek(0.0); });
    connect(transportButton(QStringLiteral("⏹"), QStringLiteral("停止")), &QToolButton::clicked, this, &MainWindow::stop);
    fPlayButton = transportButton(QStringLiteral("▶"), QStringLiteral("播放（空格）"));
    fPlayButton->setCheckable(true);
    connect(fPlayButton, &QToolButton::clicked, this, &MainWindow::togglePlay);

    bar->addSeparator();
    fTimeLcd = new QLabel(QStringLiteral("00:00:00.000"));
    fTimeLcd->setObjectName(QStringLiteral("lcdTime"));
    fTimeLcd->setToolTip(QStringLiteral("时间（时:分:秒.毫秒）"));
    bar->addWidget(fTimeLcd);
    fBarsLcd = new QLabel;
    fBarsLcd->setObjectName(QStringLiteral("lcdSmall"));
    fBarsLcd->setToolTip(QStringLiteral("小节.拍.十六分音符"));
    bar->addWidget(fBarsLcd);
    auto* tempo = new QLabel(QStringLiteral("%1 BPM · 4/4").arg(kTempo, 0, 'f', 1));
    tempo->setObjectName(QStringLiteral("lcdSmall"));
    bar->addWidget(tempo);

    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    bar->addWidget(new QLabel(QStringLiteral("音频驱动 ")));
    fDriverBox = new QComboBox;
    for (const std::string& name : chost::audioDrivers())
        fDriverBox->addItem(QString::fromStdString(name));
    bar->addWidget(fDriverBox);
    connect(fDriverBox, &QComboBox::textActivated, this, &MainWindow::startEngine);

    fDspLabel = new QLabel;
    fDspLabel->setObjectName(QStringLiteral("lcdSmall"));
    fDspLabel->setMinimumWidth(110);
    bar->addWidget(fDspLabel);
}

QWidget* MainWindow::buildInspector()
{
    auto* pane = new QWidget;
    pane->setMinimumWidth(200);
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(sectionTitle(QStringLiteral("检查器")));

    auto* body = new QVBoxLayout;
    body->setContentsMargins(8, 4, 8, 8);
    fInspectorTrack = new QLabel;
    fInspectorTrack->setObjectName(QStringLiteral("channelName"));
    body->addWidget(fInspectorTrack);
    fInspectorInfo = new QLabel;
    fInspectorInfo->setWordWrap(true);
    fInspectorInfo->setStyleSheet(QStringLiteral("color: #8b9096;"));
    body->addWidget(fInspectorInfo);

    auto* open = new QPushButton(QStringLiteral("打开音频文件…"));
    body->addWidget(open);
    connect(open, &QPushButton::clicked, this, &MainWindow::openAudioFile);

    body->addSpacing(6);
    auto* insertsTitle = new QLabel(QStringLiteral("插入效果"));
    insertsTitle->setStyleSheet(QStringLiteral("color: #8b9096;"));
    body->addWidget(insertsTitle);
    fInspectorInserts = new InsertRack(8);
    body->addWidget(fInspectorInserts);
    connectInsertRack(fInspectorInserts);
    body->addStretch();

    layout->addLayout(body);
    return pane;
}

QWidget* MainWindow::buildBrowser()
{
    auto* pane = new QWidget;
    pane->setMinimumWidth(240);
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(sectionTitle(QStringLiteral("插件（双击加到插入效果）")));

    auto* body = new QVBoxLayout;
    body->setContentsMargins(6, 2, 6, 6);
    fSearch = new QLineEdit;
    fSearch->setPlaceholderText(QStringLiteral("搜索名称、厂商或格式"));
    fSearch->setClearButtonEnabled(true);
    body->addWidget(fSearch);

    fBrowser = new QTreeWidget;
    fBrowser->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("格式"), QStringLiteral("厂商")});
    fBrowser->setRootIsDecorated(false);
    fBrowser->setAlternatingRowColors(true);
    fBrowser->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    fBrowser->sortByColumn(0, Qt::AscendingOrder);
    body->addWidget(fBrowser);

    auto* rescan = new QPushButton(QStringLiteral("重新扫描插件"));
    body->addWidget(rescan);
    layout->addLayout(body);

    connect(fSearch, &QLineEdit::textChanged, this, [this](const QString& text) { filterPluginTree(fBrowser, text); });
    connect(fBrowser, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        addPluginSpec(item->data(0, Qt::UserRole).toString());
    });
    connect(rescan, &QPushButton::clicked, this, &MainWindow::rescanPlugins);
    return pane;
}

QWidget* MainWindow::buildMixConsole()
{
    auto* pane = new QWidget;
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(sectionTitle(QStringLiteral("调音台")));

    auto* strips = new QWidget;
    auto* row = new QHBoxLayout(strips);
    row->setContentsMargins(8, 8, 8, 8);
    row->setSpacing(6);

    fTrackStrip = new ChannelStrip(kTrackName, false);
    row->addWidget(fTrackStrip);
    connectInsertRack(fTrackStrip->inserts());
    connect(fTrackStrip, &ChannelStrip::volumeChanged, this, &MainWindow::setTrackVolume);
    connect(fTrackStrip, &ChannelStrip::muteToggled, this, &MainWindow::setTrackMuted);

    row->addStretch();
    fOutputStrip = new ChannelStrip(QStringLiteral("Stereo Out"), true);
    row->addWidget(fOutputStrip);

    auto* scroll = new QScrollArea;
    scroll->setWidget(strips);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll);
    return pane;
}

void MainWindow::connectInsertRack(InsertRack* rack)
{
    connect(rack, &InsertRack::addRequested, this, &MainWindow::addInsert);
    connect(rack, &InsertRack::editRequested, this, &MainWindow::editInsert);
    connect(rack, &InsertRack::activeToggled, this, &MainWindow::setInsertActive);
    connect(rack, &InsertRack::removeRequested, this, &MainWindow::removeInsert);
    connect(rack, &InsertRack::moveRequested, this, &MainWindow::moveInsert);
}

// ---------------------------------------------------------------------------------------------------------------------
// engine

void MainWindow::startEngine(const QString& driver)
{
    closeEditors();
    if (fHost.start(driver.toStdString()))
    {
        statusBar()->showMessage(QStringLiteral("音频引擎已启动：%1").arg(driver), 5000);
    }
    else
    {
        const QString error = QString::fromStdString(fHost.lastError());
        // keep the window usable even without a sound card
        if (driver != QLatin1String("Dummy") && fHost.start("Dummy"))
            showError(QStringLiteral("无法用 %1 启动音频引擎，已切换到 Dummy（静音）驱动。\n\n%2").arg(driver, error));
        else
            showError(QStringLiteral("无法启动音频引擎：%1").arg(error));
    }

    const QSignalBlocker blocker(fDriverBox);
    fDriverBox->setCurrentText(QString::fromStdString(fHost.driverName()));
    fTrackFile.clear();
    refreshTrack();
}

void MainWindow::onHostEvent(HostEvent event, uint pluginId, int index, float value, const QString& text)
{
    switch (event)
    {
    case HostEvent::PluginAdded:
    case HostEvent::PluginRemoved:
    case HostEvent::PluginRenamed:
    case HostEvent::Reload:
        refreshInserts();
        break;
    case HostEvent::ParameterChanged:
        if (auto it = fEditors.find(pluginId); it != fEditors.end() && it->second && index >= 0)
            it->second->parameterChanged(static_cast<uint32_t>(index), value);
        break;
    case HostEvent::UiClosed:
        break;
    case HostEvent::Error:
        showError(text);
        break;
    case HostEvent::EngineStopped:
        statusBar()->showMessage(QStringLiteral("音频引擎已停止"));
        refreshTrack();
        break;
    }
}

void MainWindow::tick()
{
    fHost.idle();

    // the end of the chain is both the track's post-insert signal and the output
    const uint count = fHost.pluginCount();
    const auto [l, r] = count > 0 ? fHost.outputPeaks(count - 1) : std::pair<float, float>{0.0f, 0.0f};
    fTrackStrip->setLevels(l, r);
    fOutputStrip->setLevels(l, r);

    const double rate = fHost.sampleRate();
    const double seconds = rate > 0.0 ? static_cast<double>(fHost.transportFrame()) / rate : 0.0;
    const bool playing = fHost.isPlaying();
    fArrange->setPlayhead(seconds, playing);

    const int ms = static_cast<int>(seconds * 1000.0);
    fTimeLcd->setText(QStringLiteral("%1:%2:%3.%4")
                          .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
                          .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
                          .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
                          .arg(ms % 1000, 3, 10, QLatin1Char('0')));
    const double beats = seconds * kTempo / 60.0;
    const int sixteenths = static_cast<int>(beats * 4.0);
    fBarsLcd->setText(QStringLiteral("%1. %2. %3").arg(sixteenths / 16 + 1, 3).arg(sixteenths / 4 % 4 + 1).arg(sixteenths % 4 + 1));

    {
        const QSignalBlocker blocker(fPlayButton);
        fPlayButton->setChecked(playing);
    }

    for (auto& [id, editor] : fEditors)
        if (editor)
            editor->refreshOutputs();

    // the dummy driver has no real deadline, its load figure is meaningless
    if (!fHost.isRunning())
        fDspLabel->setText(QStringLiteral("引擎未运行"));
    else if (fHost.driverName() == "Dummy")
        fDspLabel->setText(QStringLiteral("DSP —"));
    else
        fDspLabel->setText(QStringLiteral("DSP %1%").arg(fHost.dspLoad(), 0, 'f', 0));
    if (fHost.isRunning())
        statusBar()->showMessage(QStringLiteral("%1 · %2 Hz · 缓冲 %3")
                                     .arg(QString::fromStdString(fHost.driverName()))
                                     .arg(rate, 0, 'f', 0)
                                     .arg(fHost.bufferSize()));
}

// ---------------------------------------------------------------------------------------------------------------------
// track model

int MainWindow::trackSlot() const
{
    for (uint id = 0; id < fHost.pluginCount(); ++id)
        if (fHost.slot(id).label == "audiofile")
            return static_cast<int>(id);
    return -1;
}

std::vector<uint> MainWindow::insertIds() const
{
    std::vector<uint> ids;
    const int track = trackSlot();
    for (uint id = 0; id < fHost.pluginCount(); ++id)
        if (static_cast<int>(id) != track)
            ids.push_back(id);
    return ids;
}

void MainWindow::refreshTrack()
{
    const int track = trackSlot();
    fInspectorTrack->setText(kTrackName);

    if (track < 0)
    {
        fArrange->clearTrack();
        fInspectorInfo->setText(QStringLiteral("还没有音频，打开一个音频文件开始。"));
        fTrackStrip->setFaderEnabled(false);
        refreshInserts();
        return;
    }

    // after loading a project only carla knows which file the player has
    if (fTrackFile.isEmpty())
        fTrackFile = QString::fromStdString(fHost.audioFilePath(static_cast<uint>(track)));

    std::vector<float> peaks;
    chost::AudioFileInfo info;
    const bool decoded = chost::computePeaks(fTrackFile.toStdString(), 40000, peaks, info);
    if (decoded && info.sampleRate > 0)
    {
        fTrackSeconds = static_cast<double>(info.frames) / info.sampleRate;
    }
    else
    {
        // a format chost can't decode (e.g. OGG): carla still plays it, ask the player for the length
        const int length = fHost.findParameter(static_cast<uint>(track), "Length");
        fTrackSeconds = length >= 0 ? fHost.parameterValue(static_cast<uint>(track), static_cast<uint32_t>(length)) : 0.0;
    }

    const QFileInfo file(fTrackFile);
    fArrange->setTrack(kTrackName, file.completeBaseName(), peaks, fTrackSeconds);
    fInspectorInfo->setText(decoded
                                ? QStringLiteral("%1\n%2 Hz · %3 声道 · %4 秒")
                                      .arg(file.fileName())
                                      .arg(info.sampleRate)
                                      .arg(info.channels)
                                      .arg(fTrackSeconds, 0, 'f', 1)
                                : file.fileName());

    const chost::RackSlot slot = fHost.slot(static_cast<uint>(track));
    fTrackStrip->setFaderEnabled(true);
    fTrackStrip->setVolume(slot.volume);
    fTrackStrip->setMuted(!slot.active);
    fArrange->setMuted(!slot.active);
    refreshInserts();
}

void MainWindow::refreshInserts()
{
    std::vector<InsertRack::Slot> items;
    for (uint id : insertIds())
    {
        const chost::RackSlot s = fHost.slot(id);
        items.push_back({QString::fromStdString(s.name), s.active});
    }
    fInspectorInserts->setSlots(items);
    fTrackStrip->inserts()->setSlots(items);
}

void MainWindow::closeEditors()
{
    for (auto& [id, editor] : fEditors)
        if (editor)
            editor->close();
    fEditors.clear();
}

void MainWindow::addInsert()
{
    PluginPicker picker(fPlugins, this);
    if (picker.exec() == QDialog::Accepted && !picker.selectedSpec().isEmpty())
        addPluginSpec(picker.selectedSpec());
}

bool MainWindow::addPluginSpec(const QString& spec)
{
    if (!fHost.addPlugin(spec.toStdString()))
    {
        showError(QStringLiteral("加载插件失败：%1\n\n%2").arg(spec, QString::fromStdString(fHost.lastError())));
        return false;
    }
    refreshInserts();
    return true;
}

void MainWindow::editInsert(int index)
{
    const std::vector<uint> ids = insertIds();
    if (index < 0 || index >= static_cast<int>(ids.size()))
        return;
    const uint id = ids[static_cast<size_t>(index)];

    // plugins with their own GUI open it, like double clicking an insert in Cubase
    if (fHost.slot(id).hasCustomUi)
    {
        fHost.showCustomUi(id, true);
        return;
    }
    QPointer<PluginEditor>& editor = fEditors[id];
    if (!editor)
        editor = new PluginEditor(fHost, id, this);
    editor->show();
    editor->raise();
    editor->activateWindow();
}

void MainWindow::setInsertActive(int index, bool active)
{
    const std::vector<uint> ids = insertIds();
    if (index >= 0 && index < static_cast<int>(ids.size()))
        fHost.setActive(ids[static_cast<size_t>(index)], active);
    refreshInserts();
}

void MainWindow::removeInsert(int index)
{
    const std::vector<uint> ids = insertIds();
    if (index < 0 || index >= static_cast<int>(ids.size()))
        return;
    // plugin ids shift after a removal, open editors would point at the wrong plugin
    closeEditors();
    fHost.removePlugin(ids[static_cast<size_t>(index)]);
    refreshInserts();
}

void MainWindow::moveInsert(int index, int delta)
{
    const std::vector<uint> ids = insertIds();
    const int target = index + delta;
    if (index < 0 || target < 0 || index >= static_cast<int>(ids.size()) || target >= static_cast<int>(ids.size()))
        return;
    closeEditors();
    fHost.movePlugin(ids[static_cast<size_t>(index)], static_cast<int>(ids[static_cast<size_t>(target)]) - static_cast<int>(ids[static_cast<size_t>(index)]));
    refreshInserts();
}

void MainWindow::setTrackMuted(bool muted)
{
    const int track = trackSlot();
    if (track >= 0)
        fHost.setActive(static_cast<uint>(track), !muted);
    fTrackStrip->setMuted(muted);
    fArrange->setMuted(muted);
}

void MainWindow::setTrackVolume(float volume)
{
    const int track = trackSlot();
    if (track >= 0)
        fHost.setVolume(static_cast<uint>(track), volume);
}

// ---------------------------------------------------------------------------------------------------------------------
// files and transport

void MainWindow::rescanPlugins()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    fPlugins = chost::scanPlugins();
    QApplication::restoreOverrideCursor();
    fillPluginTree(fBrowser, fPlugins);
    filterPluginTree(fBrowser, fSearch->text());
    statusBar()->showMessage(QStringLiteral("找到 %1 个插件").arg(fPlugins.size()), 5000);
}

void MainWindow::play()
{
    fHost.play();
}

bool MainWindow::loadAudioFile(const QString& path)
{
    // one audio track: a new file replaces the old one, the inserts stay
    closeEditors();
    if (const int track = trackSlot(); track >= 0)
        fHost.removePlugin(static_cast<uint>(track));

    if (!fHost.loadAudioFile(path.toStdString()))
    {
        showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(path, QString::fromStdString(fHost.lastError())));
        fTrackFile.clear();
        refreshTrack();
        return false;
    }

    // play once along the timeline instead of looping
    if (const int loop = fHost.findParameter(0, "Loop Mode"); loop >= 0)
        fHost.setParameter(0, static_cast<uint32_t>(loop), 0.0f);

    fTrackFile = path;
    refreshTrack();
    seek(0.0);
    return true;
}

void MainWindow::openAudioFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开音频文件"), {},
                                                      QStringLiteral("音频文件 (*.wav *.flac *.ogg *.mp3 *.aiff *.aif);;所有文件 (*)"));
    if (!path.isEmpty())
        loadAudioFile(path);
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开工程"), {},
                                                      QStringLiteral("Carla 工程 (*.carxp)"));
    if (path.isEmpty())
        return;
    closeEditors();
    if (!fHost.loadProject(path.toStdString()))
        showError(QStringLiteral("无法打开工程：%1").arg(QString::fromStdString(fHost.lastError())));
    fTrackFile.clear();
    refreshTrack();
}

void MainWindow::saveProject()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存工程"), QStringLiteral("project.carxp"),
                                                QStringLiteral("Carla 工程 (*.carxp)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".carxp");
    if (fHost.saveProject(path.toStdString()))
        statusBar()->showMessage(QStringLiteral("已保存：%1").arg(path), 5000);
    else
        showError(QStringLiteral("保存失败：%1").arg(QString::fromStdString(fHost.lastError())));
}

void MainWindow::togglePlay()
{
    if (fHost.isPlaying())
        fHost.pause();
    else
        fHost.play();
}

void MainWindow::stop()
{
    fHost.pause();
    seek(0.0);
}

void MainWindow::seek(double seconds)
{
    fHost.relocate(static_cast<uint64_t>(std::max(0.0, seconds) * fHost.sampleRate()));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile())
        loadAudioFile(urls.first().toLocalFile());
}

void MainWindow::showError(const QString& what)
{
    // offscreen and --screenshot runs (CI) must never block on a dialog
    if (QGuiApplication::platformName() == QLatin1String("offscreen") || qApp->property("chost-noninteractive").toBool())
    {
        qWarning("%s", qPrintable(what));
        return;
    }
    QMessageBox::warning(this, QStringLiteral("chost"), what);
}
