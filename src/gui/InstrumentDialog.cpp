#include "InstrumentDialog.h"

#include "Instruments.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

// combo item data: the spec, or one of these
static const QString kBundled = QStringLiteral("@bundled");
static const QString kRemembered = QStringLiteral("@remembered");

InstrumentDialog::InstrumentDialog(const std::vector<chost::PluginEntry>& plugins, bool discovering, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("乐器设置"));
    resize(620, 520);
    for (const chost::PluginEntry& e : plugins)
        if (e.isSynth)
            fSynths.push_back(e);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral(
        "转出来的 MIDI 轨和新建的乐器轨，按它是什么乐器自动挂上这里选的音源。点“自动匹配”会从你装的插件里找专门的乐器"
        "（比如 Superior Drummer、Trilian、Keyscape）。\n\nKontakt 这类采样器要先选好音色：在乐器轨上点“打开界面”加载音色库，"
        "再点检查器里的“记住这个音色”，以后同一种乐器就自动用它。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* grid = new QGridLayout;
    int row = 0;
    for (const instruments::Role& role : instruments::roles())
    {
        grid->addWidget(new QLabel(role.name), row, 0);
        auto* box = new QComboBox;
        box->setMinimumWidth(380);
        box->setMaxVisibleItems(24);
        grid->addWidget(box, row, 1);
        fBoxes.push_back(box);
        connect(box, &QComboBox::activated, this, [this, row] { chosen(row); });
        ++row;
    }
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);

    fStatus = new QLabel;
    fStatus->setWordWrap(true);
    fStatus->setStyleSheet(QStringLiteral("color: #8b9096;"));
    if (discovering)
        fStatus->setText(QStringLiteral("还在识别你的插件，识别完的乐器会陆续出现，可以稍后再打开这个窗口。"));
    else if (fSynths.empty())
        fStatus->setText(QStringLiteral("没找到乐器插件。如果你的插件装在别的文件夹，请在 插件 → 插件文件夹 里添加。"));
    layout->addWidget(fStatus);
    layout->addStretch();

    auto* buttons = new QHBoxLayout;
    auto* match = new QPushButton(QStringLiteral("自动匹配我的乐器"));
    buttons->addWidget(match);
    buttons->addStretch();
    auto* apply = new QPushButton(QStringLiteral("应用到现有 MIDI 轨"));
    apply->setToolTip(QStringLiteral("把工程里所有 MIDI 轨换成上面选的乐器"));
    buttons->addWidget(apply);
    auto* close = new QPushButton(QStringLiteral("关闭"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(match, &QPushButton::clicked, this, &InstrumentDialog::autoMatch);
    connect(apply, &QPushButton::clicked, this, &InstrumentDialog::applyRequested);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    fill();
}

void InstrumentDialog::fill()
{
    const std::vector<instruments::Role>& roles = instruments::roles();
    for (size_t i = 0; i < roles.size(); ++i)
    {
        QComboBox* box = fBoxes[i];
        const instruments::Choice current = instruments::stored(roles[i].key);
        box->clear();
        box->addItem(roles[i].key == QLatin1String("default") ? QStringLiteral("自带 GM 音色") : QStringLiteral("跟“其余 MIDI 轨”一样"),
                     kBundled);
        if (!current.spec.isEmpty() && !current.state.isEmpty())
            box->addItem(QStringLiteral("%1（已记住的音色）").arg(current.name), kRemembered);
        for (const chost::PluginEntry& e : fSynths)
        {
            const QString name = QString::fromStdString(e.name);
            box->addItem(instruments::isEmptySampler(name) ? QStringLiteral("%1（需要先选音色）").arg(name)
                                                            : QStringLiteral("%1 · %2").arg(name, QString::fromStdString(e.maker)),
                         QString::fromStdString(e.spec));
        }
        int index = 0;
        if (!current.spec.isEmpty())
            index = !current.state.isEmpty() ? 1 : std::max(0, box->findData(current.spec));
        box->setCurrentIndex(index);
    }
}

void InstrumentDialog::chosen(int row)
{
    const QString key = instruments::roles()[static_cast<size_t>(row)].key;
    const QString data = fBoxes[static_cast<size_t>(row)]->currentData().toString();
    if (data == kRemembered)
        return;
    if (data == kBundled)
    {
        instruments::clear(key);
        return;
    }
    for (const chost::PluginEntry& e : fSynths)
        if (QString::fromStdString(e.spec) == data)
            instruments::store(key, {data, QString::fromStdString(e.name), {}});
}

void InstrumentDialog::autoMatch()
{
    const std::map<QString, chost::PluginEntry> found = instruments::autoMatch(fSynths);
    QStringList names;
    for (const auto& [key, entry] : found)
    {
        // a sound the user remembered on purpose stays
        if (!instruments::stored(key).state.isEmpty())
            continue;
        instruments::store(key, {QString::fromStdString(entry.spec), QString::fromStdString(entry.name), {}});
        names << QStringLiteral("%1 → %2").arg(instruments::roleName(key), QString::fromStdString(entry.name));
    }
    fill();
    fStatus->setText(names.isEmpty() ? QStringLiteral("没在你的插件里找到专门的鼓、贝斯、钢琴、吉他或合成器音源，可以在上面手动选。")
                                     : QStringLiteral("找到了：") + names.join(QStringLiteral("，")));
}
