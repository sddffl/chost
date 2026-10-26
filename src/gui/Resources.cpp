#include "Resources.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace resources {

static const char* const kPythonKey = "ai/python";

static QString firstExisting(const QStringList& candidates)
{
    for (const QString& path : candidates)
        if (QFileInfo::exists(path))
            return QDir::cleanPath(path);
    return {};
}

QString toolScript(const QString& name)
{
    QStringList candidates{QCoreApplication::applicationDirPath() + QStringLiteral("/tools/") + name};
#ifdef CHOST_SOURCE_DIR
    candidates << QStringLiteral(CHOST_SOURCE_DIR "/tools/") + name;
#endif
    return firstExisting(candidates);
}

QString renderTool()
{
#ifdef _WIN32
    return firstExisting({QCoreApplication::applicationDirPath() + QStringLiteral("/chost-render.exe")});
#else
    return firstExisting({QCoreApplication::applicationDirPath() + QStringLiteral("/chost-render")});
#endif
}

QString soundFile(const QString& name)
{
    QStringList candidates{QCoreApplication::applicationDirPath() + QStringLiteral("/sounds/") + name};
#ifdef CHOST_SOURCE_DIR
    candidates << QStringLiteral(CHOST_SOURCE_DIR "/resources/sounds/") + name;
#endif
    return firstExisting(candidates);
}

QString aiPython()
{
    // tests point at a prepared environment without touching the settings
    const QString forced = qEnvironmentVariable("CHOST_AI_PYTHON");
    if (!forced.isEmpty())
        return forced;
    return QSettings().value(kPythonKey).toString();
}

void setAiPython(const QString& python)
{
    QSettings().setValue(kPythonKey, python);
}

QString defaultInstrumentSpec()
{
    const QString font = soundFile(QStringLiteral("TimGM6mb.sf2"));
    return font.isEmpty() ? QString() : QStringLiteral("sf2:") + QDir::toNativeSeparators(font);
}

} // namespace resources
