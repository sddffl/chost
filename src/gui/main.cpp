#include "ClaudeConnect.h"
#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QIcon>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("chost"));
    QApplication::setOrganizationName(QStringLiteral("chost"));
    QApplication::setApplicationVersion(QStringLiteral(CHOST_VERSION));
    theme::apply(app);
    QIcon icon;
    for (int size : {16, 32, 48, 256, 512})
        icon.addFile(QStringLiteral(":/icons/icon-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(icon);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("chost audio plugin host"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption pluginOpt(QStringLiteral("load"), QStringLiteral("Load a plugin at startup (lv2:URI, vst3:PATH, ...)."), QStringLiteral("spec"));
    QCommandLineOption fileOpt(QStringLiteral("file"), QStringLiteral("Load an audio file as a new track at startup (repeatable)."), QStringLiteral("path"));
    // not "session": Qt takes -session / --session for itself (desktop session management) before the parser sees it
    QCommandLineOption sessionOpt(QStringList{QStringLiteral("open"), QStringLiteral("project")}, QStringLiteral("Open a .chost project at startup."), QStringLiteral("path"));
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot of the window after startup and quit."), QStringLiteral("png"));
    QCommandLineOption separateOpt(QStringLiteral("separate"), QStringLiteral("Open the AI stem separation for the selected track and start it."));
    QCommandLineOption delayOpt(QStringLiteral("screenshot-delay"), QStringLiteral("Milliseconds before the screenshot (default 1500)."), QStringLiteral("ms"), QStringLiteral("1500"));
    QCommandLineOption transcribeOpt(QStringLiteral("transcribe"), QStringLiteral("Open AI audio to MIDI for every audio track and start it."));
    QCommandLineOption selectOpt(QStringLiteral("select"), QStringLiteral("Select a track at startup (0 is the first)."), QStringLiteral("n"));
    QCommandLineOption applyOpt(QStringLiteral("apply-instruments"), QStringLiteral("Put the instruments from the settings on every MIDI track."));
    QCommandLineOption instrumentsOpt(QStringLiteral("instrument-settings"), QStringLiteral("Open the instrument settings."));
    QCommandLineOption notesOpt(QStringLiteral("edit-notes"), QStringLiteral("Open the MIDI editor of the selected track."));
    QCommandLineOption exportOpt(QStringLiteral("export-mix"), QStringLiteral("Open File > Export mixdown and start it."));
    QCommandLineOption masterOpt(QStringLiteral("master"), QStringLiteral("Open AI mastering and start it."));
    QCommandLineOption analyzeOpt(QStringLiteral("analyze"), QStringLiteral("Find the tempo and key of the selected (or first) audio track."));
    QCommandLineOption undoOpt(QStringLiteral("test-undo"), QStringLiteral("Self test of undo and redo on the loaded tracks, with this insert plugin."), QStringLiteral("spec"));
    QCommandLineOption tuneOpt(QStringLiteral("tune"), QStringLiteral("Open AI pitch correction for the selected track and start it."));
    QCommandLineOption arrangeOpt(QStringLiteral("arrange"), QStringLiteral("Open AI auto-arrangement for the selected track and start it."));
    QCommandLineOption chordsOpt(QStringLiteral("arrange-chords"), QStringLiteral("Open AI auto-arrangement with these chords and start it."), QStringLiteral("chords"));
    QCommandLineOption recoverOpt(QStringLiteral("recover"), QStringLiteral("Bring back a project a crashed chost left in the autosave, without asking."));
    QCommandLineOption saveOpt(QStringLiteral("save"), QStringLiteral("Save the project (over the --open file) once everything is loaded."));
    QCommandLineOption claudeOpt(QStringLiteral("claude-config"), QStringLiteral("Add chost-mcp to this Claude Desktop settings file and quit (tests)."), QStringLiteral("json"));
    QCommandLineOption playOpt(QStringLiteral("play"), QStringLiteral("Start playback at startup."));
    parser.addOptions({pluginOpt, fileOpt, sessionOpt, shotOpt, playOpt, separateOpt, transcribeOpt, selectOpt, applyOpt, instrumentsOpt, notesOpt, exportOpt, masterOpt, analyzeOpt, arrangeOpt, chordsOpt, tuneOpt, undoOpt, recoverOpt, saveOpt, claudeOpt, delayOpt});
    parser.process(app);

    if (parser.isSet(shotOpt))
        app.setProperty("chost-noninteractive", true);
    if (parser.isSet(claudeOpt))
    {
        QString error;
        const bool ok = claude::addToConfig(parser.value(claudeOpt), claude::mcpProgram(), error);
        std::printf("%s\n", ok ? "CLAUDE CONFIG WRITTEN" : qPrintable(error));
        return ok ? 0 : 1;
    }

    MainWindow window;
    window.recoverAtStart(parser.isSet(recoverOpt));
    if (parser.isSet(sessionOpt))
        window.openSession(parser.value(sessionOpt));
    for (const QString& file : parser.values(fileOpt))
        window.addAudioTrack(file);
    for (const QString& spec : parser.values(pluginOpt))
        window.addPluginSpec(spec);
    for (const QString& file : parser.positionalArguments())
        window.loadAudioFile(file); // "open with" / a file dropped on chost.exe
    if (parser.isSet(applyOpt))
        window.applyInstruments();
    if (parser.isSet(selectOpt))
        window.selectTrack(parser.value(selectOpt).toInt());
    window.show();
    if (parser.isSet(saveOpt))
        window.saveSessionNow();
    if (parser.isSet(notesOpt) && parser.isSet(selectOpt))
        window.editNotes(parser.value(selectOpt).toInt());
    if (parser.isSet(playOpt))
        window.play();
    if (parser.isSet(separateOpt))
        window.separateStems(true);
    if (parser.isSet(instrumentsOpt))
        QTimer::singleShot(500, &window, &MainWindow::instrumentSettings);
    if (parser.isSet(transcribeOpt))
        window.transcribe(true, true);
    if (parser.isSet(analyzeOpt))
        window.analyzeSong();
    if (parser.isSet(exportOpt))
        window.exportMix(true);
    if (parser.isSet(masterOpt))
        window.masterMix(true);
    if (parser.isSet(arrangeOpt) || parser.isSet(chordsOpt))
        window.arrange(true, parser.value(chordsOpt));
    if (parser.isSet(tuneOpt))
        window.tune(true);
    if (parser.isSet(undoOpt))
    {
        app.setProperty("chost-noninteractive", true);
        QTimer::singleShot(500, &window, [&window, spec = parser.value(undoOpt)] { QApplication::exit(window.testUndo(spec)); });
    }

    if (parser.isSet(shotOpt))
    {
        const QString path = parser.value(shotOpt);
        QTimer::singleShot(parser.value(delayOpt).toInt(), &window, [&window, path] {
            window.grab().save(path);
            // open dialogs too, next to the main picture: shot.png -> shot-1.png, ...
            int n = 0;
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w != &window && w->isVisible() && qobject_cast<QDialog*>(w))
                    w->grab().save(QString(path).replace(QStringLiteral(".png"), QStringLiteral("-%1.png").arg(++n)));
            QApplication::quit();
        });
    }

    return QApplication::exec();
}
