#include "MainWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

using chost::HostEvent;

static constexpr int kSliderSteps = 1000;

// peak (linear 0..1) to a 0..100 meter value over a 60 dB range
static int peakToMeter(float peak)
{
    if (peak <= 0.001f)
        return 0;
    const float db = 20.0f * std::log10(peak);
    return std::clamp(static_cast<int>((db + 60.0f) / 60.0f * 100.0f), 0, 100);
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("chost 插件宿主"));
    resize(1200, 720);

    buildUi();

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
    fHost.setEventCallback(nullptr);
    fHost.stop();
}

// ---------------------------------------------------------------------------------------------------------------------
// layout

void MainWindow::buildUi()
{
    // toolbar: audio driver, transport, files
    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);

    toolbar->addWidget(new QLabel(QStringLiteral(" 音频驱动 ")));
    fDriverBox = new QComboBox;
    for (const std::string& name : chost::audioDrivers())
        fDriverBox->addItem(QString::fromStdString(name));
    toolbar->addWidget(fDriverBox);
    connect(fDriverBox, &QComboBox::textActivated, this, &MainWindow::startEngine);

    toolbar->addSeparator();
    fPlayButton = new QPushButton(QStringLiteral("▶ 播放"));
    toolbar->addWidget(fPlayButton);
    connect(fPlayButton, &QPushButton::clicked, this, &MainWindow::togglePlay);

    toolbar->addSeparator();
    toolbar->addAction(QStringLiteral("打开音频文件…"), this, &MainWindow::openAudioFile);
    toolbar->addAction(QStringLiteral("打开工程…"), this, &MainWindow::openProject);
    toolbar->addAction(QStringLiteral("保存工程…"), this, &MainWindow::saveProject);

    // left: plugin browser
    auto* browserPane = new QWidget;
    auto* browserLayout = new QVBoxLayout(browserPane);
    browserLayout->addWidget(new QLabel(QStringLiteral("<b>插件浏览器</b>（双击加入机架）")));
    fSearch = new QLineEdit;
    fSearch->setPlaceholderText(QStringLiteral("搜索名称、厂商或格式"));
    fSearch->setClearButtonEnabled(true);
    browserLayout->addWidget(fSearch);
    connect(fSearch, &QLineEdit::textChanged, this, &MainWindow::filterPlugins);

    fBrowser = new QTreeWidget;
    fBrowser->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("格式"), QStringLiteral("厂商")});
    fBrowser->setRootIsDecorated(false);
    fBrowser->setSortingEnabled(true);
    fBrowser->sortByColumn(0, Qt::AscendingOrder);
    fBrowser->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    browserLayout->addWidget(fBrowser);
    connect(fBrowser, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        addPluginSpec(item->data(0, Qt::UserRole).toString());
    });

    auto* rescan = new QPushButton(QStringLiteral("重新扫描插件"));
    browserLayout->addWidget(rescan);
    connect(rescan, &QPushButton::clicked, this, &MainWindow::rescanPlugins);

    // middle: rack
    auto* rackPane = new QWidget;
    auto* rackLayout = new QVBoxLayout(rackPane);
    rackLayout->addWidget(new QLabel(QStringLiteral("<b>机架</b>（从上到下串联处理，勾选框 = 启用）")));
    fRack = new QListWidget;
    rackLayout->addWidget(fRack);
    connect(fRack, &QListWidget::currentRowChanged, this, &MainWindow::rebuildParameters);
    connect(fRack, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!fRebuildingRack)
            fHost.setActive(static_cast<uint>(fRack->row(item)), item->checkState() == Qt::Checked);
    });

    auto* rackButtons = new QHBoxLayout;
    auto* up = new QPushButton(QStringLiteral("上移"));
    auto* down = new QPushButton(QStringLiteral("下移"));
    auto* remove = new QPushButton(QStringLiteral("移除"));
    fUiButton = new QPushButton(QStringLiteral("打开插件界面"));
    for (QPushButton* b : {up, down, remove, fUiButton})
        rackButtons->addWidget(b);
    rackLayout->addLayout(rackButtons);
    connect(up, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelected(+1); });
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeSelected);
    connect(fUiButton, &QPushButton::clicked, this, &MainWindow::showSelectedUi);

    auto* meterBox = new QGridLayout;
    fMeterL = new QProgressBar;
    fMeterR = new QProgressBar;
    for (QProgressBar* m : {fMeterL, fMeterR})
    {
        m->setRange(0, 100);
        m->setTextVisible(false);
        m->setMaximumHeight(12);
    }
    meterBox->addWidget(new QLabel(QStringLiteral("输出 L")), 0, 0);
    meterBox->addWidget(fMeterL, 0, 1);
    meterBox->addWidget(new QLabel(QStringLiteral("输出 R")), 1, 0);
    meterBox->addWidget(fMeterR, 1, 1);
    rackLayout->addLayout(meterBox);

    // right: parameters of the selected plugin
    auto* paramPane = new QWidget;
    auto* paramPaneLayout = new QVBoxLayout(paramPane);
    fParamTitle = new QLabel(QStringLiteral("<b>参数</b>"));
    paramPaneLayout->addWidget(fParamTitle);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    fParamContainer = new QWidget;
    fParamLayout = new QVBoxLayout(fParamContainer);
    fParamLayout->addStretch();
    scroll->setWidget(fParamContainer);
    paramPaneLayout->addWidget(scroll);

    auto* splitter = new QSplitter;
    splitter->addWidget(browserPane);
    splitter->addWidget(rackPane);
    splitter->addWidget(paramPane);
    splitter->setSizes({380, 360, 460});
    setCentralWidget(splitter);

    fEngineLabel = new QLabel;
    statusBar()->addPermanentWidget(fEngineLabel);
}

// ---------------------------------------------------------------------------------------------------------------------
// engine

void MainWindow::startEngine(const QString& driver)
{
    fParamPluginId = -1;
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
    rebuildRack();
}

void MainWindow::onHostEvent(HostEvent event, uint pluginId, int index, float value, const QString& text)
{
    switch (event)
    {
    case HostEvent::PluginAdded:
    case HostEvent::PluginRemoved:
    case HostEvent::PluginRenamed:
    case HostEvent::Reload:
        rebuildRack();
        break;
    case HostEvent::ParameterChanged:
        if (static_cast<int>(pluginId) == fParamPluginId)
            for (ParamRow& row : fParamRows)
                if (static_cast<int>(row.index) == index)
                    updateParamRow(row, value);
        break;
    case HostEvent::UiClosed:
        break;
    case HostEvent::Error:
        showError(text);
        break;
    case HostEvent::EngineStopped:
        statusBar()->showMessage(QStringLiteral("音频引擎已停止"));
        rebuildRack();
        break;
    }
}

void MainWindow::tick()
{
    fHost.idle();

    const uint count = fHost.pluginCount();
    if (count > 0)
    {
        const auto [l, r] = fHost.outputPeaks(count - 1);
        fMeterL->setValue(peakToMeter(l));
        fMeterR->setValue(peakToMeter(r));
    }
    else
    {
        fMeterL->setValue(0);
        fMeterR->setValue(0);
    }

    // output parameters (meters, gain reduction, ...) only change from the plugin side
    if (fParamPluginId >= 0)
        for (ParamRow& row : fParamRows)
            if (row.isOutput)
                updateParamRow(row, fHost.parameterValue(static_cast<uint>(fParamPluginId), row.index));

    fPlayButton->setText(fHost.isPlaying() ? QStringLiteral("⏸ 暂停") : QStringLiteral("▶ 播放"));
    fEngineLabel->setText(fHost.isRunning()
                              ? QStringLiteral("%1 · %2 Hz · 缓冲 %3 · DSP %4%")
                                    .arg(QString::fromStdString(fHost.driverName()))
                                    .arg(fHost.sampleRate(), 0, 'f', 0)
                                    .arg(fHost.bufferSize())
                                    .arg(fHost.dspLoad(), 0, 'f', 1)
                              : QStringLiteral("引擎未运行"));
}

// ---------------------------------------------------------------------------------------------------------------------
// browser

void MainWindow::rescanPlugins()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    fPlugins = chost::scanPlugins();
    QApplication::restoreOverrideCursor();

    fBrowser->setSortingEnabled(false);
    fBrowser->clear();
    for (const chost::PluginEntry& e : fPlugins)
    {
        auto* item = new QTreeWidgetItem(fBrowser);
        item->setText(0, QString::fromStdString(e.name));
        item->setText(1, QString::fromStdString(e.format) + (e.isSynth ? QStringLiteral(" · 乐器") : QString()));
        item->setText(2, QString::fromStdString(e.maker));
        item->setToolTip(0, QString::fromStdString(e.spec));
        item->setData(0, Qt::UserRole, QString::fromStdString(e.spec));
    }
    fBrowser->setSortingEnabled(true);
    filterPlugins(fSearch->text());
    statusBar()->showMessage(QStringLiteral("找到 %1 个插件").arg(fPlugins.size()), 5000);
}

void MainWindow::filterPlugins(const QString& text)
{
    for (int i = 0; i < fBrowser->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = fBrowser->topLevelItem(i);
        const bool match = text.isEmpty()
                           || item->text(0).contains(text, Qt::CaseInsensitive)
                           || item->text(1).contains(text, Qt::CaseInsensitive)
                           || item->text(2).contains(text, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
}

bool MainWindow::addPluginSpec(const QString& spec)
{
    if (fHost.addPlugin(spec.toStdString()))
    {
        rebuildRack();
        selectRackSlot(static_cast<int>(fHost.pluginCount()) - 1);
        return true;
    }
    showError(QStringLiteral("加载插件失败：%1\n\n%2").arg(spec, QString::fromStdString(fHost.lastError())));
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// rack

void MainWindow::rebuildRack()
{
    const int previous = fRack->currentRow();
    fRebuildingRack = true;
    fRack->clear();
    for (uint id = 0; id < fHost.pluginCount(); ++id)
    {
        const chost::RackSlot s = fHost.slot(id);
        auto* item = new QListWidgetItem(QStringLiteral("%1.  %2").arg(id + 1).arg(QString::fromStdString(s.name)), fRack);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(s.active ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(QString::fromStdString(s.maker));
    }
    fRebuildingRack = false;

    if (fRack->count() > 0)
        fRack->setCurrentRow(std::clamp(previous, 0, fRack->count() - 1));
    fParamPluginId = -1;
    rebuildParameters();
}

void MainWindow::selectRackSlot(int row)
{
    fRack->setCurrentRow(row);
}

int MainWindow::selectedSlot() const
{
    return fRack->currentRow();
}

void MainWindow::moveSelected(int delta)
{
    const int row = selectedSlot();
    if (row < 0 || !fHost.movePlugin(static_cast<uint>(row), delta))
        return;
    rebuildRack();
    selectRackSlot(row + delta);
}

void MainWindow::removeSelected()
{
    const int row = selectedSlot();
    if (row >= 0 && fHost.removePlugin(static_cast<uint>(row)))
        rebuildRack();
}

void MainWindow::showSelectedUi()
{
    const int row = selectedSlot();
    if (row < 0)
        return;
    if (!fHost.slot(static_cast<uint>(row)).hasCustomUi)
    {
        statusBar()->showMessage(QStringLiteral("这个插件没有自带界面，请用右侧参数面板调节"), 5000);
        return;
    }
    fHost.showCustomUi(static_cast<uint>(row), true);
}

// ---------------------------------------------------------------------------------------------------------------------
// parameters

void MainWindow::rebuildParameters()
{
    const int id = selectedSlot();
    if (id == fParamPluginId)
        return;
    fParamPluginId = id;

    // the trailing stretch stays, everything above it is replaced
    while (fParamLayout->count() > 1)
    {
        QLayoutItem* item = fParamLayout->takeAt(0);
        delete item->widget();
        delete item;
    }
    fParamRows.clear();

    if (id < 0)
    {
        fParamTitle->setText(QStringLiteral("<b>参数</b>"));
        fUiButton->setEnabled(false);
        return;
    }

    const chost::RackSlot slot = fHost.slot(static_cast<uint>(id));
    fParamTitle->setText(QStringLiteral("<b>参数 · %1</b>").arg(QString::fromStdString(slot.name).toHtmlEscaped()));
    fUiButton->setEnabled(slot.hasCustomUi);

    const std::vector<chost::ParamInfo> params = fHost.parameters(static_cast<uint>(id));
    fParamRows.reserve(params.size());

    int insertAt = 0;
    for (const chost::ParamInfo& p : params)
    {
        ParamRow row;
        row.index = p.index;
        row.min = p.min;
        row.max = p.max;
        row.isOutput = p.isOutput;
        row.isBoolean = p.isBoolean;
        row.isInteger = p.isInteger;
        row.unit = QString::fromStdString(p.unit);

        auto* line = new QWidget;
        auto* layout = new QHBoxLayout(line);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* name = new QLabel(QString::fromStdString(p.name));
        name->setMinimumWidth(130);
        name->setToolTip(QString::fromStdString(p.symbol));
        layout->addWidget(name);

        const uint32_t index = p.index;
        if (p.isBoolean)
        {
            row.toggle = new QCheckBox;
            row.toggle->setEnabled(!p.isOutput);
            layout->addWidget(row.toggle, 1);
            connect(row.toggle, &QCheckBox::toggled, this, [this, id, index, p](bool on) {
                fHost.setParameter(static_cast<uint>(id), index, on ? p.max : p.min);
            });
        }
        else
        {
            row.slider = new QSlider(Qt::Horizontal);
            row.slider->setRange(0, kSliderSteps);
            row.slider->setEnabled(!p.isOutput);
            layout->addWidget(row.slider, 1);
        }

        row.valueLabel = new QLabel;
        row.valueLabel->setMinimumWidth(90);
        row.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(row.valueLabel);

        if (p.isOutput)
            name->setText(name->text() + QStringLiteral(" (输出)"));

        fParamLayout->insertWidget(insertAt++, line);
        fParamRows.push_back(row);
        updateParamRow(fParamRows.back(), p.value);

        if (row.slider != nullptr && !p.isOutput)
        {
            const size_t rowIndex = fParamRows.size() - 1;
            connect(row.slider, &QSlider::valueChanged, this, [this, id, rowIndex](int pos) {
                ParamRow& r = fParamRows[rowIndex];
                float value = r.min + (r.max - r.min) * static_cast<float>(pos) / kSliderSteps;
                if (r.isInteger)
                    value = std::round(value);
                fHost.setParameter(static_cast<uint>(id), r.index, value);
                r.valueLabel->setText(QStringLiteral("%1 %2").arg(value, 0, 'g', 4).arg(r.unit));
            });
            // double click resets to the default value
            line->setToolTip(QStringLiteral("默认值：%1 %2").arg(p.def, 0, 'g', 4).arg(row.unit));
        }
    }

    if (params.empty())
        fParamLayout->insertWidget(0, new QLabel(QStringLiteral("这个插件没有可调参数")));
}

void MainWindow::updateParamRow(ParamRow& row, float value)
{
    if (row.toggle != nullptr)
    {
        const QSignalBlocker blocker(row.toggle);
        row.toggle->setChecked(value > (row.min + row.max) * 0.5f);
    }
    if (row.slider != nullptr && !row.slider->isSliderDown())
    {
        const QSignalBlocker blocker(row.slider);
        const float range = row.max - row.min;
        row.slider->setValue(range > 0.0f ? static_cast<int>((value - row.min) / range * kSliderSteps) : 0);
    }
    row.valueLabel->setText(QStringLiteral("%1 %2").arg(value, 0, 'g', 4).arg(row.unit));
}

// ---------------------------------------------------------------------------------------------------------------------
// files and transport

bool MainWindow::loadAudioFile(const QString& path)
{
    if (!fHost.loadAudioFile(path.toStdString()))
    {
        showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(path, QString::fromStdString(fHost.lastError())));
        return false;
    }
    rebuildRack();
    fHost.play();
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
    if (!fHost.loadProject(path.toStdString()))
        showError(QStringLiteral("无法打开工程：%1").arg(QString::fromStdString(fHost.lastError())));
    rebuildRack();
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
