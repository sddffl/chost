#include "Instruments.h"

#include "Resources.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>

namespace instruments {

const std::vector<Role>& roles()
{
    static const std::vector<Role> all{
        {QStringLiteral("lead"), QStringLiteral("主唱旋律")},   {QStringLiteral("vocals"), QStringLiteral("人声旋律")},
        {QStringLiteral("backing"), QStringLiteral("和声")},     {QStringLiteral("bass"), QStringLiteral("贝斯")},
        {QStringLiteral("guitar"), QStringLiteral("吉他")},      {QStringLiteral("piano"), QStringLiteral("钢琴")},
        {QStringLiteral("other"), QStringLiteral("其他乐器")},   {QStringLiteral("drums"), QStringLiteral("鼓")},
        {QStringLiteral("default"), QStringLiteral("其余 MIDI 轨")},
    };
    return all;
}

QString keyFor(const QString& role)
{
    static const QStringList drumParts{QStringLiteral("drums"), QStringLiteral("kick"), QStringLiteral("snare"), QStringLiteral("toms"),
                                       QStringLiteral("hh"), QStringLiteral("ride"), QStringLiteral("crash")};
    if (drumParts.contains(role))
        return QStringLiteral("drums");
    for (const Role& r : roles())
        if (r.key == role)
            return role;
    return QStringLiteral("default");
}

QString roleName(const QString& key)
{
    for (const Role& r : roles())
        if (r.key == key)
            return r.name;
    return key;
}

static QString settingsKey(const QString& key, const char* field)
{
    return QStringLiteral("instruments/%1/%2").arg(key, QLatin1String(field));
}

Choice stored(const QString& key)
{
    const QSettings settings;
    Choice c;
    c.spec = settings.value(settingsKey(key, "spec")).toString();
    c.name = settings.value(settingsKey(key, "name")).toString();
    c.state = settings.value(settingsKey(key, "state")).toString();
    if (!c.state.isEmpty() && !QFile::exists(c.state))
        c.state.clear();
    return c;
}

Choice choiceFor(const QString& role)
{
    Choice c = stored(keyFor(role));
    if (c.spec.isEmpty())
        c = stored(QStringLiteral("default"));
    if (c.spec.isEmpty())
        c = Choice{bundledSpec(), QStringLiteral("TimGM6mb"), {}};
    return c;
}

void store(const QString& key, const Choice& choice)
{
    QSettings settings;
    settings.setValue(settingsKey(key, "spec"), choice.spec);
    settings.setValue(settingsKey(key, "name"), choice.name);
    settings.setValue(settingsKey(key, "state"), choice.state);
}

void clear(const QString& key)
{
    QSettings().remove(QStringLiteral("instruments/") + key);
}

QString statePath(const QString& key)
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/instruments");
    QDir().mkpath(folder);
    return QDir(folder).filePath(key + QStringLiteral(".carxs"));
}

QString bundledSpec()
{
    return resources::defaultInstrumentSpec();
}

bool isEmptySampler(const QString& name)
{
    static const QStringList samplers{QStringLiteral("kontakt"), QStringLiteral("halion"), QStringLiteral("uvi workstation"),
                                      QStringLiteral("falcon"), QStringLiteral("sine player"), QStringLiteral("decent sampler"),
                                      QStringLiteral("sforzando"), QStringLiteral("sitala"), QStringLiteral("komplete kontrol")};
    const QString n = name.toLower();
    for (const QString& s : samplers)
        if (n.contains(s))
            return true;
    return false;
}

std::map<QString, chost::PluginEntry> autoMatch(const std::vector<chost::PluginEntry>& plugins)
{
    // best first: a dedicated product beats a plugin that merely has the word in its name
    static const std::vector<std::pair<QString, QStringList>> wanted{
        {QStringLiteral("drums"), {QStringLiteral("superior drummer"), QStringLiteral("ezdrummer"), QStringLiteral("addictive drums"),
                                   QStringLiteral("steven slate drums"), QStringLiteral("ssd5"), QStringLiteral("bfd"), QStringLiteral("getgood"),
                                   QStringLiteral("ggd"), QStringLiteral("mt power"), QStringLiteral("studio drummer"), QStringLiteral("battery"),
                                   QStringLiteral("drum")}},
        {QStringLiteral("bass"), {QStringLiteral("trilian"), QStringLiteral("modo bass"), QStringLiteral("ample bass"), QStringLiteral("ezbass"),
                                  QStringLiteral("scarbee"), QStringLiteral("bass")}},
        {QStringLiteral("piano"), {QStringLiteral("keyscape"), QStringLiteral("pianoteq"), QStringLiteral("ivory"), QStringLiteral("noire"),
                                   QStringLiteral("grandeur"), QStringLiteral("addictive keys"), QStringLiteral("ezkeys"), QStringLiteral("piano")}},
        {QStringLiteral("guitar"), {QStringLiteral("ample guitar"), QStringLiteral("electri6ity"), QStringLiteral("shreddage"),
                                    QStringLiteral("strum"), QStringLiteral("guitar")}},
        {QStringLiteral("lead"), {QStringLiteral("omnisphere"), QStringLiteral("serum"), QStringLiteral("vital"), QStringLiteral("sylenth"),
                                  QStringLiteral("spire"), QStringLiteral("pigments"), QStringLiteral("massive"), QStringLiteral("diva"),
                                  QStringLiteral("nexus")}},
        {QStringLiteral("backing"), {QStringLiteral("omnisphere"), QStringLiteral("nexus"), QStringLiteral("pigments"), QStringLiteral("serum"),
                                     QStringLiteral("vital")}},
    };
    std::map<QString, chost::PluginEntry> out;
    for (const auto& [key, words] : wanted)
    {
        for (const QString& word : words)
        {
            auto it = std::find_if(plugins.begin(), plugins.end(), [&](const chost::PluginEntry& e) {
                const QString name = QString::fromStdString(e.name);
                return e.isSynth && !isEmptySampler(name) && name.toLower().contains(word);
            });
            if (it != plugins.end())
            {
                out[key] = *it;
                break;
            }
        }
    }
    // the melody of a sung part plays well on the lead sound too
    if (out.count(QStringLiteral("lead")) != 0)
        out[QStringLiteral("vocals")] = out[QStringLiteral("lead")];
    return out;
}

} // namespace instruments
