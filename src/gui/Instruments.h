// Which instrument plays which kind of part: the user's choice per role ("bass", "drums", ...), with the
// plugin's saved state so a sampler like Kontakt comes back with its library loaded.

#pragma once

#include "core/HostEngine.h"

#include <QString>

#include <map>
#include <vector>

namespace instruments {

struct Role {
    QString key;  // settings key: lead, vocals, backing, bass, guitar, piano, other, drums, default
    QString name; // shown to the user
};
const std::vector<Role>& roles();

// The settings key for a track role: every drum part shares "drums", a whole mix uses "default".
QString keyFor(const QString& role);
QString roleName(const QString& key);

struct Choice {
    QString spec;  // empty: the bundled General MIDI sounds
    QString name;  // the plugin's name
    QString state; // a .carxs file with the plugin's sound, may be empty
};

Choice stored(const QString& key);            // what the user set for this key, spec empty without one
Choice choiceFor(const QString& role);         // for a track: its key, then "default", then the GM sounds
void store(const QString& key, const Choice& choice);
void clear(const QString& key);
QString statePath(const QString& key);         // where "remember this sound" saves the plugin state
QString bundledSpec();                         // the General MIDI sound font

// Dedicated instruments found among the user's plugins (Superior Drummer, Trilian, Keyscape, ...),
// by name. Samplers that start empty (Kontakt) are left out: they need a sound loaded and remembered.
std::map<QString, chost::PluginEntry> autoMatch(const std::vector<chost::PluginEntry>& plugins);

// Plugins that load empty and only make sound once a library is chosen inside them.
bool isEmptySampler(const QString& name);

} // namespace instruments
