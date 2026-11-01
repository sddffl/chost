#!/usr/bin/env python3
"""Scores MIDI files written by tools/chost_transcribe.py against the notes tests/make_song.py played.

usage: check_transcription.py DIR [ROLE...]   reads DIR/<role>.mid, fails below the minimum F-score
"""
import struct
import sys

BEAT = 0.5  # make_song.py plays at 120 bpm
CHORDS = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]]
MINIMUM = 0.8


def truth(role):
    out = []
    if role == "lead":
        melody = [69, 71, 72, 76, 74, 72, 71, 69, 67, 69, 71, 72, 74, 72, 71, 67]
        out = [(i * BEAT, m) for i, m in enumerate(melody * 2) if i % 8 != 7]
    elif role == "bass":
        out = [(bar * 2 + q * BEAT / 2, CHORDS[bar % 4][0] - 24) for bar in range(8) for q in range(8)]
    elif role == "piano":
        out = [(bar * 2, m) for bar in range(8) for m in CHORDS[bar % 4]]
    elif role == "kick":
        out = [(b * BEAT, 36) for b in range(32) if b % 4 in (0, 2) or b % 8 == 7]
    elif role == "snare":
        out = [(b * BEAT, 38) for b in range(32) if b % 4 in (1, 3)]
    elif role == "hh":
        out = [(b * BEAT + e * BEAT / 2, 42) for b in range(32) for e in range(2)]
    return out


def read_notes(path):
    """Note onsets (seconds, pitch) of a format 0/1 file at one tempo, enough for chost's own files."""
    data = open(path, "rb").read()
    division = struct.unpack(">H", data[12:14])[0]
    tempo = 500000
    pos, events = 14, []
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        p, end, tick, running = pos + 8, pos + 8 + length, 0, 0
        while p < end:
            delta = 0
            while True:
                b = data[p]; p += 1
                delta = (delta << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            tick += delta
            status = data[p]
            if status & 0x80:
                p += 1
            else:
                status = running
            if status == 0xFF:
                kind, size = data[p], data[p + 1]
                if kind == 0x51:
                    tempo = int.from_bytes(data[p + 2:p + 5], "big")
                p += 2 + size
            else:
                running = status
                size = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
                if status & 0xF0 == 0x90 and data[p + 1] > 0:
                    events.append((tick, data[p]))
                p += size
        pos = end
    return [(tick * tempo / 1e6 / division, pitch) for tick, pitch in events]


def score(role, path):
    expected, found = truth(role), read_notes(path)
    used, hits = set(), 0
    for t, pitch in expected:
        for j, (s, q) in enumerate(found):
            same = q == pitch
            if j not in used and abs(s - t) < 0.06 and same:
                used.add(j)
                hits += 1
                break
    precision, recall = hits / max(1, len(found)), hits / max(1, len(expected))
    f = 2 * precision * recall / max(1e-9, precision + recall)
    print(f"{role:6} expected {len(expected):3}  found {len(found):3}  precision {precision:.2f}  recall {recall:.2f}  F {f:.2f}")
    return f


def main():
    folder, roles = sys.argv[1], sys.argv[2:] or ["lead", "bass", "piano", "kick", "snare", "hh"]
    worst = min(score(role, f"{folder}/{role}.mid") for role in roles)
    sys.exit(0 if worst >= MINIMUM else 1)


if __name__ == "__main__":
    main()
