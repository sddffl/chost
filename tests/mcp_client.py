"""Works chost the way an AI assistant does: through chost-mcp, over MCP on its stdin and stdout.

    python3 tests/mcp_client.py <chost-mcp> <a.wav> <b.wav> <insert spec>

chost-mcp starts chost when none runs (offscreen with QT_QPA_PLATFORM=offscreen). Prints every step and
ends with "MCP TEST PASSED", or stops at the first thing that is wrong.
"""
import json
import os
import subprocess
import sys
import time

mcp, wav_a, wav_b, insert_spec = sys.argv[1:5]
proc = subprocess.Popen([mcp], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
next_id = 0


def rpc(method, params=None):
    global next_id
    next_id += 1
    message = {"jsonrpc": "2.0", "id": next_id, "method": method}
    if params is not None:
        message["params"] = params
    proc.stdin.write(json.dumps(message) + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    if not line:
        sys.exit("FAIL chost-mcp ended")
    answer = json.loads(line)
    assert answer["id"] == next_id, answer
    return answer


def call(tool, expect_error=False, **arguments):
    answer = rpc("tools/call", {"name": tool, "arguments": arguments})
    result = answer["result"]
    text = result["content"][0]["text"]
    if result["isError"] != expect_error:
        sys.exit("FAIL %s %s: %s" % (tool, arguments, text))
    print("ok   %s %s" % (tool, json.dumps(arguments, ensure_ascii=False)))
    return text if expect_error else json.loads(text)


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        sys.exit(1)


init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})["result"]
check(init["protocolVersion"] == "2025-06-18" and init["serverInfo"]["name"] == "chost", "initialize")
proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
tools = rpc("tools/list")["result"]["tools"]
check(len(tools) >= 30 and all("inputSchema" in t and t["description"] for t in tools), "%d tools listed" % len(tools))
check("error" in rpc("no/such/method"), "unknown methods are refused")

# the first call starts chost
project = call("get_project")
if project["tracks"]:
    call("new_project", discard_unsaved=True)
a = call("add_track", file=wav_a)
b = call("add_track", file=wav_b, name="Tone B")
check(a["track"] == 0 and b["track"] == 1 and b["name"] == "Tone B", "two tracks added")
t = call("set_track", track=1, volume_db=-6, muted=True)
check(abs(t["volume_db"] + 6) < 0.15 and t["muted"], "fader and mute set")
call("set_track", track=1, muted=False)
call("set_track", track=9, expect_error=True)

ins = call("add_insert", track=0, plugin=insert_spec)
check(ins["insert"] == 0 and ins["parameters"], "insert added with its parameters")
param = next(p for p in ins["parameters"] if not p.get("output"))
target = param["min"] + (param["max"] - param["min"]) * 0.25
s = call("set_parameters", track=0, insert=0, values={param["name"]: target})
check(abs(s["set"][param["name"]] - target) < 1e-3 * max(1, param["max"] - param["min"]), "parameter %s set" % param["name"])
got = call("get_parameters", track=0, insert=0, query=param["name"])
check(abs(got["parameters"][0]["value"] - target) < 1e-3 * max(1, param["max"] - param["min"]), "parameter reads back")
call("set_insert", track=0, insert=0, active=False)
check(not call("get_project")["tracks"][0]["inserts"][0]["active"], "insert bypassed")
call("set_insert", track=0, insert=0, active=True)
call("get_parameters", track=0, insert=5, expect_error=True)

song = call("set_song", tempo=98, key="A minor")
check(song["tempo"] == 98 and song["key"] == "A minor", "tempo and key")

# a found plugin by its name, once chost has listed them
for _ in range(20):
    found = call("find_plugins", query="gain", type="effect")
    if found["plugins"]:
        break
    time.sleep(1)
check(found["found"] >= 1, "find_plugins finds %d" % found["found"])

project = call("get_project")
if project["audio_driver"] != "Dummy":
    call("transport", action="play")
    time.sleep(1.5)
    levels = call("get_levels")
    call("transport", action="stop")
    check(levels["master_peak_db"] > -20, "the mix plays: master peak %.1f dB" % levels["master_peak_db"])

history = call("get_history")
check(history["done"] >= 6, "%d steps in the history" % history["done"])
removed = call("remove_track", track=1)
check(removed["tracks"] == 1, "track removed")
undone = call("undo")
check(len(call("get_project")["tracks"]) == 2, "undo brings it back (%s)" % undone["undone"][0])
call("redo")
call("undo")

# (the first song gets its tempo and key found by itself when chost has its AI components)
waited = call("wait_for_jobs", timeout_seconds=60)
check(not any(job["running"] for job in waited["jobs"]), "no jobs left running (%d done)" % len(waited["jobs"]))
call("save_project", expect_error=True)  # never saved, no path
path = os.path.abspath("mcp_test.chost")
saved = call("save_project", path=path)
check(os.path.exists(path) and not call("get_project")["unsaved_changes"], "saved to %s" % saved["file"])
call("new_project", discard_unsaved=False)
check(call("get_project")["tracks"] == [], "a new project (nothing unsaved)")
opened = call("open_project", path=path)
check(opened["tracks"] == 2, "reopened with both tracks")

# chords: what follows G7 in C, and a progression written as a piano part
sugg = call("suggest_chords", key="C major", after="G7")
check(sugg["chords"][0]["chord"] == "C", "after G7 comes C (%s)" % ", ".join(c["chord"] for c in sugg["chords"][:3]))
part = call("add_chord_track", chords="Am F | C G", pattern="arpeggio")
check(part["notes"] >= 16 and len(call("get_project")["tracks"]) == 3, "a chord track with %d notes" % part["notes"])
call("add_chord_track", chords="Am H7", expect_error=True)

# notes pulled into a scale: the A minor chords into D major move, then nothing is left outside
fit = call("fit_notes_to_scale", track=part["track"], scale="D major")
check(fit["moved"] >= 1 and fit["file"].endswith("_edit.mid"), "%d notes moved into %s" % (fit["moved"], fit["scale"]))
check(call("fit_notes_to_scale", track=part["track"], scale="D major")["moved"] == 0, "none left outside")
call("undo")
call("fit_notes_to_scale", track=part["track"], scale="H major", expect_error=True)

# sheet music of the chord track, as PDF and MusicXML
pdf = os.path.abspath("mcp_score.pdf")
sheet = call("export_score", path=pdf, tracks=[part["track"]], title="MCP")
check(os.path.getsize(pdf) > 1000 and sheet["pages"] == 1 and sheet["bars"] >= 2, "score PDF: %d bars, %s" % (sheet["bars"], sheet["parts"][0]["staff"]))
xml = os.path.abspath("mcp_score.musicxml")
call("export_score", path=xml, shortest="eighth")
check("<score-partwise" in open(xml, encoding="utf-8").read(), "score MusicXML")
call("export_score", path=pdf, tracks=[0], expect_error=True)  # an audio track

# clips: a fade on the audio track and a move of the chord part, each into a new file, undoable
before = call("get_project")["tracks"][0]["file"]
faded = call("edit_clip", track=0, action="fade_in", end=0.5)
check("_编辑" in faded["file"] and faded["file"].endswith(".wav") and os.path.exists(faded["file"]), "fade into %s" % os.path.basename(faded["file"]))
call("edit_clip", track=0, action="gain", db=-6, start=0.2, end=0.8)
call("undo")
call("undo")
check(call("get_project")["tracks"][0]["file"] == before, "undo goes back to the original file")
moved = call("edit_clip", track=part["track"], action="move", beats=4)
check(moved["file"].endswith(".mid"), "chord part moved a bar later")
call("edit_clip", track=part["track"], action="fade_in", expect_error=True)  # MIDI has no fades
call("edit_clip", track=0, action="stretch", expect_error=True)
call("edit_clip", track=0, action="keep", start=1.0, end=0.5, expect_error=True)

# markers: added by bar, renamed, moved, removed
added = call("add_marker", bar=2, name="副歌")
check(added["markers"][added["index"]]["name"] == "副歌" and abs(added["markers"][added["index"]]["bar"] - 2) < 0.01, "a marker at bar 2")
call("add_marker", seconds=0)
markers = call("get_project")["markers"]
check(len(markers) == 2 and markers[0]["seconds"] == 0 and markers[1]["name"] == "副歌", "markers in order: %s" % ", ".join(m["name"] for m in markers))
call("edit_marker", index=0, name="前奏")
check(call("edit_marker", index=1, seconds=10)["markers"][1]["seconds"] == 10, "marker moved")
check(len(call("edit_marker", index=0, remove=True)["markers"]) == 1, "marker removed")
call("undo")
check(call("get_project")["markers"][0]["name"] == "前奏", "undo brings the marker back")
call("edit_marker", index=5, remove=True, expect_error=True)
path2 = os.path.abspath("mcp_markers.chost")
call("save_project", path=path2)
call("new_project")
call("open_project", path=path2)
reopened = call("get_project")
check([m["name"] for m in reopened["markers"]] == ["前奏", "副歌"] and "_编辑" in reopened["tracks"][2]["file"],
      "markers and the edited clip are saved with the project")

# buses: an effect bus the audio track sends to, then a group holding both audio tracks
reverb = call("add_bus", kind="reverb", tracks=[0], name="Room")
project = call("get_project")
room = reverb["track"]
check(project["tracks"][room]["kind"] == "bus" and project["tracks"][0]["sends"][0]["bus"] == room
      and abs(project["tracks"][0]["sends"][0]["level_db"] + 10) < 0.1, "track 0 sends to the Room bus at -10 dB")
call("set_send", track=0, bus=room, level_db=-6)
check(abs(call("get_project")["tracks"][0]["sends"][0]["level_db"] + 6) < 0.1, "send level changed")
call("set_send", track=0, bus=0, expect_error=True)  # not a bus
group = call("add_bus", kind="group", tracks=[0, 1])
project = call("get_project")
g = group["track"]
members = [i for i, t in enumerate(project["tracks"]) if t.get("output") == g]
check(len(members) == 2 and members == [g + 1, g + 2], "both audio tracks in the group, right under it")
call("set_output", track=g, bus=members[0], expect_error=True)  # an audio track is no bus
call("set_output", track=members[0], bus=-1)
check("output" not in call("get_project")["tracks"][members[0]], "back to Stereo Out")
call("undo")
room = [i for i, t in enumerate(call("get_project")["tracks"]) if t["name"] == "Room"][0]
check(room == 4, "the Room bus moved down one, below the group")
call("set_send", track=room, bus=room, expect_error=True)  # a bus can't feed itself
call("set_send", track=g, bus=room, remove=True, expect_error=True)  # the group sends nothing
call("set_output", track=room, bus=g)
call("set_send", track=g, bus=room, expect_error=True)  # Room goes into the group: a loop

# the metronome: on, not part of the project's tracks or its export, off again
check(call("set_metronome", on=True, volume_db=-6)["metronome"], "metronome on")
check(call("get_project")["metronome"], "get_project says so")
call("set_metronome", on=False)
check(not call("get_project")["metronome"], "metronome off")

proc.stdin.close()
proc.wait(timeout=10)
print("MCP TEST PASSED")
