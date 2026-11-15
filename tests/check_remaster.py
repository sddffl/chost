"""Checks of tools/chost_workflow_remaster.py (docs/AUDIT.md, Phase 0).

    python3 tests/check_remaster.py <tools dir> <work dir>

1. load_stereo: mono becomes stereo, extra channels go, another sample rate is resampled with its pitch kept.
2. mix_and_master with a mono 44.1 kHz vocal, a stereo 48 kHz drum stem and a 3-channel bass: it used to
   crash (IndexError); every stem must reach the master, run through the real chost_master.py.
3. The separation model folder: --model-dir, else AUDIO_SEPARATOR_MODEL_DIR, else the separator's default;
   no developer path is set when the script is imported.
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
wf.run_command = lambda cmd, desc="": (calls.append((cmd, desc)), (0, []))[1]
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

print(f"{'ALL PASSED' if failures == 0 else 'FAILED'} ({failures} failure{'' if failures == 1 else 's'})")
sys.exit(1 if failures else 0)
