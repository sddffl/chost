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

proc.stdin.close()
proc.wait(timeout=10)
print("MCP TEST PASSED")
