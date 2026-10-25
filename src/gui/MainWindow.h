#pragma once

#include "core/ClipEdit.h"
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
class ChordPadDialog;
class Control;
class History;
class MasterDialog;
class ScoreDialog;
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
    PianoRollDialog* pianoRoll() const { return fPianoRoll; } // the one open, if any
    // a MIDI track's file was rewritten (to newPath, maybe the same): its track plays and shows it, undoably
    void midiFileEdited(const QString& oldPath, const QString& newPath, const QString& what);
    bool isDrumTrack(int track);
    void play();
    void applyInstruments();   // every MIDI track gets the instrument set for its kind of part
    void instrumentSettings();
    void exportMix(bool autoStart = false);   // File > Export mixdown
    void masterMix(bool autoStart = false);   // AI > AI mastering
    void analyzeSong(bool quiet = false);     // tempo and key of the selected (or first) audio track
    void arrange(bool autoStart = false, const QString& chords = {}); // AI > auto-arrangement
    void tune(bool autoStart = false);        // AI > pitch correction of a vocal track
    ChordPadDialog* chordPad();               // AI > chord pad, opened (or raised)
    ScoreDialog* scoreDialog();               // File > sheet music, opened (or raised)
    // a clip edit on a track (its range from the arrangement's), into a new file the track then plays;
    // false, said in an error, when it can't be done
    bool editClip(int track, const chost::ClipEdit& edit, const QString& what);
    int addMarker(double seconds, const QString& name = {}); // its index
    void setRange(double start, double end);                  // the timeline's selection; equal: none
    double playhead() const;
    std::vector<int> scoreTracks();           // the MIDI tracks sheet music can be made of (not drums)

    // buses. A group takes the tracks' faders in (and shows them as a folder); an effect bus gets a reverb
    // or a delay from the user's plugins and a send from each track. The new bus's index, -1 on error.
    enum class BusKind { Group, Reverb, Delay };
    int addBus(BusKind kind, const std::vector<int>& tracks = {}, const QString& name = {});
    bool setTrackOutput(int track, int bus);            // bus -1: Stereo Out
    bool setTrackSend(int track, int bus, float level); // adds the send or sets its level; level < 0 removes it
    void setFolded(int track, bool folded);
    void foldGroups(); // every group folded (tests)

    // the metronome (Transport > Metronome, C): on or off and its volume are remembered for every project
    void setMetronome(bool on);
    void setMetronomeVolume(double db);
    bool metronomeOn() const;
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
    void addClipActions(QMenu* menu, int track); // the clip edits of a track, for its range
    void markerMenu(int marker, const QPoint& globalPos);
    void renameMarker(int marker);
    void jumpToMarker(int direction);            // the next (1) or previous (-1) one from the playhead
    void refreshMarkers();
    QString editFolder() const;                  // where edited clips are written
    void dropFiles(const QStringList& paths, int track);
    void refreshClick();       // the metronome follows the tempo and the song's length
    void outputMenu(int track, const QPoint& globalPos);
    void sendMenu(int track, int slot, const QPoint& globalPos);
    void addBusMenu(QMenu* menu, int track);             // new group / effect bus entries
    QString findEffect(BusKind kind) const;              // the user's best reverb or delay, empty without one
    void makeWet(uint plugin);                           // a plugin on an effect bus plays only the effect
    void placeInGroup(int track, chost::Uid group);       // display order: right after the group's tracks
    int depthOf(int track) const;                        // how many groups a track sits in

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
    void refreshChordPad();                                 // the tracks the chord pad plays on and listens to
    int placeChords(int track, const QString& midiPath);    // chords onto that empty instrument track, else a new one
    QString chordFolder() const;                            // where chord parts are written

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
    QPointer<ChordPadDialog> fChordPad;
    QPointer<ScoreDialog> fScoreDialog;
    History* fHistory = nullptr;
    Autosave* fAutosave = nullptr;
    Control* fControl = nullptr;
    QStringList* fErrorSink = nullptr; // while the remote control works: errors go here, not into message boxes
    ScriptJob* fAnalysis = nullptr;
    bool fAnalysisTried = false; // the automatic analysis of the first audio file, once per project
    bool fAnalysisQuiet = false;

    // transport
    QToolButton* fPlayButton = nullptr;
    QAction* fClickAction = nullptr;
    QString fClickFile;        // the click part the metronome plays, made for the tempo and length it has
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
