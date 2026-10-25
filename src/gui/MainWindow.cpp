#include "MainWindow.h"

#include "ExportDialog.h"
#include "InstrumentDialog.h"
#include "Instruments.h"
#include "MasterDialog.h"
#include "PianoRoll.h"
#include "PluginDiscovery.h"
#include "PluginWindows.h"
#include "RenderJob.h"
#include "Resources.h"
#include "ScriptJob.h"
#include "StemDialog.h"
#include "Theme.h"
#include "TranscribeDialog.h"
#include "Widgets.h"
#include "core/AudioConvert.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

using chost::HostEvent;

static const char* const kPluginFoldersKey = "plugins/folders";

static QLabel* sectionTitle(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

static bool isSessionFile(const QString& path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("chost"), Qt::CaseInsensitive) == 0;
}

static bool isMidiFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("mid") || suffix == QLatin1String("midi");
}

// The kind of part an audio track holds when it wasn't made by stem separation, guessed from its name.
static QString guessRole(const QString& name)
{
    const QString n = name.toLower();
    static const std::vector<std::pair<QStringList, QString>> words{
        {{QStringLiteral("kick"), QStringLiteral("底鼓")}, QStringLiteral("kick")},
        {{QStringLiteral("snare"), QStringLiteral("军鼓")}, QStringLiteral("snare")},
        {{QStringLiteral("hihat"), QStringLiteral("hi-hat"), QStringLiteral("hh"), QStringLiteral("踩镲")}, QStringLiteral("hh")},
        {{QStringLiteral("tom"), QStringLiteral("通鼓")}, QStringLiteral("toms")},
        {{QStringLiteral("drum"), QStringLiteral("鼓")}, QStringLiteral("drums")},
        {{QStringLiteral("bass"), QStringLiteral("贝斯")}, QStringLiteral("bass")},
        {{QStringLiteral("lead"), QStringLiteral("melody"), QStringLiteral("主旋律")}, QStringLiteral("lead")},
        {{QStringLiteral("backing"), QStringLiteral("harmony"), QStringLiteral("和声")}, QStringLiteral("backing")},
        {{QStringLiteral("vocal"), QStringLiteral("vox"), QStringLiteral("voice"), QStringLiteral("人声"), QStringLiteral("主唱")}, QStringLiteral("vocals")},
        {{QStringLiteral("guitar"), QStringLiteral("gtr"), QStringLiteral("吉他")}, QStringLiteral("guitar")},
        {{QStringLiteral("piano"), QStringLiteral("keys"), QStringLiteral("钢琴")}, QStringLiteral("piano")},
    };
    for (const auto& [keys, role] : words)
        for (const QString& key : keys)
            if (n.contains(key))
                return role;
    return {};
}

// Tracks that hold a master made by AI mastering, kept out of the next mix to master.
static const char* const kMasterRole = "master";

// "A minor" -> "A 小调"
static QString keyText(const std::string& key)
{
    const QString k = QString::fromStdString(key);
    if (k.isEmpty())
        return QStringLiteral("调性 ?");
    const QString tonic = k.section(QLatin1Char(' '), 0, 0);
    return tonic + (k.endsWith(QLatin1String("minor")) ? QStringLiteral(" 小调") : QStringLiteral(" 大调"));
}

// the relative key shares every note: "C major" <-> "A minor"
static QString relativeKeyText(const std::string& key)
{
    static const QStringList names{QStringLiteral("C"), QStringLiteral("C#"), QStringLiteral("D"), QStringLiteral("Eb"),
                                   QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("F#"), QStringLiteral("G"),
                                   QStringLiteral("Ab"), QStringLiteral("A"), QStringLiteral("Bb"), QStringLiteral("B")};
    const QString k = QString::fromStdString(key);
    const int tonic = names.indexOf(k.section(QLatin1Char(' '), 0, 0));
    if (tonic < 0)
        return {};
    const bool minor = k.endsWith(QLatin1String("minor"));
    return keyText((names[(tonic + (minor ? 3 : 9)) % 12] + (minor ? QStringLiteral(" major") : QStringLiteral(" minor"))).toStdString());
}

static const QString kImportFilter = QStringLiteral(
    "音频和 MIDI (*.wav *.flac *.ogg *.mp3 *.aiff *.aif *.mid *.midi);;音频文件 (*.wav *.flac *.ogg *.mp3 *.aiff *.aif);;"
    "MIDI 文件 (*.mid *.midi);;所有文件 (*)");

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    resize(1440, 920);
    setAcceptDrops(true);

    std::vector<std::string> folders;
    for (const QString& folder : QSettings().value(kPluginFoldersKey).toStringList())
        folders.push_back(QDir::toNativeSeparators(folder).toStdString());
    chost::setExtraPluginFolders(folders);

    buildMenus();
    buildTransport();

    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(buildInspector());
    fArrange = new ArrangeView;
    fArrange->setTempo(fSession.tempo());
    top->addWidget(fArrange);
    top->addWidget(buildBrowser());
    top->setStretchFactor(1, 1);
    top->setSizes({230, 880, 320});

    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(top);
    main->addWidget(buildMixConsole());
    main->setStretchFactor(0, 1);
    main->setSizes({500, 400});
    setCentralWidget(main);

    connect(fArrange, &ArrangeView::seekRequested, this, &MainWindow::seek);
    connect(fArrange, &ArrangeView::trackSelected, this, &MainWindow::selectTrack);
    connect(fArrange, &ArrangeView::muteToggled, this, &MainWindow::setMuted);
    connect(fArrange, &ArrangeView::soloToggled, this, &MainWindow::setSolo);
    connect(fArrange, &ArrangeView::trackMenuRequested, this, &MainWindow::trackMenu);
    connect(fArrange, &ArrangeView::filesDropped, this, &MainWindow::dropFiles);
    connect(fArrange, &ArrangeView::clipDoubleClicked, this, &MainWindow::editNotes);

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

    fAnalysis = new ScriptJob(this);
    connect(fAnalysis, &ScriptJob::result, this, [this](const QString& key, const QString& value) {
        if (key == QLatin1String("TEMPO"))
            setTempo(value.section(QLatin1Char('\t'), 0, 0).toDouble());
        else if (key == QLatin1String("KEY"))
            fSession.setKey(value.section(QLatin1Char('\t'), 0, 0).toStdString());
        refreshSongInfo();
    });
    connect(fAnalysis, &ScriptJob::finished, this, [this](bool ok, const QString& message) {
        if (ok)
            statusBar()->showMessage(QStringLiteral("识别结果：%1 BPM，%2（也可能是 %3）。不对的话可以在上方直接改速度。")
                                         .arg(fSession.tempo(), 0, 'f', 1)
                                         .arg(keyText(fSession.key()), relativeKeyText(fSession.key())),
                                     12000);
        else if (!fAnalysisQuiet)
            showError(QStringLiteral("没能识别速度和调性：%1").arg(message));
    });

    fDiscovery = new PluginDiscovery(this);
    connect(fDiscovery, &PluginDiscovery::progress, this, [this](int done, int total, const QString& name) {
        if (done < total)
            statusBar()->showMessage(QStringLiteral("正在识别插件 %1/%2：%3").arg(done + 1).arg(total).arg(name));
    });
    connect(fDiscovery, &PluginDiscovery::finished, this, [this] {
        showPlugins();
        statusBar()->showMessage(QStringLiteral("插件识别完成，共 %1 个").arg(fPlugins.size()), 5000);
    });

    startEngine(QString::fromStdString(chost::defaultAudioDriver()));
    rescanPlugins();
    refreshSongInfo();
    updateTitle();
}

MainWindow::~MainWindow()
{
    fTimer->stop();
    delete fStemDialog;
    delete fTranscribeDialog;
    delete fPianoRoll;
    delete fExportDialog;
    delete fMasterDialog;
    closeEditors();
    fHost.setEventCallback(nullptr);
    fHost.stop();
}

// ---------------------------------------------------------------------------------------------------------------------
// layout

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(QStringLiteral("文件"));
    file->addAction(QStringLiteral("新建工程"), QKeySequence::New, this, &MainWindow::newSession);
    file->addAction(QStringLiteral("打开工程…"), QKeySequence::Open, this, &MainWindow::openSessionDialog);
    file->addAction(QStringLiteral("保存工程"), QKeySequence::Save, this, &MainWindow::saveSession);
    file->addAction(QStringLiteral("工程另存为…"), QKeySequence::SaveAs, this, &MainWindow::saveSessionAs);
    file->addSeparator();
    file->addAction(QStringLiteral("导入音频 / MIDI 文件…"), QKeySequence(QStringLiteral("Ctrl+I")), this, &MainWindow::importAudio);
    file->addAction(QStringLiteral("导出混音（WAV）…"), QKeySequence(QStringLiteral("Ctrl+E")), this, [this] { exportMix(); });
    file->addSeparator();
    file->addAction(QStringLiteral("退出"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* track = menuBar()->addMenu(QStringLiteral("轨道"));
    track->addAction(QStringLiteral("添加音轨"), QKeySequence(QStringLiteral("Ctrl+T")), this, &MainWindow::addEmptyTrack);
    track->addAction(QStringLiteral("添加乐器轨（MIDI）"), this, &MainWindow::addInstrumentTrack);
    track->addAction(QStringLiteral("导入音频 / MIDI 到新轨道…"), this, &MainWindow::importAudio);
    track->addAction(QStringLiteral("替换选中轨道的文件…"), this, &MainWindow::replaceAudio);
    track->addAction(QStringLiteral("选择乐器…"), this, &MainWindow::chooseInstrument);
    track->addSeparator();
    track->addAction(QStringLiteral("删除选中轨道"), QKeySequence(QStringLiteral("Ctrl+Del")), this, &MainWindow::removeSelectedTrack);

    QMenu* transport = menuBar()->addMenu(QStringLiteral("走带"));
    transport->addAction(QStringLiteral("播放 / 暂停（空格）"), this, &MainWindow::togglePlay);
    transport->addAction(QStringLiteral("停止"), this, &MainWindow::stop);
    transport->addAction(QStringLiteral("回到开头"), this, [this] { seek(0.0); });

    QMenu* plugins = menuBar()->addMenu(QStringLiteral("插件"));
    plugins->addAction(QStringLiteral("给选中轨道添加插入效果…"), this, &MainWindow::addInsert);
    plugins->addSeparator();
    plugins->addAction(QStringLiteral("乐器设置（每种乐器用哪个音源）…"), this, &MainWindow::instrumentSettings);
    plugins->addSeparator();
    plugins->addAction(QStringLiteral("插件文件夹…"), this, &MainWindow::editPluginFolders);
    plugins->addAction(QStringLiteral("重新扫描插件"), this, &MainWindow::rescanPlugins);

    QMenu* ai = menuBar()->addMenu(QStringLiteral("AI"));
    ai->addAction(QStringLiteral("自动分轨…"), QKeySequence(QStringLiteral("Ctrl+Shift+S")), this, [this] { separateStems(); });
    ai->addAction(QStringLiteral("转 MIDI（选中轨道）…"), QKeySequence(QStringLiteral("Ctrl+Shift+M")), this, [this] { transcribe(false); });
    ai->addAction(QStringLiteral("转 MIDI（多条轨道）…"), this, [this] { transcribe(true); });
    ai->addAction(QStringLiteral("MIDI 轨全部换成真实音源"), this, &MainWindow::applyInstruments);
    ai->addSeparator();
    ai->addAction(QStringLiteral("识别速度和调性"), this, [this] { analyzeSong(); });
    ai->addAction(QStringLiteral("AI 母带…"), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this, [this] { masterMix(); });
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
    fTempoBox = new QDoubleSpinBox;
    fTempoBox->setObjectName(QStringLiteral("lcdSmall"));
    fTempoBox->setRange(20.0, 400.0);
    fTempoBox->setDecimals(1);
    fTempoBox->setSuffix(QStringLiteral(" BPM"));
    fTempoBox->setKeyboardTracking(false);
    fTempoBox->setToolTip(QStringLiteral("歌曲速度（每分钟拍数）。导入音频时会自动识别，也可以自己改"));
    fTempoBox->setValue(fSession.tempo());
    bar->addWidget(fTempoBox);
    connect(fTempoBox, &QDoubleSpinBox::valueChanged, this, &MainWindow::setTempo);
    fKeyLabel = new QLabel;
    fKeyLabel->setObjectName(QStringLiteral("lcdSmall"));
    bar->addWidget(fKeyLabel);
    auto* analyze = new QToolButton;
    analyze->setText(QStringLiteral("识别"));
    analyze->setToolTip(QStringLiteral("AI 识别选中音轨（或第一条音轨）的速度和调性"));
    bar->addWidget(analyze);
    connect(analyze, &QToolButton::clicked, this, [this] { analyzeSong(); });

    bar->addSeparator();
    auto* stems = new QToolButton;
    stems->setText(QStringLiteral("AI 分轨"));
    stems->setToolTip(QStringLiteral("把选中轨道的音频分离成人声、鼓、贝斯、吉他、钢琴等单独的轨道"));
    bar->addWidget(stems);
    connect(stems, &QToolButton::clicked, this, [this] { separateStems(); });
    auto* midi = new QToolButton;
    midi->setText(QStringLiteral("AI 转 MIDI"));
    midi->setToolTip(QStringLiteral("把音轨（最好是分好的单个乐器）转成 MIDI 乐器轨"));
    bar->addWidget(midi);
    connect(midi, &QToolButton::clicked, this, [this] { transcribe(true); });
    auto* mastering = new QToolButton;
    mastering->setText(QStringLiteral("AI 母带"));
    mastering->setToolTip(QStringLiteral("把整首混音做成可以发布的母带：音色平衡、压缩、响度标准"));
    bar->addWidget(mastering);
    connect(mastering, &QToolButton::clicked, this, [this] { masterMix(); });

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
    pane->setMinimumWidth(210);
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(sectionTitle(QStringLiteral("检查器")));

    auto* body = new QVBoxLayout;
    body->setContentsMargins(8, 4, 8, 8);
    fInspectorName = new QLineEdit;
    fInspectorName->setToolTip(QStringLiteral("轨道名称，可以直接改"));
    body->addWidget(fInspectorName);
    connect(fInspectorName, &QLineEdit::editingFinished, this, [this] { renameSelectedTrack(fInspectorName->text()); });
    fInspectorInfo = new QLabel;
    fInspectorInfo->setWordWrap(true);
    fInspectorInfo->setStyleSheet(QStringLiteral("color: #8b9096;"));
    body->addWidget(fInspectorInfo);

    fInspectorReplace = new QPushButton(QStringLiteral("替换音频…"));
    body->addWidget(fInspectorReplace);
    connect(fInspectorReplace, &QPushButton::clicked, this, &MainWindow::replaceAudio);
    fInspectorStems = new QPushButton(QStringLiteral("AI 自动分轨…"));
    body->addWidget(fInspectorStems);
    connect(fInspectorStems, &QPushButton::clicked, this, [this] { separateStems(); });
    fInspectorMidi = new QPushButton(QStringLiteral("AI 转 MIDI…"));
    fInspectorMidi->setToolTip(QStringLiteral("把这条音轨转成 MIDI，放到下面一条新的乐器轨上"));
    body->addWidget(fInspectorMidi);
    connect(fInspectorMidi, &QPushButton::clicked, this, [this] { transcribe(false); });

    // instrument tracks: which instrument plays the MIDI, and its sound
    fInstrumentBox = new QWidget;
    auto* instrument = new QVBoxLayout(fInstrumentBox);
    instrument->setContentsMargins(0, 6, 0, 0);
    auto* instrumentTitle = new QLabel(QStringLiteral("乐器"));
    instrumentTitle->setStyleSheet(QStringLiteral("color: #8b9096;"));
    instrument->addWidget(instrumentTitle);
    fInstrumentName = new QLabel;
    fInstrumentName->setWordWrap(true);
    fInstrumentName->setStyleSheet(QStringLiteral("font-weight: bold;"));
    instrument->addWidget(fInstrumentName);
    fProgramBox = new QComboBox;
    fProgramBox->setToolTip(QStringLiteral("音色"));
    fProgramBox->setMaxVisibleItems(24);
    instrument->addWidget(fProgramBox);
    auto* instrumentButtons = new QHBoxLayout;
    auto* choose = new QPushButton(QStringLiteral("换乐器…"));
    choose->setToolTip(QStringLiteral("换成你自己的乐器插件，比如 Kontakt"));
    instrumentButtons->addWidget(choose);
    fInstrumentEdit = new QPushButton(QStringLiteral("打开界面"));
    instrumentButtons->addWidget(fInstrumentEdit);
    instrument->addLayout(instrumentButtons);
    fInstrumentNotes = new QPushButton(QStringLiteral("编辑音符（钢琴卷帘）…"));
    fInstrumentNotes->setToolTip(QStringLiteral("改错音、加音、删音，也可以双击轨道上的 MIDI"));
    instrument->addWidget(fInstrumentNotes);
    connect(fInstrumentNotes, &QPushButton::clicked, this, [this] { editNotes(fSelected); });
    fInstrumentRemember = new QPushButton(QStringLiteral("记住这个音色"));
    fInstrumentRemember->setToolTip(QStringLiteral("把这个乐器和你在它里面选好的音色记下来，以后同一种乐器的 MIDI 轨自动用它"));
    instrument->addWidget(fInstrumentRemember);
    auto* settings = new QPushButton(QStringLiteral("乐器设置…"));
    instrument->addWidget(settings);
    connect(fInstrumentRemember, &QPushButton::clicked, this, &MainWindow::rememberInstrument);
    connect(settings, &QPushButton::clicked, this, &MainWindow::instrumentSettings);
    body->addWidget(fInstrumentBox);
    connect(choose, &QPushButton::clicked, this, &MainWindow::chooseInstrument);
    connect(fInstrumentEdit, &QPushButton::clicked, this, &MainWindow::editInstrument);
    connect(fProgramBox, &QComboBox::activated, this, [this](int index) {
        const std::vector<chost::Track>& tracks = fSession.tracks();
        if (fSelected >= 0 && fSelected < static_cast<int>(tracks.size()) && tracks[fSelected].instrument >= 0 && index >= 0)
            fHost.setMidiProgram(static_cast<uint>(tracks[fSelected].instrument), static_cast<uint32_t>(index));
    });

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
    layout->addWidget(sectionTitle(QStringLiteral("插件（双击加到选中轨道）")));

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

    auto* buttons = new QHBoxLayout;
    auto* folders = new QPushButton(QStringLiteral("插件文件夹…"));
    folders->setToolTip(QStringLiteral("添加你自己装插件的文件夹，比如 D:\\VST"));
    buttons->addWidget(folders);
    auto* rescan = new QPushButton(QStringLiteral("重新扫描"));
    buttons->addWidget(rescan);
    body->addLayout(buttons);
    layout->addLayout(body);

    connect(fSearch, &QLineEdit::textChanged, this, [this](const QString& text) { filterPluginTree(fBrowser, text); });
    connect(fBrowser, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        addPluginSpec(item->data(0, Qt::UserRole).toString());
    });
    connect(folders, &QPushButton::clicked, this, &MainWindow::editPluginFolders);
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

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    // the track channels scroll, the master stays put on the right like Cubase's output channel
    auto* strips = new QWidget;
    fStripRow = new QHBoxLayout(strips);
    fStripRow->setContentsMargins(8, 8, 8, 8);
    fStripRow->setSpacing(6);
    fStripRow->addStretch();
    auto* scroll = new QScrollArea;
    scroll->setWidget(strips);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    row->addWidget(scroll, 1);

    auto* master = new QWidget;
    auto* masterLayout = new QVBoxLayout(master);
    masterLayout->setContentsMargins(8, 8, 8, 8);
    fOutputStrip = new ChannelStrip(QStringLiteral("Stereo Out"), true);
    masterLayout->addWidget(fOutputStrip);
    row->addWidget(master);
    connect(fOutputStrip, &ChannelStrip::volumeChanged, this, [this](float v) { fSession.setMasterVolume(v); });

    layout->addLayout(row);
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

    // a driver change restarts carla; the tracks come back through a temporary session file
    QString carry;
    if (fSession.isReady() && !fSession.tracks().empty())
    {
        carry = QDir::temp().filePath(QStringLiteral("chost-driver-switch.chost"));
        if (!fSession.save(carry.toStdString()))
            carry.clear();
    }

    using Mode = chost::HostEngine::Mode;
    QString warning;
    if (!fHost.start(driver.toStdString(), Mode::Patchbay))
    {
        const QString error = QString::fromStdString(fHost.lastError());
        // the Dummy driver only knows the rack, it's silent anyway so the tracks just aren't mixed
        if (fHost.start(driver.toStdString(), Mode::Rack))
        {
            if (driver != QLatin1String("Dummy"))
                warning = QStringLiteral("%1 驱动不支持多轨混音，现在只能听到一条链路。请换一个音频驱动。").arg(driver);
        }
        else if (driver != QLatin1String("Dummy") && fHost.start("Dummy", Mode::Rack))
        {
            warning = QStringLiteral("无法用 %1 启动音频引擎，已切换到 Dummy（静音）驱动。\n\n%2").arg(driver, error);
        }
        else
        {
            warning = QStringLiteral("无法启动音频引擎：%1").arg(error);
        }
    }

    const QSignalBlocker blocker(fDriverBox);
    fDriverBox->setCurrentText(QString::fromStdString(fHost.driverName()));

    fColours.clear();
    if (carry.isEmpty() || !fSession.load(carry.toStdString()))
        fSession.init();
    if (!carry.isEmpty())
    {
        QFile::remove(carry);
        QFile::remove(QDir::temp().filePath(QStringLiteral("chost-driver-switch.carxp")));
    }
    refreshTracks();

    if (!warning.isEmpty())
        showError(warning);
    else
        statusBar()->showMessage(QStringLiteral("音频引擎已启动：%1").arg(driver), 5000);
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
        break;
    }
}

void MainWindow::tick()
{
    fHost.idle();

    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size() && i < fStrips.size(); ++i)
    {
        const auto [l, r] = fHost.outputPeaks(tracks[i].fader);
        fStrips[i]->setLevels(l, r);
    }
    if (fSession.isReady())
    {
        const auto [l, r] = fHost.outputPeaks(fSession.master());
        fOutputStrip->setLevels(l, r);
    }

    const double rate = fHost.sampleRate();
    const double seconds = rate > 0.0 ? static_cast<double>(fHost.transportFrame()) / rate : 0.0;
    const bool playing = fHost.isPlaying();
    fArrange->setPlayhead(seconds, playing);
    if (fPianoRoll)
        fPianoRoll->setPlayhead(seconds);

    const int ms = static_cast<int>(seconds * 1000.0);
    fTimeLcd->setText(QStringLiteral("%1:%2:%3.%4")
                          .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
                          .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
                          .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
                          .arg(ms % 1000, 3, 10, QLatin1Char('0')));
    const double beats = seconds * fSession.tempo() / 60.0;
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
    if (fHost.isRunning() && statusBar()->currentMessage().isEmpty())
        statusBar()->showMessage(QStringLiteral("%1 · %2 Hz · 缓冲 %3 · %4 条轨道")
                                     .arg(QString::fromStdString(fHost.driverName()))
                                     .arg(rate, 0, 'f', 0)
                                     .arg(fHost.bufferSize())
                                     .arg(tracks.size()),
                                 1000);
}

// ---------------------------------------------------------------------------------------------------------------------
// views of the session

const MainWindow::Clip& MainWindow::clip(const QString& path)
{
    auto it = fClips.find(path);
    if (it != fClips.end())
        return *it;

    Clip c;
    if (isMidiFile(path))
    {
        chost::MidiClip midi;
        std::string error;
        c.midi = true;
        c.decoded = chost::readMidiFile(path.toStdString(), midi, error);
        c.notes = std::move(midi.notes);
        c.drums = midi.drums;
        c.seconds = midi.seconds;
        return *fClips.insert(path, c);
    }
    chost::AudioFileInfo info;
    c.decoded = chost::computePeaks(path.toStdString(), 40000, c.peaks, info);
    if (c.decoded && info.sampleRate > 0)
    {
        c.seconds = static_cast<double>(info.frames) / info.sampleRate;
        c.sampleRate = static_cast<int>(info.sampleRate);
        c.channels = static_cast<int>(info.channels);
    }
    return *fClips.insert(path, c);
}

QColor MainWindow::trackColour(int track) const
{
    return track >= 0 && track < static_cast<int>(fColours.size()) ? fColours[track] : theme::trackColor;
}

void MainWindow::refreshTracks()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    while (fColours.size() < tracks.size())
        fColours.push_back(theme::trackColour(fNextColour++));
    fColours.resize(tracks.size());

    std::vector<ArrangeView::Lane> lanes;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const chost::Track& t = tracks[i];
        ArrangeView::Lane lane;
        lane.name = QString::fromStdString(t.name);
        lane.muted = t.muted;
        lane.solo = t.solo;
        lane.colour = fColours[i];
        lane.midi = t.kind == chost::TrackKind::Instrument;
        lane.kindText = lane.midi ? instrumentText(static_cast<int>(i)) : QStringLiteral("音频");
        if (!t.file.empty())
        {
            const QString path = QString::fromStdString(t.file);
            const Clip& c = clip(path);
            lane.clipName = QFileInfo(path).completeBaseName();
            lane.peaks = c.peaks;
            lane.seconds = c.seconds;
            for (const chost::MidiNote& n : c.notes)
                lane.notes.push_back({static_cast<float>(n.start), static_cast<float>(n.end), n.pitch});
            if (!c.decoded && t.source >= 0)
            {
                // a format chost can't decode (e.g. OGG): carla still plays it, ask the player for the length
                const int length = fHost.findParameter(static_cast<uint>(t.source), "Length");
                lane.seconds = length >= 0 ? fHost.parameterValue(static_cast<uint>(t.source), static_cast<uint32_t>(length)) : 0.0;
            }
        }
        lanes.push_back(std::move(lane));
    }
    fArrange->setLanes(lanes);

    // rebuild the channel strips, one per track
    for (ChannelStrip* strip : fStrips)
    {
        // out of the layout right away, a strip may be the sender of the signal that got us here
        fStripRow->removeWidget(strip);
        strip->hide();
        strip->deleteLater();
    }
    fStrips.clear();
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const int index = static_cast<int>(i);
        auto* strip = new ChannelStrip(QString::fromStdString(tracks[i].name), false);
        strip->setColour(fColours[i]);
        fStripRow->insertWidget(index, strip);
        // a strip's insert rack works on its own track: select it first, then act on the selection
        InsertRack* rack = strip->inserts();
        connect(rack, &InsertRack::addRequested, this, [this, index] { selectTrack(index); });
        connect(rack, &InsertRack::editRequested, this, [this, index](int) { selectTrack(index); });
        connect(rack, &InsertRack::activeToggled, this, [this, index](int, bool) { selectTrack(index); });
        connect(rack, &InsertRack::removeRequested, this, [this, index](int) { selectTrack(index); });
        connect(rack, &InsertRack::moveRequested, this, [this, index](int, int) { selectTrack(index); });
        connectInsertRack(rack);
        connect(strip, &ChannelStrip::selected, this, [this, index] { selectTrack(index); });
        connect(strip, &ChannelStrip::volumeChanged, this, [this, index](float v) { setVolume(index, v); });
        connect(strip, &ChannelStrip::muteToggled, this, [this, index](bool m) { setMuted(index, m); });
        connect(strip, &ChannelStrip::soloToggled, this, [this, index](bool s) { setSolo(index, s); });
        fStrips.push_back(strip);
    }
    fOutputStrip->setVolume(fSession.masterVolume());

    if (fSelected >= static_cast<int>(tracks.size()))
        fSelected = static_cast<int>(tracks.size()) - 1;
    if (fSelected < 0 && !tracks.empty())
        fSelected = 0;
    refreshMixState();
    refreshSelection();
}

void MainWindow::refreshSelection()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    const bool valid = fSelected >= 0 && fSelected < static_cast<int>(tracks.size());
    fArrange->setSelected(valid ? fSelected : -1);
    for (size_t i = 0; i < fStrips.size(); ++i)
        fStrips[i]->setSelected(static_cast<int>(i) == fSelected);

    fInspectorName->setEnabled(valid);
    fInspectorReplace->setEnabled(valid);
    if (!valid)
    {
        fInspectorName->setText(QString());
        fInspectorName->setStyleSheet(QString());
        fInspectorInfo->setText(QStringLiteral("还没有轨道。把音频或 MIDI 文件拖进来，或用 文件 → 导入。"));
        fInspectorStems->setEnabled(false);
        fInspectorMidi->setEnabled(false);
        fInstrumentBox->hide();
        refreshInserts();
        return;
    }

    const chost::Track& t = tracks[fSelected];
    const bool instrument = t.kind == chost::TrackKind::Instrument;
    fInspectorName->setText(QString::fromStdString(t.name));
    fInspectorName->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: white; font-weight: bold; }").arg(trackColour(fSelected).name()));
    fInspectorReplace->setText(instrument ? QStringLiteral("替换 MIDI…") : QStringLiteral("替换音频…"));
    fInspectorStems->setVisible(!instrument);
    fInspectorMidi->setVisible(!instrument);
    fInspectorStems->setEnabled(!t.file.empty());
    fInspectorMidi->setEnabled(!t.file.empty());
    if (t.file.empty())
    {
        fInspectorInfo->setText(instrument ? QStringLiteral("这条乐器轨还没有 MIDI。") : QStringLiteral("这条轨道还没有音频。"));
    }
    else
    {
        const QString path = QString::fromStdString(t.file);
        const Clip& c = clip(path);
        const QString name = QFileInfo(path).fileName();
        if (c.midi)
            fInspectorInfo->setText(c.decoded ? QStringLiteral("%1\n%2 个音符 · %3 秒").arg(name).arg(c.notes.size()).arg(c.seconds, 0, 'f', 1) : name);
        else
            fInspectorInfo->setText(c.decoded ? QStringLiteral("%1\n%2 Hz · %3 声道 · %4 秒").arg(name).arg(c.sampleRate).arg(c.channels).arg(c.seconds, 0, 'f', 1)
                                              : name);
    }
    refreshInstrument();
    refreshInserts();
}

void MainWindow::refreshInserts()
{
    auto slotsOf = [this](const chost::Track& t) {
        std::vector<InsertRack::Slot> items;
        for (uint id : t.inserts)
        {
            const chost::RackSlot s = fHost.slot(id);
            items.push_back({QString::fromStdString(s.name), s.active});
        }
        return items;
    };
    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size() && i < fStrips.size(); ++i)
        fStrips[i]->inserts()->setSlots(slotsOf(tracks[i]));
    fInspectorInserts->setSlots(fSelected >= 0 && fSelected < static_cast<int>(tracks.size()) ? slotsOf(tracks[fSelected])
                                                                                             : std::vector<InsertRack::Slot>{});
    fInspectorInserts->setEnabled(fSelected >= 0);
}

void MainWindow::refreshMixState()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        fArrange->setLaneState(static_cast<int>(i), tracks[i].muted, tracks[i].solo);
        if (i < fStrips.size())
        {
            fStrips[i]->setMuted(tracks[i].muted);
            fStrips[i]->setSolo(tracks[i].solo);
            fStrips[i]->setVolume(tracks[i].volume);
        }
    }
}

void MainWindow::closeEditors()
{
    for (auto& [id, editor] : fEditors)
        if (editor)
            editor->close();
    fEditors.clear();
}

std::vector<uint> MainWindow::selectedInserts() const
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    return fSelected >= 0 && fSelected < static_cast<int>(tracks.size()) ? tracks[fSelected].inserts : std::vector<uint>{};
}

void MainWindow::selectTrack(int track)
{
    if (track == fSelected)
        return;
    fSelected = track;
    refreshSelection();
}

// ---------------------------------------------------------------------------------------------------------------------
// tracks

bool MainWindow::fits(int track, const QString& path) const
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    return track >= 0 && track < static_cast<int>(tracks.size()) &&
           (tracks[track].kind == chost::TrackKind::Instrument) == isMidiFile(path);
}

int MainWindow::addAudioTrack(const QString& path, const QString& name)
{
    if (isMidiFile(path))
        return addMidiTrack(path, name);
    const QString trackName = name.isEmpty() ? QFileInfo(path).completeBaseName() : name;
    const int track = fSession.addTrack(trackName.toStdString());
    if (track < 0)
    {
        showError(QStringLiteral("无法添加轨道：%1").arg(QString::fromStdString(fSession.lastError())));
        return -1;
    }
    if (!fSession.setTrackFile(track, path.toStdString()))
    {
        const QString error = QString::fromStdString(fSession.lastError());
        fSession.removeTrack(track);
        refreshTracks();
        showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(path, error));
        return -1;
    }
    fSelected = track;
    refreshTracks();
    // the first song in a project: find its tempo and key in the background
    if (!fAnalysisTried && fSession.key().empty() && !resources::aiPython().isEmpty())
        analyzeSong(true);
    return track;
}

int MainWindow::addMidiTrack(const QString& path, const QString& name, const QString& role)
{
    QString trackName = name;
    if (trackName.isEmpty())
    {
        chost::MidiClip midi;
        std::string error;
        if (chost::readMidiFile(path.toStdString(), midi, error) && !midi.name.empty())
            trackName = QString::fromStdString(midi.name);
        else
            trackName = QFileInfo(path).completeBaseName();
    }
    const int track = fSession.addTrack(trackName.toStdString(), chost::TrackKind::Instrument);
    if (track < 0)
    {
        showError(QStringLiteral("无法添加轨道：%1").arg(QString::fromStdString(fSession.lastError())));
        return -1;
    }
    // an imported file says what it is by its name, e.g. "bass.mid"
    fSession.setRole(track, (role.isEmpty() ? guessRole(trackName + QLatin1Char(' ') + QFileInfo(path).completeBaseName()) : role).toStdString());
    if (!fSession.setTrackFile(track, path.toStdString()))
    {
        const QString error = QString::fromStdString(fSession.lastError());
        fSession.removeTrack(track);
        refreshTracks();
        showError(QStringLiteral("无法打开 MIDI 文件：%1\n\n%2").arg(path, error));
        return -1;
    }
    if (!loadInstrument(track, instruments::choiceFor(QString::fromStdString(fSession.tracks()[track].role))))
        statusBar()->showMessage(QStringLiteral("没有加载到乐器，请在检查器里选择一个乐器"), 8000);
    fSelected = track;
    refreshTracks();
    return track;
}

bool MainWindow::loadAudioFile(const QString& path)
{
    if (isSessionFile(path))
        return openSession(path);
    // an empty selected track takes the file, otherwise it gets a track of its own
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (fSelected >= 0 && fSelected < static_cast<int>(tracks.size()) && tracks[fSelected].file.empty() && fits(fSelected, path))
    {
        closeEditors();
        if (!fSession.setTrackFile(fSelected, path.toStdString()))
        {
            showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(path, QString::fromStdString(fSession.lastError())));
            return false;
        }
        refreshTracks();
        return true;
    }
    return addAudioTrack(path) >= 0;
}

void MainWindow::addEmptyTrack()
{
    const int track = fSession.addTrack(QStringLiteral("Audio %1").arg(fSession.tracks().size() + 1, 2, 10, QLatin1Char('0')).toStdString());
    if (track < 0)
    {
        showError(QStringLiteral("无法添加轨道：%1").arg(QString::fromStdString(fSession.lastError())));
        return;
    }
    fSelected = track;
    refreshTracks();
}

void MainWindow::addInstrumentTrack()
{
    const int track = fSession.addTrack(QStringLiteral("Instrument %1").arg(fSession.tracks().size() + 1, 2, 10, QLatin1Char('0')).toStdString(),
                                        chost::TrackKind::Instrument);
    if (track < 0)
    {
        showError(QStringLiteral("无法添加轨道：%1").arg(QString::fromStdString(fSession.lastError())));
        return;
    }
    loadInstrument(track, instruments::choiceFor({}));
    fSelected = track;
    refreshTracks();
}

bool MainWindow::loadInstrument(int track, const instruments::Choice& choice)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = !choice.spec.isEmpty() && fSession.setInstrument(track, choice.spec.toStdString());
    if (ok && !choice.state.isEmpty())
        fHost.loadPluginState(static_cast<uint>(fSession.tracks()[track].instrument), choice.state.toStdString());
    // the user's instrument may be uninstalled since: the bundled sounds still play the part
    const QString bundled = instruments::bundledSpec();
    if (!ok && choice.spec != bundled && !bundled.isEmpty())
        ok = fSession.setInstrument(track, bundled.toStdString());
    QApplication::restoreOverrideCursor();
    return ok;
}

void MainWindow::applyInstruments()
{
    closeEditors();
    int changed = 0;
    const int count = static_cast<int>(fSession.tracks().size());
    for (int i = 0; i < count; ++i)
    {
        const chost::Track& t = fSession.tracks()[i];
        if (t.kind != chost::TrackKind::Instrument)
            continue;
        const instruments::Choice choice = instruments::choiceFor(QString::fromStdString(t.role));
        if (choice.spec.toStdString() == t.instrumentSpec && choice.state.isEmpty())
            continue;
        statusBar()->showMessage(QStringLiteral("正在加载 %1…").arg(choice.name));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (loadInstrument(i, choice))
            ++changed;
    }
    const int keep = fSelected;
    refreshTracks();
    selectTrack(keep);
    statusBar()->showMessage(QStringLiteral("已给 %1 条 MIDI 轨换上设置的乐器").arg(changed), 6000);
}

void MainWindow::instrumentSettings()
{
    InstrumentDialog dialog(fPlugins, fDiscovery->isRunning(), this);
    connect(&dialog, &InstrumentDialog::applyRequested, this, &MainWindow::applyInstruments);
    dialog.exec();
}

void MainWindow::rememberInstrument()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (fSelected < 0 || fSelected >= static_cast<int>(tracks.size()) || tracks[fSelected].instrument < 0)
        return;
    const chost::Track& t = tracks[fSelected];
    const QString key = instruments::keyFor(QString::fromStdString(t.role));
    const uint id = static_cast<uint>(t.instrument);
    instruments::Choice choice{QString::fromStdString(t.instrumentSpec), QString::fromStdString(fHost.slot(id).name), {}};
    if (choice.spec.isEmpty())
    {
        showError(QStringLiteral("这个乐器是从旧工程里打开的，请先点“换乐器…”重新选一次。"));
        return;
    }
    // the sound chosen inside the plugin (a Kontakt library, a preset) comes back with it
    const QString state = instruments::statePath(key);
    if (fHost.savePluginState(id, state.toStdString()))
        choice.state = state;
    instruments::store(key, choice);
    statusBar()->showMessage(QStringLiteral("记住了：以后的“%1”都用 %2").arg(instruments::roleName(key), choice.name), 6000);
    refreshInstrument();
}

bool MainWindow::isDrumTrack(int track)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    return track >= 0 && track < static_cast<int>(tracks.size()) && tracks[track].kind == chost::TrackKind::Instrument &&
           !tracks[track].file.empty() && clip(QString::fromStdString(tracks[track].file)).drums;
}

QString MainWindow::instrumentText(int track)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (track < 0 || track >= static_cast<int>(tracks.size()) || tracks[track].instrument < 0)
        return QStringLiteral("MIDI · 没有乐器");
    const uint id = static_cast<uint>(tracks[track].instrument);
    QString text = QString::fromStdString(fHost.slot(id).name);
    if (isDrumTrack(track))
        return text + QStringLiteral(" · 鼓组");
    const std::vector<chost::HostEngine::MidiProgram> programs = fHost.midiPrograms(id);
    const int current = fHost.currentMidiProgram(id);
    if (current >= 0 && current < static_cast<int>(programs.size()))
        text += QStringLiteral(" · ") + QString::fromStdString(programs[static_cast<size_t>(current)].name);
    return text;
}

void MainWindow::chooseInstrument()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (fSelected < 0 || fSelected >= static_cast<int>(tracks.size()) || tracks[fSelected].kind != chost::TrackKind::Instrument)
    {
        showError(QStringLiteral("先选中一条乐器轨（MIDI 轨）。"));
        return;
    }
    PluginPicker picker(fPlugins, this, PluginPicker::Instruments);
    if (picker.exec() != QDialog::Accepted || picker.selectedSpec().isEmpty())
        return;
    const QString spec = picker.selectedSpec();
    closeEditors();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = fSession.setInstrument(fSelected, spec.toStdString());
    QApplication::restoreOverrideCursor();
    if (!ok)
    {
        showError(QStringLiteral("加载乐器失败：%1\n\n%2").arg(spec, QString::fromStdString(fSession.lastError())));
        refreshTracks();
        return;
    }
    const int keep = fSelected;
    refreshTracks();
    selectTrack(keep);

    statusBar()->showMessage(QStringLiteral("选好音色后，点检查器里的“记住这个音色”，以后同一种乐器就自动用它"), 8000);
}

void MainWindow::editInstrument()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (fSelected < 0 || fSelected >= static_cast<int>(tracks.size()) || tracks[fSelected].instrument < 0)
        return;
    const uint id = static_cast<uint>(tracks[fSelected].instrument);
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

void MainWindow::refreshInstrument()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    const bool show = fSelected >= 0 && fSelected < static_cast<int>(tracks.size()) && tracks[fSelected].kind == chost::TrackKind::Instrument;
    fInstrumentBox->setVisible(show);
    if (!show)
        return;
    const int id = tracks[fSelected].instrument;
    fInstrumentName->setText(id >= 0 ? QString::fromStdString(fHost.slot(static_cast<uint>(id)).name) : QStringLiteral("没有乐器"));
    fInstrumentEdit->setEnabled(id >= 0);
    fInstrumentNotes->setEnabled(!tracks[fSelected].file.empty());
    const QString key = instruments::keyFor(QString::fromStdString(tracks[fSelected].role));
    fInstrumentRemember->setEnabled(id >= 0);
    fInstrumentRemember->setText(key == QLatin1String("default") ? QStringLiteral("记住这个音色")
                                                                : QStringLiteral("记住这个音色（以后的%1都用它）").arg(instruments::roleName(key)));

    const QSignalBlocker blocker(fProgramBox);
    fProgramBox->clear();
    const std::vector<chost::HostEngine::MidiProgram> programs = id >= 0 ? fHost.midiPrograms(static_cast<uint>(id))
                                                                        : std::vector<chost::HostEngine::MidiProgram>{};
    for (const auto& p : programs)
        fProgramBox->addItem(QStringLiteral("%1  %2").arg(p.program + 1, 3, 10, QLatin1Char('0')).arg(QString::fromStdString(p.name)));
    // drums play on channel 10, which always has the drum kit whatever the sound chosen here
    fProgramBox->setVisible(!programs.empty() && !isDrumTrack(fSelected));
    if (id >= 0)
        fProgramBox->setCurrentIndex(fHost.currentMidiProgram(static_cast<uint>(id)));
}

void MainWindow::importAudio()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("导入音频 / MIDI 文件"), {}, kImportFilter);
    for (const QString& path : paths)
        addAudioTrack(path);
}

void MainWindow::replaceAudio()
{
    if (fSelected < 0)
        return;
    const bool instrument = fSession.tracks()[fSelected].kind == chost::TrackKind::Instrument;
    const QString path = QFileDialog::getOpenFileName(this, instrument ? QStringLiteral("替换 MIDI") : QStringLiteral("替换音频"), {},
                                                      instrument ? QStringLiteral("MIDI 文件 (*.mid *.midi);;所有文件 (*)") : kImportFilter);
    if (path.isEmpty())
        return;
    if (!fits(fSelected, path))
    {
        addAudioTrack(path); // audio can't go on an instrument track and the other way round
        return;
    }
    closeEditors();
    if (!fSession.setTrackFile(fSelected, path.toStdString()))
        showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(path, QString::fromStdString(fSession.lastError())));
    refreshTracks();
}

void MainWindow::removeSelectedTrack()
{
    if (fSelected < 0 || fSelected >= static_cast<int>(fSession.tracks().size()))
        return;
    closeEditors();
    const int track = fSelected;
    fSession.removeTrack(track);
    fColours.erase(fColours.begin() + track);
    fSelected = std::min(track, static_cast<int>(fSession.tracks().size()) - 1);
    refreshTracks();
}

void MainWindow::renameSelectedTrack(const QString& name)
{
    if (fSelected < 0 || name.trimmed().isEmpty() || name.toStdString() == fSession.tracks()[fSelected].name)
        return;
    fSession.renameTrack(fSelected, name.trimmed().toStdString());
    const int keep = fSelected;
    refreshTracks();
    selectTrack(keep);
}

void MainWindow::trackMenu(int track, const QPoint& globalPos)
{
    QMenu menu;
    if (track >= 0)
    {
        menu.addAction(QStringLiteral("重命名"), this, [this] {
            fInspectorName->setFocus();
            fInspectorName->selectAll();
        });
        const chost::Track& t = fSession.tracks()[track];
        if (t.kind == chost::TrackKind::Instrument)
        {
            menu.addAction(QStringLiteral("编辑音符…"), this, [this, track] { editNotes(track); })->setEnabled(!t.file.empty());
            menu.addAction(QStringLiteral("替换 MIDI…"), this, &MainWindow::replaceAudio);
            menu.addAction(QStringLiteral("换乐器…"), this, &MainWindow::chooseInstrument);
            menu.addAction(QStringLiteral("打开乐器界面"), this, &MainWindow::editInstrument)->setEnabled(t.instrument >= 0);
        }
        else
        {
            menu.addAction(QStringLiteral("替换音频…"), this, &MainWindow::replaceAudio);
            menu.addAction(QStringLiteral("AI 自动分轨…"), this, [this] { separateStems(); })->setEnabled(!t.file.empty());
            menu.addAction(QStringLiteral("AI 转 MIDI…"), this, [this] { transcribe(false); })->setEnabled(!t.file.empty());
        }
        menu.addAction(QStringLiteral("添加插入效果…"), this, &MainWindow::addInsert);
        menu.addSeparator();
        menu.addAction(QStringLiteral("删除轨道"), this, &MainWindow::removeSelectedTrack);
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("添加音轨"), this, &MainWindow::addEmptyTrack);
    menu.addAction(QStringLiteral("添加乐器轨（MIDI）"), this, &MainWindow::addInstrumentTrack);
    menu.addAction(QStringLiteral("导入音频 / MIDI 到新轨道…"), this, &MainWindow::importAudio);
    menu.exec(globalPos);
}

void MainWindow::dropFiles(const QStringList& paths, int track)
{
    if (paths.size() == 1 && isSessionFile(paths.front()))
    {
        openSession(paths.front());
        return;
    }
    if (track >= 0 && paths.size() == 1 && fits(track, paths.front()))
    {
        fSelected = track;
        closeEditors();
        if (!fSession.setTrackFile(track, paths.front().toStdString()))
            showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(paths.front(), QString::fromStdString(fSession.lastError())));
        refreshTracks();
        return;
    }
    for (const QString& path : paths)
        addAudioTrack(path);
}

// ---------------------------------------------------------------------------------------------------------------------
// inserts and mixing

void MainWindow::addInsert()
{
    PluginPicker picker(fPlugins, this);
    if (picker.exec() == QDialog::Accepted && !picker.selectedSpec().isEmpty())
        addPluginSpec(picker.selectedSpec());
}

bool MainWindow::addPluginSpec(const QString& spec)
{
    if (fSelected < 0)
        addEmptyTrack();
    if (fSelected < 0)
        return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = fSession.addInsert(fSelected, spec.toStdString());
    QApplication::restoreOverrideCursor();
    if (!ok)
    {
        showError(QStringLiteral("加载插件失败：%1\n\n%2").arg(spec, QString::fromStdString(fSession.lastError())));
        return false;
    }
    refreshInserts();
    return true;
}

void MainWindow::editInsert(int index)
{
    const std::vector<uint> ids = selectedInserts();
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
    const std::vector<uint> ids = selectedInserts();
    if (index >= 0 && index < static_cast<int>(ids.size()))
    {
        fHost.setActive(ids[static_cast<size_t>(index)], active);
        fSession.reroute();
    }
    refreshInserts();
}

void MainWindow::removeInsert(int index)
{
    // plugin ids shift after a removal, open editors would point at the wrong plugin
    closeEditors();
    fSession.removeInsert(fSelected, index);
    refreshInserts();
}

void MainWindow::moveInsert(int index, int delta)
{
    closeEditors();
    fSession.moveInsert(fSelected, index, delta);
    refreshInserts();
}

void MainWindow::setMuted(int track, bool muted)
{
    fSession.setMuted(track, muted);
    refreshMixState();
}

void MainWindow::setSolo(int track, bool solo)
{
    fSession.setSolo(track, solo);
    refreshMixState();
}

void MainWindow::setVolume(int track, float volume)
{
    fSession.setVolume(track, volume);
}

// ---------------------------------------------------------------------------------------------------------------------
// AI

void MainWindow::separateStems(bool autoStart)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (fSelected < 0 || fSelected >= static_cast<int>(tracks.size()) || tracks[fSelected].file.empty())
    {
        showError(QStringLiteral("先选中一条有音频的轨道，再做自动分轨。"));
        return;
    }
    if (fStemDialog && fStemDialog->isVisible())
    {
        fStemDialog->raise();
        fStemDialog->activateWindow();
        return;
    }
    delete fStemDialog;

    const QString source = QString::fromStdString(tracks[fSelected].file);
    fStemDialog = new StemDialog(QString::fromStdString(tracks[fSelected].name), source, this);

    // finds a track by its audio file, indexes move while tracks are added or removed
    auto trackOf = [this](const QString& path) {
        const std::vector<chost::Track>& all = fSession.tracks();
        for (size_t i = 0; i < all.size(); ++i)
            if (QString::fromStdString(all[i].file) == path)
                return static_cast<int>(i);
        return -1;
    };
    auto stems = std::make_shared<QHash<QString, QString>>(); // stem id -> file, for this run
    auto parts = std::make_shared<QHash<QString, int>>();     // group -> parts placed below it

    connect(fStemDialog, &StemDialog::stemReady, this, [=](const QString& id, const QString& label, const QString& path) {
        // the separated parts replace the original mix, and the finer parts replace their group
        if (stems->isEmpty())
            if (const int original = trackOf(source); original >= 0)
                fSession.setMuted(original, true);
        static const QStringList drumParts{QStringLiteral("kick"), QStringLiteral("snare"), QStringLiteral("toms"),
                                           QStringLiteral("hh"), QStringLiteral("ride"), QStringLiteral("crash")};
        static const QStringList vocalParts{QStringLiteral("lead"), QStringLiteral("backing")};
        const QString group = drumParts.contains(id) ? QStringLiteral("drums") : vocalParts.contains(id) ? QStringLiteral("vocals") : QString();
        if (!group.isEmpty() && stems->contains(group))
            if (const int parent = trackOf(stems->value(group)); parent >= 0)
                fSession.setMuted(parent, true);
        stems->insert(id, path);

        const int keep = fSelected;
        const int track = addAudioTrack(path, label);
        if (track < 0)
            return;
        fSession.setRole(track, id.toStdString());
        // the finer parts go right below their group and share its colour, like a folder in Cubase
        if (const int parent = group.isEmpty() ? -1 : trackOf(stems->value(group)); parent >= 0)
        {
            const int to = std::min(track, parent + 1 + (*parts)[group]++);
            fSession.moveTrack(track, to);
            fColours.erase(fColours.begin() + track);
            fColours.insert(fColours.begin() + to, fColours[parent]);
            --fNextColour; // the part didn't use up a colour of its own
            refreshTracks();
        }
        selectTrack(keep);
    });
    connect(fStemDialog, &StemDialog::separationFinished, this, [this](bool ok) {
        if (ok)
            statusBar()->showMessage(QStringLiteral("自动分轨完成"), 5000);
    });
    fStemDialog->setAutoStart(autoStart);
    fStemDialog->show();
}

void MainWindow::transcribe(bool allTracks, bool autoStart)
{
    if (fTranscribeDialog && fTranscribeDialog->isVisible())
    {
        fTranscribeDialog->raise();
        fTranscribeDialog->activateWindow();
        return;
    }
    const std::vector<chost::Track>& tracks = fSession.tracks();
    std::vector<TranscribeDialog::Source> sources;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const chost::Track& t = tracks[i];
        if (t.kind != chost::TrackKind::Audio || t.file.empty())
            continue;
        if (!allTracks && static_cast<int>(i) != fSelected)
            continue;
        TranscribeDialog::Source s;
        s.name = QString::fromStdString(t.name);
        s.file = QString::fromStdString(t.file);
        s.role = t.role.empty() ? guessRole(s.name) : QString::fromStdString(t.role);
        // by default the single parts you hear (stems not replaced by finer ones), and the selection
        s.checked = !allTracks || static_cast<int>(i) == fSelected || (!s.role.isEmpty() && !t.muted);
        sources.push_back(s);
    }
    if (sources.empty())
    {
        showError(allTracks ? QStringLiteral("没有可以转换的音轨。先导入音频，最好先用“AI 分轨”分成单个乐器。")
                            : QStringLiteral("先选中一条有音频的轨道，再转 MIDI。"));
        return;
    }
    delete fTranscribeDialog;
    fTranscribeDialog = new TranscribeDialog(sources, fSession.tempo(), this);

    // the drum parts of one run end up on one track, so a single drum instrument plays the whole kit
    struct Part { QString source, midi; };
    auto drumParts = std::make_shared<std::vector<Part>>();
    connect(fTranscribeDialog, &TranscribeDialog::midiReady, this,
            [this, drumParts](const QString& sourceFile, const QString& role, const QString& trackName, const QString& midiPath) {
        static const QStringList parts{QStringLiteral("kick"), QStringLiteral("snare"), QStringLiteral("toms"),
                                       QStringLiteral("hh"), QStringLiteral("ride"), QStringLiteral("crash")};
        if (parts.contains(role))
            drumParts->push_back({sourceFile, midiPath});
        else
            placeMidiTrack({sourceFile}, midiPath, trackName + QStringLiteral(" MIDI"), role);
    });
    connect(fTranscribeDialog, &TranscribeDialog::transcriptionFinished, this, [this, drumParts](bool ok) {
        if (drumParts->size() == 1)
        {
            const Part& p = drumParts->front();
            placeMidiTrack({p.source}, p.midi, QFileInfo(p.source).completeBaseName() + QStringLiteral(" MIDI"),
                           guessRole(QFileInfo(p.midi).completeBaseName()));
        }
        else if (drumParts->size() > 1)
        {
            chost::MidiClip kit;
            kit.name = "Drums";
            QStringList sources;
            for (const Part& p : *drumParts)
            {
                chost::MidiClip clip;
                std::string error;
                if (chost::readMidiFile(p.midi.toStdString(), clip, error))
                {
                    kit.tempo = clip.tempo;
                    kit.notes.insert(kit.notes.end(), clip.notes.begin(), clip.notes.end());
                }
                sources << p.source;
            }
            std::stable_sort(kit.notes.begin(), kit.notes.end(), [](const chost::MidiNote& a, const chost::MidiNote& b) { return a.start < b.start; });
            const QString path = QFileInfo(drumParts->front().midi).absoluteDir().filePath(QStringLiteral("drum-kit.mid"));
            std::string error;
            if (chost::writeMidiFile(path.toStdString(), kit, error))
                placeMidiTrack(sources, path, QStringLiteral("鼓 MIDI"), QStringLiteral("drums"));
            else
                showError(QString::fromStdString(error));
        }
        drumParts->clear();
        if (ok)
            statusBar()->showMessage(QStringLiteral("转 MIDI 完成"), 5000);
    });
    fTranscribeDialog->setAutoStart(autoStart);
    fTranscribeDialog->show();
}

void MainWindow::placeMidiTrack(const QStringList& sources, const QString& midiPath, const QString& name, const QString& role)
{
    auto trackOf = [this](const QString& path) {
        const std::vector<chost::Track>& all = fSession.tracks();
        for (size_t i = 0; i < all.size(); ++i)
            if (QString::fromStdString(all[i].file) == path)
                return static_cast<int>(i);
        return -1;
    };
    // converting again replaces the earlier MIDI track of the same part
    if (const int old = trackOf(midiPath); old >= 0)
    {
        closeEditors();
        fSession.removeTrack(old);
        fColours.erase(fColours.begin() + old);
    }
    fClips.remove(midiPath);
    const int keep = fSelected;
    const int track = addMidiTrack(midiPath, name, role);
    if (track < 0)
        return;

    // the MIDI is what you hear from now on: its audio is muted
    int below = -1;
    for (const QString& source : sources)
        if (const int t = trackOf(source); t >= 0)
        {
            fSession.setMuted(t, true);
            below = std::max(below, t);
        }
    // it goes below its audio, in the same colour; a whole drum kit below the drum parts
    if (below >= 0)
    {
        const int to = std::min(track, below + 1);
        const QColor colour = fColours[below];
        fSession.moveTrack(track, to);
        fColours.erase(fColours.begin() + track);
        fColours.insert(fColours.begin() + to, colour);
        --fNextColour;
        refreshTracks();
    }
    selectTrack(keep);
}

// ---------------------------------------------------------------------------------------------------------------------
// MIDI editing

int MainWindow::trackOfFile(const QString& path) const
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size(); ++i)
        if (QString::fromStdString(tracks[i].file) == path)
            return static_cast<int>(i);
    return -1;
}

void MainWindow::editNotes(int track)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (track < 0 || track >= static_cast<int>(tracks.size()) || tracks[track].kind != chost::TrackKind::Instrument ||
        tracks[track].file.empty())
        return;
    selectTrack(track);
    const QString path = QString::fromStdString(tracks[track].file);
    if (fPianoRoll && fPianoRoll->midiPath() == path)
    {
        fPianoRoll->raise();
        fPianoRoll->activateWindow();
        return;
    }
    delete fPianoRoll;
    fPianoRoll = new PianoRollDialog(QString::fromStdString(tracks[track].name), path, trackColour(track), this);
    fPianoRoll->setAttribute(Qt::WA_DeleteOnClose);

    connect(fPianoRoll, &PianoRollDialog::saved, this, [this](const QString& oldPath, const QString& newPath) {
        const int t = trackOfFile(oldPath);
        if (t < 0)
            return;
        fSession.reloadMidiFile(t, newPath.toStdString());
        fClips.remove(oldPath);
        fClips.remove(newPath);
        const int keep = fSelected;
        refreshTracks();
        selectTrack(keep);
    });
    connect(fPianoRoll, &PianoRollDialog::audition, this, [this](const QString& midiPath, int pitch, int channel) {
        const int t = trackOfFile(midiPath);
        if (t < 0 || fSession.tracks()[t].instrument < 0)
            return;
        const uint id = static_cast<uint>(fSession.tracks()[t].instrument);
        fHost.sendNote(id, static_cast<uint8_t>(channel), static_cast<uint8_t>(pitch), 100);
        QTimer::singleShot(300, this, [this, id, channel, pitch] {
            if (id < fHost.pluginCount())
                fHost.sendNote(id, static_cast<uint8_t>(channel), static_cast<uint8_t>(pitch), 0);
        });
    });
    fPianoRoll->show();
}

// ---------------------------------------------------------------------------------------------------------------------
// plugins

void MainWindow::editPluginFolders()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("插件文件夹"));
    dialog.resize(560, 360);
    auto* layout = new QVBoxLayout(&dialog);

    auto* help = new QLabel(QStringLiteral("chost 会自动扫描系统默认的 VST3 和 VST2 文件夹。如果你的插件装在别的地方（比如 D:\\VST、"
                                           "D:\\Native Instruments），把那个文件夹加到这里，VST2、VST3、LV2 插件都会被找到。"));
    help->setWordWrap(true);
    layout->addWidget(help);

    auto* defaults = new QLabel;
    defaults->setWordWrap(true);
    defaults->setStyleSheet(QStringLiteral("color: #8b9096;"));
    std::vector<std::string> extra = chost::extraPluginFolders();
    chost::setExtraPluginFolders({});
    defaults->setText(QStringLiteral("默认位置：\nVST3：%1\nVST2：%2")
                          .arg(QString::fromStdString(chost::defaultPluginPath(CarlaBackend::PLUGIN_VST3)),
                               QString::fromStdString(chost::defaultPluginPath(CarlaBackend::PLUGIN_VST2))));
    chost::setExtraPluginFolders(extra);
    layout->addWidget(defaults);

    auto* list = new QListWidget;
    for (const std::string& folder : extra)
        list->addItem(QString::fromStdString(folder));
    layout->addWidget(list, 1);

    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("添加文件夹…"));
    auto* remove = new QPushButton(QStringLiteral("移除"));
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    layout->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存并重新扫描"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    connect(add, &QPushButton::clicked, &dialog, [&] {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("选择插件文件夹"));
        if (!dir.isEmpty() && list->findItems(QDir::toNativeSeparators(dir), Qt::MatchExactly).isEmpty())
            list->addItem(QDir::toNativeSeparators(dir));
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] { delete list->currentItem(); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;
    QStringList folders;
    std::vector<std::string> paths;
    for (int i = 0; i < list->count(); ++i)
    {
        folders << list->item(i)->text();
        paths.push_back(list->item(i)->text().toStdString());
    }
    QSettings().setValue(kPluginFoldersKey, folders);
    chost::setExtraPluginFolders(paths);
    rescanPlugins();
}

void MainWindow::rescanPlugins()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    fPluginFiles = chost::scanPlugins();
    QApplication::restoreOverrideCursor();
    showPlugins();
    statusBar()->showMessage(QStringLiteral("找到 %1 个插件").arg(fPlugins.size()), 5000);
    // what the VST files hold is found out in the background, known ones come from the cache right away
    fDiscovery->start(fPluginFiles);
}

void MainWindow::showPlugins()
{
    fPlugins = fDiscovery->resolve(fPluginFiles);
    fillPluginTree(fBrowser, fPlugins);
    filterPluginTree(fBrowser, fSearch->text());
}

// ---------------------------------------------------------------------------------------------------------------------
// sessions and transport

void MainWindow::newSession()
{
    closeEditors();
    fHost.pause();
    fHost.removeAllPlugins();
    fSession.init();
    fColours.clear();
    fSelected = -1;
    fSessionPath.clear();
    fAnalysisTried = false;
    refreshTracks();
    refreshSongInfo();
    updateTitle();
    seek(0.0);
}

bool MainWindow::openSession(const QString& path)
{
    closeEditors();
    fHost.pause();
    if (!fSession.load(path.toStdString()))
    {
        showError(QStringLiteral("无法打开工程：%1\n\n%2").arg(path, QString::fromStdString(fSession.lastError())));
        fHost.removeAllPlugins();
        fSession.init();
        fSessionPath.clear();
    }
    else
    {
        fSessionPath = path;
    }
    fColours.clear();
    fSelected = -1;
    fAnalysisTried = !fSession.key().empty();
    refreshTracks();
    refreshSongInfo();
    updateTitle();
    seek(0.0);
    return !fSessionPath.isEmpty();
}

void MainWindow::openSessionDialog()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开工程"), {}, QStringLiteral("chost 工程 (*.chost)"));
    if (!path.isEmpty())
        openSession(path);
}

void MainWindow::saveSession()
{
    if (fSessionPath.isEmpty())
    {
        saveSessionAs();
        return;
    }
    if (fSession.save(fSessionPath.toStdString()))
        statusBar()->showMessage(QStringLiteral("已保存：%1").arg(fSessionPath), 5000);
    else
        showError(QStringLiteral("保存失败：%1").arg(QString::fromStdString(fSession.lastError())));
}

void MainWindow::saveSessionAs()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存工程"), QStringLiteral("project.chost"),
                                                QStringLiteral("chost 工程 (*.chost)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".chost");
    fSessionPath = path;
    updateTitle();
    saveSession();
}

// ---------------------------------------------------------------------------------------------------------------------
// tempo and key, export, mastering

void MainWindow::setTempo(double bpm)
{
    if (bpm <= 0.0)
        return;
    fSession.setTempo(bpm);
    fArrange->setTempo(fSession.tempo());
    refreshSongInfo();
}

void MainWindow::refreshSongInfo()
{
    {
        const QSignalBlocker blocker(fTempoBox);
        fTempoBox->setValue(fSession.tempo());
    }
    fArrange->setTempo(fSession.tempo());
    fKeyLabel->setText(keyText(fSession.key()) + QStringLiteral(" · 4/4"));
    fKeyLabel->setToolTip(fSession.key().empty()
                              ? QStringLiteral("调性还不知道：点“识别”让 AI 听一下")
                              : QStringLiteral("调性：%1（关系调 %2，用的音完全一样）").arg(keyText(fSession.key()), relativeKeyText(fSession.key())));
}

void MainWindow::analyzeSong(bool quiet)
{
    if (fAnalysis->isRunning())
        return;
    // the selected audio track, or the first one: usually the whole song
    const std::vector<chost::Track>& tracks = fSession.tracks();
    int track = -1;
    if (fSelected >= 0 && fSelected < static_cast<int>(tracks.size()) && tracks[fSelected].kind == chost::TrackKind::Audio
        && !tracks[fSelected].file.empty())
        track = fSelected;
    for (size_t i = 0; track < 0 && i < tracks.size(); ++i)
        if (tracks[i].kind == chost::TrackKind::Audio && !tracks[i].file.empty() && tracks[i].role != kMasterRole)
            track = static_cast<int>(i);
    if (track < 0)
    {
        if (!quiet)
            showError(QStringLiteral("先导入一首歌（音频文件），再识别速度和调性。"));
        return;
    }
    fAnalysisTried = true;
    fAnalysisQuiet = quiet;
    statusBar()->showMessage(QStringLiteral("正在识别速度和调性：%1…").arg(QString::fromStdString(tracks[track].name)));
    fAnalysis->start(QStringLiteral("chost_analyze.py"), {QStringLiteral("--input"), QString::fromStdString(tracks[track].file)});
}

QString MainWindow::projectName() const
{
    if (!fSessionPath.isEmpty())
        return QFileInfo(fSessionPath).completeBaseName();
    for (const chost::Track& t : fSession.tracks())
        if (t.kind == chost::TrackKind::Audio && !t.file.empty() && t.role != kMasterRole)
            return QFileInfo(QString::fromStdString(t.file)).completeBaseName();
    return QStringLiteral("mix");
}

QString MainWindow::outputFolder() const
{
    if (!fSessionPath.isEmpty())
        return QFileInfo(fSessionPath).absolutePath();
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QDir(music.isEmpty() ? QDir::homePath() : music).filePath(QStringLiteral("chost"));
}

RenderSnapshot MainWindow::renderSnapshot(const QString& name)
{
    RenderSnapshot snapshot;
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/render");
    if (!QDir().mkpath(folder))
    {
        snapshot.error = QStringLiteral("没法写入 %1").arg(folder);
        return snapshot;
    }
    // one file per window: an export and a mastering may run at the same time
    const QString path = folder + QLatin1Char('/') + name + QStringLiteral(".chost");
    if (!fSession.isReady() || !fSession.save(path.toStdString()))
    {
        snapshot.error = QString::fromStdString(fSession.lastError());
        return snapshot;
    }
    snapshot.session = path;
    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].role == kMasterRole)
            snapshot.masters << static_cast<int>(i);
    return snapshot;
}

void MainWindow::exportMix(bool autoStart)
{
    if (fSession.tracks().empty())
    {
        showError(QStringLiteral("工程里还没有轨道，没有可以导出的。"));
        return;
    }
    if (!fExportDialog)
    {
        std::vector<ExportDialog::Track> tracks;
        const std::vector<chost::Track>& all = fSession.tracks();
        for (size_t i = 0; i < all.size(); ++i)
            if (!all[i].muted && all[i].role != kMasterRole)
                tracks.push_back({static_cast<int>(i), QString::fromStdString(all[i].name)});
        const QString path = QDir(outputFolder()).filePath(projectName() + QStringLiteral(".wav"));
        fExportDialog = new ExportDialog(path, tracks, [this] { return renderSnapshot(QStringLiteral("export")); }, this);
        fExportDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(fExportDialog, &ExportDialog::exported, this, [this](bool ok, const QString& path) {
            if (ok)
                statusBar()->showMessage(QStringLiteral("已导出：%1").arg(QDir::toNativeSeparators(path)), 8000);
        });
    }
    fExportDialog->show();
    fExportDialog->raise();
    fExportDialog->activateWindow();
    fExportDialog->setAutoStart(autoStart);
}

void MainWindow::masterMix(bool autoStart)
{
    bool any = false;
    for (const chost::Track& t : fSession.tracks())
        any = any || (t.role != kMasterRole && !t.file.empty());
    if (!any)
    {
        showError(QStringLiteral("工程里还没有可以做母带的音频或 MIDI 轨道。"));
        return;
    }
    if (!fMasterDialog)
    {
        const QString path = QDir(outputFolder()).filePath(projectName() + QStringLiteral("_母带.wav"));
        fMasterDialog = new MasterDialog(path, [this] { return renderSnapshot(QStringLiteral("master")); }, this);
        fMasterDialog->setAttribute(Qt::WA_DeleteOnClose);
        // the master track of an earlier run goes: the mix to master is without it, and its file is
        // about to be written again
        auto removeMasters = [this] {
            bool removed = false;
            for (int i = static_cast<int>(fSession.tracks().size()) - 1; i >= 0; --i)
                if (fSession.tracks()[i].role == kMasterRole)
                {
                    if (!removed)
                        closeEditors();
                    fClips.remove(QString::fromStdString(fSession.tracks()[i].file));
                    fSession.removeTrack(i);
                    if (i < static_cast<int>(fColours.size()))
                        fColours.erase(fColours.begin() + i);
                    removed = true;
                }
            if (removed)
            {
                fSelected = -1;
                refreshTracks();
                refreshMixState();
            }
        };
        connect(fMasterDialog, &MasterDialog::aboutToStart, this, removeMasters);
        connect(fMasterDialog, &MasterDialog::compareRequested, this, [this, removeMasters](const QString& path) {
            fAnalysisTried = true; // a master is no song to analyse
            removeMasters();
            const int track = addAudioTrack(path, QStringLiteral("母带"));
            if (track >= 0)
                fSession.setRole(track, kMasterRole);
            refreshTracks();
        });
        connect(fMasterDialog, &MasterDialog::listen, this, &MainWindow::listenToMaster);
        connect(fMasterDialog, &MasterDialog::playToggled, this, &MainWindow::togglePlay);
        connect(fMasterDialog, &MasterDialog::mastered, this, [this](bool ok, const QString& path) {
            if (ok)
                statusBar()->showMessage(QStringLiteral("母带完成：%1").arg(QDir::toNativeSeparators(path)), 8000);
        });
    }
    fMasterDialog->show();
    fMasterDialog->raise();
    fMasterDialog->activateWindow();
    fMasterDialog->setAutoStart(autoStart);
}

// A/B: the master alone (soloed), or the mix as it was (master muted, the solo off)
void MainWindow::listenToMaster(bool master)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].role == kMasterRole)
        {
            fSession.setSolo(static_cast<int>(i), master);
            fSession.setMuted(static_cast<int>(i), !master);
        }
    refreshMixState();
}

void MainWindow::updateTitle()
{
    setWindowTitle(fSessionPath.isEmpty() ? QStringLiteral("chost") : QStringLiteral("chost — %1").arg(QFileInfo(fSessionPath).fileName()));
}

void MainWindow::play()
{
    fHost.play();
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
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (!paths.isEmpty())
        dropFiles(paths, -1);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (fStemDialog)
        fStemDialog->close();
    if (fTranscribeDialog)
        fTranscribeDialog->close();
    QMainWindow::closeEvent(event);
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
