#pragma once

#include "core/HostEngine.h"

#include <QMainWindow>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QTimer;
class QTreeWidget;
class QVBoxLayout;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // used by --screenshot and tests: load a plugin as if double-clicked in the browser
    bool addPluginSpec(const QString& spec);
    bool loadAudioFile(const QString& path);
    void selectRackSlot(int row);

private:
    // one row in the parameter panel
    struct ParamRow {
        uint32_t index = 0;
        float min = 0.0f;
        float max = 1.0f;
        bool isOutput = false;
        bool isBoolean = false;
        bool isInteger = false;
        QString unit;
        QSlider* slider = nullptr;
        QCheckBox* toggle = nullptr;
        QLabel* valueLabel = nullptr;
    };

    void buildUi();
    void startEngine(const QString& driver);
    void onHostEvent(chost::HostEvent event, uint pluginId, int index, float value, const QString& text);

    void rescanPlugins();
    void filterPlugins(const QString& text);
    void rebuildRack();
    void rebuildParameters();
    void updateParamRow(ParamRow& row, float value);
    void tick();

    int selectedSlot() const;
    void moveSelected(int delta);
    void removeSelected();
    void showSelectedUi();

    void openAudioFile();
    void openProject();
    void saveProject();
    void togglePlay();
    void showError(const QString& what);

    chost::HostEngine fHost;
    std::vector<chost::PluginEntry> fPlugins;
    std::vector<ParamRow> fParamRows;
    int fParamPluginId = -1;
    bool fRebuildingRack = false;

    QComboBox* fDriverBox = nullptr;
    QPushButton* fPlayButton = nullptr;
    QLineEdit* fSearch = nullptr;
    QTreeWidget* fBrowser = nullptr;
    QListWidget* fRack = nullptr;
    QPushButton* fUiButton = nullptr;
    QLabel* fParamTitle = nullptr;
    QWidget* fParamContainer = nullptr;
    QVBoxLayout* fParamLayout = nullptr;
    QProgressBar* fMeterL = nullptr;
    QProgressBar* fMeterR = nullptr;
    QLabel* fEngineLabel = nullptr;
    QTimer* fTimer = nullptr;
};
