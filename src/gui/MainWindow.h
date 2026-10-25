#pragma once

#include "core/HostEngine.h"
#include "core/MidiFile.h"
#include "core/Session.h"
#include "Instruments.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

#include <map>
#include <vector>

class ArrangeDialog;
class ArrangeView;
class ChannelStrip;
class ExportDialog;
class Autosave;
class Control;
class History;
class MasterDialog;
class ScriptJob;
struct RenderSnapshot;
class InsertRack;
class PianoRollDialog;
class PluginDiscovery;
class PluginEditor;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;
class QTreeWidget;
class StemDialog;
class TranscribeDialog;
class TuneDialog;

// Cubase-style main window: transport bar on top, inspector on the left, the project's audio tracks in
// the middle, the plugin browser on the right and the MixConsole below.
//
// The tracks live in a chost::Session on Carla's patchbay: every track is an audio file player, its
// inserts and a fader, all summed into the "Stereo Out" master.
class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class Control; // the remote control does what the user does in the window

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool addPluginSpec(const QString& spec);       // as an insert of the selected track
    bool loadAudioFile(const QString& path);       // into the selected track, or a new one
    int addAudioTrack(const QString& path, const QString& name = {}); // a .mid makes an instrument track; -1 on error
    int addMidiTrack(const QString& path, const QString& name = {}, const QString& role = {});
    bool openSession(const QString& path);
    // a project an earlier chost could not save before it went down: asks whether to bring it back
    // (autoRecover: without asking, for tests)
    void recoverAtStart(bool autoRecover);
    void saveSessionNow() { saveSession(); }
    void selectTrack(int track);
    void separateStems(bool autoStart = false); // AI stem separation of the selected track
    void transcribe(bool allTracks, bool autoStart = false); // AI audio to MIDI
    void editNotes(int track);                               // the piano roll of a MIDI track
    bool isDrumTrack(int track);
    void play();
    void applyInstruments();   // every MIDI track gets the instrument set for its kind of part
    void instrumentSettings();
    void exportMix(bool autoStart = false);   // File > Export mixdown
    void masterMix(bool autoStart = false);   // AI > AI mastering
    void analyzeSong(bool quiet = false);     // tempo and key of the selected (or first) audio track
    void arrange(bool autoStart = false, const QString& chords = {}); // AI > auto-arrangement
    void tune(bool autoStart = false);        // AI > pitch correction of a vocal track
    int testUndo(const QString& insertSpec);  // the self test of undo and redo (CI), 0 when it passes

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct Clip {
        bool midi = false;
        bool drums = false; // a MIDI clip with only General MIDI drums
        std::vector<chost::MidiNote> notes;
        std::vector<float> peaks;
        double seconds = 0.0;
        int sampleRate = 0;
        int channels = 0;
        bool decoded = false;
    };

    void buildMenus();
    void buildTransport();
    QWidget* buildInspector();
    QWidget* buildBrowser();
    QWidget* buildMixConsole();
    void connectInsertRack(InsertRack* rack);

    void startEngine(const QString& driver);
    void onHostEvent(chost::HostEvent event, uint pluginId, int index, float value, const QString& text);
    void tick();

    // views of the session
    const Clip& clip(const QString& path);
    QColor trackColour(int track) const;
    void refreshTracks();      // after tracks were added, removed or renamed
    void refreshSelection();   // inspector and highlights for the selected track
    void refreshInserts();
    void refreshMixState();    // mute / solo / volume on every view
    void closeEditors();
    std::vector<uint> selectedInserts() const;

    void addEmptyTrack();
    void addInstrumentTrack();
    bool loadInstrument(int track, const instruments::Choice& choice); // falls back to the bundled sounds
    void rememberInstrument(); // the selected track's instrument and sound become the default for its part
    QString instrumentText(int track); // instrument and sound, shown on the lane
    bool fits(int track, const QString& path) const; // MIDI on instrument tracks, audio on audio tracks
    void chooseInstrument();
    void editInstrument();
    void refreshInstrument();
    void importAudio();
    void replaceAudio();
    void removeSelectedTrack();
    void renameSelectedTrack(const QString& name);
    int trackOfFile(const QString& path) const;
    void placeMidiTrack(const QStringList& sources, const QString& midiPath, const QString& name, const QString& role);
    void trackMenu(int track, const QPoint& globalPos);
    void dropFiles(const QStringList& paths, int track);

    void addInsert();
    void editInsert(int index);
    void setInsertActive(int index, bool active);
    void removeInsert(int index);
    void moveInsert(int index, int delta);
    void setMuted(int track, bool muted);
    void setSolo(int track, bool solo);
    void setVolume(int track, float volume);

    void editPluginFolders();
    void rescanPlugins();
    void showPlugins();
    void newSession();
    void resetSession();     // newSession without asking about unsaved changes
    void openSessionDialog();
    void saveSession();
    void saveSessionAs();
    void openVersion();      // one of the versions kept from before the project was saved over
    bool maybeSave();        // unsaved changes: save, discard or (false) leave things as they are
    // a project file opened in place of fSessionPath: saving puts it there, until then it is unsaved
    bool openAsUnsaved(const QString& file, const QString& projectPath);
    void togglePlay();
    void stop();
    void seek(double seconds);
    void showError(const QString& what);
    void updateTitle();

    RenderSnapshot renderSnapshot(const QString& name); // the project saved for chost-render, as <name>.chost
    QString outputFolder() const;    // where exports go by default
    QString projectName() const;
    void setTempo(double bpm);
    void refreshSongInfo();          // tempo and key in the transport bar
    void listenToMaster(bool master);
    void afterUndo();                                       // the views follow the session put back
    QString originalOf(const QString& file) const;          // the take a tuned file was made from, else the file
    int trackPlaying(const QString& original) const;        // the track with this take or its tuned one
    void playTake(const QString& original, bool tuned);     // swaps a track between the take and its tuned one

    chost::HostEngine fHost;
    chost::Session fSession{fHost};
    std::vector<chost::PluginEntry> fPluginFiles; // as scanned: VST files by name
    std::vector<chost::PluginEntry> fPlugins;     // what they hold, as far as discovered
    PluginDiscovery* fDiscovery = nullptr;
    std::map<uint, QPointer<PluginEditor>> fEditors;
    QHash<QString, Clip> fClips;
    int fNextColour = 0;
    int fSelected = -1;
    QString fSessionPath;
    QPointer<StemDialog> fStemDialog;
    QPointer<TranscribeDialog> fTranscribeDialog;
    QPointer<PianoRollDialog> fPianoRoll;
    QPointer<ExportDialog> fExportDialog;
    QPointer<MasterDialog> fMasterDialog;
    QPointer<ArrangeDialog> fArrangeDialog;
    QPointer<TuneDialog> fTuneDialog;
    History* fHistory = nullptr;
    Autosave* fAutosave = nullptr;
    Control* fControl = nullptr;
    QStringList* fErrorSink = nullptr; // while the remote control works: errors go here, not into message boxes
    ScriptJob* fAnalysis = nullptr;
    bool fAnalysisTried = false; // the automatic analysis of the first audio file, once per project
    bool fAnalysisQuiet = false;

    // transport
    QToolButton* fPlayButton = nullptr;
    QLabel* fTimeLcd = nullptr;
    QLabel* fBarsLcd = nullptr;
    QDoubleSpinBox* fTempoBox = nullptr;
    QLabel* fKeyLabel = nullptr;
    QComboBox* fDriverBox = nullptr;
    QLabel* fDspLabel = nullptr;

    // zones
    QLineEdit* fInspectorName = nullptr;
    QLabel* fInspectorInfo = nullptr;
    QPushButton* fInspectorReplace = nullptr;
    QPushButton* fInspectorStems = nullptr;
    QPushButton* fInspectorMidi = nullptr;
    QPushButton* fInspectorTune = nullptr;
    QWidget* fInstrumentBox = nullptr;
    QLabel* fInstrumentName = nullptr;
    QPushButton* fInstrumentEdit = nullptr;
    QPushButton* fInstrumentRemember = nullptr;
    QPushButton* fInstrumentNotes = nullptr;
    QComboBox* fProgramBox = nullptr;
    InsertRack* fInspectorInserts = nullptr;
    ArrangeView* fArrange = nullptr;
    QLineEdit* fSearch = nullptr;
    QTreeWidget* fBrowser = nullptr;
    QHBoxLayout* fStripRow = nullptr;
    std::vector<ChannelStrip*> fStrips;
    ChannelStrip* fOutputStrip = nullptr;

    QTimer* fTimer = nullptr;
};
