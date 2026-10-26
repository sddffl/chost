"""chost tempo and key detection.

chost runs this script with the Python of its AI environment and reads stdout line by line:

    PROGRESS <0-100> <message>
    TEMPO <bpm>\t<first beat, seconds>
    KEY <tonic> <major|minor>\t<confidence 0-1>
    ERROR <message>
    DONE

    python chost_analyze.py --input song.wav

The tempo comes from librosa's beat tracker, refined with a straight line through the beats it found and
folded into 70-180 BPM. The key correlates the song's average chroma (of its harmonic part, drums taken
out) with the Krumhansl-Kessler key profiles. Requires numpy, scipy and librosa.
"""

import argparse
import sys
import traceback

import numpy as np

SR = 22050
MAX_SECONDS = 240.0
NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]

# Krumhansl & Kessler (1982), probe tone ratings for C major and C minor
MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])


def progress(pct, message):
    print("PROGRESS", int(pct), message, flush=True)


def fold(bpm, low=70.0, high=180.0):
    while bpm < low:
        bpm *= 2.0
    while bpm >= high:
        bpm /= 2.0
    return bpm


def detect_tempo(y, sr):
    """Returns (bpm, first beat in seconds), bpm 0 when the audio has no beat."""
    import librosa

    hop = 512
    onset = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop, aggregate=np.median)
    if onset.max() <= 0:
        return 0.0, 0.0
    estimate = float(np.atleast_1d(librosa.feature.tempo(onset_envelope=onset, sr=sr, hop_length=hop))[0])
    _, beats = librosa.beat.beat_track(onset_envelope=onset, sr=sr, hop_length=hop, start_bpm=estimate, units="time")
    beats = np.asarray(beats, dtype=float)
    if len(beats) >= 8:
        # the beats lie on a line: its slope is the beat period, steadier than any one interval
        index = np.arange(len(beats))
        period, start = np.polyfit(index, beats, 1)
        residual = beats - (start + period * index)
        keep = np.abs(residual) < 0.25 * period # beats the tracker put off the grid don't count
        if keep.sum() >= 8:
            period, start = np.polyfit(index[keep], beats[keep], 1)
        bpm = 60.0 / period
    else:
        bpm, start = estimate, (beats[0] if len(beats) else 0.0)
    bpm = fold(bpm)
    # the tracker likes beats near 120 BPM: when it counts every other beat of a fast song, the moments
    # halfway between its beats hit about as hard as the beats themselves (off-beat hi-hats alone are far weaker)
    if bpm * 2.0 < 200.0 and len(beats) >= 8:
        times = librosa.times_like(onset, sr=sr, hop_length=hop)
        period = 60.0 / bpm
        grid = start % period + period * np.arange(int(times[-1] / period))
        on = np.interp(grid, times, onset).mean()
        off = np.interp(grid + period / 2.0, times, onset).mean()
        if off >= 0.5 * on:
            bpm *= 2.0
    if abs(bpm - round(bpm)) < 0.3: # nearly every song is in whole beats per minute
        bpm = float(round(bpm))
    period = 60.0 / bpm
    first = float(start % period) if np.isfinite(start) else 0.0
    return round(bpm, 1), first


def detect_key(y, sr):
    """Returns (tonic 0-11, "major" or "minor", confidence 0-1)."""
    import librosa

    harmonic = librosa.effects.harmonic(y, margin=2.0)
    # C2 to C7: below is rumble and kick drums, above are cymbals ringing like notes
    chroma = librosa.feature.chroma_cqt(y=harmonic, sr=sr, hop_length=2048, fmin=librosa.note_to_hz("C2"), n_octaves=5)
    energy = librosa.feature.rms(y=harmonic, frame_length=4096, hop_length=2048)[0][: chroma.shape[1]]
    # every sounding moment counts the same, so loud drum hits don't outweigh the chords
    profile = chroma[:, energy > 0.05 * energy.max()].sum(axis=1)
    if profile.sum() <= 0:
        raise RuntimeError("the audio has no pitched sound")
    scores = []
    for mode, template in (("major", MAJOR), ("minor", MINOR)):
        for tonic in range(12):
            scores.append((np.corrcoef(profile, np.roll(template, tonic))[0, 1], tonic, mode))
    scores.sort(reverse=True)
    best, tonic, mode = scores[0]
    # the relative key shares every note, it's the runner-up that tells how clear the answer is
    runner = next(s for s, t, m in scores[1:] if not (m != mode and t == (tonic + (3 if mode == "minor" else 9)) % 12))
    confidence = float(np.clip((best - runner) * 4.0 + 0.5 * max(best, 0.0), 0.0, 1.0))
    return tonic, mode, confidence


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True)
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    try:
        import librosa

        progress(5, "reading the audio")
        y, sr = librosa.load(args.input, sr=SR, mono=True, duration=MAX_SECONDS)
        if len(y) < sr * 2 or np.max(np.abs(y)) < 1e-4:
            raise RuntimeError("the audio is too short or silent")
        progress(20, "finding the beat")
        bpm, first = detect_tempo(y, sr)
        if bpm > 0:
            print("TEMPO %.1f\t%.3f" % (bpm, first), flush=True)
        progress(55, "finding the key")
        tonic, mode, confidence = detect_key(y, sr)
        print("KEY %s %s\t%.2f" % (NAMES[tonic], mode, confidence), flush=True)
        progress(100, "done")
        print("DONE", flush=True)
        return 0
    except Exception as e:
        traceback.print_exc()
        print("ERROR", str(e) or e.__class__.__name__, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
