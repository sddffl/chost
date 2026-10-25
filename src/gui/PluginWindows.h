// Floating windows: a generic parameter editor for one plugin, and the plugin picker.

#pragma once

#include "core/HostEngine.h"

#include <QDialog>
#include <QWidget>

#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSlider;
class QTreeWidget;
class QVBoxLayout;

// Sliders for every parameter of one plugin, like the generic editor Cubase shows for plugins without a GUI.
class PluginEditor : public QWidget {
    Q_OBJECT

public:
    PluginEditor(chost::HostEngine& host, uint pluginId, QWidget* parent = nullptr);

    uint pluginId() const { return fPluginId; }
    void parameterChanged(uint32_t index, float value);
    void refreshOutputs();

private:
    struct Row {
        uint32_t index = 0;
        float min = 0.0f;
        float max = 1.0f;
        bool isOutput = false;
        bool isInteger = false;
        QString unit;
        QSlider* slider = nullptr;
        QCheckBox* toggle = nullptr;
        QLabel* valueLabel = nullptr;
    };

    void showValue(Row& row, float value);

    chost::HostEngine& fHost;
    uint fPluginId;
    std::vector<Row> fRows;
};

// Searchable list of the scanned plugins; returns the chosen plugin's spec.
class PluginPicker : public QDialog {
    Q_OBJECT

public:
    PluginPicker(const std::vector<chost::PluginEntry>& plugins, QWidget* parent = nullptr);
    QString selectedSpec() const;

private:
    void filter(const QString& text);

    QLineEdit* fSearch = nullptr;
    QTreeWidget* fList = nullptr;
};

// Fills a tree widget with plugins (name / format / maker), the spec stored as user data in column 0.
void fillPluginTree(QTreeWidget* tree, const std::vector<chost::PluginEntry>& plugins);
void filterPluginTree(QTreeWidget* tree, const QString& text);
