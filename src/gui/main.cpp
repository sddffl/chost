#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("chost"));
    QApplication::setOrganizationName(QStringLiteral("chost"));
    QApplication::setApplicationVersion(QStringLiteral(CHOST_VERSION));
    theme::apply(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("chost audio plugin host"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption pluginOpt(QStringLiteral("load"), QStringLiteral("Load a plugin at startup (lv2:URI, vst3:PATH, ...)."), QStringLiteral("spec"));
    QCommandLineOption fileOpt(QStringLiteral("file"), QStringLiteral("Load an audio file as a new track at startup (repeatable)."), QStringLiteral("path"));
    QCommandLineOption sessionOpt(QStringLiteral("session"), QStringLiteral("Open a .chost session at startup."), QStringLiteral("path"));
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot of the window after startup and quit."), QStringLiteral("png"));
    QCommandLineOption separateOpt(QStringLiteral("separate"), QStringLiteral("Open the AI stem separation for the selected track and start it."));
    QCommandLineOption delayOpt(QStringLiteral("screenshot-delay"), QStringLiteral("Milliseconds before the screenshot (default 1500)."), QStringLiteral("ms"), QStringLiteral("1500"));
    QCommandLineOption transcribeOpt(QStringLiteral("transcribe"), QStringLiteral("Open AI audio to MIDI for every audio track and start it."));
    QCommandLineOption selectOpt(QStringLiteral("select"), QStringLiteral("Select a track at startup (0 is the first)."), QStringLiteral("n"));
    QCommandLineOption applyOpt(QStringLiteral("apply-instruments"), QStringLiteral("Put the instruments from the settings on every MIDI track."));
    QCommandLineOption instrumentsOpt(QStringLiteral("instrument-settings"), QStringLiteral("Open the instrument settings."));
    QCommandLineOption notesOpt(QStringLiteral("edit-notes"), QStringLiteral("Open the MIDI editor of the selected track."));
    QCommandLineOption playOpt(QStringLiteral("play"), QStringLiteral("Start playback at startup."));
    parser.addOptions({pluginOpt, fileOpt, sessionOpt, shotOpt, playOpt, separateOpt, transcribeOpt, selectOpt, applyOpt, instrumentsOpt, notesOpt, delayOpt});
    parser.process(app);

    if (parser.isSet(shotOpt))
        app.setProperty("chost-noninteractive", true);

    MainWindow window;
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
