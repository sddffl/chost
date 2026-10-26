"""Writes MIDI melodies for the sheet music checks, played a little unevenly the way a person plays:

    python make_rhythms.py triplets.mid   4/4 with beats of triplets among straight eighths and sixteenths
    python make_rhythms.py waltz.mid      3/4
    python make_rhythms.py jig.mid        6/8
    python make_rhythms.py drums.mid      a rock beat on channel 10: kick, snare, hi-hat, a crash and a tom fill
"""
import random
import struct
import sys

TPQ = 480
random.seed(7)

# (start, length) in quarter notes and a pitch, one bar after another
PATTERNS = {
    "triplets": [
        (0, 1, 60), (1, 1 / 3, 62), (4 / 3, 1 / 3, 64), (5 / 3, 1 / 3, 65), (2, 0.5, 67), (2.5, 0.5, 65), (3, 0.25, 64), (3.25, 0.25, 62),
        (3.5, 0.25, 60), (3.75, 0.25, 62),
        (4, 2 / 3, 64), (14 / 3, 2 / 3, 65), (16 / 3, 2 / 3, 67), (6, 1, 69), (7, 1 / 3, 67), (22 / 3, 1 / 3, 65), (23 / 3, 1 / 3, 64),
        (8, 4, 60),
    ],
    "waltz": [
        (0, 3, 64), (3, 1, 67), (4, 2, 65), (6, 1, 64), (7, 0.5, 62), (7.5, 0.5, 64), (8, 1, 65), (9, 1.5, 67), (10.5, 1.5, 64),
    ],
    "jig": [
        (0, 0.5, 67), (0.5, 0.5, 69), (1, 0.5, 71), (1.5, 1, 72), (2.5, 0.5, 71),
        (3, 1.5, 69), (4.5, 0.5, 67), (5, 0.5, 66), (5.5, 0.5, 64),
        (6, 3, 67),
    ],
}

# kick on 1 and 3, snare on 2 and 4, eighths on the hi-hat; then a fill on the toms and a crash
DRUMS = []
for bar in range(2):
    for eighth in range(8):
        DRUMS.append((bar * 4 + eighth / 2, 0.1, 42))
    for beat in (0, 2):
        DRUMS.append((bar * 4 + beat, 0.1, 36))
    for beat in (1, 3):
        DRUMS.append((bar * 4 + beat, 0.1, 38))
DRUMS += [(8 + i / 4, 0.1, (50, 50, 48, 48, 45, 45, 43, 43)[i]) for i in range(8)] + [(10, 0.1, 49), (10, 0.1, 36)]
PATTERNS["drums"] = DRUMS


def var(n):
    out = bytearray()
    out.append(n & 0x7F)
    n >>= 7
    while n:
        out.insert(0, 0x80 | (n & 0x7F))
        n >>= 7
    return bytes(out)


def write(path, notes, channel=0):
    events = []
    for start, length, pitch in notes:
        jitter = lambda: random.uniform(-0.03, 0.03)  # about 15 ms at 120 bpm
        on = max(0, round((start + jitter()) * TPQ))
        off = round((start + length * 0.92 + jitter()) * TPQ)
        events.append((on, 1, bytes([0x90 | channel, pitch, 90])))
        events.append((off, 0, bytes([0x80 | channel, pitch, 0])))
    events.sort()
    track = bytearray(b"\x00\xff\x51\x03\x07\xa1\x20")  # 120 bpm
    now = 0
    for time, _, data in events:
        track += var(time - now) + data
        now = time
    track += b"\x00\xff\x2f\x00"
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQ))
        f.write(b"MTrk" + struct.pack(">I", len(track)) + track)


path = sys.argv[1]
name = path.rsplit("/", 1)[-1].split(".")[0]
write(path, PATTERNS[name], 9 if name == "drums" else 0)
