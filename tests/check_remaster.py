"""Checks of tools/chost_workflow_remaster.py (docs/AUDIT.md, Phase 0).

    python3 tests/check_remaster.py <tools dir> <work dir>

1. load_stereo: mono becomes stereo, extra channels go, another sample rate is resampled with its pitch kept.
2. mix_and_master with a mono 44.1 kHz vocal, a stereo 48 kHz drum stem and a 3-channel bass: it used to
   crash (IndexError); every stem must reach the master, run through the real chost_master.py.
3. The separation model folder: --model-dir, else AUDIO_SEPARATOR_MODEL_DIR, else the separator's default;
   no developer path is set when the script is imported.
4. What chost reads: a tool's own lines (chost_transcribe.py's "MIDI <file>\t<note count>", which chost
   took for a MIDI file named "1077") are passed on indented, its progress within the step's share.
5. Placement: kick, snare, bass and lead vocal in the centre, guitar and piano at L65 / R65, backing
   vocals at L80 / R80, each part as loud as it went in.
6. Mastering: the dynamic 250-400 Hz cut on a muddy mix (and not on a balanced one), the air shelf, the
   Mid/Side widening above 200 Hz, through chost_master.py itself.
Needs numpy, scipy and soundfile (the parts of the AI environment these steps use).
"""
import contextlib
import importlib.util
import io
import os
import sys

import numpy as np
import soundfile as sf

tools, work = sys.argv[1:3]
os.makedirs(work, exist_ok=True)
os.environ.pop("AUDIO_SEPARATOR_MODEL_DIR", None)
spec = importlib.util.spec_from_file_location("workflow", os.path.join(tools, "chost_workflow_remaster.py"))
wf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wf)
failures = 0


def check(ok, what):
    global failures
    print(("  PASS  " if ok else "  FAIL  ") + what, flush=True)
    failures += not ok


def tone(name, hz, sr, channels, seconds=3.0):
    t = np.arange(int(sr * seconds)) / sr
    x = 0.3 * np.sin(2 * np.pi * hz * t)
    path = os.path.join(work, name)
    sf.write(path, np.stack([x * (1 - 0.2 * c) for c in range(channels)], 1) if channels > 1 else x, sr, subtype="FLOAT")
    return path


def strongest(x, sr):
    s = np.abs(np.fft.rfft(x[:, 0] * np.hanning(len(x))))
    return np.argmax(s) * sr / len(x)


vocals = tone("song_vocals.wav", 440, 44100, 1)
drums = tone("song_drums.wav", 110, 48000, 2)
bass = tone("song_bass.wav", 55, 48000, 3)

print("1. load_stereo")
d, sr = wf.load_stereo(vocals)
check(d.shape == (132300, 2) and sr == 44100 and np.array_equal(d[:, 0], d[:, 1]), "mono becomes stereo, both sides the same")
d, sr = wf.load_stereo(bass)
check(d.shape[1] == 2 and sr == 48000, "3 channels keep the first two")
d, sr = wf.load_stereo(vocals, 48000)
check(sr == 48000 and abs(len(d) - 144000) <= 1 and d.dtype == np.float32, f"44.1 kHz resampled to 48 kHz ({len(d)} frames for 3 s)")
check(abs(strongest(d, 48000) - 440) < 2, f"pitch kept through resampling ({strongest(d, 48000):.1f} Hz)")

print("2. mix_and_master with mono, stereo and 3-channel stems at 44.1 and 48 kHz")
stems = {"vocals": vocals, "drums": drums, "bass": bass}
master = os.path.join(work, "master.wav")
wf.TOOLS_DIR = tools
try:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        wf.mix_and_master(stems, dict(stems), master)
    m, msr = sf.read(master, always_2d=True)
    check(m.shape[1] == 2 and abs(len(m) / msr - 3.0) < 0.01, f"a stereo master, {len(m) / msr:.2f} s at {msr} Hz")
    s = np.abs(np.fft.rfft(m[:, 0] * np.hanning(len(m))))
    hz = np.fft.rfftfreq(len(m), 1 / msr)
    level = lambda f: 20 * np.log10(s[np.argmin(np.abs(hz - f))] / s.max())
    check(all(level(f) > -20 for f in (55, 110, 440)), "every stem in the master (55 Hz %.1f, 110 Hz %.1f, 440 Hz %.1f dB)" % tuple(level(f) for f in (55, 110, 440)))
    check(level(400) < -40, f"nothing smeared between them (400 Hz at {level(400):.1f} dB)")
    check(sum(l.startswith("BALANCE") for l in out.getvalue().splitlines()) == 3, "a BALANCE line for each stem")
except Exception as e:
    check(False, f"crashed: {type(e).__name__}: {e}")

print("3. the separation model folder")
calls = []
real_run_command = wf.run_command
wf.run_command = lambda cmd, desc="", span=None: (calls.append((cmd, desc)), (0, []))[1]
flag = lambda cmd: cmd[cmd.index("--model-dir") + 1] if "--model-dir" in cmd else None
check("AUDIO_SEPARATOR_MODEL_DIR" not in os.environ, "importing the script sets no model folder")
wf.separate_audio("x.wav", os.path.join(work, "s1"), model_dir="/from/chost")
check(flag(calls[-1][0]) == "/from/chost", "--model-dir is passed on")
os.environ["AUDIO_SEPARATOR_MODEL_DIR"] = "/from/env"
wf.separate_audio("x.wav", os.path.join(work, "s2"))
check(flag(calls[-1][0]) == "/from/env", "else AUDIO_SEPARATOR_MODEL_DIR")
wf.separate_audio("x.wav", os.path.join(work, "s3"), model_dir="/from/chost")
check(flag(calls[-1][0]) == "/from/chost", "the argument wins over the environment")
del os.environ["AUDIO_SEPARATOR_MODEL_DIR"]
wf.separate_audio("x.wav", os.path.join(work, "s4"))
check(flag(calls[-1][0]) is None, "else no flag: the separator's own default")
source = open(os.path.join(tools, "chost_workflow_remaster.py"), encoding="utf-8").read()
check("AI_Models" not in source and "3090" not in source, "no developer path or GPU name in the script")

print("4. what chost reads from the workflow")
fake = os.path.join(work, "fake_tool.py")
with open(fake, "w", encoding="utf-8") as f:
    f.write("print('PROGRESS 50 listening')\nprint('MIDI /home/a b/My Song_remaster_work/bass.mid\\t1077')\n"
            "print('TUNED 0.0100\\t0,60.1')\nprint('STEM kick\\tkick\\t/a b/k.wav')\nprint('DONE')\n")
out = io.StringIO()
with contextlib.redirect_stdout(out):
    code, lines = real_run_command([sys.executable, fake], "正在转谱...", span=(70, 80))
shown = out.getvalue().splitlines()
check(code == 0 and "MIDI /home/a b/My Song_remaster_work/bass.mid\t1077" in lines, "the tool's lines come back to the workflow as they were")
check(not any(l.startswith(("MIDI", "TUNED", "STEM", "DONE")) for l in shown), "none of them reaches chost as an event of its own")
check("    MIDI /home/a b/My Song_remaster_work/bass.mid\t1077" in shown, "they are in the log, indented")
check(any(l.startswith("PROGRESS 75 ") for l in shown) and not any(l.startswith("PROGRESS 50") for l in shown),
      "the tool's 50 % is the step's 75 % (" + "; ".join(l for l in shown if l.startswith("PROGRESS")) + ")")

print("5. placement")
places = wf.places_for(["kick", "snare", "bass", "lead", "guitar", "piano", "backing"])
centre = lambda p: (p[0] + p[1]) / 2
check(all(abs(centre(places[n])) < 0.01 for n in ("kick", "snare", "bass", "lead")), "kick, snare, bass, lead vocal in the centre")
check(abs(centre(places["guitar"]) + 0.65) < 0.01 and abs(centre(places["piano"]) - 0.65) < 0.01, "guitar at L65, piano at R65")
check(places["backing"] == (-0.80, 0.80), "backing vocals spread to L80 / R80")
check(wf.places_for(["guitar"])["guitar"] == (-0.65, 0.65), "a guitar alone spreads over L65 / R65")
t = np.arange(48000) / 48000
same = np.stack([np.sin(2 * np.pi * 220 * t)] * 2, 1)
rms = lambda x: float(np.sqrt(np.mean(x ** 2)))
for name in ("kick", "guitar", "backing"):
    out = wf.place(same, *places[name])
    l, r = rms(out[:, 0]), rms(out[:, 1])
    check(abs(rms(out) - rms(same)) < 0.01 * rms(same), f"{name}: as loud as it went in ({20 * np.log10(rms(out) / rms(same)):+.2f} dB), L/R {20 * np.log10(max(l, 1e-9) / max(r, 1e-9)):+.1f} dB")
left_only = np.stack([np.sin(2 * np.pi * 220 * t), np.zeros_like(t)], 1)
out = wf.place(left_only, *places["backing"])
check(rms(out[:, 0]) > 5 * rms(out[:, 1]), "backing vocals: what was on the left stays on the left (L80)")
out = wf.place(left_only, *places["kick"])
check(abs(rms(out[:, 0]) - rms(out[:, 1])) < 1e-6, "kick: folded into the centre")

print("6. mastering: clarity and width")
rng = np.random.default_rng(3)
sr = 48000
n = sr * 8
def pink():
    s = np.fft.rfft(rng.standard_normal(n)); f = np.fft.rfftfreq(n, 1 / sr); s[1:] /= np.sqrt(f[1:]); s[0] = 0
    y = np.fft.irfft(s, n); return y / np.std(y) * 0.05
a, b = pink(), pink()
mix = np.stack([a + 0.4 * b, a - 0.4 * b], 1) # correlated, some width
spec = importlib.util.spec_from_file_location("chost_master", os.path.join(tools, "chost_master.py"))
cm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cm)
muddy = mix + (10 ** (6 / 20) - 1) * cm.zero_phase(mix, sr, "bandpass", (250, 400))
def band(x, lo, hi, ch=None):
    y = x.mean(axis=1) if ch is None else ch
    s = np.abs(np.fft.rfft(y)) ** 2; f = np.fft.rfftfreq(len(y), 1 / sr)
    return 10 * np.log10(s[(f >= lo) & (f < hi)].mean())
for name, x, want in (("muddy mix", muddy, (-2.6, -1.8)), ("balanced mix", mix, (-0.5, 0.01))):
    y, avg, most = cm.demud(x, sr, 2.5)
    d = band(y, 280, 360) - band(x, 280, 360)
    check(want[0] <= d <= want[1] and abs(band(y, 900, 1100) - band(x, 900, 1100)) < 0.05,
          f"{name}: 300 Hz {d:+.2f} dB (cut at most {most:.1f} dB), 1 kHz untouched")
y = cm.air(mix, sr, 2.0)
d12, d10, d3 = (band(y, f * 0.95, f * 1.05) - band(mix, f * 0.95, f * 1.05) for f in (12000, 10000, 3000))
check(1.9 <= d12 <= 2.05 and d10 >= 1.5 and abs(d3) < 0.05, f"air: {d10:+.2f} dB at 10 kHz, {d12:+.2f} at 12 kHz, {d3:+.2f} at 3 kHz")
y = cm.widen(mix, sr, 1.3)
side = lambda x: (x[:, 0] - x[:, 1]) / 2
mid = lambda x: (x[:, 0] + x[:, 1]) / 2
s2k = band(None, 1800, 2200, side(y)) - band(None, 1800, 2200, side(mix))
s60 = band(None, 50, 70, side(y)) - band(None, 50, 70, side(mix))
check(2.0 < s2k < 2.5 and abs(s60) < 0.1 and np.allclose(mid(y), mid(mix)), f"width: side {s2k:+.2f} dB at 2 kHz, {s60:+.2f} at 60 Hz, mid unchanged")
wide = np.stack([pink(), pink()], 1)
check(abs(cm.side_to_mid(cm.widen(wide, sr, 1.3)) - cm.side_to_mid(wide)) < 0.01, "an already wide mix is not widened further (mono stays safe)")
src = os.path.join(work, "muddy.wav")
sf.write(src, muddy.astype(np.float32), sr, subtype="FLOAT")
import subprocess
run = subprocess.run([sys.executable, os.path.join(tools, "chost_master.py"), "--input", src, "--output", os.path.join(work, "muddy_master.wav")],
                     capture_output=True, text=True)
said = dict(l.split(" ", 1) for l in run.stdout.splitlines() if " " in l)
check(run.returncode == 0 and "CLARITY" in said and "WIDTH" in said, "chost_master.py reports CLARITY " + said.get("CLARITY", "?").replace("\t", " / ") +
      " and WIDTH " + said.get("WIDTH", "?").replace("\t", " -> "))

print(f"{'ALL PASSED' if failures == 0 else 'FAILED'} ({failures} failure{'' if failures == 1 else 's'})")
sys.exit(1 if failures else 0)
