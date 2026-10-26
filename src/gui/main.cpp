#include "BalanceDialog.h"
#include "ChordPad.h"
#include "ClaudeConnect.h"
#include "MainWindow.h"
#include "PianoRoll.h"
#include "ScoreDialog.h"
#include "Theme.h"

#include <QApplication>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDialog>
#include <QIcon>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>

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
    QCommandLineOption balanceOpt(QStringLiteral("mix-balance"), QStringLiteral("Run AI auto mix balance; vocals forward, normal or blend."), QStringLiteral("vocals"));
    QCommandLineOption masterOpt(QStringLiteral("master"), QStringLiteral("Open AI mastering and start it."));
    QCommandLineOption analyzeOpt(QStringLiteral("analyze"), QStringLiteral("Find the tempo and key of the selected (or first) audio track."));
    QCommandLineOption undoOpt(QStringLiteral("test-undo"), QStringLiteral("Self test of undo and redo on the loaded tracks, with this insert plugin."), QStringLiteral("spec"));
    QCommandLineOption tuneOpt(QStringLiteral("tune"), QStringLiteral("Open AI pitch correction for the selected track and start it."));
    QCommandLineOption arrangeOpt(QStringLiteral("arrange"), QStringLiteral("Open AI auto-arrangement for the selected track and start it."));
    QCommandLineOption chordsOpt(QStringLiteral("arrange-chords"), QStringLiteral("Open AI auto-arrangement with these chords and start it."), QStringLiteral("chords"));
    QCommandLineOption recoverOpt(QStringLiteral("recover"), QStringLiteral("Bring back a project a crashed chost left in the autosave, without asking."));
    QCommandLineOption saveOpt(QStringLiteral("save"), QStringLiteral("Save the project (over the --open file) once everything is loaded."));
    QCommandLineOption claudeOpt(QStringLiteral("claude-config"), QStringLiteral("Add chost-mcp to this Claude Desktop settings file and quit (tests)."), QStringLiteral("json"));
    QCommandLineOption chordPadOpt(QStringLiteral("chord-pad"), QStringLiteral("Open the chord pad."));
    QCommandLineOption progressionOpt(QStringLiteral("chord-progression"), QStringLiteral("Open the chord pad with these chords and write them as a MIDI track (tests)."), QStringLiteral("chords"));
    QCommandLineOption recogniseOpt(QStringLiteral("recognise-chords"), QStringLiteral("Open the chord pad and recognise the chords of the first audio track (tests)."));
    QCommandLineOption noteScaleOpt(QStringLiteral("note-scale"), QStringLiteral("With --edit-notes: show this scale (\"A minor\", \"D dorian\", \"off\")."), QStringLiteral("scale"));
    QCommandLineOption fitScaleOpt(QStringLiteral("fit-scale"), QStringLiteral("With --edit-notes: move the notes outside the scale into it (tests)."));
    QCommandLineOption scoreTimeOpt(QStringLiteral("score-time"), QStringLiteral("The time signature for --score, e.g. 6/8."), QStringLiteral("meter"));
    QCommandLineOption scoreOpt(QStringLiteral("score"), QStringLiteral("Open File > Sheet music (the selected track, else every MIDI track) and export it: .pdf or .musicxml (tests)."), QStringLiteral("path"));
    QCommandLineOption markerOpt(QStringLiteral("marker"), QStringLiteral("Add a marker on the timeline (repeatable)."), QStringLiteral("seconds:name"));
    QCommandLineOption rangeOpt(QStringLiteral("range"), QStringLiteral("Select a range of the timeline."), QStringLiteral("start:end"));
    QCommandLineOption groupOpt(QStringLiteral("group"), QStringLiteral("Put these tracks into a new group (e.g. 0,1)."), QStringLiteral("tracks"));
    QCommandLineOption reverbOpt(QStringLiteral("reverb"), QStringLiteral("Send these tracks to a new reverb bus (e.g. 0,1)."), QStringLiteral("tracks"));
    QCommandLineOption foldOpt(QStringLiteral("fold"), QStringLiteral("Fold the groups."));
    QCommandLineOption playOpt(QStringLiteral("play"), QStringLiteral("Start playback at startup."));
    parser.addOptions({pluginOpt, fileOpt, sessionOpt, shotOpt, playOpt, separateOpt, transcribeOpt, selectOpt, applyOpt, instrumentsOpt, notesOpt, exportOpt, balanceOpt, masterOpt, analyzeOpt, arrangeOpt, chordsOpt, tuneOpt, undoOpt, recoverOpt, saveOpt, claudeOpt, chordPadOpt, progressionOpt, recogniseOpt, noteScaleOpt, fitScaleOpt, scoreOpt, scoreTimeOpt, markerOpt, rangeOpt, groupOpt, reverbOpt, foldOpt, delayOpt});
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
    auto trackList = [](const QString& text) {
        std::vector<int> list;
        for (const QString& t : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
            list.push_back(t.toInt());
        return list;
    };
    if (parser.isSet(groupOpt))
        window.addBus(MainWindow::BusKind::Group, trackList(parser.value(groupOpt)));
    if (parser.isSet(reverbOpt))
        window.addBus(MainWindow::BusKind::Reverb, trackList(parser.value(reverbOpt)));
    if (parser.isSet(foldOpt))
        window.foldGroups();
    for (const QString& marker : parser.values(markerOpt))
        window.addMarker(marker.section(QLatin1Char(':'), 0, 0).toDouble(), marker.section(QLatin1Char(':'), 1));
    if (parser.isSet(rangeOpt))
        window.setRange(parser.value(rangeOpt).section(QLatin1Char(':'), 0, 0).toDouble(), parser.value(rangeOpt).section(QLatin1Char(':'), 1).toDouble());
    window.show();
    if (parser.isSet(saveOpt))
        window.saveSessionNow();
    if (parser.isSet(notesOpt) && parser.isSet(selectOpt))
    {
        window.editNotes(parser.value(selectOpt).toInt());
        if (PianoRollDialog* roll = window.pianoRoll())
        {
            if (parser.isSet(noteScaleOpt))
                roll->setScale(parser.value(noteScaleOpt));
            if (parser.isSet(fitScaleOpt))
                std::printf("FITTED %d notes into the scale\n", roll->fitToScale());
            std::fflush(stdout);
        }
    }
    if (parser.isSet(scoreOpt))
    {
        ScoreDialog* dialog = window.scoreDialog();
        if (!parser.isSet(selectOpt))
            dialog->setChosen(window.scoreTracks());
        if (parser.isSet(scoreTimeOpt) && !dialog->setMeter(parser.value(scoreTimeOpt)))
            std::printf("SCORE TIME %s is not one of 4/4 3/4 2/4 6/8 12/8 9/8 3/8\n", qPrintable(parser.value(scoreTimeOpt)));
        QString error;
        const bool ok = dialog->exportTo(parser.value(scoreOpt), error);
        std::printf("%s\n", ok ? qPrintable(QStringLiteral("SCORE WRITTEN %1 (%2 pages)").arg(parser.value(scoreOpt)).arg(dialog->pages()))
                              : qPrintable(QStringLiteral("SCORE FAILED %1").arg(error)));
        std::fflush(stdout);
    }
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
    if (parser.isSet(balanceOpt))
    {
        BalanceDialog* dialog = window.mixBalance(false);
        const QString vocals = parser.value(balanceOpt);
        dialog->setVocalStyle(vocals == QLatin1String("forward") ? chost::VocalStyle::Forward
                              : vocals == QLatin1String("blend") ? chost::VocalStyle::Blend
                                                                 : chost::VocalStyle::Normal);
        QObject::connect(dialog, &BalanceDialog::balanced, dialog, [dialog](bool ok) {
            const auto* results = dialog->findChild<QTreeWidget*>(QStringLiteral("results"));
            for (int i = 0; results && i < results->topLevelItemCount(); ++i)
            {
                const QTreeWidgetItem* item = results->topLevelItem(i);
                const auto* role = qobject_cast<QComboBox*>(results->itemWidget(const_cast<QTreeWidgetItem*>(item), 1));
                std::printf("BALANCE %s\t%s\t%s\t%s\t%s\n", qPrintable(item->text(0)), role ? qPrintable(role->currentText()) : "", qPrintable(item->text(2)),
                            qPrintable(item->text(3)), qPrintable(item->text(4)));
            }
            const auto* status = dialog->findChild<QLabel*>(QStringLiteral("status"));
            std::printf("%s %s\n", ok ? "BALANCED" : "BALANCE FAILED", status ? qPrintable(status->text()) : "");
            std::fflush(stdout);
        });
        dialog->setAutoStart(true);
    }
    if (parser.isSet(masterOpt))
        window.masterMix(true);
    if (parser.isSet(arrangeOpt) || parser.isSet(chordsOpt))
        window.arrange(true, parser.value(chordsOpt));
    if (parser.isSet(tuneOpt))
        window.tune(true);
    if (parser.isSet(chordPadOpt) || parser.isSet(progressionOpt) || parser.isSet(recogniseOpt))
    {
        ChordPadDialog* pad = window.chordPad();
        if (parser.isSet(recogniseOpt))
            pad->recognise(0);
        if (parser.isSet(progressionOpt))
        {
            pad->setProgression(parser.value(progressionOpt));
            pad->write();
        }
    }
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
