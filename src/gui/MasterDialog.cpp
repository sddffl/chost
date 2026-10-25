#include "MasterDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

// ---------------------------------------------------------------------------------------------------------------------
// EqCurveView

static constexpr double kRangeDb = 6.0;

EqCurveView::EqCurveView(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(120);
    setToolTip(QStringLiteral("母带对音色做的调整：线在中间以上是加强，以下是减弱"));
}

void EqCurveView::setCurve(const std::vector<std::pair<double, double>>& points)
{
    fPoints = points;
    update();
}

void EqCurveView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(34, 8, -8, -18);
    p.fillRect(rect(), QColor(0x1b, 0x1e, 0x22));
    auto x = [&](double hz) { return area.left() + area.width() * std::log10(hz / 20.0) / 3.0; };
    auto y = [&](double gain) { return area.center().y() - area.height() / 2.0 * std::clamp(gain, -kRangeDb, kRangeDb) / kRangeDb; };

    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.8);
    p.setFont(small);
    p.setPen(QColor(0x3a, 0x3f, 0x46));
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
    {
        p.drawLine(QPointF(x(hz), area.top()), QPointF(x(hz), area.bottom()));
        const QString label = hz >= 1000.0 ? QStringLiteral("%1k").arg(hz / 1000.0) : QString::number(hz);
        p.setPen(QColor(0x8b, 0x90, 0x96));
        p.drawText(QRectF(x(hz) - 20, area.bottom() + 2, 40, 14), Qt::AlignCenter, label);
        p.setPen(QColor(0x3a, 0x3f, 0x46));
    }
    for (double gain : {-6.0, -3.0, 0.0, 3.0, 6.0})
    {
        p.setPen(gain == 0.0 ? QColor(0x5a, 0x60, 0x68) : QColor(0x3a, 0x3f, 0x46));
        p.drawLine(QPointF(area.left(), y(gain)), QPointF(area.right(), y(gain)));
        p.setPen(QColor(0x8b, 0x90, 0x96));
        p.drawText(QRectF(0, y(gain) - 7, 30, 14), Qt::AlignRight | Qt::AlignVCenter,
                   gain > 0 ? QStringLiteral("+%1").arg(gain) : QString::number(gain));
    }

    if (fPoints.size() < 2)
        return;
    QPainterPath path;
    for (size_t i = 0; i < fPoints.size(); ++i)
    {
        const QPointF pt(x(std::clamp(fPoints[i].first, 20.0, 20000.0)), y(fPoints[i].second));
        if (i == 0)
            path.moveTo(pt);
        else
            path.lineTo(pt);
    }
    p.setPen(QPen(QColor(0xe0, 0xa4, 0x3a), 2.0));
    p.drawPath(path);
}

// ---------------------------------------------------------------------------------------------------------------------
// MasterDialog

MasterDialog::MasterDialog(const QString& defaultOutput, SnapshotFunction snapshot, QWidget* parent)
    : QDialog(parent)
    , fSnapshot(std::move(snapshot))
{
    setWindowTitle(QStringLiteral("AI 母带"));
    resize(600, 0);
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(QStringLiteral(
        "把整首混音做成可以发布的母带：自动调整音色平衡、低频收成单声道、轻微压缩让声音更整体，"
        "再把响度推到发布平台推荐的标准，峰值不超过 -1 dB。给一首你喜欢的歌做参考，音色会向它靠拢。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    auto* refRow = new QHBoxLayout;
    fReference = new QLineEdit;
    fReference->setPlaceholderText(QStringLiteral("可以不选"));
    refRow->addWidget(fReference, 1);
    auto* refBrowse = new QPushButton(QStringLiteral("选择…"));
    refRow->addWidget(refBrowse);
    form->addRow(QStringLiteral("参考歌曲"), refRow);

    fStyle = new QComboBox;
    fStyle->addItem(QStringLiteral("自然（只做平衡，不改变风格）"), QStringLiteral("natural"));
    fStyle->addItem(QStringLiteral("温暖（低频饱满，高频柔和）"), QStringLiteral("warm"));
    fStyle->addItem(QStringLiteral("明亮（高频清晰，中低频干净）"), QStringLiteral("bright"));
    fStyle->addItem(QStringLiteral("有力（鼓和低频更冲，压缩多一点）"), QStringLiteral("punchy"));
    form->addRow(QStringLiteral("风格"), fStyle);

    fTarget = new QComboBox;
    fTarget->addItem(QStringLiteral("流媒体标准 -14 LUFS（网易云、QQ音乐、Spotify、Apple Music）"), QStringLiteral("-14"));
    fTarget->addItem(QStringLiteral("响亮 -10 LUFS（抖音、快手等短视频）"), QStringLiteral("-10"));
    fTarget->addItem(QStringLiteral("最响 -8 LUFS（电子舞曲，动态会少一些）"), QStringLiteral("-8"));
    fTarget->addItem(QStringLiteral("和参考歌曲一样响"), QStringLiteral("reference"));
    form->addRow(QStringLiteral("响度"), fTarget);

    fMonoBass = new QCheckBox(QStringLiteral("120 Hz 以下的低频收成单声道（推荐）"));
    fMonoBass->setChecked(true);
    form->addRow(QString(), fMonoBass);

    auto* pathRow = new QHBoxLayout;
    fPath = new QLineEdit(QDir::toNativeSeparators(defaultOutput));
    pathRow->addWidget(fPath, 1);
    auto* browse = new QPushButton(QStringLiteral("浏览…"));
    pathRow->addWidget(browse);
    form->addRow(QStringLiteral("保存到"), pathRow);
    layout->addLayout(form);

    fStatus = new QLabel;
    fStatus->setObjectName(QStringLiteral("status")); // these and the busy property: what the remote control reports
    fStatus->setWordWrap(true);
    layout->addWidget(fStatus);
    fProgress = new QProgressBar;
    fProgress->setObjectName(QStringLiteral("progress"));
    fProgress->setRange(0, 100);
    layout->addWidget(fProgress);
    fResult = new QLabel;
    fResult->setWordWrap(true);
    fResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(fResult);
    fCurve = new EqCurveView;
    fCurve->hide();
    layout->addWidget(fCurve);

    auto* compare = new QHBoxLayout;
    fCompare = new QPushButton(QStringLiteral("放到新轨道上试听对比"));
    fCompare->setToolTip(QStringLiteral("母带放到一条新轨道上，用 A/B 按钮在母带和原混音之间切换"));
    compare->addWidget(fCompare);
    fAB = new QPushButton;
    fAB->setCheckable(true);
    compare->addWidget(fAB);
    fPlay = new QPushButton(QStringLiteral("▶ 播放 / 暂停"));
    compare->addWidget(fPlay);
    compare->addStretch();
    layout->addLayout(compare);

    auto* buttons = new QHBoxLayout;
    fOpenFolder = new QPushButton(QStringLiteral("打开所在文件夹"));
    buttons->addWidget(fOpenFolder);
    buttons->addStretch();
    fCancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(fCancel);
    fStart = new QPushButton(QStringLiteral("开始母带"));
    fStart->setDefault(true);
    buttons->addWidget(fStart);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(refBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择参考歌曲"), {},
                                                          QStringLiteral("音频文件 (*.wav *.flac *.mp3 *.ogg *.aiff *.aif);;所有文件 (*)"));
        if (!path.isEmpty())
            fReference->setText(QDir::toNativeSeparators(path));
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getSaveFileName(this, QStringLiteral("母带保存到"), QDir::fromNativeSeparators(fPath->text()),
                                                    QStringLiteral("WAV 音频 (*.wav)"));
        if (path.isEmpty())
            return;
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".wav");
        fPath->setText(QDir::toNativeSeparators(path));
    });
    connect(fStart, &QPushButton::clicked, this, &MasterDialog::start);
    connect(fCancel, &QPushButton::clicked, this, [this] {
        fRender.cancel();
        fScript.cancel();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(fOpenFolder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(fOutput).absolutePath()));
    });
    connect(fCompare, &QPushButton::clicked, this, [this] {
        fCompared = true;
        emit compareRequested(fOutput);
        setListening(true);
        emit listen(true);
        setBusy(false);
    });
    connect(fAB, &QPushButton::clicked, this, [this](bool master) {
        setListening(master);
        emit listen(master);
    });
    connect(fPlay, &QPushButton::clicked, this, &MasterDialog::playToggled);

    connect(&fRender, &RenderJob::progress, this, [this](int percent) {
        fStatus->setText(QStringLiteral("第 1 步：混音…"));
        fProgress->setValue(percent * 30 / 100);
    });
    connect(&fRender, &RenderJob::finished, this, [this](bool ok, const QString&, const QString& message) {
        if (!ok)
            finish(false, QStringLiteral("混音没有完成：%1").arg(message));
        else
            runMaster();
    });
    connect(&fScript, &ScriptJob::progress, this, [this](int percent, const QString&) {
        fStatus->setText(QStringLiteral("第 2 步：AI 母带处理…"));
        fProgress->setValue(30 + percent * 70 / 100);
    });
    connect(&fScript, &ScriptJob::result, this, &MasterDialog::showResult);
    connect(&fScript, &ScriptJob::finished, this, [this](bool ok, const QString& message) {
        finish(ok, ok ? QString() : message);
    });

    setListening(true);
    setBusy(false);
}

void MasterDialog::setAutoStart(bool autoStart)
{
    if (autoStart)
        QTimer::singleShot(0, this, &MasterDialog::start);
}

void MasterDialog::setListening(bool master)
{
    fAB->setChecked(master);
    fAB->setText(master ? QStringLiteral("A/B：正在听母带") : QStringLiteral("A/B：正在听原混音"));
}

void MasterDialog::start()
{
    if (fBusy)
        return;
    const QString reference = QDir::fromNativeSeparators(fReference->text().trimmed());
    if (!reference.isEmpty() && !QFileInfo::exists(reference))
    {
        fStatus->setText(QStringLiteral("找不到参考歌曲：%1").arg(reference));
        return;
    }
    if (reference.isEmpty() && fTarget->currentData().toString() == QLatin1String("reference"))
    {
        fStatus->setText(QStringLiteral("选了“和参考歌曲一样响”，请先选一首参考歌曲。"));
        return;
    }
    fOutput = QDir::fromNativeSeparators(fPath->text().trimmed());
    if (fOutput.isEmpty())
    {
        fStatus->setText(QStringLiteral("先选好母带保存到哪里。"));
        return;
    }
    if (QFileInfo(fOutput).suffix().compare(QLatin1String("wav"), Qt::CaseInsensitive) != 0)
        fOutput += QStringLiteral(".wav");
    fOutput = QFileInfo(fOutput).absoluteFilePath();
    QDir().mkpath(QFileInfo(fOutput).absolutePath());

    emit aboutToStart();
    fCompared = false;
    const RenderSnapshot snapshot = fSnapshot();
    if (snapshot.session.isEmpty())
    {
        fStatus->setText(QStringLiteral("没能准备工程：%1").arg(snapshot.error));
        setBusy(false);
        return;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/render");
    QDir().mkpath(folder);
    fMixFile = folder + QStringLiteral("/premaster.wav");
    fResult->clear();
    fCurve->hide();
    fProgress->setValue(0);
    fStatus->setText(QStringLiteral("第 1 步：混音（正在加载插件…）"));
    fBusy = true;
    setBusy(true);
    // 32-bit float: the mix may go over 0 dB, mastering brings it back without clipping
    fRender.start(snapshot.session, fMixFile, 32, {}, snapshot.masters);
}

void MasterDialog::runMaster()
{
    QStringList args{QStringLiteral("--input"), fMixFile, QStringLiteral("--output"), fOutput,
                     QStringLiteral("--style"), fStyle->currentData().toString(),
                     QStringLiteral("--target"), fTarget->currentData().toString(),
                     QStringLiteral("--ceiling"), QStringLiteral("-1")};
    const QString reference = QDir::fromNativeSeparators(fReference->text().trimmed());
    if (!reference.isEmpty())
        args << QStringLiteral("--reference") << reference;
    if (!fMonoBass->isChecked())
        args << QStringLiteral("--no-mono-bass");
    fStatus->setText(QStringLiteral("第 2 步：AI 母带处理…"));
    fScript.start(QStringLiteral("chost_master.py"), args);
}

void MasterDialog::showResult(const QString& key, const QString& value)
{
    const QStringList v = value.split(QLatin1Char('\t'));
    if (key == QLatin1String("BEFORE"))
    {
        fBefore[0] = v.value(0).toDouble();
        fBefore[1] = v.value(1).toDouble();
    }
    else if (key == QLatin1String("AFTER"))
    {
        const double limited = v.value(2).toDouble();
        QString text = QStringLiteral("响度：%1 → %2 LUFS\n峰值：%3 → %4 dB\n压缩 %5 dB，限制 %6 dB")
                           .arg(fBefore[0], 0, 'f', 1).arg(v.value(0).toDouble(), 0, 'f', 1)
                           .arg(fBefore[1], 0, 'f', 1).arg(v.value(1).toDouble(), 0, 'f', 1)
                           .arg(v.value(3).toDouble(), 0, 'f', 1).arg(limited, 0, 'f', 1);
        if (limited > 6.0)
            text += QStringLiteral("\n限制得比较多，鼓可能听起来被压扁了，可以试试小一点的响度。");
        fResult->setText(text);
    }
    else if (key == QLatin1String("EQ"))
    {
        std::vector<std::pair<double, double>> points;
        for (const QString& pair : value.split(QLatin1Char(',')))
            points.emplace_back(pair.section(QLatin1Char(':'), 0, 0).toDouble(), pair.section(QLatin1Char(':'), 1, 1).toDouble());
        fCurve->setCurve(points);
        fCurve->show();
    }
}

void MasterDialog::finish(bool ok, const QString& message)
{
    fBusy = false;
    if (ok)
    {
        fProgress->setValue(100);
        fStatus->setText(QStringLiteral("✓ 母带完成：%1").arg(QDir::toNativeSeparators(fOutput)));
    }
    else
    {
        QString reason = message;
        if (message.contains(QLatin1String("silent")))
            reason = QStringLiteral("混音没有声音（是不是所有轨道都静音了？）");
        else if (message.contains(QLatin1String("Permission denied")) || message.contains(QLatin1String("Error opening")))
            reason = QStringLiteral("文件写不进去（可能被别的软件打开着），换个保存位置或文件名再试：%1").arg(message);
        fStatus->setText(QStringLiteral("母带没有完成：%1").arg(reason));
    }
    setBusy(false);
    emit mastered(ok, fOutput);
}

void MasterDialog::setBusy(bool busy)
{
    setProperty("busy", busy);
    const bool done = !busy && QFileInfo::exists(fOutput) && fProgress->value() == 100;
    fStart->setEnabled(!busy);
    fCancel->setEnabled(busy);
    fCompare->setEnabled(done && !fCompared);
    fAB->setEnabled(fCompared && !busy);
    fPlay->setEnabled(fCompared);
    fOpenFolder->setEnabled(done);
    for (QWidget* w : std::initializer_list<QWidget*>{fReference, fStyle, fTarget, fMonoBass, fPath})
        w->setEnabled(!busy);
}

void MasterDialog::closeEvent(QCloseEvent* event)
{
    if (fBusy && QMessageBox::question(this, windowTitle(), QStringLiteral("还在做母带，要取消吗？")) != QMessageBox::Yes)
    {
        event->ignore();
        return;
    }
    fRender.cancel();
    fScript.cancel();
    QDialog::closeEvent(event);
}
