#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("chost"));
    QApplication::setApplicationVersion(QStringLiteral(CHOST_VERSION));
    theme::apply(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("chost audio plugin host"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption pluginOpt(QStringLiteral("load"), QStringLiteral("Load a plugin at startup (lv2:URI, vst3:PATH, ...)."), QStringLiteral("spec"));
    QCommandLineOption fileOpt(QStringLiteral("file"), QStringLiteral("Load an audio file at startup."), QStringLiteral("path"));
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot of the window after startup and quit."), QStringLiteral("png"));
    QCommandLineOption playOpt(QStringLiteral("play"), QStringLiteral("Start playback at startup."));
    parser.addOptions({pluginOpt, fileOpt, shotOpt, playOpt});
    parser.process(app);

    if (parser.isSet(shotOpt))
        app.setProperty("chost-noninteractive", true);

    MainWindow window;
    for (const QString& spec : parser.values(pluginOpt))
        window.addPluginSpec(spec);
    if (parser.isSet(fileOpt))
        window.loadAudioFile(parser.value(fileOpt));
    window.show();
    if (parser.isSet(playOpt))
        window.play();

    if (parser.isSet(shotOpt))
    {
        const QString path = parser.value(shotOpt);
        QTimer::singleShot(1500, &window, [&window, path] {
            window.grab().save(path);
            QApplication::quit();
        });
    }

    return QApplication::exec();
}
