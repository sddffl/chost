#include "BalanceDialog.h"

#include "core/AudioConvert.h"
#include "core/Loudness.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

double toDb(float volume)
{
    return volume > 0.0f ? 20.0 * std::log10(volume) : -120.0;
}

QString dbText(double db)
{
    return db <= -100.0 ? QStringLiteral("-∞") : QString::number(db, 'f', 1);
}

// the parts a track can be, in the order the menu offers them
const std::vector<std::pair<QString, QString>>& roleNames()
{
    static const std::vector<std::pair<QString, QString>> names{
        {QStringLiteral("lead"), QStringLiteral("主唱")},    {QStringLiteral("vocals"), QStringLiteral("人声")},
        {QStringLiteral("backing"), QStringLiteral("和声")}, {QStringLiteral("drums"), QStringLiteral("鼓（整套）")},
        {QStringLiteral("kick"), QStringLiteral("底鼓")},    {QStringLiteral("snare"), QStringLiteral("军鼓")},
        {QStringLiteral("toms"), QStringLiteral("通鼓")},    {QStringLiteral("hh"), QStringLiteral("踩镲")},
        {QStringLiteral("ride"), QStringLiteral("叮叮镲")},  {QStringLiteral("crash"), QStringLiteral("吊镲")},
        {QStringLiteral("bass"), QStringLiteral("贝斯")},    {QStringLiteral("guitar"), QStringLiteral("吉他")},
        {QStringLiteral("piano"), QStringLiteral("钢琴")},   {QStringLiteral("keys"), QStringLiteral("键盘")},
        {QStringLiteral("strings"), QStringLiteral("弦乐")}, {QStringLiteral("pad"), QStringLiteral("铺底")},
        {QStringLiteral("chords"), QStringLiteral("和弦")},  {QStringLiteral("other"), QStringLiteral("其他")},
    };
    return names;
}

} // namespace

BalanceDialog::BalanceDialog(TrackFunction tracks, SnapshotFunction snapshot, ApplyFunction apply, RoleFunction role, QWidget* parent)
    : QDialog(parent)
    , fTrackList(std::move(tracks))
    , fSnapshot(std::move(snapshot))
    , fApply(std::move(apply))
    , fRole(std::move(role))
{
    setWindowTitle(QStringLiteral("AI 自动混音平衡"));
    resize(600, 480);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral(
        "把每条轨道单独过一遍（连同它的插件），测出听起来有多响，再把推子调到一个常见的流行歌平衡："
        "人声在最前面，鼓和贝斯紧跟着，吉他、钢琴、弦乐往后放。这只是一个好的起点，之后照自己的耳朵再微调。"
        "静音的轨道不动。一步就能撤销（Ctrl+Z）。做完以后，“当作”认错了的轨道可以直接改，推子马上跟着重新调。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    fStyle = new QComboBox;
    fStyle->addItem(QStringLiteral("人声突出（流行、民谣）"), static_cast<int>(chost::VocalStyle::Forward));
    fStyle->addItem(QStringLiteral("正常"), static_cast<int>(chost::VocalStyle::Normal));
    fStyle->addItem(QStringLiteral("人声融进乐队（摇滚、电子）"), static_cast<int>(chost::VocalStyle::Blend));
    fStyle->setCurrentIndex(1);
    form->addRow(QStringLiteral("人声"), fStyle);
    layout->addLayout(form);

    fResults = new QTreeWidget;
    fResults->setObjectName(QStringLiteral("results"));
    fResults->setHeaderLabels({QStringLiteral("轨道"), QStringLiteral("当作"), QStringLiteral("原来响度"), QStringLiteral("推子")});
    fResults->setRootIsDecorated(false);
    fResults->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(fResults, 1);

    fStatus = new QLabel;
    fStatus->setObjectName(QStringLiteral("status")); // these and the busy property: what the remote control reports
    fStatus->setWordWrap(true);
    layout->addWidget(fStatus);
    fProgress = new QProgressBar;
    fProgress->setObjectName(QStringLiteral("progress"));
    fProgress->setRange(0, 100);
    layout->addWidget(fProgress);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    fCancel = new QPushButton(QStringLiteral("取消"));
    buttons->addWidget(fCancel);
    fStart = new QPushButton(QStringLiteral("开始"));
    fStart->setDefault(true);
    buttons->addWidget(fStart);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(fStart, &QPushButton::clicked, this, &BalanceDialog::start);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(fCancel, &QPushButton::clicked, this, [this] {
        fTracks.clear();
        fRender.cancel();
    });
    connect(&fRender, &RenderJob::progress, this, [this](int percent) {
        const size_t total = fTracks.size() + 1;
        const size_t done = fCheckingMix ? fTracks.size() : fCurrent;
        fProgress->setValue(static_cast<int>((done * 100 + static_cast<size_t>(percent)) / total));
    });
    connect(&fRender, &RenderJob::finished, this, [this](bool ok, const QString& output, const QString& message) {
        if (fTracks.empty())
        {
            fStatus->setText(QStringLiteral("已取消，推子没有动。"));
            setBusy(false);
            emit balanced(false);
            return;
        }
        if (!ok)
        {
            fStatus->setText(QStringLiteral("没做完：%1").arg(message));
            setBusy(false);
            emit balanced(false);
            return;
        }
        std::vector<float> samples;
        chost::AudioFileInfo info;
        const bool read = chost::readAudioFile(output.toStdString(), samples, info);
        QFile::remove(output);
        const chost::Loudness loudness = read ? chost::measureLoudness(samples, info.channels, info.sampleRate) : chost::Loudness{};
        if (fCheckingMix)
        {
            // too close to 0 dB: every balanced fader comes down by the same amount
            QString line = QStringLiteral("完成。整首混音的响度 %1 LUFS，最高电平 %2 dB").arg(loudness.integrated, 0, 'f', 1).arg(loudness.peak, 0, 'f', 1);
            if (read && loudness.peak > -3.0)
            {
                const float scale = static_cast<float>(std::pow(10.0, (-3.0 - loudness.peak) / 20.0));
                std::vector<std::pair<chost::Uid, float>> volumes;
                for (size_t i = 0; i < fTracks.size(); ++i)
                    if (!fParts[i].silent)
                    {
                        fNew[i] *= scale;
                        volumes.push_back({fTracks[i].uid, fNew[i]});
                    }
                fApply(volumes);
                line += QStringLiteral("，为了留出余量，所有推子一起降了 %1 dB").arg(-20.0 * std::log10(scale), 0, 'f', 1);
                for (int i = 0; i < fResults->topLevelItemCount() && i < static_cast<int>(fNew.size()); ++i)
                    if (!fParts[static_cast<size_t>(i)].silent)
                        fResults->topLevelItem(i)->setText(
                            3, QStringLiteral("%1 → %2 dB").arg(dbText(toDb(fTracks[static_cast<size_t>(i)].volume)), dbText(toDb(fNew[static_cast<size_t>(i)]))));
            }
            fProgress->setValue(100);
            fStatus->setText(line + QStringLiteral("。不满意按 Ctrl+Z 撤销。"));
            setBusy(false);
            emit balanced(true);
            return;
        }
        fParts[fCurrent].loudness = loudness.integrated;
        fParts[fCurrent].silent = !read || loudness.silent;
        auto* item = fResults->topLevelItem(static_cast<int>(fCurrent));
        item->setText(2, fParts[fCurrent].silent ? QStringLiteral("没有声音") : QStringLiteral("%1 LUFS").arg(loudness.integrated, 0, 'f', 1));
        ++fCurrent;
        next();
    });
    setBusy(false);
}

void BalanceDialog::setAutoStart(bool autoStart)
{
    if (autoStart)
        QTimer::singleShot(0, this, &BalanceDialog::start);
}

void BalanceDialog::setVocalStyle(chost::VocalStyle style)
{
    fStyle->setCurrentIndex(fStyle->findData(static_cast<int>(style)));
}

QString BalanceDialog::renderPath(int n) const
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/render");
    QDir().mkpath(folder);
    return folder + QStringLiteral("/balance-%1.wav").arg(n);
}

void BalanceDialog::start()
{
    if (fRender.isRunning())
        return;
    fTracks = fTrackList();
    if (fTracks.empty())
    {
        fStatus->setText(QStringLiteral("没有可以平衡的轨道：要有声音、没有静音的音轨或 MIDI 轨。"));
        return;
    }
    const RenderSnapshot snapshot = fSnapshot();
    if (snapshot.session.isEmpty())
    {
        fStatus->setText(QStringLiteral("没能准备工程：%1").arg(snapshot.error));
        return;
    }
    fSession = snapshot.session;
    fParts.clear();
    fNew.clear();
    fResults->clear();
    fRoles.clear();
    for (size_t i = 0; i < fTracks.size(); ++i)
    {
        const Track& t = fTracks[i];
        chost::BalancePart part;
        part.role = t.role.toStdString();
        part.volume = t.volume;
        fParts.push_back(part);
        auto* item = new QTreeWidgetItem({t.name, QString(), QStringLiteral("…"), dbText(toDb(t.volume))});
        fResults->addTopLevelItem(item);
        auto* role = new QComboBox;
        for (const auto& [key, name] : roleNames())
            role->addItem(name, key);
        const int known = role->findData(t.role);
        role->setCurrentIndex(known >= 0 ? known : role->findData(QStringLiteral("other")));
        role->setEnabled(false);
        fResults->setItemWidget(item, 1, role);
        fRoles.push_back(role);
        connect(role, &QComboBox::activated, this, [this, i, role] { roleChosen(i, role->currentData().toString()); });
    }
    fCurrent = 0;
    fCheckingMix = false;
    fProgress->setValue(0);
    setBusy(true);
    next();
}

void BalanceDialog::next()
{
    if (fCurrent < fTracks.size())
    {
        fStatus->setText(QStringLiteral("正在听：%1（%2/%3）").arg(fTracks[fCurrent].name).arg(fCurrent + 1).arg(fTracks.size()));
        fRender.start(fSession, renderPath(static_cast<int>(fCurrent)), 32, {fTracks[fCurrent].index}, {}, false);
        return;
    }
    finish();
}

void BalanceDialog::finish()
{
    const auto style = static_cast<chost::VocalStyle>(fStyle->currentData().toInt());
    fNew = chost::balanceVolumes(fParts, style);
    std::vector<std::pair<chost::Uid, float>> volumes;
    for (size_t i = 0; i < fTracks.size(); ++i)
    {
        if (fParts[i].silent)
            continue;
        volumes.push_back({fTracks[i].uid, fNew[i]});
        fResults->topLevelItem(static_cast<int>(i))->setText(
            3, QStringLiteral("%1 → %2 dB").arg(dbText(toDb(fTracks[i].volume)), dbText(toDb(fNew[i]))));
    }
    if (volumes.empty())
    {
        fStatus->setText(QStringLiteral("这些轨道都没有声音，推子没有动。"));
        setBusy(false);
        emit balanced(false);
        return;
    }
    fApply(volumes);

    // the whole mix once more, to keep it off 0 dB
    const RenderSnapshot snapshot = fSnapshot();
    if (snapshot.session.isEmpty())
    {
        fStatus->setText(QStringLiteral("推子调好了，但没能检查整首的电平：%1").arg(snapshot.error));
        setBusy(false);
        emit balanced(true);
        return;
    }
    fCheckingMix = true;
    fStatus->setText(QStringLiteral("推子调好了，正在检查整首混音的电平…"));
    fRender.start(snapshot.session, renderPath(999), 32, {}, snapshot.masters, false);
}

void BalanceDialog::roleChosen(size_t track, const QString& role)
{
    if (fRender.isRunning() || track >= fTracks.size() || fParts.size() != fTracks.size())
        return;
    fTracks[track].role = role;
    fParts[track].role = role.toStdString();
    if (fRole)
        fRole(fTracks[track].uid, role);
    setBusy(true);
    finish();
}

void BalanceDialog::setBusy(bool busy)
{
    // a part can be changed once every track was measured
    const bool measured = !busy && !fTracks.empty() && fParts.size() == fTracks.size() && fCurrent >= fTracks.size();
    for (QComboBox* role : fRoles)
        role->setEnabled(measured);
    setProperty("busy", busy);
    fStart->setEnabled(!busy);
    fCancel->setEnabled(busy);
    fStyle->setEnabled(!busy);
}

void BalanceDialog::closeEvent(QCloseEvent* event)
{
    if (fRender.isRunning())
    {
        fTracks.clear();
        fRender.cancel();
    }
    QDialog::closeEvent(event);
}
