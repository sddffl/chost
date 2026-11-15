"""Checks that tools/chost_tune.py corrects smoothly ("一顿一顿" was the complaint: tuned singing stuttered).

    python3 tests/check_tune_smooth.py <tools dir> <work dir> [<another chost_tune.py to compare>]

A synthetic phrase, its true pitch known, sung off key: legato notes, a note pitch tracking loses for 30 ms,
an "s" between two notes that pitch tracking hears a (wrong) pitch in, a breath, and a long note with
vibrato and a slow drift. The tracker is replaced by the true pitch (with those faults put in), so this
needs no librosa and measures the correction itself:
  1. no steps: the shift changes by at most a few cents per millisecond anywhere in the singing, and from
     one note's correction to the next it takes about 35 ms (2 % to 98 %)
  2. the "s" and the breath come out exactly as they went in
  3. the vibrato stays whole, and most of the slow drift (the singer's expression) too
  4. every note still ends up in tune (its centre within 8 cents, measured on the output audio)
With a third argument the same numbers for that version of the script are printed next to them.
"""
import argparse
import importlib.util
import os
import sys

import numpy as np
import soundfile as sf
from scipy.signal import butter, lfilter, sosfiltfilt

parser = argparse.ArgumentParser()
parser.add_argument("tools")
parser.add_argument("work")
parser.add_argument("compare", nargs="?")
args = parser.parse_args()
os.makedirs(args.work, exist_ok=True)
sys.path.insert(0, args.tools)

SR = 44100
HOP_S = 0.01
rng = np.random.default_rng(11)

# (start s, length s, MIDI note it should be, cents off, vibrato semitones peak to peak, drift semitones over the note)
NOTES = [(0.30, 0.50, 69, 35, 0.0, 0.0),  # A4, sharp
         (0.80, 0.45, 72, -40, 0.0, 0.0), # C5, flat, legato from the A (no gap)
         (1.25, 0.50, 67, 30, 0.0, 0.0),  # G4, legato; pitch tracking loses it for 30 ms in the middle
         (1.90, 0.45, 69, -30, 0.0, 0.0), # A4 after an "s"
         (2.75, 1.30, 72, 25, 0.5, 0.3)]  # C5 after a breath: vibrato and a slow upward drift
S_AT = (1.75, 1.90)       # the "s"
BREATH_AT = (2.35, 2.75)  # the breath
DROPOUT = (1.47, 1.50)    # pitch tracking loses the B
LENGTH = 4.4


def sing():
    n = int(LENGTH * SR)
    t = np.arange(n) / SR
    pitch = np.full(n, np.nan)
    amp = np.zeros(n)
    for start, length, note, cents, vib, drift in NOTES:
        i, j = int(start * SR), int((start + length) * SR)
        x = t[i:j] - start
        p = note + cents / 100.0 + vib / 2.0 * np.sin(2 * np.pi * 5.5 * x) * np.clip((x - 0.15) / 0.15, 0, 1)
        p += drift * (x / length - 0.5)
        pitch[i:j] = p
        amp[i:j] = np.minimum(1, x / 0.02) * np.minimum(1, (length - x) / 0.03)
    # legato: the pitch glides 30 ms between joined notes, the voice doesn't stop
    for (s0, l0, *_), (s1, *_) in zip(NOTES[:-1], NOTES[1:]):
        if abs(s0 + l0 - s1) < 1e-6:
            i, j = int((s1 - 0.015) * SR), int((s1 + 0.015) * SR)
            pitch[i:j] = np.linspace(pitch[i - 1], pitch[j], j - i)
            amp[i:j] = 1.0
    f0 = 440.0 * 2 ** ((np.nan_to_num(pitch, nan=57.0) - 69) / 12)
    phase = 2 * np.pi * np.cumsum(f0) / SR
    source = sum(np.sin(h * phase) / h ** 1.2 * (f0 * h < SR / 2 - 1000) for h in range(1, 30)) * amp

    def formant(x, freq, bw, gain):
        r = np.exp(-np.pi * bw / SR)
        return gain * lfilter([1 - r], [1, -2 * r * np.cos(2 * np.pi * freq / SR), r * r], x)

    voice = formant(source, 800, 80, 1.0) + formant(source, 1150, 90, 0.6) + formant(source, 2900, 120, 0.25)
    voice /= np.max(np.abs(voice)) / 0.5
    # the "s": noise above 4 kHz; the breath: quiet broadband noise
    i, j = int(S_AT[0] * SR), int(S_AT[1] * SR)
    hiss = sosfiltfilt(butter(4, 4500, "highpass", fs=SR, output="sos"), rng.standard_normal(j - i))
    voice[i:j] += hiss / np.max(np.abs(hiss)) * 0.15 * np.hanning(j - i)
    i, j = int(BREATH_AT[0] * SR), int(BREATH_AT[1] * SR)
    breath = sosfiltfilt(butter(2, [300, 6000], "bandpass", fs=SR, output="sos"), rng.standard_normal(j - i))
    voice[i:j] += breath / np.max(np.abs(breath)) * 0.012 * np.hanning(j - i)
    return voice, pitch


def tracked(true_pitch, frames):
    """What a pitch tracker gives, in 10 ms frames: the true pitch, but lost for 30 ms in the B, and a
    (wrong) pitch heard in the "s" and the breath."""
    times = np.arange(frames) * HOP_S
    idx = np.minimum((times * SR).astype(int), len(true_pitch) - 1)
    p = true_pitch[idx].copy()
    p[(times >= DROPOUT[0]) & (times < DROPOUT[1])] = np.nan
    for a, b, wrong in ((S_AT[0] + 0.02, S_AT[1] - 0.02, 86.3), (BREATH_AT[0] + 0.05, BREATH_AT[1] - 0.05, 58.6)):
        sel = (times >= a) & (times < b)
        p[sel] = wrong + 0.3 * rng.standard_normal(sel.sum())
    return p


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(script, voice, true_pitch, out_path):
    """Runs the script's main() with the tracker replaced; returns what PSOLA was asked for."""
    tune = load(script, "tune_under_test_%d" % abs(hash(script)))
    seen = {}
    frames = 1 + int(len(voice) / SR / HOP_S)
    tune.track_pitch = lambda mono, sr: (tracked(true_pitch, frames), 0.9)
    real_psola = tune.psola

    def psola(audio, sr, marks, voiced_at, ratio_at):
        seen["voiced"], seen["ratio"] = voiced_at.copy(), ratio_at.copy()
        return real_psola(audio, sr, marks, voiced_at, ratio_at)

    tune.psola = psola
    inp = os.path.join(args.work, "phrase.wav")
    sf.write(inp, voice, SR, subtype="FLOAT")
    argv = sys.argv
    sys.argv = ["chost_tune.py", "--input", inp, "--output", out_path, "--key", "C major", "--strength", "1"]
    try:
        import contextlib
        import io

        with contextlib.redirect_stdout(io.StringIO()):
            code = tune.main()
    finally:
        sys.argv = argv
    out, _ = sf.read(out_path)
    # the shift PSOLA applies: none where it passes the input through
    shift = np.where(seen["voiced"], 12 * np.log2(seen["ratio"]), 0.0)
    return code, out, shift


def measure(voice, true_pitch, out, shift):
    m = {}
    sung = np.isfinite(true_pitch)
    ms = SR // 1000
    # 1. the largest change of the shift in any 1 ms of singing, in cents
    step = np.abs(shift[ms:] - shift[:-ms]) * 100
    m["jump"] = float(np.max(np.where(sung[ms:] & sung[:-ms], step, 0)))
    # the legato A -> C: from 2 % to 98 % of the way
    a, b = int(0.70 * SR), int(0.90 * SR)
    seg = shift[a:b]
    lo, hi = seg[0], seg[-1]
    frac = (seg - lo) / (hi - lo)
    m["transition"] = (np.argmax(frac >= 0.98) - np.argmax(frac >= 0.02)) / SR * 1000
    # 2. the "s" and the breath
    for name, (s, e) in (("s", S_AT), ("breath", BREATH_AT)):
        i, j = int(s * SR), int(e * SR)
        m[name] = float(np.max(np.abs(out[i:j] - voice[i:j])))
    # 3. the long note: how much of its vibrato and of its drift is left in the pitch after the shift
    start, length = NOTES[4][0], NOTES[4][1]
    i, j = int((start + 0.35) * SR), int((start + length - 0.1) * SR)
    after = true_pitch[i:j] + shift[i:j]
    def band(x, lo_hz, hi_hz):
        return sosfiltfilt(butter(2, [lo_hz, hi_hz], "bandpass", fs=SR, output="sos"), x - x.mean())
    vib_in, vib_out = band(true_pitch[i:j], 4, 8), band(after, 4, 8)
    m["vibrato"] = float(np.std(vib_out) / np.std(vib_in))
    slope = lambda y: np.polyfit(np.arange(len(y)) / SR, y, 1)[0]
    m["drift"] = float(slope(after) / slope(true_pitch[i:j]))
    # 4. note centres in the output audio, by autocorrelation on the clean synthetic voice
    offs = []
    for start, length, note, *_ in NOTES:
        i, j = int((start + 0.12) * SR), int((start + length - 0.08) * SR)
        offs.append(abs(centre(out[i:j]) - note) * 100)
    m["worst note"] = max(offs)
    return m


def centre(x):
    """The median pitch (MIDI) of a voiced stretch, by autocorrelation in 40 ms windows."""
    win = int(0.04 * SR)
    found = []
    for a in range(0, len(x) - win, win // 2):
        w = x[a:a + win] * np.hanning(win)
        ac = np.correlate(w, w, "full")[win - 1:]
        lo, hi = int(SR / 1000), int(SR / 150)
        k = lo + int(np.argmax(ac[lo:hi]))
        # parabolic refinement of the peak
        y0, y1, y2 = ac[k - 1], ac[k], ac[k + 1]
        k = k + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
        found.append(69 + 12 * np.log2(SR / k / 440.0))
    return float(np.median(found))


voice, true_pitch = sing()
code, out, shift = run(os.path.join(args.tools, "chost_tune.py"), voice, true_pitch, os.path.join(args.work, "tuned.wav"))
new = measure(voice, true_pitch, out, shift)
old = None
if args.compare:
    _, old_out, old_shift = run(args.compare, voice, true_pitch, os.path.join(args.work, "tuned_compare.wav"))
    old = measure(voice, true_pitch, old_out, old_shift)

failures = 0


def check(ok, what, key, unit):
    global failures
    failures += not ok
    line = "  %s  %s: %.3g %s" % ("PASS" if ok else "FAIL", what, new[key], unit)
    if old is not None:
        line += "   (compared: %.3g %s)" % (old[key], unit)
    print(line, flush=True)


print("chost_tune.py on a phrase sung off key (exit code %d)" % code)
check(code == 0 and new["jump"] < 6.0, "1. largest change of the correction in 1 ms of singing", "jump", "cents")
check(28.0 <= new["transition"] <= 42.0, "1. the glide from one note's correction to the next (2-98 %)", "transition", "ms")
check(new["s"] < 1e-4, "2. the \"s\" (pitch tracking heard a pitch in it): largest change", "s", "")
check(new["breath"] < 1e-4, "2. the breath: largest change", "breath", "")
check(0.95 <= new["vibrato"] <= 1.05, "3. vibrato left in the long note", "vibrato", "x")
check(new["drift"] >= 0.8, "3. slow drift (expression) left in the long note", "drift", "x")
check(new["worst note"] <= 8.0, "4. every note in tune: the worst note's centre", "worst note", "cents off")
print("%s (%d failure%s)" % ("ALL PASSED" if failures == 0 else "FAILED", failures, "" if failures == 1 else "s"))
sys.exit(1 if failures else 0)
