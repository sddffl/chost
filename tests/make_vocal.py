"""Synthesises a sung melody that is out of tune, for the pitch correction tests and screenshots.

    python tests/make_vocal.py <folder>      (needs numpy and scipy)

Writes vocal.wav (mono, 44.1 kHz): a voice singing an A minor line on the vowel "ah", each note a bit
sharp or flat, with a scoop into it, vibrato and an "s" at either end; and vocal_notes.txt, one line per
note: start, end (seconds), the note it should be (MIDI), how far off it was sung (cents).
"""
import os
import sys
import wave

import numpy as np
from scipy.signal import lfilter

sr = 44100
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rng = np.random.default_rng(7)

# (MIDI note, beats, cents off): A minor, sung at 100 BPM; the G# is the raised 7th over the dominant
melody = [(69, 1, 35), (72, 1, -30), (71, 1, 20), (69, 1, -40), (76, 2, 25), (74, 1, -35), (72, 1, 45),
          (71, 1, -25), (68, 1, 30), (69, 2, -20), (64, 1, 40), (65, 1, -30), (67, 2, 15), (69, 2, -45)]
beat = 0.6
gap = 0.08 # breath between notes
total = sum(b for _, b, _ in melody) * beat + 1.0
n = int(total * sr)
f0 = np.zeros(n)
amp = np.zeros(n)
lines = []
t = 0.5
for note, beats, cents in melody:
    length = beats * beat - gap
    i, j = int(t * sr), int((t + length) * sr)
    x = np.arange(j - i) / sr
    sung = note + cents / 100.0
    scoop = -0.8 * np.exp(-x / 0.04) # slides up into the note
    vibrato = 0.25 * np.sin(2 * np.pi * 5.5 * x) * np.clip((x - 0.25) / 0.2, 0, 1)
    f0[i:j] = 440.0 * 2 ** ((sung + scoop + vibrato - 69) / 12)
    amp[i:j] = np.minimum(1, x / 0.03) * np.minimum(1, (length - x) / 0.05)
    lines.append("%.3f\t%.3f\t%d\t%d" % (t, t + length, note, cents))
    t += beats * beat

# a glottal source: harmonics with a falling spectrum, phase from the integrated pitch
phase = 2 * np.pi * np.cumsum(f0) / sr
source = np.zeros(n)
for h in range(1, 40):
    audible = (f0 * h < sr / 2 - 1000).astype(float)
    source += audible * np.sin(h * phase) / h ** 1.2
source *= amp


def formant(x, freq, bw, gain):
    r = np.exp(-np.pi * bw / sr)
    a = [1, -2 * r * np.cos(2 * np.pi * freq / sr), r * r]
    return gain * lfilter([1 - r], a, x)


voice = formant(source, 800, 80, 1.0) + formant(source, 1150, 90, 0.6) + formant(source, 2900, 120, 0.25)
voice /= np.max(np.abs(voice)) / 0.5
# an "s" before the first note and after the last, where nothing is pitched
for s in (0.1, t + 0.3):
    i = int(s * sr)
    hiss = np.diff(rng.standard_normal(int(0.15 * sr)), prepend=0) * np.hanning(int(0.15 * sr)) * 0.05
    voice[i:i + len(hiss)] += hiss

with wave.open(os.path.join(out, "vocal.wav"), "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes((np.clip(voice, -1, 1) * 32767).astype("<i2").tobytes())
with open(os.path.join(out, "vocal_notes.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")
