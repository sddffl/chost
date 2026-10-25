#include "Tools.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace control {

// Tracks, inserts and parameters are numbered from 0 as get_project lists them. Volumes are in dB.
static const char* const kTools = R"json([
{
  "name": "get_project",
  "description": "The project open in chost: its file, whether it has unsaved changes, tempo, key, master volume, the transport (playing, position, length) and every track with its index, kind (audio or instrument), name, role, file, volume, mute, solo, instrument and insert effects. Call this first: everything else refers to tracks and inserts by these indices, which change when tracks are added or removed.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "new_project",
  "description": "Closes the project and starts an empty one. Refuses while there are unsaved changes, unless discard_unsaved is true (ask the user first).",
  "inputSchema": {"type": "object", "properties": {
    "discard_unsaved": {"type": "boolean", "description": "Throw away unsaved changes."}
  }}
},
{
  "name": "open_project",
  "description": "Opens a .chost project file. Refuses while there are unsaved changes, unless discard_unsaved is true (ask the user first).",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string", "description": "Full path of the .chost file."},
    "discard_unsaved": {"type": "boolean"}
  }, "required": ["path"]}
},
{
  "name": "save_project",
  "description": "Saves the project, to its own file or to path. The version it overwrites is kept in <project>_备份 next to it.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string", "description": "Full path of a .chost file to save to; needed when the project was never saved."}
  }}
},
{
  "name": "add_track",
  "description": "Adds a track. With a file: an audio file (wav, flac, mp3, ogg, ...) makes an audio track; a MIDI file (.mid) makes an instrument track with an instrument fitting its part. Without a file: an empty audio track, or an instrument track with the default instrument. Returns the new track's index.",
  "inputSchema": {"type": "object", "properties": {
    "file": {"type": "string", "description": "Full path of an audio or MIDI file."},
    "kind": {"type": "string", "enum": ["audio", "instrument"], "description": "Without a file: which kind of empty track."},
    "name": {"type": "string"}
  }}
},
{
  "name": "remove_track",
  "description": "Removes a track (undo brings it back with its plugins and settings).",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0}
  }, "required": ["track"]}
},
{
  "name": "set_track",
  "description": "Changes a track: name, fader volume, mute, solo, or the audio or MIDI file it plays. Only the given fields change.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "name": {"type": "string"},
    "volume_db": {"type": "number", "maximum": 2, "description": "Fader in dB: 0 is unity, at most +2, -60 or lower is silence."},
    "muted": {"type": "boolean"},
    "solo": {"type": "boolean"},
    "file": {"type": "string", "description": "Replace the track's audio file (audio tracks) or MIDI file (instrument tracks)."}
  }, "required": ["track"]}
},
{
  "name": "set_master",
  "description": "Sets the master (Stereo Out) fader.",
  "inputSchema": {"type": "object", "properties": {
    "volume_db": {"type": "number", "maximum": 2}
  }, "required": ["volume_db"]}
},
{
  "name": "set_song",
  "description": "Sets the song's tempo (the grid, bars and beats, the tempo of new MIDI parts) and key (used by pitch correction and arrangement).",
  "inputSchema": {"type": "object", "properties": {
    "tempo": {"type": "number", "minimum": 20, "maximum": 400, "description": "Beats per minute."},
    "key": {"type": "string", "description": "e.g. \"A minor\", \"F# major\"; empty for unknown."}
  }}
},
{
  "name": "find_plugins",
  "description": "Searches the plugins installed on this computer (VST3, VST2, LV2, ... as chost scanned them) by name, maker or category. Use the spec or the name it returns with add_insert or set_instrument.",
  "inputSchema": {"type": "object", "properties": {
    "query": {"type": "string", "description": "Words to find in name, maker, format or category, e.g. \"compressor\", \"FabFilter\", \"reverb\". Empty lists everything."},
    "type": {"type": "string", "enum": ["effect", "instrument", "any"], "description": "Default any."},
    "limit": {"type": "integer", "minimum": 1, "maximum": 500, "description": "Default 40."}
  }}
},
{
  "name": "add_insert",
  "description": "Adds an insert effect at the end of a track's chain. Returns its insert index and its parameters.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "plugin": {"type": "string", "description": "A spec from find_plugins (e.g. \"vst3:C:/Program Files/Common Files/VST3/Pro-Q 3.vst3\") or a plugin name."}
  }, "required": ["track", "plugin"]}
},
{
  "name": "remove_insert",
  "description": "Removes an insert effect from a track.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "insert": {"type": "integer", "minimum": 0}
  }, "required": ["track", "insert"]}
},
{
  "name": "set_insert",
  "description": "Bypasses or enables an insert effect, or moves it up or down the chain.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "insert": {"type": "integer", "minimum": 0},
    "active": {"type": "boolean", "description": "false bypasses it."},
    "move": {"type": "integer", "description": "-1 one place earlier in the chain, 1 one place later."}
  }, "required": ["track", "insert"]}
},
{
  "name": "set_instrument",
  "description": "Loads (or replaces) the instrument of an instrument track, e.g. Kontakt, a synth, or an SF2/SFZ sound bank.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "plugin": {"type": "string", "description": "A spec from find_plugins or an instrument's name."}
  }, "required": ["track", "plugin"]}
},
{
  "name": "apply_instruments",
  "description": "Gives every instrument (MIDI) track the instrument and sound the user picked for its kind of part in chost's instrument settings (their own Kontakt, a piano library, ...), falling back to chost's built-in General MIDI sounds.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "suggest_chords",
  "description": "Chord help for writing a song: the chords of the song's key (or the key given) with their roman numerals, and how well each would follow the given chord (score 0 to 1, best first). Without after: the chords songs usually start on.",
  "inputSchema": {"type": "object", "properties": {
    "key": {"type": "string", "description": "e.g. \"A minor\"; default the song's key."},
    "after": {"type": "string", "description": "The chord before, e.g. \"G7\"."},
    "sevenths": {"type": "boolean", "description": "Seventh chords instead of triads."}
  }}
},
{
  "name": "add_chord_track",
  "description": "Writes a chord progression as a MIDI part played on a piano (or the instrument the user set for piano parts) and adds it as an instrument track, from the start of the song or from a bar. Chord symbols as C, Am, F#m7, Bb, Gsus4, C/E; \"|\" separates bars, chords in one bar share it. To have drums, bass and more play the chords, use ai_arrange with chords instead.",
  "inputSchema": {"type": "object", "properties": {
    "chords": {"type": "string", "description": "e.g. \"Am F | C G\" or \"C G Am F\" (a bar each)."},
    "pattern": {"type": "string", "enum": ["block", "arpeggio", "rhythm"], "description": "Held chords, broken in eighths, or a pop piano rhythm. Default block."},
    "start_bar": {"type": "integer", "minimum": 1, "description": "The bar it starts in, default 1."},
    "name": {"type": "string", "description": "The track's name, default 和弦."}
  }, "required": ["chords"]}
},
{
  "name": "fit_notes_to_scale",
  "description": "Moves the notes of a MIDI track that are outside a scale to the nearest note of it, as the piano roll's 修正到调内 does: for wrong notes audio to MIDI left, or a part written in the wrong key. The track's file is kept; the result goes to a copy the track then plays. Undoable.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "scale": {"type": "string", "description": "e.g. \"A minor\", \"A harmonic minor\", \"D dorian\", \"C major pentatonic\", \"E blues\"; default the song's key (major or natural minor)."}
  }, "required": ["track"]}
},
{
  "name": "export_score",
  "description": "Writes MIDI tracks as sheet music: a PDF (A4, to print or send) or MusicXML (for MuseScore, Sibelius, Finale, Dorico). Notes are quantized to sixteenths (or eighths), 4/4, one voice per staff; wide parts like piano get a grand staff. Drum tracks are left out. The user can also do this in File > 出谱.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string", "description": "Absolute path ending in .pdf or .musicxml."},
    "tracks": {"type": "array", "items": {"type": "integer"}, "description": "MIDI tracks, top to bottom; default every MIDI track that isn't drums."},
    "title": {"type": "string", "description": "Default the project's name."},
    "key": {"type": "string", "description": "The key signature, e.g. \"A minor\"; default the song's key, else guessed from the notes."},
    "staff": {"type": "string", "enum": ["auto", "treble", "bass", "grand"], "description": "Default auto."},
    "shortest": {"type": "string", "enum": ["sixteenth", "eighth"], "description": "The grid notes snap to; eighth reads more easily for transcribed parts. Default sixteenth."}
  }, "required": ["path"]}
},
{
  "name": "edit_clip",
  "description": "Edits the clip of a track (audio or MIDI) over a range of the song, as the track menu's 片段 actions do: move it later or earlier, silence the range (MIDI: delete its notes), keep only the range, fade in or out over it, change its gain (MIDI: velocities), or normalize its peak. Leave start and end out for the whole clip. The original file is kept; the track plays an edited copy. Undoable.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "action": {"type": "string", "enum": ["move", "silence", "keep", "fade_in", "fade_out", "gain", "normalize"]},
    "start": {"type": "number", "minimum": 0, "description": "Seconds in the song; default 0."},
    "end": {"type": "number", "minimum": 0, "description": "Seconds in the song; default the clip's end."},
    "seconds": {"type": "number", "description": "move: how far, later (> 0) or earlier (< 0)."},
    "beats": {"type": "number", "description": "move: how far in beats at the song's tempo, instead of seconds."},
    "db": {"type": "number", "description": "gain: the change in dB; normalize: the peak to bring the range to (default -1)."}
  }, "required": ["track", "action"]}
},
{
  "name": "add_marker",
  "description": "Adds a marker on the timeline, e.g. where the chorus starts, so the song's parts can be found and jumped to (Alt+Left/Right). Give seconds, or bar (1 is the song's start) at the song's tempo in 4/4. Returns the markers.",
  "inputSchema": {"type": "object", "properties": {
    "seconds": {"type": "number", "minimum": 0},
    "bar": {"type": "number", "minimum": 1},
    "name": {"type": "string", "description": "e.g. 主歌, 副歌, 间奏; default 标记 N."}
  }}
},
{
  "name": "edit_marker",
  "description": "Renames, moves or removes a marker; index is from get_project's markers.",
  "inputSchema": {"type": "object", "properties": {
    "index": {"type": "integer", "minimum": 0},
    "name": {"type": "string"},
    "seconds": {"type": "number", "minimum": 0},
    "remove": {"type": "boolean"}
  }, "required": ["index"]}
},
{
  "name": "add_bus",
  "description": "Adds a group or an effect bus. A group (a folder in the arrangement) takes the given tracks' faders in, so they can be turned up and down, muted or processed together (drums, backing vocals). A reverb or delay bus gets the user's best reverb or delay plugin (fully wet) and a send from each given track, so several tracks share one room. Returns the new bus's index. Undoable.",
  "inputSchema": {"type": "object", "properties": {
    "kind": {"type": "string", "enum": ["group", "reverb", "delay"]},
    "tracks": {"type": "array", "items": {"type": "integer"}, "description": "Tracks to put in the group, or to send to the effect."},
    "name": {"type": "string", "description": "Default 编组, 混响 or 延迟."}
  }, "required": ["kind"]}
},
{
  "name": "set_output",
  "description": "Where a track's fader goes: a group (bus index from get_project) or, with bus -1, Stereo Out.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "bus": {"type": "integer", "minimum": -1}
  }, "required": ["track", "bus"]}
},
{
  "name": "set_send",
  "description": "Sends a copy of a track's sound, after its fader, to a bus (a reverb or delay bus), or changes how much; remove: true takes the send away. A track has at most 2 sends.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "bus": {"type": "integer", "minimum": 0},
    "level_db": {"type": "number", "maximum": 2, "description": "Default -10."},
    "remove": {"type": "boolean"}
  }, "required": ["track", "bus"]}
},
{
  "name": "get_parameters",
  "description": "The parameters of a plugin: an insert of a track, or its instrument. Each has its index, name, unit, range, default and current value; outputs (meters) are marked.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "insert": {"type": "integer", "minimum": 0, "description": "Which insert; leave out with instrument true."},
    "instrument": {"type": "boolean", "description": "The track's instrument instead of an insert."},
    "query": {"type": "string", "description": "Only parameters whose name contains this."}
  }, "required": ["track"]}
},
{
  "name": "set_parameters",
  "description": "Sets parameters of a plugin (an insert, or the instrument), in the plugin's own units and range as get_parameters reports them. Returns the values the plugin took.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "insert": {"type": "integer", "minimum": 0},
    "instrument": {"type": "boolean"},
    "values": {"type": "object", "description": "Parameter name (or index) to value, e.g. {\"Threshold\": -18, \"Ratio\": 4}.", "additionalProperties": {"type": "number"}}
  }, "required": ["track", "values"]}
},
{
  "name": "show_plugin",
  "description": "Opens (or closes) a plugin's own window on the user's screen, for them to look at or tweak.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0},
    "insert": {"type": "integer", "minimum": 0},
    "instrument": {"type": "boolean"},
    "show": {"type": "boolean", "description": "Default true."}
  }, "required": ["track"]}
},
{
  "name": "transport",
  "description": "Plays, pauses, stops (and goes back to the start) or moves the play position.",
  "inputSchema": {"type": "object", "properties": {
    "action": {"type": "string", "enum": ["play", "pause", "stop", "seek"]},
    "seconds": {"type": "number", "minimum": 0, "description": "For seek: where to."}
  }, "required": ["action"]}
},
{
  "name": "get_levels",
  "description": "The meters right now: peak level in dBFS of every track and of the master. Only moves while playing; take several readings for a picture of a passage.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "undo",
  "description": "Undoes the last change(s), as Edit > Undo does.",
  "inputSchema": {"type": "object", "properties": {
    "steps": {"type": "integer", "minimum": 1, "description": "Default 1."}
  }}
},
{
  "name": "redo",
  "description": "Redoes what was undone.",
  "inputSchema": {"type": "object", "properties": {
    "steps": {"type": "integer", "minimum": 1}
  }}
},
{
  "name": "get_history",
  "description": "The undo history: every step's name, and which one the project is at now.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "ai_analyze",
  "description": "AI: finds the tempo and key of an audio track and sets them for the song. Runs in the background: see get_jobs / wait_for_jobs.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0, "description": "Default: the first audio track."}
  }}
},
{
  "name": "ai_separate_stems",
  "description": "AI stem separation of an audio track (a song): vocals, drums, bass, guitar, piano and the rest, the drums split further into kick, snare, toms and cymbals, each on a track of its own. Takes minutes; runs in the background: see wait_for_jobs.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0}
  }, "required": ["track"]}
},
{
  "name": "ai_transcribe",
  "description": "AI audio to MIDI: turns audio tracks (best on separated stems) into instrument tracks with MIDI notes. Runs in the background: see wait_for_jobs.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0, "description": "One track; leave out for every audio track."}
  }}
},
{
  "name": "ai_tune",
  "description": "AI pitch correction of a vocal (or other solo) track, in the song's key, keeping vibrato and slides. The tuned take replaces the track's sound; the original stays and undo goes back to it. Runs in the background: see wait_for_jobs.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0}
  }, "required": ["track"]}
},
{
  "name": "ai_arrange",
  "description": "AI auto-arrangement: drums, bass, chords and more as new instrument tracks, following the chords heard in a track or the chords given. Runs in the background: see wait_for_jobs.",
  "inputSchema": {"type": "object", "properties": {
    "track": {"type": "integer", "minimum": 0, "description": "The track to follow (audio or MIDI)."},
    "chords": {"type": "string", "description": "Chords to play instead, one per bar, e.g. \"C G Am F\"."}
  }}
},
{
  "name": "ai_master",
  "description": "AI mastering of the whole mix: loudness, EQ balance and limiting. The master lands on a track of its own for an A/B comparison. Runs in the background: see wait_for_jobs.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "export_mix",
  "description": "Exports the mix as a WAV file, rendered offline (faster than real time), into the project's export folder. Runs in the background: see wait_for_jobs; its message says where the file is.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "get_jobs",
  "description": "The AI jobs and exports started in this chost: which are running, their progress and their latest message (the result or the error when done).",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "wait_for_jobs",
  "description": "Waits until no AI job or export is running any more (or the timeout passes) and returns get_jobs. Call again if one is still running.",
  "inputSchema": {"type": "object", "properties": {
    "timeout_seconds": {"type": "number", "minimum": 1, "maximum": 110, "description": "Default 50."}
  }}
}
])json";

QJsonArray tools()
{
    static const QJsonArray list = QJsonDocument::fromJson(kTools).array();
    return list;
}

QString serverName()
{
    // a name of the user's own: chost of another user on the same computer is not theirs to control
    QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));
    user.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    return QStringLiteral("chost-control-") + (user.isEmpty() ? QStringLiteral("user") : user);
}

QString instructions()
{
    return QStringLiteral(
        "chost is the user's music production app (a plugin host and mixer with AI tools) running on their computer. "
        "Start with get_project to see the tracks; tracks, inserts and parameters are numbered from 0. "
        "Every change shows up in the chost window at once and can be undone (undo, or Ctrl+Z in chost). "
        "AI jobs (ai_*) and export_mix run in the background: follow them with wait_for_jobs. "
        "Saving, opening another project and starting a new one only happen when asked to; never discard unsaved work "
        "without the user's go-ahead. Files are full paths on the user's computer.");
}

int protocolVersion()
{
    return 1;
}

} // namespace control
