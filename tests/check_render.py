"""chost-render checks (docs/AUDIT.md, Phase 0).

    python3 tests/check_render.py <chost-test-offline> <chost-render> <work dir>

1. Null test of the Scissors: A.chost and B.chost in the work dir (written by chost-test-engine: a project
   before and after a cut, with a stateful insert, a bypassed insert and a send) must render the same.
2. Settling: a 40-track project (every track its own tone, 2 inserts each, sends to a bus) must render with
   every track, the same every time, and right from the first sample (no fade-in, no missing start). The
   renderer must have seen carla apply the connections ("SETTLED <ms>", not "fixed").
3. The settle probe never reaches the mix: everything muted renders as digital silence.
Needs numpy and soundfile.
"""
import os
import subprocess
import sys

import numpy as np
import soundfile as sf

maker, render, work = sys.argv[1:4]
os.makedirs(work, exist_ok=True)
failures = 0
SR = 48000
TRACKS = 40


def check(ok, what):
    global failures
    print(("  PASS  " if ok else "  FAIL  ") + what, flush=True)
    failures += not ok


def run_render(session, out, *extra, seconds=3.0):
    p = subprocess.run([render, "--session", session, "--output", out, "--bits", "32", "--seconds", str(seconds), *extra],
                       capture_output=True, text=True)
    settled = [l for l in p.stdout.splitlines() if l.startswith("SETTLED")]
    done = [l for l in p.stdout.splitlines() if l.startswith("DONE")]
    return p.returncode, (settled[0] if settled else ""), (done[0] if done else ""), p.stdout + p.stderr


print("1. Scissors null test: the project before and after a cut renders the same")
a, b = os.path.join(work, "A.chost"), os.path.join(work, "B.chost")
if not (os.path.exists(a) and os.path.exists(b)):
    check(False, "A.chost and B.chost from chost-test-engine are missing")
else:
    for session, out in ((a, "A.wav"), (b, "B.wav")):
        code, _, done, log = run_render(session, os.path.join(work, out), seconds=4.5)
        check(code == 0 and done, f"{out} rendered" + ("" if code == 0 else f": {log[-300:]}"))
    if failures == 0:
        x, y = sf.read(os.path.join(work, "A.wav"))[0], sf.read(os.path.join(work, "B.wav"))[0]
        diff = float(np.max(np.abs(x - y)))
        check(x.shape == y.shape and diff < 1e-6, f"largest difference {diff:.1e} ({20 * np.log10(max(diff, 1e-12) / np.max(np.abs(x))):.0f} dB)")

print(f"2. Settling: a {TRACKS}-track project renders whole, the same every time, from its first sample")
t = np.arange(SR * 3) / SR
for k in range(TRACKS):
    path = os.path.join(work, f"t{k:02d}.wav")
    x = 0.02 * np.sin(2 * np.pi * (200 + 53 * k) * t)
    sf.write(path, np.stack([x, x], 1), SR, subtype="FLOAT")
heavy = os.path.join(work, "heavy.chost")
# The project is this test's setup, not what it tests. Building a big project quickly crashes carla 2.5's graph
# more often than not, in the live and the offline engine alike (docs/AUDIT.md, finding #20; loading a saved
# one, what is tested here, does not), so the setup is tried again until it is made. The renders never are.
for attempt in range(1, 16):
    for f in ("heavy.chost", "heavy.carxp"):
        if os.path.exists(os.path.join(work, f)):
            os.remove(os.path.join(work, f))
    p = subprocess.run([maker, "make-project", work, str(TRACKS)], capture_output=True, text=True)
    if p.returncode == 0 and os.path.exists(heavy):
        break
check(p.returncode == 0 and os.path.exists(heavy), f"{TRACKS}-track project made by chost-test-offline (attempt {attempt}; setup only, see finding #20)")
ideal = sum(sf.read(os.path.join(work, f"t{k:02d}.wav"))[0][:, 0] for k in range(TRACKS))
renders = []
for run in range(3):
    out = os.path.join(work, f"heavy{run}.wav")
    code, settled, done, log = run_render(heavy, out)
    check(code == 0 and settled and "fixed" not in settled, f"render {run + 1}: {settled or 'no SETTLED line'}" + ("" if code == 0 else f" (exit {code})"))
    if code == 0:
        renders.append(sf.read(out)[0][:, 0])
if renders:
    y = renders[0]
    spec = np.abs(np.fft.rfft(y[int(0.2 * SR):int(2.8 * SR)] * np.hanning(int(2.6 * SR))))
    hz = np.fft.rfftfreq(int(2.6 * SR), 1 / SR)
    heard = sum(spec[np.argmin(np.abs(hz - (200 + 53 * k)))] > spec.max() * 10 ** (-30 / 20) for k in range(TRACKS))
    check(heard == TRACKS, f"every track is in the mix ({heard}/{TRACKS})")
    check(all(np.array_equal(y, r) for r in renders[1:]), f"{len(renders)} renders are identical")
    n = min(len(y), len(ideal))
    mid = slice(SR, 2 * SR)
    gain = np.dot(y[mid], ideal[mid]) / np.dot(ideal[mid], ideal[mid])
    start = float(np.max(np.abs(y[:SR // 20] - gain * ideal[:SR // 20])))
    whole = float(np.max(np.abs(y[:n] - gain * ideal[:n])))
    check(start < 1e-3, f"right from the first sample: first 50 ms off the ideal mix by {start:.1e}")
    check(whole < 1e-3, f"whole render off the ideal mix by {whole:.1e}")

print("3. The settle probe never reaches the mix")
code, settled, done, log = run_render(heavy, os.path.join(work, "muted.wav"), *sum((["--mute", str(k)] for k in range(TRACKS + 1)), []), seconds=2)
check(code == 0 and done.endswith("\t-120.0"), f"everything muted: {done.split(chr(9))[-1] if done else 'no DONE line'} dB (digital silence is -120.0)")

print(f"{'ALL PASSED' if failures == 0 else 'FAILED'} ({failures} failure{'' if failures == 1 else 's'})")
sys.exit(1 if failures else 0)
