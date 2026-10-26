"""Checks an arrangement written by tools/chost_arrange.py from typed chords: every part has notes,
the pitched parts play the chord of the moment (the Chinese style's added second aside) and stay in
their instruments' ranges.

    python3 tests/check_arrangement.py DIR "C G | Am F" TEMPO STYLE
"""
import glob
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import chost_arrange as arrange  # noqa: E402

RANGES = {"bass": (28, 60), "keys": (36, 96), "guitar": (40, 84), "strings": (36, 88)}


def main():
    folder, chords, tempo, style = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
    progression = arrange.parse_progression(chords)
    spb = 60.0 / tempo

    def chord_at(t):
        bar = int(t // (4 * spb))
        beat = (t - bar * 4 * spb) / spb
        bar_chords = progression[bar % len(progression)]
        return bar_chords[min(len(bar_chords) - 1, int(beat // (4.0 / len(bar_chords))))]

    ok = True
    files = sorted(glob.glob(os.path.join(folder, "*.mid")))
    if not files:
        print("no MIDI files in", folder)
        return 1
    for path in files:
        part = path.rsplit("_", 1)[-1][:-4]
        if part == "drums":
            size = os.path.getsize(path)
            print("%-8s %d bytes" % (part, size))
            ok = ok and size > 200
            continue
        notes = arrange.read_midi_notes(path)
        fits = 0
        for start, end, pitch, vel in notes:
            c = chord_at(start + 0.02)
            if pitch % 12 in c.pcs() or pitch % 12 == c.bass or (style == "chinese" and pitch % 12 == (c.root + 2) % 12):
                fits += 1
        share = fits / max(1, len(notes))
        low, high = min(n[2] for n in notes), max(n[2] for n in notes)
        lo, hi = RANGES[part]
        good = len(notes) > 0 and share >= 0.95 and lo <= low and high <= hi
        print("%-8s %4d notes, %3.0f%% in the chord, range %d-%d %s" % (part, len(notes), share * 100, low, high, "" if good else "FAIL"))
        ok = ok and good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
