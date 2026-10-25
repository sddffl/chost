#pragma once

#include "core/HostEngine.h"

#include <QMainWindow>
#include <QPointer>

#include <map>
#include <vector>

class ArrangeView;
class ChannelStrip;
class InsertRack;
class PluginEditor;
class QComboBox;
class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;
class QTreeWidget;

// Cubase-style main window: transport bar on top, inspector on the left, the project's audio track in
// the middle, the plugin browser on the right and the MixConsole below.
//
// The engine is Carla's rack: the audio file player sits in slot 0 and acts as the audio track,
// every plugin after it is one of the track's inserts.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool addPluginSpec(const QString& spec);
    bool loadAudioFile(const QString& path);
    void play();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildMenus();
    void buildTransport();
    QWidget* buildInspector();
    QWidget* buildBrowser();
    QWidget* buildMixConsole();
    void connectInsertRack(InsertRack* rack);

    void startEngine(const QString& driver);
    void onHostEvent(chost::HostEvent event, uint pluginId, int index, float value, const QString& text);
    void tick();

    // track model on top of the rack
    int trackSlot() const;
    std::vector<uint> insertIds() const;
    void refreshTrack();
    void refreshInserts();
    void closeEditors();

    void addInsert();
    void editInsert(int index);
    void setInsertActive(int index, bool active);
    void removeInsert(int index);
    void moveInsert(int index, int delta);
    void setTrackMuted(bool muted);
    void setTrackVolume(float volume);

    void rescanPlugins();
    void openAudioFile();
    void openProject();
    void saveProject();
    void togglePlay();
    void stop();
    void seek(double seconds);
    void showError(const QString& what);

    chost::HostEngine fHost;
    std::vector<chost::PluginEntry> fPlugins;
    std::map<uint, QPointer<PluginEditor>> fEditors;
    QString fTrackFile;
    double fTrackSeconds = 0.0;

    // transport
    QToolButton* fPlayButton = nullptr;
    QLabel* fTimeLcd = nullptr;
    QLabel* fBarsLcd = nullptr;
    QComboBox* fDriverBox = nullptr;
    QLabel* fDspLabel = nullptr;

    // zones
    QLabel* fInspectorTrack = nullptr;
    QLabel* fInspectorInfo = nullptr;
    InsertRack* fInspectorInserts = nullptr;
    ArrangeView* fArrange = nullptr;
    QLineEdit* fSearch = nullptr;
    QTreeWidget* fBrowser = nullptr;
    ChannelStrip* fTrackStrip = nullptr;
    ChannelStrip* fOutputStrip = nullptr;

    QTimer* fTimer = nullptr;
};
