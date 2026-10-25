#include "PluginWindows.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

static constexpr int kSliderSteps = 1000;

PluginEditor::PluginEditor(chost::HostEngine& host, uint pluginId, QWidget* parent)
    : QWidget(parent, Qt::Tool)
    , fHost(host)
    , fPluginId(pluginId)
{
    setAttribute(Qt::WA_DeleteOnClose);
    const chost::RackSlot slot = host.slot(pluginId);
    setWindowTitle(QString::fromStdString(slot.name));
    resize(460, 420);

    auto* layout = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    auto* maker = new QLabel(QString::fromStdString(slot.maker));
    maker->setStyleSheet(QStringLiteral("color: #8b9096;"));
    top->addWidget(maker, 1);
    if (slot.hasCustomUi)
    {
        auto* custom = new QPushButton(QStringLiteral("插件自带界面"));
        top->addWidget(custom);
        connect(custom, &QPushButton::clicked, this, [this] { fHost.showCustomUi(fPluginId, true); });
    }
    layout->addLayout(top);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* container = new QWidget;
    auto* rows = new QVBoxLayout(container);
    scroll->setWidget(container);
    layout->addWidget(scroll);

    const std::vector<chost::ParamInfo> params = host.parameters(pluginId);
    fRows.reserve(params.size());
    for (const chost::ParamInfo& p : params)
    {
        Row row;
        row.index = p.index;
        row.min = p.min;
        row.max = p.max;
        row.isOutput = p.isOutput;
        row.isInteger = p.isInteger;
        row.unit = QString::fromStdString(p.unit);

        auto* line = new QHBoxLayout;
        auto* name = new QLabel(QString::fromStdString(p.name) + (p.isOutput ? QStringLiteral(" (输出)") : QString()));
        name->setMinimumWidth(140);
        line->addWidget(name);

        if (p.isBoolean)
        {
            row.toggle = new QCheckBox;
            row.toggle->setEnabled(!p.isOutput);
            line->addWidget(row.toggle, 1);
            const uint32_t index = p.index;
            const float on = p.max, off = p.min;
            connect(row.toggle, &QCheckBox::toggled, this, [this, index, on, off](bool checked) {
                fHost.setParameter(fPluginId, index, checked ? on : off);
            });
        }
        else
        {
            row.slider = new QSlider(Qt::Horizontal);
            row.slider->setRange(0, kSliderSteps);
            row.slider->setEnabled(!p.isOutput);
            line->addWidget(row.slider, 1);
        }

        row.valueLabel = new QLabel;
        row.valueLabel->setMinimumWidth(80);
        row.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        line->addWidget(row.valueLabel);
        rows->addLayout(line);

        fRows.push_back(row);
        showValue(fRows.back(), p.value);

        if (row.slider != nullptr && !p.isOutput)
        {
            const size_t i = fRows.size() - 1;
            connect(row.slider, &QSlider::valueChanged, this, [this, i](int pos) {
                Row& r = fRows[i];
                float value = r.min + (r.max - r.min) * static_cast<float>(pos) / kSliderSteps;
                if (r.isInteger)
                    value = std::round(value);
                fHost.setParameter(fPluginId, r.index, value);
                r.valueLabel->setText(QStringLiteral("%1 %2").arg(value, 0, 'g', 4).arg(r.unit));
            });
        }
    }
    if (params.empty())
        rows->addWidget(new QLabel(QStringLiteral("这个插件没有可调参数")));
    rows->addStretch();
}

void PluginEditor::parameterChanged(uint32_t index, float value)
{
    for (Row& row : fRows)
        if (row.index == index)
            showValue(row, value);
}

void PluginEditor::refreshOutputs()
{
    for (Row& row : fRows)
        if (row.isOutput)
            showValue(row, fHost.parameterValue(fPluginId, row.index));
}

void PluginEditor::showValue(Row& row, float value)
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

void fillPluginTree(QTreeWidget* tree, const std::vector<chost::PluginEntry>& plugins)
{
    tree->setSortingEnabled(false);
    tree->clear();
    for (const chost::PluginEntry& e : plugins)
    {
        auto* item = new QTreeWidgetItem(tree);
        item->setText(0, QString::fromStdString(e.name));
        item->setText(1, QString::fromStdString(e.format) + (e.isSynth ? QStringLiteral(" · 乐器") : QString()));
        item->setText(2, QString::fromStdString(e.maker));
        item->setToolTip(0, QString::fromStdString(e.spec));
        item->setData(0, Qt::UserRole, QString::fromStdString(e.spec));
    }
    tree->setSortingEnabled(true);
}

void filterPluginTree(QTreeWidget* tree, const QString& text)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        const bool match = text.isEmpty()
                           || item->text(0).contains(text, Qt::CaseInsensitive)
                           || item->text(1).contains(text, Qt::CaseInsensitive)
                           || item->text(2).contains(text, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
}

PluginPicker::PluginPicker(const std::vector<chost::PluginEntry>& plugins, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("添加插入效果"));
    resize(520, 560);

    auto* layout = new QVBoxLayout(this);
    fSearch = new QLineEdit;
    fSearch->setPlaceholderText(QStringLiteral("搜索名称、厂商或格式"));
    fSearch->setClearButtonEnabled(true);
    layout->addWidget(fSearch);

    fList = new QTreeWidget;
    fList->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("格式"), QStringLiteral("厂商")});
    fList->setRootIsDecorated(false);
    fList->setAlternatingRowColors(true);
    fList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    fillPluginTree(fList, plugins);
    fList->sortByColumn(0, Qt::AscendingOrder);
    layout->addWidget(fList);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("添加"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    connect(fSearch, &QLineEdit::textChanged, this, &PluginPicker::filter);
    connect(fList, &QTreeWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    fSearch->setFocus();
}

QString PluginPicker::selectedSpec() const
{
    const QTreeWidgetItem* item = fList->currentItem();
    return item != nullptr ? item->data(0, Qt::UserRole).toString() : QString();
}

void PluginPicker::filter(const QString& text)
{
    filterPluginTree(fList, text);
}
