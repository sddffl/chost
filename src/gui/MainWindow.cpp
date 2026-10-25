#include "MainWindow.h"

#include "ArrangeDialog.h"
#include "ExportDialog.h"
#include "InstrumentDialog.h"
#include "Autosave.h"
#include "ChordPad.h"
#include "ClaudeConnect.h"
#include "Control.h"
#include "History.h"
#include "Instruments.h"
#include "MasterDialog.h"
#include "PianoRoll.h"
#include "ScoreDialog.h"
#include "PluginDiscovery.h"
#include "PluginWindows.h"
#include "RenderJob.h"
#include "Resources.h"
#include "ScriptJob.h"
#include "StemDialog.h"
#include "Theme.h"
#include "TranscribeDialog.h"
#include "TuneDialog.h"
#include "Widgets.h"
#include "core/AudioConvert.h"
#include "core/Click.h"

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
#include <QInputDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSplitter>
#include <QStatusBar>
#include <QElapsedTimer>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUndoView>
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
    fHistory = new History(fSession, this);
    connect(fHistory, &History::restored, this, &MainWindow::afterUndo);
    connect(fHistory, &History::modifiedChanged, this, &MainWindow::updateTitle);
    fAutosave = new Autosave(fSession, *fHistory, [this] { return fSessionPath; }, this);

    std::vector<std::string> folders;
    for (const QString& folder : QSettings().value(kPluginFoldersKey).toStringList())
        folders.push_back(QDir::toNativeSeparators(folder).toStdString());
    chost::setExtraPluginFolders(folders);

    buildMenus();
    buildTransport();
    {
        const QSignalBlocker blocker(fClickAction);
        fClickAction->setChecked(QSettings().value(QStringLiteral("metronome/on"), false).toBool());
    }

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
    connect(fArrange, &ArrangeView::foldToggled, this, &MainWindow::setFolded);
    connect(fArrange, &ArrangeView::trackMenuRequested, this, &MainWindow::trackMenu);
    connect(fArrange, &ArrangeView::filesDropped, this, &MainWindow::dropFiles);
    connect(fArrange, &ArrangeView::clipDoubleClicked, this, &MainWindow::editNotes);
    connect(fArrange, &ArrangeView::clipMoveRequested, this, [this](int lane, double seconds) {
        chost::ClipEdit edit;
        edit.kind = chost::ClipEditKind::Move;
        edit.seconds = seconds;
        const double beats = seconds * fSession.tempo() / 60.0;
        editClip(lane, edit, QStringLiteral("移动片段 %1 拍").arg(beats > 0 ? QStringLiteral("+") + QString::number(beats, 'g', 3) : QString::number(beats, 'g', 3)));
    });
    connect(fArrange, &ArrangeView::rangeChanged, this, [this](double start, double end) {
        if (end > start)
        {
            const double beat = 60.0 / fSession.tempo();
            auto where = [beat](double t) {
                const int sixteenths = static_cast<int>(std::lround(t / beat * 4.0));
                return QStringLiteral("%1.%2").arg(sixteenths / 16 + 1).arg(sixteenths / 4 % 4 + 1);
            };
            statusBar()->showMessage(QStringLiteral("选区 %1 – %2（%3 秒）：右键轨道可以对选区淡入、淡出、静音、调音量；Esc 取消")
                                         .arg(where(start), where(end)).arg(end - start, 0, 'f', 2), 10000);
        }
    });
    connect(fArrange, &ArrangeView::markerMoved, this, [this](int marker, double seconds) {
        std::vector<chost::Marker> markers = fSession.markers();
        if (marker < 0 || marker >= static_cast<int>(markers.size()))
            return;
        markers[static_cast<size_t>(marker)].seconds = seconds;
        fSession.setMarkers(markers);
        refreshMarkers();
        fHistory->record(QStringLiteral("移动标记"));
    });
    connect(fArrange, &ArrangeView::markerRenameRequested, this, &MainWindow::renameMarker);
    connect(fArrange, &ArrangeView::markerMenuRequested, this, &MainWindow::markerMenu);

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
        fHistory->record(QStringLiteral("识别速度和调性"), QStringLiteral("analysis"));
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

    // an AI assistant working the project through chost-mcp
    fControl = new Control(*this);
    fControl->listen();
}

MainWindow::~MainWindow()
{
    fTimer->stop();
    delete fStemDialog;
    delete fTranscribeDialog;
    delete fPianoRoll;
    delete fExportDialog;
    delete fMasterDialog;
    delete fArrangeDialog;
    delete fChordPad;
    delete fScoreDialog;
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
    file->addAction(QStringLiteral("以前的版本…"), this, &MainWindow::openVersion);
    file->addSeparator();
    file->addAction(QStringLiteral("导入音频 / MIDI 文件…"), QKeySequence(QStringLiteral("Ctrl+I")), this, &MainWindow::importAudio);
    file->addAction(QStringLiteral("导出混音（WAV）…"), QKeySequence(QStringLiteral("Ctrl+E")), this, [this] { exportMix(); });
    file->addAction(QStringLiteral("出谱（五线谱 PDF / MusicXML）…"), QKeySequence(QStringLiteral("Ctrl+Shift+P")), this, [this] { scoreDialog(); });
    file->addSeparator();
    file->addAction(QStringLiteral("退出"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(QStringLiteral("编辑"));
    QAction* undo = fHistory->stack()->createUndoAction(this, QStringLiteral("撤销"));
    undo->setShortcuts(QKeySequence::Undo);
    edit->addAction(undo);
    QAction* redo = fHistory->stack()->createRedoAction(this, QStringLiteral("重做"));
    redo->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Y")), QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    edit->addAction(redo);
    edit->addSeparator();
    edit->addAction(QStringLiteral("操作历史…"), this, [this] {
        // every step so far: clicking one goes back (or forward) to it
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("操作历史"));
        dialog.resize(360, 460);
        auto* layout = new QVBoxLayout(&dialog);
        auto* view = new QUndoView(fHistory->stack());
        view->setEmptyLabel(QStringLiteral("打开工程时"));
        layout->addWidget(view);
        auto* hint = new QLabel(QStringLiteral("点一步就回到那一步之后的样子，再点下面的也能重新回来。"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        dialog.exec();
    });

    QMenu* track = menuBar()->addMenu(QStringLiteral("轨道"));
    track->addAction(QStringLiteral("添加音轨"), QKeySequence(QStringLiteral("Ctrl+T")), this, &MainWindow::addEmptyTrack);
    track->addAction(QStringLiteral("添加乐器轨（MIDI）"), this, &MainWindow::addInstrumentTrack);
    track->addAction(QStringLiteral("导入音频 / MIDI 到新轨道…"), this, &MainWindow::importAudio);
    track->addAction(QStringLiteral("替换选中轨道的文件…"), this, &MainWindow::replaceAudio);
    track->addAction(QStringLiteral("选择乐器…"), this, &MainWindow::chooseInstrument);
    track->addSeparator();
    // with a track selected, it goes into the new group or sends to the new effect
    auto selectedOnly = [this] {
        const bool real = fSelected >= 0 && fSelected < static_cast<int>(fSession.tracks().size());
        return real ? std::vector<int>{fSelected} : std::vector<int>{};
    };
    track->addAction(QStringLiteral("新建编组（文件夹），放入选中轨道"), QKeySequence(QStringLiteral("Ctrl+G")), this,
                     [this, selectedOnly] { addBus(BusKind::Group, selectedOnly()); });
    track->addAction(QStringLiteral("新建混响效果轨，选中轨道发送过去"), this, [this, selectedOnly] { addBus(BusKind::Reverb, selectedOnly()); });
    track->addAction(QStringLiteral("新建延迟效果轨，选中轨道发送过去"), this, [this, selectedOnly] { addBus(BusKind::Delay, selectedOnly()); });
    track->addSeparator();
    track->addAction(QStringLiteral("删除选中轨道"), QKeySequence(QStringLiteral("Ctrl+Del")), this, &MainWindow::removeSelectedTrack);

    QMenu* transport = menuBar()->addMenu(QStringLiteral("走带"));
    transport->addAction(QStringLiteral("播放 / 暂停（空格）"), this, &MainWindow::togglePlay);
    transport->addAction(QStringLiteral("停止"), this, &MainWindow::stop);
    transport->addAction(QStringLiteral("回到开头"), this, [this] { seek(0.0); });
    transport->addSeparator();
    transport->addAction(QStringLiteral("在播放位置添加标记"), QKeySequence(QStringLiteral("Ctrl+M")), this, [this] {
        const int marker = addMarker(playhead());
        renameMarker(marker);
    });
    transport->addAction(QStringLiteral("跳到上一个标记"), QKeySequence(QStringLiteral("Alt+Left")), this, [this] { jumpToMarker(-1); });
    transport->addAction(QStringLiteral("跳到下一个标记"), QKeySequence(QStringLiteral("Alt+Right")), this, [this] { jumpToMarker(1); });
    transport->addSeparator();
    fClickAction = transport->addAction(QStringLiteral("节拍器"));
    fClickAction->setShortcut(QKeySequence(QStringLiteral("C")));
    fClickAction->setCheckable(true);
    fClickAction->setToolTip(QStringLiteral("节拍器（C）：按歌曲速度打拍子，第一拍重音；不会进导出的混音"));
    connect(fClickAction, &QAction::toggled, this, &MainWindow::setMetronome);
    QMenu* clickVolume = transport->addMenu(QStringLiteral("节拍器音量"));
    for (const auto& [text, db] : {std::pair{QStringLiteral("小声"), -18.0}, {QStringLiteral("正常"), -10.0}, {QStringLiteral("大声"), -4.0}})
        clickVolume->addAction(text, this, [this, db = db] { setMetronomeVolume(db); });

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
    ai->addAction(QStringLiteral("自动编曲…"), QKeySequence(QStringLiteral("Ctrl+Shift+A")), this, [this] { arrange(); });
    ai->addAction(QStringLiteral("AI 修音…"), QKeySequence(QStringLiteral("Ctrl+Shift+T")), this, [this] { tune(); });
    ai->addAction(QStringLiteral("和弦板（和弦提示）…"), QKeySequence(QStringLiteral("Ctrl+Shift+K")), this, [this] { chordPad(); });
    ai->addSeparator();
    ai->addAction(QStringLiteral("识别速度和调性"), this, [this] { analyzeSong(); });
    ai->addAction(QStringLiteral("AI 母带…"), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this, [this] { masterMix(); });
    ai->addSeparator();
    ai->addAction(QStringLiteral("连接 Claude（让 Claude 操控 chost）…"), this, [this] { claude::showDialog(this); });
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
    auto* click = new QToolButton;
    click->setObjectName(QStringLiteral("transport"));
    click->setDefaultAction(fClickAction);
    click->setText(QStringLiteral("♩"));
    connect(fClickAction, &QAction::changed, click, [click] { click->setText(QStringLiteral("♩")); });
    bar->addWidget(click);

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
    auto* arranging = new QToolButton;
    arranging->setText(QStringLiteral("AI 编曲"));
    arranging->setToolTip(QStringLiteral("按风格自动写出鼓、贝斯、键盘、吉他和弦乐"));
    bar->addWidget(arranging);
    connect(arranging, &QToolButton::clicked, this, [this] { arrange(); });
    auto* tuning = new QToolButton;
    tuning->setText(QStringLiteral("AI 修音"));
    tuning->setToolTip(QStringLiteral("把人声里跑调的音逐个修准，颤音和滑音都保留"));
    bar->addWidget(tuning);
    connect(tuning, &QToolButton::clicked, this, [this] { tune(); });
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
    fInspectorTune = new QPushButton(QStringLiteral("AI 修音…"));
    fInspectorTune->setToolTip(QStringLiteral("把这条人声里跑调的音修准"));
    body->addWidget(fInspectorTune);
    connect(fInspectorTune, &QPushButton::clicked, this, [this] { tune(); });

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
    connect(fOutputStrip, &ChannelStrip::volumeChanged, this, [this](float v) {
        fSession.setMasterVolume(v);
        fHistory->record(QStringLiteral("主音量"), QStringLiteral("master-volume"));
    });

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

    if (carry.isEmpty() || !fSession.load(carry.toStdString()))
        fSession.init();
    if (!carry.isEmpty())
    {
        QFile::remove(carry);
        QFile::remove(QDir::temp().filePath(QStringLiteral("chost-driver-switch.carxp")));
    }
    // the plugins were all loaded anew: the history can't refer to them any more
    fHistory->reset(fHistory->isModified());
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
    if (fChordPad)
        fChordPad->setPlayhead(seconds, playing);

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
    const std::vector<chost::Track>& tracks = fSession.tracks();
    return track >= 0 && track < static_cast<int>(tracks.size()) && tracks[track].colour ? QColor(QRgb(tracks[track].colour))
                                                                                          : theme::trackColor;
}

void MainWindow::refreshTracks()
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    // a new track takes the next colour; the colours stay with the tracks (and in the project file)
    for (size_t i = 0; i < tracks.size(); ++i)
        if (!tracks[i].colour)
            fSession.setColour(static_cast<int>(i), theme::trackColour(fNextColour++).rgb() & 0xffffff);

    std::vector<ArrangeView::Lane> lanes;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const chost::Track& t = tracks[i];
        ArrangeView::Lane lane;
        lane.name = QString::fromStdString(t.name);
        lane.muted = t.muted;
        lane.solo = t.solo;
        lane.colour = trackColour(static_cast<int>(i));
        lane.midi = t.kind == chost::TrackKind::Instrument;
        lane.kindText = lane.midi ? instrumentText(static_cast<int>(i)) : QStringLiteral("音频");
        lane.depth = depthOf(static_cast<int>(i));
        for (int up = fSession.trackOf(t.output), steps = 0; up >= 0 && steps < 64; up = fSession.trackOf(tracks[static_cast<size_t>(up)].output), ++steps)
            lane.hidden = lane.hidden || tracks[static_cast<size_t>(up)].folded;
        if (t.kind == chost::TrackKind::Bus)
        {
            lane.folder = true;
            lane.folded = t.folded;
            int members = 0, senders = 0;
            for (const chost::Track& other : tracks)
            {
                if (other.output == t.uid)
                {
                    ++members;
                    if (!other.file.empty())
                        lane.seconds = std::max(lane.seconds, clip(QString::fromStdString(other.file)).seconds);
                }
                senders += static_cast<int>(std::count_if(other.sends.begin(), other.sends.end(), [&](const chost::Send& x) { return x.bus == t.uid; }));
            }
            // a group's band covers its groups' tracks too
            for (size_t k = 0; k < tracks.size(); ++k)
                if (k != i && fSession.feeds(tracks[k].uid, t.uid) && !tracks[k].file.empty())
                    lane.seconds = std::max(lane.seconds, clip(QString::fromStdString(tracks[k].file)).seconds);
            QStringList parts;
            if (members > 0 || senders == 0)
                parts << QStringLiteral("编组 · %1 条轨道").arg(members);
            if (senders > 0)
                parts << QStringLiteral("效果 · %1 个发送").arg(senders);
            lane.kindText = parts.join(QStringLiteral(" · "));
        }
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
        strip->setColour(trackColour(static_cast<int>(i)));
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
        connect(strip, &ChannelStrip::outputMenuRequested, this, [this, index](const QPoint& at) { outputMenu(index, at); });
        connect(strip, &ChannelStrip::sendMenuRequested, this, [this, index](int slot, const QPoint& at) { sendMenu(index, slot, at); });
        connect(strip, &ChannelStrip::sendLevelChanged, this, [this, index](int slot, float level) {
            const std::vector<chost::Send>& sends = fSession.tracks()[static_cast<size_t>(index)].sends;
            if (slot < static_cast<int>(sends.size()))
                setTrackSend(index, fSession.trackOf(sends[static_cast<size_t>(slot)].bus), level);
        });
        fStrips.push_back(strip);
    }
    fOutputStrip->setVolume(fSession.masterVolume());

    if (fSelected >= static_cast<int>(tracks.size()))
        fSelected = static_cast<int>(tracks.size()) - 1;
    if (fSelected < 0 && !tracks.empty())
        fSelected = 0;
    refreshMixState();
    refreshSelection();
    refreshChordPad();
    refreshMarkers();
    refreshClick();
    if (fScoreDialog)
    {
        std::vector<ScoreDialog::Track> list;
        for (int i : scoreTracks())
            list.push_back({QString::fromStdString(tracks[i].name), i, QString::fromStdString(tracks[i].file)});
        fScoreDialog->setTracks(list, fSelected);
    }
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
        fInspectorTune->setEnabled(false);
        fInstrumentBox->hide();
        refreshInserts();
        return;
    }

    const chost::Track& t = tracks[fSelected];
    const bool instrument = t.kind == chost::TrackKind::Instrument;
    const bool bus = t.kind == chost::TrackKind::Bus;
    fInspectorReplace->setVisible(!bus);
    fInspectorName->setText(QString::fromStdString(t.name));
    fInspectorName->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: white; font-weight: bold; }").arg(trackColour(fSelected).name()));
    fInspectorReplace->setText(instrument ? QStringLiteral("替换 MIDI…") : QStringLiteral("替换音频…"));
    fInspectorStems->setVisible(!instrument && !bus);
    fInspectorMidi->setVisible(!instrument && !bus);
    fInspectorTune->setVisible(!instrument && !bus);
    fInspectorStems->setEnabled(!t.file.empty());
    fInspectorMidi->setEnabled(!t.file.empty());
    fInspectorTune->setEnabled(!t.file.empty() && t.role != kMasterRole);
    if (bus)
    {
        QStringList members, senders;
        for (const chost::Track& other : tracks)
        {
            if (other.output == t.uid)
                members << QString::fromStdString(other.name);
            if (std::any_of(other.sends.begin(), other.sends.end(), [&](const chost::Send& x) { return x.bus == t.uid; }))
                senders << QString::fromStdString(other.name);
        }
        QString info = QStringLiteral("编组 / 效果轨");
        if (!members.isEmpty())
            info += QStringLiteral("\n编组里的轨道：%1").arg(members.join(QStringLiteral("、")));
        if (!senders.isEmpty())
            info += QStringLiteral("\n发送到这里：%1").arg(senders.join(QStringLiteral("、")));
        if (members.isEmpty() && senders.isEmpty())
            info += QStringLiteral("\n还没有轨道送到这里：在轨道的“输出”或“发送”里选它");
        fInspectorInfo->setText(info);
    }
    else if (t.file.empty())
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
            const int out = fSession.trackOf(tracks[i].output);
            fStrips[i]->setOutput(out >= 0 ? QString::fromStdString(tracks[static_cast<size_t>(out)].name) : QStringLiteral("Stereo Out"));
            std::vector<ChannelStrip::SendSlot> sends;
            for (const chost::Send& send : tracks[i].sends)
                if (const int bus = fSession.trackOf(send.bus); bus >= 0)
                    sends.push_back({QString::fromStdString(tracks[static_cast<size_t>(bus)].name), send.level});
            fStrips[i]->setSends(sends);
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
    return track >= 0 && track < static_cast<int>(tracks.size()) && tracks[track].kind != chost::TrackKind::Bus &&
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
    fHistory->record(QStringLiteral("添加轨道：%1").arg(trackName));
    // the first song in a project: find its tempo and key in the background
    if (!fAnalysisTried && fSession.key().empty() && !resources::aiPython().isEmpty())
        analyzeSong(true);
    return track;
}

int MainWindow::addMidiTrack(const QString& path, const QString& name, const QString& role)
{
    QString trackName = name;
    chost::MidiClip midi;
    std::string readError;
    const bool readable = chost::readMidiFile(path.toStdString(), midi, readError);
    if (trackName.isEmpty())
        trackName = readable && !midi.name.empty() ? QString::fromStdString(midi.name) : QFileInfo(path).completeBaseName();
    // the first track of a project sets its tempo: a MIDI file's bars are the song's
    const bool first = fSession.tracks().empty();
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
    if (first && readable && midi.tempo > 0.0 && std::abs(midi.tempo - fSession.tempo()) > 0.01)
    {
        fSession.setTempo(midi.tempo);
        fArrange->setTempo(fSession.tempo());
        refreshSongInfo();
    }
    fSelected = track;
    refreshTracks();
    fHistory->record(QStringLiteral("添加乐器轨：%1").arg(trackName));
    return track;
}

bool MainWindow::loadAudioFile(const QString& path)
{
    if (isSessionFile(path))
        return maybeSave() && openSession(path);
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
        fHistory->record(QStringLiteral("导入音频"));
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
    fHistory->record(QStringLiteral("添加音轨"));
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
    fHistory->record(QStringLiteral("添加乐器轨"));
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
    History::Group step(*fHistory, QStringLiteral("换上设置的乐器"));
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
    fHistory->record(QStringLiteral("换乐器"));

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
    if (fSelected < 0 || fSession.tracks()[fSelected].kind == chost::TrackKind::Bus)
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
    fHistory->record(instrument ? QStringLiteral("替换 MIDI") : QStringLiteral("替换音频"));
}

void MainWindow::removeSelectedTrack()
{
    if (fSelected < 0 || fSelected >= static_cast<int>(fSession.tracks().size()))
        return;
    closeEditors();
    const int track = fSelected;
    const QString name = QString::fromStdString(fSession.tracks()[track].name);
    fSession.removeTrack(track);
    fSelected = std::min(track, static_cast<int>(fSession.tracks().size()) - 1);
    refreshTracks();
    fHistory->record(QStringLiteral("删除轨道：%1").arg(name));
}

void MainWindow::renameSelectedTrack(const QString& name)
{
    if (fSelected < 0 || name.trimmed().isEmpty() || name.toStdString() == fSession.tracks()[fSelected].name)
        return;
    fSession.renameTrack(fSelected, name.trimmed().toStdString());
    const int keep = fSelected;
    refreshTracks();
    selectTrack(keep);
    fHistory->record(QStringLiteral("重命名轨道"));
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
        if (t.kind == chost::TrackKind::Bus)
        {
            menu.addAction(t.folded ? QStringLiteral("展开文件夹") : QStringLiteral("折叠文件夹"), this,
                           [this, track, folded = t.folded] { setFolded(track, !folded); });
        }
        else if (t.kind == chost::TrackKind::Instrument)
        {
            menu.addAction(QStringLiteral("编辑音符…"), this, [this, track] { editNotes(track); })->setEnabled(!t.file.empty());
            menu.addAction(QStringLiteral("出谱…"), this, [this, track] {
                selectTrack(track);
                ScoreDialog* dialog = scoreDialog();
                dialog->setChosen({track});
            })->setEnabled(!t.file.empty() && !isDrumTrack(track));
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
        QMenu* routing = menu.addMenu(QStringLiteral("输出和发送"));
        routing->addAction(QStringLiteral("输出到…"), this, [this, track, globalPos] { outputMenu(track, globalPos); });
        if (static_cast<int>(t.sends.size()) < ChannelStrip::kSendSlots)
            routing->addAction(QStringLiteral("发送到…"), this, [this, track, globalPos, slot = static_cast<int>(t.sends.size())] { sendMenu(track, slot, globalPos); });
        for (size_t k = 0; k < t.sends.size(); ++k)
            if (const int bus = fSession.trackOf(t.sends[k].bus); bus >= 0)
                routing->addAction(QStringLiteral("去掉发送到 %1").arg(QString::fromStdString(fSession.tracks()[static_cast<size_t>(bus)].name)), this,
                                   [this, track, bus] { setTrackSend(track, bus, -1.0f); });
        routing->addSeparator();
        addBusMenu(routing, track);
        if (!t.file.empty())
        {
            QMenu* clip = menu.addMenu(fArrange->hasRange() ? QStringLiteral("片段（选区）") : QStringLiteral("片段（整段）"));
            addClipActions(clip, track);
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("删除轨道"), this, &MainWindow::removeSelectedTrack);
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("添加音轨"), this, &MainWindow::addEmptyTrack);
    menu.addAction(QStringLiteral("添加乐器轨（MIDI）"), this, &MainWindow::addInstrumentTrack);
    menu.addAction(QStringLiteral("导入音频 / MIDI 到新轨道…"), this, &MainWindow::importAudio);
    addBusMenu(&menu, -1);
    menu.exec(globalPos);
}

void MainWindow::addClipActions(QMenu* menu, int track)
{
    const chost::Track& t = fSession.tracks()[static_cast<size_t>(track)];
    const bool midi = t.kind == chost::TrackKind::Instrument;
    const bool range = fArrange->hasRange();
    auto ranged = [this](chost::ClipEditKind kind) {
        chost::ClipEdit edit;
        edit.kind = kind;
        if (fArrange->hasRange())
        {
            edit.start = fArrange->rangeStart();
            edit.end = fArrange->rangeEnd();
        }
        return edit;
    };
    auto add = [&](const QString& text, chost::ClipEditKind kind, bool enabled, const QString& tip) {
        QAction* a = menu->addAction(text, this, [this, track, kind, text, ranged] { editClip(track, ranged(kind), text); });
        a->setEnabled(enabled);
        a->setToolTip(tip);
    };
    if (!range)
    {
        QAction* hint = menu->addAction(QStringLiteral("（在标尺上拖出选区，就能只改其中一段）"));
        hint->setEnabled(false);
    }
    add(midi ? QStringLiteral("删除选区里的音符") : QStringLiteral("静音选区"), chost::ClipEditKind::Silence, range, {});
    add(QStringLiteral("只保留选区"), chost::ClipEditKind::Keep, range, {});
    add(QStringLiteral("选区淡入"), chost::ClipEditKind::FadeIn, range && !midi, {});
    add(QStringLiteral("选区淡出"), chost::ClipEditKind::FadeOut, range && !midi, {});
    menu->addSeparator();
    menu->addAction(midi ? QStringLiteral("力度…") : QStringLiteral("音量…"), this, [this, track, ranged, midi] {
        bool ok = false;
        const double db = QInputDialog::getDouble(this, midi ? QStringLiteral("力度") : QStringLiteral("音量"),
                                                  midi ? QStringLiteral("把力度加大或减小多少 dB（每 6 dB 大约一倍）")
                                                       : QStringLiteral("把音量加大或减小多少 dB（+6 dB 大约响一倍）"),
                                                  -3.0, -48.0, 24.0, 1, &ok, Qt::WindowFlags(), 0.5);
        if (!ok || db == 0.0)
            return;
        chost::ClipEdit edit = ranged(chost::ClipEditKind::Gain);
        edit.db = db;
        editClip(track, edit, QStringLiteral("片段音量 %1 dB").arg(db > 0 ? QStringLiteral("+") + QString::number(db) : QString::number(db)));
    });
    if (!midi)
        menu->addAction(QStringLiteral("标准化（峰值到 -1 dB）"), this, [this, track, ranged] {
            chost::ClipEdit edit = ranged(chost::ClipEditKind::Normalize);
            edit.db = -1.0;
            editClip(track, edit, QStringLiteral("标准化"));
        });
    menu->addAction(QStringLiteral("移动片段…"), this, [this, track] {
        bool ok = false;
        const double beats = QInputDialog::getDouble(this, QStringLiteral("移动片段"),
                                                     QStringLiteral("往后移几拍（负数往前）；也可以直接用鼠标拖动片段"), 1.0, -256.0, 256.0, 2, &ok,
                                                     Qt::WindowFlags(), 1.0);
        if (!ok || beats == 0.0)
            return;
        chost::ClipEdit edit;
        edit.kind = chost::ClipEditKind::Move;
        edit.seconds = beats * 60.0 / fSession.tempo();
        editClip(track, edit, QStringLiteral("移动片段 %1 拍").arg(beats));
    });
    if (range)
    {
        menu->addSeparator();
        menu->addAction(QStringLiteral("取消选区"), this, [this] { fArrange->setRange(0.0, 0.0); });
    }
}

QString MainWindow::editFolder() const
{
    const QString base = fSessionPath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                                : QFileInfo(fSessionPath).absolutePath();
    return QDir(base).filePath(projectName() + QStringLiteral("_编辑"));
}

bool MainWindow::editClip(int track, const chost::ClipEdit& edit, const QString& what)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (track < 0 || track >= static_cast<int>(tracks.size()) || tracks[static_cast<size_t>(track)].file.empty())
    {
        showError(QStringLiteral("这条轨道上没有片段"));
        return false;
    }
    const chost::Track& t = tracks[static_cast<size_t>(track)];
    const bool midi = t.kind == chost::TrackKind::Instrument;
    const QString in = QString::fromStdString(t.file);
    const QString folder = editFolder();
    QDir().mkpath(folder);
    // a new file each time, so undo can go back to the one before
    QString stem = QFileInfo(in).completeBaseName();
    stem.remove(QRegularExpression(QStringLiteral("_编辑\\d+$")));
    QString out;
    for (int n = 1; out.isEmpty() || QFileInfo::exists(out); ++n)
        out = QDir(folder).filePath(QStringLiteral("%1_编辑%2.%3").arg(stem).arg(n).arg(midi ? QStringLiteral("mid") : QStringLiteral("wav")));

    std::string error, note;
    bool ok = false;
    if (midi)
    {
        chost::MidiClip clip;
        ok = chost::readMidiFile(in.toStdString(), clip, error) && chost::editMidiClip(clip, edit, error) &&
             chost::writeMidiFile(out.toStdString(), clip, error);
    }
    else
    {
        ok = chost::editAudioFile(in.toStdString(), out.toStdString(), edit, error, &note);
    }
    if (!ok)
    {
        showError(QStringLiteral("%1没有做成：%2").arg(what, QString::fromStdString(error)));
        return false;
    }

    if (midi)
    {
        if (fPianoRoll && fPianoRoll->midiPath() == in)
            delete fPianoRoll; // it shows the notes as they were
        midiFileEdited(in, out, what);
    }
    else
    {
        closeEditors();
        if (!fSession.setTrackFile(track, out.toStdString()))
        {
            showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(out, QString::fromStdString(fSession.lastError())));
            return false;
        }
        fClips.remove(out);
        refreshTracks();
        selectTrack(track);
        fHistory->record(what);
    }
    statusBar()->showMessage(note.empty() ? QStringLiteral("%1：完成（Ctrl+Z 撤销）").arg(what)
                                          : QStringLiteral("%1：完成，但片段峰值超过 0 dB，会削波，最好把音量调小一点").arg(what),
                             8000);
    return true;
}

void MainWindow::setRange(double start, double end)
{
    fArrange->setRange(start, end);
}

double MainWindow::playhead() const
{
    const double rate = fHost.sampleRate();
    return rate > 0.0 ? static_cast<double>(fHost.transportFrame()) / rate : 0.0;
}

int MainWindow::addMarker(double seconds, const QString& name)
{
    std::vector<chost::Marker> markers = fSession.markers();
    QString text = name;
    for (int n = static_cast<int>(markers.size()) + 1; text.isEmpty(); ++n)
    {
        const QString candidate = QStringLiteral("标记 %1").arg(n);
        if (std::none_of(markers.begin(), markers.end(), [&](const chost::Marker& m) { return QString::fromStdString(m.name) == candidate; }))
            text = candidate;
    }
    markers.push_back({std::max(0.0, seconds), text.toStdString()});
    fSession.setMarkers(markers);
    refreshMarkers();
    fHistory->record(QStringLiteral("添加标记"));
    const std::vector<chost::Marker>& sorted = fSession.markers();
    for (size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i].name == text.toStdString() && std::abs(sorted[i].seconds - std::max(0.0, seconds)) < 1e-9)
            return static_cast<int>(i);
    return -1;
}

void MainWindow::renameMarker(int marker)
{
    std::vector<chost::Marker> markers = fSession.markers();
    if (marker < 0 || marker >= static_cast<int>(markers.size()) || qApp->property("chost-noninteractive").toBool())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("标记"), QStringLiteral("名字（比如 主歌、副歌、间奏）："), QLineEdit::Normal,
                                               QString::fromStdString(markers[static_cast<size_t>(marker)].name), &ok).trimmed();
    if (!ok || name.isEmpty() || name.toStdString() == markers[static_cast<size_t>(marker)].name)
        return;
    markers[static_cast<size_t>(marker)].name = name.toStdString();
    fSession.setMarkers(markers);
    refreshMarkers();
    fHistory->record(QStringLiteral("标记改名"));
}

void MainWindow::markerMenu(int marker, const QPoint& globalPos)
{
    QMenu menu;
    if (marker >= 0)
    {
        menu.addAction(QStringLiteral("改名…"), this, [this, marker] { renameMarker(marker); });
        menu.addAction(QStringLiteral("删除标记"), this, [this, marker] {
            std::vector<chost::Marker> markers = fSession.markers();
            if (marker >= static_cast<int>(markers.size()))
                return;
            markers.erase(markers.begin() + marker);
            fSession.setMarkers(markers);
            refreshMarkers();
            fHistory->record(QStringLiteral("删除标记"));
        });
    }
    else
    {
        menu.addAction(QStringLiteral("在这里添加标记"), this, [this] { renameMarker(addMarker(playhead())); });
        if (fArrange->hasRange())
            menu.addAction(QStringLiteral("取消选区"), this, [this] { fArrange->setRange(0.0, 0.0); });
    }
    menu.exec(globalPos);
}

void MainWindow::jumpToMarker(int direction)
{
    const double now = playhead();
    const std::vector<chost::Marker>& markers = fSession.markers();
    double target = direction < 0 ? 0.0 : -1.0;
    for (const chost::Marker& m : markers)
    {
        if (direction > 0 && m.seconds > now + 0.01)
        {
            target = m.seconds;
            break;
        }
        if (direction < 0 && m.seconds < now - 0.25) // just after one, the one before it
            target = m.seconds;
    }
    if (target >= 0.0)
        seek(target);
}

void MainWindow::refreshMarkers()
{
    std::vector<ArrangeView::Marker> list;
    for (const chost::Marker& m : fSession.markers())
        list.push_back({m.seconds, QString::fromStdString(m.name)});
    fArrange->setMarkers(list);
}

void MainWindow::dropFiles(const QStringList& paths, int track)
{
    if (paths.size() == 1 && isSessionFile(paths.front()))
    {
        if (maybeSave())
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
        fHistory->record(QStringLiteral("替换音频"));
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
    fHistory->record(QStringLiteral("添加插件：%1").arg(QString::fromStdString(fHost.slot(fHost.pluginCount() - 1).name)));
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
    fHistory->record(active ? QStringLiteral("启用插件") : QStringLiteral("旁路插件"));
}

void MainWindow::removeInsert(int index)
{
    // plugin ids shift after a removal, open editors would point at the wrong plugin
    closeEditors();
    fSession.removeInsert(fSelected, index);
    refreshInserts();
    fHistory->record(QStringLiteral("删除插件"));
}

void MainWindow::moveInsert(int index, int delta)
{
    closeEditors();
    fSession.moveInsert(fSelected, index, delta);
    refreshInserts();
    fHistory->record(QStringLiteral("移动插件"));
}

void MainWindow::setMuted(int track, bool muted)
{
    fSession.setMuted(track, muted);
    refreshMixState();
    fHistory->record(muted ? QStringLiteral("静音") : QStringLiteral("取消静音"));
}

void MainWindow::setSolo(int track, bool solo)
{
    fSession.setSolo(track, solo);
    refreshMixState();
    fHistory->record(solo ? QStringLiteral("独奏") : QStringLiteral("取消独奏"));
}

void MainWindow::setVolume(int track, float volume)
{
    fSession.setVolume(track, volume);
    fHistory->record(QStringLiteral("音量"), QStringLiteral("volume %1").arg(track));
}

// ---------------------------------------------------------------------------------------------------------------------
// buses: groups (folders of tracks) and effect returns

namespace {
constexpr float kDefaultSend = 0.316f; // -10 dB
}

int MainWindow::depthOf(int track) const
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    int depth = 0;
    for (int up = fSession.trackOf(tracks[static_cast<size_t>(track)].output); up >= 0 && depth < 64;
         up = fSession.trackOf(tracks[static_cast<size_t>(up)].output))
        ++depth;
    return depth;
}

void MainWindow::placeInGroup(int track, chost::Uid group)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    auto inside = [&](chost::Uid uid, chost::Uid bus) {
        int at = fSession.trackOf(uid);
        for (int steps = 0; at >= 0 && steps < 64; ++steps)
        {
            if (tracks[static_cast<size_t>(at)].output == bus)
                return true;
            at = fSession.trackOf(tracks[static_cast<size_t>(at)].output);
        }
        return false;
    };
    // the track, with the tracks of its own group if it is one, goes after the group's last track
    const chost::Uid moving = tracks[static_cast<size_t>(track)].uid;
    std::vector<chost::Uid> block{moving};
    for (size_t k = static_cast<size_t>(track) + 1; k < tracks.size() && inside(tracks[k].uid, moving); ++k)
        block.push_back(tracks[k].uid);
    std::vector<chost::Uid> order;
    for (const chost::Track& t : tracks)
        if (std::find(block.begin(), block.end(), t.uid) == block.end())
            order.push_back(t.uid);
    const auto g = std::find(order.begin(), order.end(), group);
    if (g == order.end())
        return;
    auto end = g + 1;
    while (end != order.end() && inside(*end, group))
        ++end;
    order.insert(end, block.begin(), block.end());
    for (size_t i = 0; i < order.size(); ++i)
        if (const int from = fSession.trackOf(order[i]); from != static_cast<int>(i))
            fSession.moveTrack(from, static_cast<int>(i));
}

QString MainWindow::findEffect(BusKind kind) const
{
    // the user's own first (FabFilter, Valhalla, Waves...), then free ones, then anything named like it
    static const QStringList reverbs{QStringLiteral("Pro-R"), QStringLiteral("Valhalla"), QStringLiteral("RVerb"), QStringLiteral("TrueVerb"),
                                     QStringLiteral("H-Reverb"), QStringLiteral("Abbey Road"), QStringLiteral("Raum"), QStringLiteral("Dragonfly"),
                                     QStringLiteral("MVerb"), QStringLiteral("GVerb"), QStringLiteral("Reverb"), QStringLiteral("Verb")};
    static const QStringList delays{QStringLiteral("Timeless"), QStringLiteral("ValhallaDelay"), QStringLiteral("H-Delay"), QStringLiteral("SuperTap"),
                                    QStringLiteral("EchoBoy"), QStringLiteral("Replika"), QStringLiteral("Ping"), QStringLiteral("Delay"),
                                    QStringLiteral("Echo")};
    static const QRegularExpression notAnEffect(QStringLiteral("allpass|comb|fractional|delay line|delayorama|sampler"),
                                                QRegularExpression::CaseInsensitiveOption);
    for (const QString& name : kind == BusKind::Reverb ? reverbs : delays)
        for (const chost::PluginEntry& p : fPlugins)
        {
            const QString pluginName = QString::fromStdString(p.name);
            if (!p.isSynth && p.audioIns > 0 && p.audioOuts > 0 && pluginName.contains(name, Qt::CaseInsensitive) &&
                !pluginName.contains(notAnEffect))
                return QString::fromStdString(p.spec);
        }
    return {};
}

void MainWindow::makeWet(uint plugin)
{
    // on an effect bus the dry sound is already in the tracks' own channels
    for (const chost::ParamInfo& p : fHost.parameters(plugin))
    {
        if (p.isOutput)
            continue;
        const QString name = QString::fromStdString(p.name).toLower().trimmed();
        if (name == QLatin1String("mix") || name == QLatin1String("dry/wet") || name == QLatin1String("wet/dry") ||
            name == QLatin1String("drywet") || name == QLatin1String("dry wet") || name == QLatin1String("wet") || name.startsWith(QLatin1String("mix ")))
            fHost.setParameter(plugin, p.index, p.max);
        else if (name.contains(QLatin1String("dry")) && !name.contains(QLatin1String("wet")))
            fHost.setParameter(plugin, p.index, p.min);
    }
}

int MainWindow::addBus(BusKind kind, const std::vector<int>& members, const QString& name)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    QString base = !name.isEmpty() ? name
                   : kind == BusKind::Group ? QStringLiteral("编组")
                   : kind == BusKind::Reverb ? QStringLiteral("混响")
                                             : QStringLiteral("延迟");
    QString text = base;
    for (int n = 2; std::any_of(tracks.begin(), tracks.end(), [&](const chost::Track& t) { return QString::fromStdString(t.name) == text; }); ++n)
        text = QStringLiteral("%1 %2").arg(base).arg(n);
    std::vector<chost::Uid> uids;
    for (int m : members)
        if (m >= 0 && m < static_cast<int>(tracks.size()))
            uids.push_back(tracks[static_cast<size_t>(m)].uid);

    const int added = fSession.addTrack(text.toStdString(), chost::TrackKind::Bus);
    if (added < 0)
    {
        showError(QStringLiteral("无法添加%1：%2").arg(text, QString::fromStdString(fSession.lastError())));
        return -1;
    }
    const chost::Uid bus = fSession.tracks()[static_cast<size_t>(added)].uid;
    QString note;
    if (kind == BusKind::Group)
    {
        // the group goes where its first track was, its tracks under it
        int first = added;
        for (chost::Uid uid : uids)
            first = std::min(first, fSession.trackOf(uid));
        fSession.moveTrack(added, first);
        for (chost::Uid uid : uids)
        {
            fSession.setOutput(fSession.trackOf(uid), bus);
            placeInGroup(fSession.trackOf(uid), bus);
        }
    }
    else
    {
        const QString spec = findEffect(kind);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        if (!spec.isEmpty() && fSession.addInsert(fSession.trackOf(bus), spec.toStdString()))
        {
            const chost::Track& t = fSession.tracks()[static_cast<size_t>(fSession.trackOf(bus))];
            makeWet(t.inserts.back());
            note = QStringLiteral("用的是 %1，可以在插入效果里换成别的").arg(QString::fromStdString(fHost.slot(t.inserts.back()).name));
        }
        else
        {
            note = kind == BusKind::Reverb ? QStringLiteral("没找到混响插件：在这条效果轨的插入效果里加一个")
                                           : QStringLiteral("没找到延迟插件：在这条效果轨的插入效果里加一个");
        }
        QApplication::restoreOverrideCursor();
        for (chost::Uid uid : uids)
            fSession.setSend(fSession.trackOf(uid), bus, kDefaultSend);
    }
    const int index = fSession.trackOf(bus);
    fSelected = index;
    refreshTracks();
    fHistory->record(kind == BusKind::Group ? QStringLiteral("添加编组") : QStringLiteral("添加效果轨"));
    statusBar()->showMessage(note.isEmpty() ? QStringLiteral("已添加 %1").arg(text) : QStringLiteral("已添加 %1：%2").arg(text, note), 8000);
    return index;
}

bool MainWindow::setTrackOutput(int track, int bus)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (track < 0 || track >= static_cast<int>(tracks.size()))
        return false;
    const chost::Uid to = bus >= 0 && bus < static_cast<int>(tracks.size()) ? tracks[static_cast<size_t>(bus)].uid : 0;
    if (bus >= 0 && to == 0)
        return false;
    if (tracks[static_cast<size_t>(track)].output == to)
        return true;
    if (!fSession.setOutput(track, to))
    {
        showError(QStringLiteral("不能这样接：%1").arg(fSession.trackOf(to) >= 0 && fSession.tracks()[static_cast<size_t>(fSession.trackOf(to))].kind == chost::TrackKind::Bus
                                                          ? QStringLiteral("这个编组的声音本来就要经过这条轨，会绕成一个圈")
                                                          : QStringLiteral("只能输出到编组或效果轨")));
        return false;
    }
    const chost::Uid moved = tracks[static_cast<size_t>(track)].uid;
    if (to != 0)
        placeInGroup(track, to);
    fSelected = fSession.trackOf(moved);
    refreshTracks();
    fHistory->record(QStringLiteral("输出到 %1").arg(to ? QString::fromStdString(fSession.tracks()[static_cast<size_t>(fSession.trackOf(to))].name)
                                                     : QStringLiteral("Stereo Out")));
    return true;
}

bool MainWindow::setTrackSend(int track, int bus, float level)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (track < 0 || track >= static_cast<int>(tracks.size()) || bus < 0 || bus >= static_cast<int>(tracks.size()))
        return false;
    const chost::Uid to = tracks[static_cast<size_t>(bus)].uid;
    const QString busName = QString::fromStdString(tracks[static_cast<size_t>(bus)].name);
    const std::vector<chost::Send>& sends = tracks[static_cast<size_t>(track)].sends;
    const bool exists = std::any_of(sends.begin(), sends.end(), [to](const chost::Send& x) { return x.bus == to; });
    if (level < 0.0f)
    {
        if (!exists || !fSession.removeSend(track, to))
            return false;
        refreshTracks();
        fHistory->record(QStringLiteral("去掉发送到 %1").arg(busName));
        return true;
    }
    if (!fSession.setSend(track, to, level))
    {
        showError(QStringLiteral("不能这样发送：%1").arg(tracks[static_cast<size_t>(bus)].kind == chost::TrackKind::Bus
                                                            ? QStringLiteral("这条效果轨的声音本来就要经过这条轨，会绕成一个圈")
                                                            : QStringLiteral("只能发送到效果轨或编组")));
        return false;
    }
    if (exists)
    {
        // a drag of the send knob is one step
        fHistory->record(QStringLiteral("发送量"), QStringLiteral("send %1 %2").arg(track).arg(to));
        return true;
    }
    refreshTracks();
    fHistory->record(QStringLiteral("发送到 %1").arg(busName));
    return true;
}

void MainWindow::setFolded(int track, bool folded)
{
    fSession.setFolded(track, folded);
    if (folded && fSelected >= 0 && fSelected != track && depthOf(fSelected) > 0)
    {
        // the selection doesn't stay on a track that is out of sight
        const chost::Uid group = fSession.tracks()[static_cast<size_t>(track)].uid;
        if (fSession.feeds(fSession.tracks()[static_cast<size_t>(fSelected)].uid, group))
            fSelected = track;
    }
    refreshTracks();
}

void MainWindow::foldGroups()
{
    for (size_t i = 0; i < fSession.tracks().size(); ++i)
        if (fSession.tracks()[i].kind == chost::TrackKind::Bus)
            fSession.setFolded(static_cast<int>(i), true);
    refreshTracks();
}

void MainWindow::addBusMenu(QMenu* menu, int track)
{
    const std::vector<int> members = track >= 0 ? std::vector<int>{track} : std::vector<int>{};
    menu->addAction(track >= 0 ? QStringLiteral("放进新编组（文件夹）") : QStringLiteral("添加编组轨（文件夹）"), this,
                    [this, members] { addBus(BusKind::Group, members); });
    menu->addAction(track >= 0 ? QStringLiteral("发送到新的混响效果轨") : QStringLiteral("添加混响效果轨"), this,
                    [this, members] { addBus(BusKind::Reverb, members); });
    menu->addAction(track >= 0 ? QStringLiteral("发送到新的延迟效果轨") : QStringLiteral("添加延迟效果轨"), this,
                    [this, members] { addBus(BusKind::Delay, members); });
}

void MainWindow::outputMenu(int track, const QPoint& globalPos)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (track < 0 || track >= static_cast<int>(tracks.size()))
        return;
    const chost::Track& t = tracks[static_cast<size_t>(track)];
    QMenu menu;
    QAction* master = menu.addAction(QStringLiteral("Stereo Out（总输出）"), this, [this, track] { setTrackOutput(track, -1); });
    master->setCheckable(true);
    master->setChecked(t.output == 0);
    for (size_t b = 0; b < tracks.size(); ++b)
    {
        if (tracks[b].kind != chost::TrackKind::Bus || static_cast<int>(b) == track)
            continue;
        QAction* a = menu.addAction(QString::fromStdString(tracks[b].name), this, [this, track, b] { setTrackOutput(track, static_cast<int>(b)); });
        a->setCheckable(true);
        a->setChecked(t.output == tracks[b].uid);
        a->setEnabled(!fSession.feeds(tracks[b].uid, t.uid));
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("放进新编组（文件夹）"), this, [this, track] { addBus(BusKind::Group, {track}); });
    menu.exec(globalPos);
}

void MainWindow::sendMenu(int track, int slot, const QPoint& globalPos)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    if (track < 0 || track >= static_cast<int>(tracks.size()))
        return;
    const chost::Track& t = tracks[static_cast<size_t>(track)];
    const chost::Send* current = slot < static_cast<int>(t.sends.size()) ? &t.sends[static_cast<size_t>(slot)] : nullptr;
    QMenu menu;
    const int currentBus = current ? fSession.trackOf(current->bus) : -1;
    const float level = current ? current->level : kDefaultSend;
    bool any = false;
    for (size_t b = 0; b < tracks.size(); ++b)
    {
        if (tracks[b].kind != chost::TrackKind::Bus || static_cast<int>(b) == track)
            continue;
        const bool sending = std::any_of(t.sends.begin(), t.sends.end(), [&](const chost::Send& x) { return x.bus == tracks[b].uid; });
        QAction* a = menu.addAction(QString::fromStdString(tracks[b].name), this, [this, track, b, currentBus, level] {
            if (currentBus >= 0)
                setTrackSend(track, currentBus, -1.0f); // switched over: the old send goes
            setTrackSend(track, static_cast<int>(b), level);
        });
        a->setCheckable(true);
        a->setChecked(static_cast<int>(b) == currentBus);
        a->setEnabled(static_cast<int>(b) != currentBus && !sending && !fSession.feeds(tracks[b].uid, t.uid) && t.output != tracks[b].uid);
        any = true;
    }
    if (any)
        menu.addSeparator();
    menu.addAction(QStringLiteral("新建混响效果轨"), this, [this, track] { addBus(BusKind::Reverb, {track}); });
    menu.addAction(QStringLiteral("新建延迟效果轨"), this, [this, track] { addBus(BusKind::Delay, {track}); });
    if (currentBus >= 0)
    {
        menu.addSeparator();
        menu.addAction(QStringLiteral("去掉这个发送"), this, [this, track, currentBus] { setTrackSend(track, currentBus, -1.0f); });
    }
    menu.exec(globalPos);
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
        History::Group step(*fHistory, QStringLiteral("AI 分轨：%1").arg(label));
        const int track = addAudioTrack(path, label);
        if (track < 0)
            return;
        fSession.setRole(track, id.toStdString());
        // the finer parts go right below their group and share its colour, like a folder in Cubase
        if (const int parent = group.isEmpty() ? -1 : trackOf(stems->value(group)); parent >= 0)
        {
            const int to = std::min(track, parent + 1 + (*parts)[group]++);
            fSession.moveTrack(track, to);
            fSession.setColour(to, fSession.tracks()[parent].colour);
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
    History::Group step(*fHistory, QStringLiteral("转 MIDI：%1").arg(name));
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
        const uint32_t colour = fSession.tracks()[below].colour;
        fSession.moveTrack(track, to);
        fSession.setColour(to, colour);
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

void MainWindow::midiFileEdited(const QString& oldPath, const QString& newPath, const QString& what)
{
    const int t = trackOfFile(oldPath);
    if (t < 0)
        return;
    fSession.reloadMidiFile(t, newPath.toStdString());
    fClips.remove(oldPath);
    fClips.remove(newPath);
    const int keep = fSelected;
    refreshTracks();
    selectTrack(keep);
    fHistory->record(what);
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
    fPianoRoll = new PianoRollDialog(QString::fromStdString(tracks[track].name), path, trackColour(track),
                                     QString::fromStdString(fSession.key()), this);
    fPianoRoll->setAttribute(Qt::WA_DeleteOnClose);

    connect(fPianoRoll, &PianoRollDialog::saved, this, [this](const QString& oldPath, const QString& newPath) {
        midiFileEdited(oldPath, newPath, QStringLiteral("编辑音符"));
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
    if (maybeSave())
        resetSession();
}

void MainWindow::resetSession()
{
    closeEditors();
    fHost.pause();
    fHost.removeAllPlugins();
    fSession.init();
    fHistory->reset();
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
    fHistory->reset();
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
    if (!maybeSave())
        return;
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
    Autosave::backUp(fSessionPath); // the version saved over stays in <project>_备份
    if (fSession.save(fSessionPath.toStdString()))
    {
        fHistory->setSaved();
        fAutosave->saved();
        statusBar()->showMessage(QStringLiteral("已保存：%1").arg(fSessionPath), 5000);
    }
    else
        showError(QStringLiteral("保存失败：%1").arg(QString::fromStdString(fSession.lastError())));
}

void MainWindow::saveSessionAs()
{
    if (QGuiApplication::platformName() == QLatin1String("offscreen") || qApp->property("chost-noninteractive").toBool())
    {
        showError(QStringLiteral("工程还没有文件名，不能保存。"));
        return;
    }
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

bool MainWindow::openAsUnsaved(const QString& file, const QString& projectPath)
{
    if (!openSession(file))
        return false;
    fSessionPath = projectPath;
    fHistory->reset(true);
    updateTitle();
    return true;
}

void MainWindow::recoverAtStart(bool autoRecover)
{
    const std::optional<Autosave::Found> found = fAutosave->claim();
    if (!found)
    {
        fAutosave->settle(Autosave::Outcome::Left);
        return;
    }
    const bool interactive = QGuiApplication::platformName() != QLatin1String("offscreen") && !qApp->property("chost-noninteractive").toBool();
    if (!autoRecover && !interactive)
    {
        fAutosave->settle(Autosave::Outcome::Left);
        return;
    }
    const QString name = found->original.isEmpty() ? QStringLiteral("未命名工程") : QFileInfo(found->original).fileName();
    bool recover = autoRecover;
    if (!autoRecover)
    {
        QMessageBox box(QMessageBox::Question, QStringLiteral("chost"),
                        QStringLiteral("上次 chost 没有正常关闭。\n\n自动备份里有工程「%1」还没保存的改动（%2 自动保存），要恢复吗？")
                            .arg(name, found->time.toString(QStringLiteral("M月d日 HH:mm"))),
                        QMessageBox::NoButton, this);
        QPushButton* yes = box.addButton(QStringLiteral("恢复"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("不要了"), QMessageBox::RejectRole);
        box.setDefaultButton(yes);
        box.exec();
        recover = box.clickedButton() == yes;
    }
    if (recover && openAsUnsaved(found->session, found->original))
    {
        fAutosave->settle(Autosave::Outcome::Recovered);
        statusBar()->showMessage(QStringLiteral("已恢复「%1」，记得保存。").arg(name), 10000);
        std::printf("RECOVERED %d tracks of %s\n", static_cast<int>(fSession.tracks().size()), qPrintable(name));
        std::fflush(stdout);
    }
    else
        fAutosave->settle(recover ? Autosave::Outcome::Left : Autosave::Outcome::Discarded);
}

void MainWindow::openVersion()
{
    const std::vector<Autosave::Found> versions = Autosave::backups(fSessionPath);
    if (versions.empty())
    {
        QMessageBox::information(this, QStringLiteral("以前的版本"),
                                 fSessionPath.isEmpty()
                                     ? QStringLiteral("这个工程还没保存过，没有以前的版本。")
                                     : QStringLiteral("还没有以前的版本。\n\n以后每次保存工程，被覆盖掉的那一版都会留在旁边的「%1」文件夹里（最近 10 版）。")
                                           .arg(QFileInfo(Autosave::backupFolder(fSessionPath)).fileName()));
        return;
    }
    QStringList labels;
    for (const Autosave::Found& version : versions)
        labels << version.time.toString(QStringLiteral("yyyy年M月d日 HH:mm:ss"));
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, QStringLiteral("以前的版本"),
                                                 QStringLiteral("每次保存前的那一版。打开后，保存才会替换现在的工程："), labels, 0, false, &ok);
    if (!ok || !maybeSave())
        return;
    const Autosave::Found& version = versions[static_cast<size_t>(labels.indexOf(chosen))];
    const QString project = fSessionPath;
    if (openAsUnsaved(version.session, project))
        statusBar()->showMessage(QStringLiteral("已打开 %1 的版本；保存后会替换现在的工程。").arg(chosen), 10000);
}

bool MainWindow::maybeSave()
{
    const bool interactive = QGuiApplication::platformName() != QLatin1String("offscreen") && !qApp->property("chost-noninteractive").toBool();
    if (!interactive || !fHistory->isModified() || fSession.tracks().empty())
        return true;
    const auto answer = QMessageBox::question(this, QStringLiteral("chost"), QStringLiteral("工程有改动还没保存，要保存吗？"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
    {
        saveSession();
        return !fHistory->isModified(); // not saved after all: the file dialog was cancelled, or it failed
    }
    return true;
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
    fHistory->record(QStringLiteral("速度"), QStringLiteral("tempo"));
}

bool MainWindow::metronomeOn() const
{
    return fClickAction && fClickAction->isChecked();
}

void MainWindow::setMetronome(bool on)
{
    QSettings().setValue(QStringLiteral("metronome/on"), on);
    if (fClickAction->isChecked() != on)
    {
        const QSignalBlocker blocker(fClickAction);
        fClickAction->setChecked(on);
    }
    refreshClick();
    if (on && !fSession.clickOn())
    {
        // no instrument or a driver that doesn't mix: say so once and switch back off
        const QSignalBlocker blocker(fClickAction);
        fClickAction->setChecked(false);
        QSettings().setValue(QStringLiteral("metronome/on"), false);
        statusBar()->showMessage(QStringLiteral("节拍器没能打开：%1").arg(QString::fromStdString(fSession.lastError())), 8000);
    }
}

void MainWindow::setMetronomeVolume(double db)
{
    QSettings().setValue(QStringLiteral("metronome/volume_db"), db);
    fSession.setClickVolume(static_cast<float>(std::pow(10.0, db / 20.0)));
    statusBar()->showMessage(QStringLiteral("节拍器音量 %1 dB").arg(db), 3000);
}

void MainWindow::refreshClick()
{
    if (!metronomeOn())
    {
        fSession.setClickOn(false);
        return;
    }
    // long enough for the song and a while after it, in whole minutes so it isn't rewritten all the time
    double longest = 0.0;
    for (const chost::Track& t : fSession.tracks())
        if (!t.file.empty())
            longest = std::max(longest, clip(QString::fromStdString(t.file)).seconds);
    const int minutes = static_cast<int>(std::ceil((longest + 60.0) / 60.0));
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(folder);
    const QString file = QDir(folder).filePath(QStringLiteral("metronome-%1bpm-%2min.mid").arg(fSession.tempo(), 0, 'f', 2).arg(minutes));
    if (file != fClickFile || fSession.clickGain() < 0)
    {
        std::string error;
        if (!QFileInfo::exists(file) && !chost::writeClickTrack(file.toStdString(), fSession.tempo(), 4, minutes * 60.0, error))
        {
            statusBar()->showMessage(QStringLiteral("节拍器没能打开：%1").arg(QString::fromStdString(error)), 8000);
            return;
        }
        if (!fSession.setClick(file.toStdString(), instruments::bundledSpec().toStdString()))
            return;
        fClickFile = file;
    }
    fSession.setClickVolume(static_cast<float>(std::pow(10.0, QSettings().value(QStringLiteral("metronome/volume_db"), -10.0).toDouble() / 20.0)));
    fSession.setClickOn(true);
}

void MainWindow::refreshSongInfo()
{
    refreshClick();
    {
        const QSignalBlocker blocker(fTempoBox);
        fTempoBox->setValue(fSession.tempo());
    }
    fArrange->setTempo(fSession.tempo());
    fKeyLabel->setText(keyText(fSession.key()) + QStringLiteral(" · 4/4"));
    fKeyLabel->setToolTip(fSession.key().empty()
                              ? QStringLiteral("调性还不知道：点“识别”让 AI 听一下")
                              : QStringLiteral("调性：%1（关系调 %2，用的音完全一样）").arg(keyText(fSession.key()), relativeKeyText(fSession.key())));
    if (fChordPad)
        fChordPad->setSong(QString::fromStdString(fSession.key()), fSession.tempo());
    if (fScoreDialog)
        fScoreDialog->setSong(QString::fromStdString(fSession.key()), fSession.tempo());
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
                    removed = true;
                }
            if (removed)
            {
                fSelected = -1;
                refreshTracks();
                refreshMixState();
                fHistory->record(QStringLiteral("删除旧的母带轨道"));
            }
        };
        connect(fMasterDialog, &MasterDialog::aboutToStart, this, removeMasters);
        connect(fMasterDialog, &MasterDialog::compareRequested, this, [this, removeMasters](const QString& path) {
            History::Group step(*fHistory, QStringLiteral("AI 母带"));
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

void MainWindow::arrange(bool autoStart, const QString& chords)
{
    if (!fArrangeDialog)
    {
        // what the arrangement can follow: the songs and MIDI parts, the selected one first in line
        std::vector<ArrangeDialog::Source> sources;
        int selected = -1;
        const std::vector<chost::Track>& tracks = fSession.tracks();
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const chost::Track& t = tracks[i];
            if (t.file.empty() || t.role == kMasterRole)
                continue;
            if (static_cast<int>(i) == fSelected)
                selected = static_cast<int>(sources.size());
            sources.push_back({QString::fromStdString(t.name), QString::fromStdString(t.file), t.kind == chost::TrackKind::Instrument});
        }
        const QString base = fSessionPath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                                    : QFileInfo(fSessionPath).absolutePath();
        const QString folder = QDir(base).filePath(projectName() + QStringLiteral("_编曲"));
        fArrangeDialog = new ArrangeDialog(sources, selected, fSession.tempo(), folder, this);
        fArrangeDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(fArrangeDialog, &ArrangeDialog::arranged, this, [this, folder](const std::vector<ArrangeDialog::Part>& parts, bool replace) {
            History::Group step(*fHistory, QStringLiteral("AI 编曲"));
            closeEditors();
            if (replace)
            {
                // the tracks of an earlier arrangement are the ones playing files from its folder
                const QString prefix = QDir::cleanPath(folder) + QLatin1Char('/');
                for (int i = static_cast<int>(fSession.tracks().size()) - 1; i >= 0; --i)
                    if (QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(fSession.tracks()[i].file))).startsWith(prefix))
                    {
                        fSession.removeTrack(i);
                    }
            }
            int first = -1;
            for (const ArrangeDialog::Part& part : parts)
            {
                statusBar()->showMessage(QStringLiteral("正在加载 %1 的乐器…").arg(part.name));
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                const int track = addMidiTrack(part.file, QStringLiteral("编曲 ") + part.name, part.role);
                if (track < 0)
                    continue;
                // a starting balance: sustained strings and strummed guitars sound louder than their notes
                static const QHash<QString, float> volumes{{QStringLiteral("other"), 0.4f}, {QStringLiteral("guitar"), 0.75f},
                                                           {QStringLiteral("bass"), 0.9f}};
                fSession.setVolume(track, volumes.value(part.role, 1.0f));
                if (first < 0)
                    first = track;
            }
            refreshTracks();
            selectTrack(first);
            statusBar()->showMessage(QStringLiteral("编曲完成：%1 条乐器轨，可以双击轨道改音符").arg(parts.size()), 8000);
        });
    }
    if (!chords.isEmpty())
        fArrangeDialog->setChords(chords);
    fArrangeDialog->show();
    fArrangeDialog->raise();
    fArrangeDialog->activateWindow();
    fArrangeDialog->setAutoStart(autoStart);
}

QString MainWindow::chordFolder() const
{
    const QString base = fSessionPath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                                : QFileInfo(fSessionPath).absolutePath();
    return QDir(base).filePath(projectName() + QStringLiteral("_和弦"));
}

std::vector<int> MainWindow::scoreTracks()
{
    std::vector<int> list;
    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].kind == chost::TrackKind::Instrument && !tracks[i].file.empty() && !isDrumTrack(static_cast<int>(i)))
            list.push_back(static_cast<int>(i));
    return list;
}

ScoreDialog* MainWindow::scoreDialog()
{
    if (!fScoreDialog)
    {
        const QString folder = fSessionPath.isEmpty() ? QString() : QFileInfo(fSessionPath).absolutePath();
        fScoreDialog = new ScoreDialog(fSessionPath.isEmpty() ? QString() : projectName(), QString::fromStdString(fSession.key()),
                                       fSession.tempo(), folder, this);
        fScoreDialog->setAttribute(Qt::WA_DeleteOnClose);
        refreshTracks();
    }
    fScoreDialog->show();
    fScoreDialog->raise();
    fScoreDialog->activateWindow();
    return fScoreDialog;
}

ChordPadDialog* MainWindow::chordPad()
{
    if (!fChordPad)
    {
        fChordPad = new ChordPadDialog(QString::fromStdString(fSession.key()), fSession.tempo(), chordFolder(), this);
        fChordPad->setAttribute(Qt::WA_DeleteOnClose);
        connect(fChordPad, &ChordPadDialog::notes, this, [this](int track, const std::vector<int>& pitches, bool on) {
            const std::vector<chost::Track>& tracks = fSession.tracks();
            if (track < 0 || track >= static_cast<int>(tracks.size()) || tracks[track].instrument < 0)
                return;
            const uint id = static_cast<uint>(tracks[track].instrument);
            for (int pitch : pitches)
                fHost.sendNote(id, 0, static_cast<uint8_t>(pitch), on ? 96 : 0);
        });
        connect(fChordPad, &ChordPadDialog::newTrackRequested, this, [this] {
            const int track = fSession.addTrack(QStringLiteral("和弦").toStdString(), chost::TrackKind::Instrument);
            if (track < 0)
            {
                showError(QStringLiteral("无法添加轨道：%1").arg(QString::fromStdString(fSession.lastError())));
                return;
            }
            fSession.setRole(track, "piano");
            loadInstrument(track, instruments::choiceFor(QStringLiteral("piano")));
            fSelected = track;
            refreshTracks();
            fHistory->record(QStringLiteral("添加和弦轨"));
        });
        connect(fChordPad, &ChordPadDialog::written, this, [this](int track, const QString& path) { placeChords(track, path); });
        connect(fChordPad, &ChordPadDialog::arrangeRequested, this, [this](const QString& chords) {
            delete fArrangeDialog; // fresh, with these chords
            arrange(false, chords);
        });
        refreshChordPad();
    }
    fChordPad->show();
    fChordPad->raise();
    fChordPad->activateWindow();
    return fChordPad;
}

void MainWindow::refreshChordPad()
{
    if (!fChordPad)
        return;
    std::vector<ChordPadDialog::Track> instruments, audio;
    const std::vector<chost::Track>& tracks = fSession.tracks();
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const chost::Track& t = tracks[i];
        const ChordPadDialog::Track entry{QString::fromStdString(t.name), static_cast<int>(i), QString::fromStdString(t.file)};
        if (t.kind == chost::TrackKind::Instrument && t.instrument >= 0 && !isDrumTrack(static_cast<int>(i)))
            instruments.push_back(entry);
        else if (t.kind == chost::TrackKind::Audio && !t.file.empty() && t.role != kMasterRole)
            audio.push_back(entry);
    }
    fChordPad->setTracks(instruments, audio, fSelected);
}

int MainWindow::placeChords(int track, const QString& midiPath)
{
    const std::vector<chost::Track>& tracks = fSession.tracks();
    closeEditors();
    // the track the pads were heard on takes the chords while it has none yet
    if (track >= 0 && track < static_cast<int>(tracks.size()) && tracks[track].kind == chost::TrackKind::Instrument && tracks[track].file.empty())
    {
        if (!fSession.setTrackFile(track, midiPath.toStdString()))
        {
            showError(QStringLiteral("无法打开 MIDI 文件：%1\n\n%2").arg(midiPath, QString::fromStdString(fSession.lastError())));
            return -1;
        }
        fSelected = track;
        refreshTracks();
        fHistory->record(QStringLiteral("写入和弦"));
        return track;
    }
    return addMidiTrack(midiPath, QStringLiteral("和弦"), QStringLiteral("piano"));
}

void MainWindow::tune(bool autoStart)
{
    if (!fTuneDialog)
    {
        // the audio tracks, by their original takes: a track playing its tuned take is tuned again from scratch
        std::vector<TuneDialog::Source> sources;
        int selected = -1;
        const std::vector<chost::Track>& tracks = fSession.tracks();
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const chost::Track& t = tracks[i];
            if (t.kind != chost::TrackKind::Audio || t.file.empty() || t.role == kMasterRole)
                continue;
            if (static_cast<int>(i) == fSelected)
                selected = static_cast<int>(sources.size());
            const QString name = QString::fromStdString(t.name);
            const QString role = t.role.empty() ? guessRole(name) : QString::fromStdString(t.role);
            const bool vocal = role == QLatin1String("vocals") || role == QLatin1String("lead") || role == QLatin1String("backing");
            sources.push_back({name, originalOf(QString::fromStdString(t.file)), vocal});
        }
        fTuneDialog = new TuneDialog(sources, selected, QString::fromStdString(fSession.key()), this);
        fTuneDialog->setAttribute(Qt::WA_DeleteOnClose);
        // the tuned file is about to be written again: the track plays the original meanwhile
        connect(fTuneDialog, &TuneDialog::aboutToStart, this, [this](const QString& original) { playTake(original, false); });
        connect(fTuneDialog, &TuneDialog::tuned, this, [this](const QString& original, const QString& tunedFile) {
            QFile source(tunedFile + QStringLiteral(".source"));
            if (source.open(QIODevice::WriteOnly | QIODevice::Truncate))
                source.write(original.toUtf8());
            fClips.remove(tunedFile); // a new waveform
            playTake(original, true);
            fHistory->record(QStringLiteral("AI 修音"));
            statusBar()->showMessage(QStringLiteral("修音完成：轨道已换成修好的声音，A/B 按钮可以切回原声"), 8000);
        });
        connect(fTuneDialog, &TuneDialog::listen, this, [this](const QString& original, bool tuned) {
            playTake(original, tuned);
            fHistory->absorb(); // listening is no change to undo
        });
        connect(fTuneDialog, &TuneDialog::playToggled, this, &MainWindow::togglePlay);
    }
    fTuneDialog->show();
    fTuneDialog->raise();
    fTuneDialog->activateWindow();
    fTuneDialog->setAutoStart(autoStart);
}

QString MainWindow::originalOf(const QString& file) const
{
    QFile source(file + QStringLiteral(".source"));
    if (source.open(QIODevice::ReadOnly))
    {
        const QString original = QString::fromUtf8(source.readAll()).trimmed();
        if (QFileInfo::exists(original))
            return original;
    }
    return file;
}

int MainWindow::trackPlaying(const QString& original) const
{
    const int track = trackOfFile(original);
    return track >= 0 ? track : trackOfFile(TuneDialog::tunedFileFor(original));
}

void MainWindow::playTake(const QString& original, bool tuned)
{
    const int track = trackPlaying(original);
    const QString file = tuned ? TuneDialog::tunedFileFor(original) : original;
    if (track < 0 || QString::fromStdString(fSession.tracks()[track].file) == file || !QFileInfo::exists(file))
        return;
    if (!fSession.setTrackFile(track, file.toStdString()))
        showError(QStringLiteral("无法打开音频文件：%1\n\n%2").arg(file, QString::fromStdString(fSession.lastError())));
    refreshTracks();
    selectTrack(track);
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
    fHistory->absorb(); // listening is no change to undo
}

// Changes two tracks every way undo knows, undoes and redoes it all and checks the session is exactly
// where it was each time, that nothing removed and gone for good is still loaded, and that it still plays.
int MainWindow::testUndo(const QString& insertSpec)
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    // the plugins the session itself uses: everything else would be left over
    auto live = [this] {
        size_t n = 1;
        for (const chost::Track& t : fSession.tracks())
            n += 1 + (t.source >= 0) + (t.instrument >= 0) + t.inserts.size() + t.sends.size();
        return n;
    };
    QUndoStack* stack = fHistory->stack();
    if (fSession.tracks().size() < 2)
    {
        std::printf("FAIL needs two tracks (--file a.wav --file b.wav)\n");
        return 1;
    }
    std::vector<chost::SessionState> states{fSession.state()};
    const int base = stack->index(); // the tracks loaded so far are steps too
    selectTrack(0);
    check(addPluginSpec(insertSpec), "insert added");
    states.push_back(fSession.state());
    addMarker(1.0, QStringLiteral("副歌"));
    states.push_back(fSession.state());
    chost::ClipEdit fade;
    fade.kind = chost::ClipEditKind::FadeIn;
    fade.end = 0.5;
    check(editClip(1, fade, QStringLiteral("选区淡入")), "clip edited into a new file");
    states.push_back(fSession.state());
    selectTrack(0); // the edit selected the clip's track
    setVolume(1, 0.5f);
    setVolume(1, 0.25f); // a drag: one step
    states.push_back(fSession.state());
    setMuted(0, true);
    states.push_back(fSession.state());
    setInsertActive(0, false);
    states.push_back(fSession.state());
    selectTrack(0);
    removeSelectedTrack();
    states.push_back(fSession.state());
    renameSelectedTrack(QStringLiteral("renamed"));
    states.push_back(fSession.state());
    // buses: an effect bus with a send, the send's knob dragged, a group, then the effect bus removed
    const int fx = addBus(BusKind::Group, {}, QStringLiteral("FX"));
    states.push_back(fSession.state());
    check(setTrackSend(0, fx, 0.5f), "send added");
    states.push_back(fSession.state());
    setTrackSend(0, fx, 0.4f);
    setTrackSend(0, fx, 0.3f); // a drag: one step
    states.push_back(fSession.state());
    check(addBus(BusKind::Group, {0}) == 0, "group above its track");
    states.push_back(fSession.state());
    check(fSession.tracks()[1].output == fSession.tracks()[0].uid && fSession.tracks()[1].sends.size() == 1, "the track is in the group, still sending");
    selectTrack(2);
    removeSelectedTrack();
    states.push_back(fSession.state());
    check(fSession.tracks()[1].sends.empty(), "a removed bus takes the sends to it along");
    check(stack->count() - base == static_cast<int>(states.size()) - 1, "one step per change");

    for (int i = static_cast<int>(states.size()) - 2; i >= 0; --i)
    {
        stack->undo();
        if (!(fSession.state() == states[static_cast<size_t>(i)]))
        {
            check(false, qPrintable(QStringLiteral("undo back to state %1").arg(i)));
        }
    }
    check(fSession.state() == states.front(), "everything undone");
    check(fHost.pluginCount() == live() + 3, "the removed insert and the two buses are kept aside for redo");
    for (size_t i = 1; i < states.size(); ++i)
    {
        stack->redo();
        if (!(fSession.state() == states[i]))
            check(false, qPrintable(QStringLiteral("redo to state %1").arg(i)));
    }
    check(fSession.state() == states.back(), "everything redone");
    while (stack->index() > base)
        stack->undo();
    addEmptyTrack(); // a new change: what could have been redone is gone
    check(fHost.pluginCount() == live(), "nothing left over after a new change");
    check(stack->count() == base + 1 && !stack->canRedo(), "the redo steps are dropped");
    stack->undo();
    check(fSession.state() == states.front(), "back at the start");

    if (fHost.driverName() != "Dummy")
    {
        // the wiring came back too: both tones are heard
        fHost.relocate(0);
        fHost.play();
        float peak = 0.0f;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1500)
        {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            const auto [l, r] = fHost.outputPeaks(fSession.master());
            peak = std::max({peak, l, r});
        }
        fHost.pause();
        std::printf("master peak %.3f\n", peak);
        check(peak > 0.9f, "both tracks play after undo");
    }
    std::printf(failures ? "UNDO TEST FAILED\n" : "UNDO TEST PASSED\n");
    std::fflush(stdout);
    return failures ? 1 : 0;
}

void MainWindow::afterUndo()
{
    // plugin numbers may have changed: windows of plugins would show the wrong one
    closeEditors();
    const int count = static_cast<int>(fSession.tracks().size());
    if (fSelected >= count)
        fSelected = count - 1;
    fArrange->setTempo(fSession.tempo());
    refreshTracks();
    refreshMixState();
    refreshSongInfo();
}

void MainWindow::updateTitle()
{
    const QString modified = fHistory->isModified() ? QStringLiteral(" *") : QString();
    setWindowTitle(fSessionPath.isEmpty() ? QStringLiteral("chost%1").arg(modified)
                                          : QStringLiteral("chost — %1%2").arg(QFileInfo(fSessionPath).fileName(), modified));
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
    if (!maybeSave())
    {
        event->ignore();
        return;
    }
    if (fStemDialog)
        fStemDialog->close();
    if (fTranscribeDialog)
        fTranscribeDialog->close();
    QMainWindow::closeEvent(event);
}

void MainWindow::showError(const QString& what)
{
    if (fErrorSink)
    {
        fErrorSink->append(what);
        return;
    }
    // offscreen and --screenshot runs (CI) must never block on a dialog
    if (QGuiApplication::platformName() == QLatin1String("offscreen") || qApp->property("chost-noninteractive").toBool())
    {
        qWarning("%s", qPrintable(what));
        return;
    }
    QMessageBox::warning(this, QStringLiteral("chost"), what);
}
