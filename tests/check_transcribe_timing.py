"""Checks the timing and dynamics of tools/chost_transcribe.py's notes, without the model.

    python3 tests/check_transcribe_timing.py <tools dir>

Basic Pitch hears in 11.6 ms frames and its note starts are often a frame or two off; the notes it finds
are stood in for by the true notes with their starts moved 5-30 ms either way. Then:
  1. snapped onto the attacks in the audio, a piano part's notes (chords, overlapping notes) and a bass
     line start within a few ms of when they were played (a soft note struck while loud ones ring can stay
     further off), and no note ends up further off than the model had it
  2. a legato pitch change with no new attack keeps the start the model gave it
  3. velocities follow how loud each note was played (rank correlation), within 30-115: the loudest at
     115, one 27 dB softer near the bottom; of a chord's loud and soft note the soft one gets less
  4. drum hits use the same curve: the loudest 115, the quietest kept (12 %) still above 30
Needs numpy and scipy only.
"""
import sys

import numpy as np
from scipy.stats import spearmanr

sys.path.insert(0, sys.argv[1])
import chost_transcribe as ct

SR = ct.AUDIO_SAMPLE_RATE
rng = np.random.default_rng(5)
failures = 0


def check(ok, what):
    global failures
    failures += not ok
    print(("  PASS  " if ok else "  FAIL  ") + what, flush=True)


def pluck(f0, seconds, decay, harmonics=8):
    t = np.arange(int(seconds * SR)) / SR
    tone = sum(np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 6.28)) / h ** 1.3 for h in range(1, harmonics + 1) if f0 * h < SR / 2 - 500)
    return tone * np.exp(-t / decay) * np.minimum(1.0, t / 0.002)


def render(notes, length, decay):
    """notes: (start s, length s, MIDI pitch, level dB); returns the audio."""
    audio = np.zeros(int(length * SR))
    for start, dur, pitch, db in notes:
        x = pluck(440.0 * 2 ** ((pitch - 69) / 12), dur, decay) * 10 ** (db / 20)
        release = np.minimum(1.0, (len(x) - np.arange(len(x))) / (0.03 * SR))
        a = int(start * SR)
        audio[a:a + len(x)] += (x * release)[: len(audio) - a]
    return audio / np.max(np.abs(audio)) * 0.7


def model_like(true_notes):
    """The true notes as the model might report them: starts 5-30 ms off either way."""
    out = []
    for start, dur, pitch, _ in true_notes:
        off = rng.choice([-1, 1]) * rng.uniform(0.005, 0.030)
        out.append((max(0.0, start + off), start + dur, pitch, 0.5))
    return out


def timing(name, true_notes, audio, mono):
    given = model_like(true_notes)
    snapped = ct.snap_onsets(audio, SR, given, mono)
    by_pitch_time = lambda notes, s, p: min((abs(n[0] - s), n) for n in notes if n[2] == p)
    before = [abs(g[0] - t[0]) * 1000 for g, t in zip(given, true_notes)]
    after = [by_pitch_time(snapped, t[0], t[2])[0] * 1000 for t in true_notes]
    worse = sum(a > b + 2.0 for a, b in zip(after, before))
    check(len(snapped) == len(given) and np.median(after) < 2.0 and np.percentile(after, 90) < 8.0 and worse == 0,
          "1. %s: onset error median %.1f ms, 90 %% within %.1f ms, worst %.1f ms, none further off than the model had it"
          " (as the model gave them: %.1f, %.1f, %.1f)"
          % (name, np.median(after), np.percentile(after, 90), max(after), np.median(before), np.percentile(before, 90), max(before)))
    return snapped


# a piano part: single notes, chords, notes overlapping (the last one still ringing), levels 0 to -27 dB
piano = []
t = 0.3
for i in range(24):
    db = -27.0 * ((i * 7) % 10) / 9.0
    if i % 5 == 4: # a chord: three notes, the middle one softer
        for k, p in enumerate((60, 64, 67)):
            piano.append((t, 0.6, p + i % 3, db - (12.0 if k == 1 else 0.0)))
    else:
        piano.append((t, 0.7, 55 + (i * 5) % 24, db))
    t += rng.uniform(0.22, 0.45)
piano.sort()
piano_audio = render(piano, t + 1.0, 0.5)
snapped = timing("piano (chords, overlapping notes)", piano, piano_audio, False)

bass = []
t = 0.25
for i in range(20):
    bass.append((t, 0.28, 31 + (i * 3) % 14, -18.0 * (i % 4) / 3.0))
    t += 0.3
bass_audio = render(bass, t + 0.5, 0.35)
timing("bass line (28 to 90 Hz)", bass, bass_audio, True)

# 2. a legato change: the pitch moves, nothing is struck
tt = np.arange(int(1.6 * SR)) / SR
f = np.where(tt < 0.8, 220.0, 247.0)
legato = 0.4 * np.sin(2 * np.pi * np.cumsum(f) / SR) * np.minimum(1, tt / 0.05)
kept = ct.snap_onsets(legato, SR, [(0.0, 0.8, 57, 0.5), (0.815, 1.6, 59, 0.5)], True)
check(abs(kept[1][0] - 0.815) < 1e-9, "2. a legato change keeps its start (%.3f s)" % kept[1][0])

# 3. velocities
levels = ct.note_levels(piano_audio, SR, [(s, s + d, p, 0) for s, d, p, _ in piano])
vel = np.array(ct.velocity_curve(levels))
true_db = np.array([n[3] for n in piano])
rho = spearmanr(true_db, vel).correlation
check(rho > 0.95 and vel.min() >= 30 and vel.max() == 115,
      "3. velocities follow the playing: rank correlation %.3f, range %d-%d" % (rho, vel.min(), vel.max()))
loudest = vel[true_db == true_db.max()]
softest = vel[true_db == true_db.min()]
check(loudest.min() >= 105 and softest.max() <= 50, "3. the loudest notes at %d-%d, those 27 dB (and more) softer at %d-%d"
      % (loudest.min(), loudest.max(), softest.min(), softest.max()))
chord_ok = []
for i, (s, d, p, db) in enumerate(piano):
    for j, (s2, d2, p2, db2) in enumerate(piano):
        if s == s2 and db2 < db - 6:
            chord_ok.append(vel[j] < vel[i])
check(len(chord_ok) > 0 and all(chord_ok), "3. in each chord the softer note gets the lower velocity (%d pairs)" % len(chord_ok))

# 4. drums
hits = [ct.hit_velocity(v) for v in (1.0, 0.5, 0.25, 0.12)]
check(hits[0] == 115 and hits == sorted(hits, reverse=True) and hits[-1] > 30, "4. drum hits at 100/50/25/12 %%: %s" % hits)

print("%s (%d failure%s)" % ("ALL PASSED" if failures == 0 else "FAILED", failures, "" if failures == 1 else "s"))
sys.exit(1 if failures else 0)
