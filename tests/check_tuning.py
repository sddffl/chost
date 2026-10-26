"""Checks a pitch correction of tests/make_vocal.py's singing.

    python check_tuning.py <folder> <tuned.wav> [--hard]

Every note must now be within 8 cents of the note it should be (it was sung 15-45 cents off), its vibrato
must still be there (or gone, with --hard), the song as long as before and the "s" sounds untouched.
"""
import os
import sys

import librosa
import numpy as np
import soundfile as sf

folder, tuned_path = sys.argv[1], sys.argv[2]
hard = "--hard" in sys.argv
original, sr = sf.read(os.path.join(folder, "vocal.wav"))
tuned, sr2 = sf.read(tuned_path)
assert sr == sr2, "sample rate changed: %d -> %d" % (sr, sr2)
assert len(tuned) == len(original), "length changed: %d -> %d" % (len(original), len(tuned))
if tuned.ndim > 1:
    tuned = tuned.mean(axis=1)


def pitch(y):
    f0, voiced, _ = librosa.pyin(y, fmin=100, fmax=1000, sr=sr, frame_length=2048, hop_length=256, resolution=0.02)
    return 69 + 12 * np.log2(np.where(voiced, f0, np.nan) / 440.0)


before, after = pitch(original), pitch(tuned)
hop = 256 / sr
failures = []
for line in open(os.path.join(folder, "vocal_notes.txt")):
    start, end, note, cents = line.split("\t")
    start, end, note = float(start), float(end), int(note)
    # the steady middle of the note, after the scoop
    a, b = int((start + 0.12) / hop), int((end - 0.08) / hop)
    centre_before, centre_after = np.nanmedian(before[a:b]), np.nanmedian(after[a:b])
    off = (centre_after - note) * 100
    wobble_before, wobble_after = np.nanstd(before[a:b]), np.nanstd(after[a:b])
    print("note %d at %.2fs: sung %+.0f cents, now %+.0f cents; vibrato %.2f -> %.2f semitones"
          % (note, start, (centre_before - note) * 100, off, wobble_before, wobble_after))
    if abs(off) > 8:
        failures.append("note %d at %.2fs is %+.0f cents off" % (note, start, off))
    if not hard and not 0.6 < wobble_after / wobble_before < 1.4:
        failures.append("note %d at %.2fs lost its expression" % (note, start))
    if hard and wobble_after > 0.5 * wobble_before:
        failures.append("note %d at %.2fs still wobbles" % (note, start))

# where nothing is pitched the samples are the input's (up to the 24-bit file and the peak guard)
last = float(open(os.path.join(folder, "vocal_notes.txt")).read().split()[-3])
for name, at in (("first", 0.1), ("last", last + 0.08 + 0.3)):
    i, j = int(at * sr), int((at + 0.15) * sr)
    gain = np.dot(tuned[i:j], original[i:j]) / max(np.dot(original[i:j], original[i:j]), 1e-12)
    error = np.max(np.abs(tuned[i:j] - gain * original[i:j]))
    print("the 's' by the %s note: largest difference %.5f" % (name, error))
    if error > 1e-3:
        failures.append("the 's' by the %s note changed" % name)

if failures:
    print("FAILED:\n  " + "\n  ".join(failures))
    sys.exit(1)
print("pitch correction ok")
