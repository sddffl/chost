"""chost auto-arrangement.

chost runs this script with the Python of its AI environment and reads stdout line by line:

    PROGRESS <0-100> <message>
    CHORDS <bar 1> | <bar 2> | ...        the chords it played, a bar each ("Am F" shares a bar)
    MIDI <path>\t<role>\t<part name>\t<note count>
    ERROR <message>
    DONE

    python chost_arrange.py --output-dir DIR --tempo 120 --style pop
        (--chords "C G | Am F" | --audio song.wav | --midi melody.mid)
        [--parts drums,bass,keys,guitar,strings] [--structure song|loop|follow] [--name mysong] [--seed 1]

Where the harmony comes from:
  --chords   typed chord symbols: C, Am, F#m7, Bb, Gsus4, C/E ... "|" separates bars; without "|" every
             chord gets a bar of its own. The progression repeats over the whole song.
  --audio    a recording: the chords are recognised every half bar (chroma of the harmonic part against
             chord templates, smoothed with Viterbi and the key), the sections follow its loudness
  --midi     a MIDI file (e.g. a melody converted by chost): the chords that fit its notes

Each part is a General MIDI file of its own, written as <name>_<part>.mid in DIR, starting where the song's
first bar starts. Styles: pop rock ballad folk electronic chinese. The parts follow the song's sections:
quiet (intro, outro), middle (verse) and full (chorus), with drum fills and crashes where sections change.
Requires numpy; recognising chords from audio needs librosa too.
"""

import argparse
import os
import re
import sys
import traceback

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from chost_transcribe import write_midi  # noqa: E402

NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
PITCH = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
QUALITIES = {
    "": [0, 4, 7], "maj": [0, 4, 7], "M": [0, 4, 7],
    "m": [0, 3, 7], "min": [0, 3, 7],
    "7": [0, 4, 7, 10], "maj7": [0, 4, 7, 11], "M7": [0, 4, 7, 11],
    "m7": [0, 3, 7, 10], "min7": [0, 3, 7, 10],
    "6": [0, 4, 7, 9], "m6": [0, 3, 7, 9],
    "9": [0, 4, 7, 10, 2], "add9": [0, 4, 7, 2], "m9": [0, 3, 7, 10, 2], "madd9": [0, 3, 7, 2],
    "sus4": [0, 5, 7], "sus": [0, 5, 7], "sus2": [0, 2, 7], "7sus4": [0, 5, 7, 10],
    "dim": [0, 3, 6], "dim7": [0, 3, 6, 9], "m7b5": [0, 3, 6, 10], "aug": [0, 4, 8], "+": [0, 4, 8],
}

STYLES = ("pop", "rock", "ballad", "folk", "electronic", "chinese")
DEFAULT_PARTS = {
    "pop": ["drums", "bass", "keys", "guitar", "strings"],
    "rock": ["drums", "bass", "guitar", "strings"],
    "ballad": ["drums", "bass", "keys", "strings"],
    "folk": ["drums", "bass", "guitar"],
    "electronic": ["drums", "bass", "keys", "strings"],
    "chinese": ["drums", "bass", "keys", "strings"],
}
PART_NAMES = {"drums": "鼓", "bass": "贝斯", "keys": "键盘", "guitar": "吉他", "strings": "弦乐"}
# General MIDI program and chost's instrument role for each part, by style
PROGRAMS = {
    "bass": {"rock": 34, "electronic": 38, "*": 33},
    "keys": {"electronic": 81, "chinese": 107, "*": 0},
    "guitar": {"rock": 30, "*": 25},
    "strings": {"electronic": 89, "*": 48},
}
ROLES = {"drums": "drums", "bass": "bass", "keys": "piano", "guitar": "guitar", "strings": "other"}

# General MIDI drums
KICK, RIM, SNARE, CLAP, HH, PEDAL_HH, OPEN_HH, CRASH, RIDE, TAMBOURINE, SHAKER = 36, 37, 38, 39, 42, 44, 46, 49, 51, 54, 82
TOMS = [50, 48, 45, 43]


def progress(pct, message):
    print("PROGRESS", int(pct), message, flush=True)


# ---------------------------------------------------------------------------------------------------------------------
# chords


class Chord:
    def __init__(self, root, intervals, bass=None, name=""):
        self.root = root % 12
        self.intervals = intervals
        self.bass = self.root if bass is None else bass % 12
        self.name = name or (NAMES[self.root] + quality_name(intervals))

    def pcs(self):
        return [(self.root + i) % 12 for i in self.intervals]

    def triad(self):
        return [(self.root + i) % 12 for i in self.intervals[:3]]

    def third(self):
        return next(((self.root + i) % 12 for i in self.intervals if i in (3, 4)), (self.root + 7) % 12)

    def fifth(self):
        return next(((self.root + i) % 12 for i in self.intervals if i in (6, 7, 8)), (self.root + 7) % 12)

    def __eq__(self, other):
        return isinstance(other, Chord) and self.name == other.name

    def __hash__(self):
        return hash(self.name)


def quality_name(intervals):
    for name, ivs in QUALITIES.items():
        if ivs == intervals and name not in ("maj", "M", "min", "M7", "min7", "sus"):
            return name
    return ""


def parse_note(text):
    pc = PITCH[text[0].upper()]
    for accidental in text[1:]:
        pc += 1 if accidental in "#♯" else -1
    return pc % 12


def parse_chord(token):
    m = re.fullmatch(r"([A-Ga-g][#b♯♭]?)([^/]*)(?:/([A-Ga-g][#b♯♭]?))?", token.strip())
    if not m:
        raise ValueError("unknown chord: " + token)
    quality = m.group(2)
    if quality not in QUALITIES:
        raise ValueError("unknown chord: " + token)
    root = parse_note(m.group(1).replace("♭", "b"))
    bass = parse_note(m.group(3).replace("♭", "b")) if m.group(3) else None
    name = NAMES[root] + quality + ("/" + NAMES[bass] if bass is not None else "")
    return Chord(root, QUALITIES[quality], bass, name)


def parse_progression(text):
    """Bars of chords: [[Chord, ...], ...]."""
    text = text.replace("｜", "|").replace("，", " ").replace(",", " ").replace("、", " ")
    if "|" in text:
        bars = [b.split() for b in text.split("|") if b.strip()]
    else:
        bars = [[t] for t in text.split()]
    if not bars:
        raise ValueError("no chords given")
    return [[parse_chord(t) for t in bar] for bar in bars]


def chord_vocabulary(sevenths=True):
    chords = []
    for root in range(12):
        for quality in (("", "m", "7", "m7") if sevenths else ("", "m")):
            chords.append(Chord(root, QUALITIES[quality]))
    return chords


def diatonic(key_root, minor):
    """The chords that belong to a key: its major scale's I ii iii IV V vi (and V7)."""
    t = (key_root + 3) % 12 if minor else key_root
    names = {NAMES[t], NAMES[(t + 2) % 12] + "m", NAMES[(t + 4) % 12] + "m", NAMES[(t + 5) % 12],
             NAMES[(t + 7) % 12], NAMES[(t + 7) % 12] + "7", NAMES[(t + 9) % 12] + "m",
             NAMES[(t + 2) % 12] + "m7", NAMES[(t + 9) % 12] + "m7"}
    if minor:
        names |= {NAMES[(key_root + 7) % 12], NAMES[(key_root + 7) % 12] + "7"}  # the harmonic minor's major V
    return names


def estimate_key(profile):
    major = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
    minor = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
    best = max(((np.corrcoef(profile, np.roll(t, k))[0, 1], k, m) for m, t in ((False, major), (True, minor)) for k in range(12)))
    return best[1], best[2]


def recognise(frames, bass_frames=None, sevenths=True, key_bonus=0.06, bass_weight=0.12, change=0.12):
    """Chords for a sequence of 12-bin chroma frames (Viterbi over chord templates, favouring the key)."""
    vocab = chord_vocabulary(sevenths)
    profile = frames.sum(axis=0)
    key_root, minor = estimate_key(profile if profile.sum() > 0 else np.ones(12))
    in_key = diatonic(key_root, minor)

    templates = np.zeros((len(vocab), 12))
    for i, c in enumerate(vocab):
        for n, pc in enumerate(c.pcs()):
            templates[i, pc] = 1.0 if n < 3 else 0.6  # a seventh weighs less than the triad
    templates /= np.linalg.norm(templates, axis=1, keepdims=True)
    norm = np.linalg.norm(frames, axis=1, keepdims=True)
    unit = np.where(norm > 0, frames / np.maximum(norm, 1e-9), 0.0)
    scores = unit @ templates.T
    scores -= np.array([0.03 if len(c.intervals) > 3 else 0.0 for c in vocab])  # sevenths only when clearly there
    scores += np.array([key_bonus if c.name in in_key else 0.0 for c in vocab])
    if bass_frames is not None:
        bnorm = bass_frames / np.maximum(bass_frames.sum(axis=1, keepdims=True), 1e-9)
        scores += bass_weight * bnorm[:, [c.root for c in vocab]]

    # Viterbi: changing chord costs a little, so one odd frame doesn't make a chord of its own
    n, k = scores.shape
    total = scores[0].copy()
    back = np.zeros((n, k), dtype=int)
    for t in range(1, n):
        best_prev = int(np.argmax(total))
        stay = total
        move = total[best_prev] - change
        back[t] = np.where(stay >= move, np.arange(k), best_prev)
        total = np.maximum(stay, move) + scores[t]
    path = [int(np.argmax(total))]
    for t in range(n - 1, 0, -1):
        path.append(back[t, path[-1]])
    path.reverse()
    silent = norm[:, 0] < 1e-6
    chords = [vocab[i] for i in path]
    for t in range(n):  # silence keeps the chord before it
        if silent[t] and t > 0:
            chords[t] = chords[t - 1]
    return chords, key_root, minor


def bars_from_halves(halves):
    bars = []
    for i in range(0, len(halves) - 1, 2):
        a, b = halves[i], halves[i + 1]
        bars.append([a] if a == b else [a, b])
    return bars


# ---------------------------------------------------------------------------------------------------------------------
# reading a song


def from_audio(path, tempo):
    """(first downbeat seconds, bars of chords, loudness per bar)."""
    import librosa

    sr = 22050
    y, _ = librosa.load(path, sr=sr, mono=True)
    if len(y) < sr * 2:
        raise RuntimeError("the audio is too short")
    progress(10, "finding the beat")
    spb = 60.0 / tempo
    hop = 512
    onset = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop, aggregate=np.median)
    times = librosa.times_like(onset, sr=sr, hop_length=hop)
    # the beat's phase: the average position of the onsets within a beat, the strong ones counting most;
    # backtracked to where each sound starts rather than where it peaks
    peaks = librosa.onset.onset_detect(onset_envelope=onset, sr=sr, hop_length=hop)
    starts = librosa.onset.onset_backtrack(peaks, onset)
    if len(peaks) == 0:
        raise RuntimeError("no beat found")
    angles = 2.0 * np.pi * (librosa.frames_to_time(starts, sr=sr, hop_length=hop) % spb) / spb
    phase = (np.angle((onset[peaks] * np.exp(1j * angles)).sum()) % (2.0 * np.pi)) * spb / (2.0 * np.pi)
    beats = np.arange(phase, times[-1], spb)
    progress(25, "listening to the chords")
    harmonic = librosa.effects.harmonic(y, margin=2.0)
    chroma_hop = 1024
    chroma = librosa.feature.chroma_cqt(y=harmonic, sr=sr, hop_length=chroma_hop, fmin=librosa.note_to_hz("C2"), n_octaves=5)
    bass = librosa.feature.chroma_cqt(y=harmonic, sr=sr, hop_length=chroma_hop, fmin=librosa.note_to_hz("C1"), n_octaves=2)
    rms = librosa.feature.rms(y=y, frame_length=2048, hop_length=chroma_hop)[0]
    ct = librosa.times_like(chroma, sr=sr, hop_length=chroma_hop)

    # the downbeat: of the four beats of a bar, the one where the chords change most and the low end
    # hits hardest
    per_beat = np.array([chroma[:, (ct >= b) & (ct < b + spb)].mean(axis=1) if ((ct >= b) & (ct < b + spb)).any()
                         else np.zeros(12) for b in beats])
    unit = per_beat / np.maximum(np.linalg.norm(per_beat, axis=1, keepdims=True), 1e-9)
    change = np.concatenate([[0.0], 1.0 - (unit[1:] * unit[:-1]).sum(axis=1)])
    low = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop, fmax=150, n_mels=8)
    kick = np.interp(beats, times, low)
    def score(k):
        c, l = change[k::4], kick[k::4]
        return (c.mean() if len(c) else 0.0) / max(change.mean(), 1e-9) + 0.3 * (l.mean() if len(l) else 0.0) / max(kick.mean(), 1e-9)
    downbeat = int(np.argmax([score(k) for k in range(4)]))
    start = beats[downbeat] if downbeat < len(beats) else phase
    start -= 4 * spb * int((start + 0.05) // (4 * spb))  # the earliest bar that fits

    half = 2 * spb
    n_halves = int((len(y) / sr - start) / half + 0.25)  # a last half bar cut a little short still counts
    n_halves -= n_halves % 2
    if n_halves < 2:
        raise RuntimeError("the song is shorter than a bar")
    frames, bass_frames, levels = [], [], []
    for h in range(n_halves):
        sel = (ct >= start + h * half) & (ct < start + (h + 1) * half)
        frames.append(chroma[:, sel].mean(axis=1) if sel.any() else np.zeros(12))
        bass_frames.append(bass[:, sel].mean(axis=1) if sel.any() else np.zeros(12))
        levels.append(rms[sel[: len(rms)]].mean() if sel[: len(rms)].any() else 0.0)
    frames, bass_frames = np.array(frames), np.array(bass_frames)
    loud = rms.max() * 0.02
    frames[np.array(levels) < loud] = 0.0
    chords, _, _ = recognise(frames, bass_frames)
    bar_levels = np.array(levels).reshape(-1, 2).mean(axis=1)
    return start, bars_from_halves(chords), bar_levels


def from_midi(path, tempo):
    """(0, bars of chords, note density per bar) for the notes of a MIDI file."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    notes = read_midi_notes(path)
    if not notes:
        raise RuntimeError("the MIDI file has no notes")
    spb = 60.0 / tempo
    half = 2 * spb
    end = max(n[1] for n in notes)
    n_halves = int(np.ceil(end / half))
    n_halves += n_halves % 2
    frames = np.zeros((n_halves, 12))
    bass = np.zeros((n_halves, 12))
    density = np.zeros(n_halves)
    for start, stop, pitch, vel in notes:
        # a note on the beat that starts a half bar says more about its chord than a passing one
        weight = 2.0 if abs(start / spb - round(start / spb)) < 0.1 and round(start / spb) % 2 == 0 else 1.0
        for h in range(int(start // half), min(n_halves, int(np.ceil(stop / half)))):
            overlap = min(stop, (h + 1) * half) - max(start, h * half)
            if overlap > 0:
                frames[h, pitch % 12] += overlap * vel * weight
                if pitch < 52:
                    bass[h, pitch % 12] += overlap * vel * weight
                density[h] += overlap
    # one note at a time: a melody or a bass line, which leave the chord open, so stay plainly in the key
    overlapping = sum(1 for a, b in zip(sorted(notes), sorted(notes)[1:]) if b[0] < a[1] - 0.05)
    single = overlapping < 0.1 * len(notes)
    if single and np.median([n[2] for n in notes]) < 52:
        chords, _, _ = recognise(frames, bass, sevenths=False, key_bonus=0.2, bass_weight=0.6, change=0.2)
    elif single:
        chords, _, _ = recognise(frames, None, sevenths=False, key_bonus=0.2, change=0.2)
    else:
        chords, _, _ = recognise(frames, bass if bass.sum() > 0 else None)
    return 0.0, bars_from_halves(chords), density.reshape(-1, 2).mean(axis=1)


def read_midi_notes(path):
    """(start s, end s, pitch, velocity) of every note but drums, with the file's tempo changes."""
    data = open(path, "rb").read()
    if data[:4] != b"MThd":
        raise RuntimeError("not a MIDI file")
    ntracks, division = int.from_bytes(data[10:12], "big"), int.from_bytes(data[12:14], "big")
    pos = 14
    events = []  # (tick, kind, a, b, channel)
    for _ in range(ntracks):
        if data[pos : pos + 4] != b"MTrk":
            break
        length = int.from_bytes(data[pos + 4 : pos + 8], "big")
        end = pos + 8 + length
        pos += 8
        tick, status = 0, 0
        while pos < end:
            delta = 0
            while True:
                byte = data[pos]
                pos += 1
                delta = (delta << 7) | (byte & 0x7F)
                if byte < 0x80:
                    break
            tick += delta
            if data[pos] >= 0x80:
                status = data[pos]
                pos += 1
            if status == 0xFF:
                kind = data[pos]
                pos += 1
                size = 0
                while True:
                    byte = data[pos]
                    pos += 1
                    size = (size << 7) | (byte & 0x7F)
                    if byte < 0x80:
                        break
                if kind == 0x51:
                    events.append((tick, "tempo", int.from_bytes(data[pos : pos + 3], "big"), 0, 0))
                pos += size
            elif status in (0xF0, 0xF7):
                size = 0
                while True:
                    byte = data[pos]
                    pos += 1
                    size = (size << 7) | (byte & 0x7F)
                    if byte < 0x80:
                        break
                pos += size
            else:
                kind = status & 0xF0
                n = 1 if kind in (0xC0, 0xD0) else 2
                a = data[pos]
                b = data[pos + 1] if n == 2 else 0
                pos += n
                if kind in (0x80, 0x90):
                    events.append((tick, "on" if kind == 0x90 and b > 0 else "off", a, b, status & 0x0F))
        pos = end
    events.sort(key=lambda e: (e[0], e[1] != "tempo"))
    tempo, last_tick, seconds = 500000, 0, 0.0
    open_notes, notes = {}, []
    for tick, kind, a, b, ch in events:
        seconds += (tick - last_tick) * tempo / 1e6 / division
        last_tick = tick
        if kind == "tempo":
            tempo = a
        elif ch == 9:
            continue
        elif kind == "on":
            open_notes[(ch, a)] = (seconds, b)
        elif (ch, a) in open_notes:
            start, vel = open_notes.pop((ch, a))
            notes.append((start, seconds, a, vel))
    return notes


# ---------------------------------------------------------------------------------------------------------------------
# the song's form


def sections_from_levels(levels):
    """Intensity 1-3 per bar from loudness, in steady blocks of four bars."""
    n = len(levels)
    blocks = [levels[i : i + 4].mean() for i in range(0, n, 4)]
    live = np.array([b for b in blocks if b > 0])
    if len(live) == 0:
        return [2] * n
    low, high = np.quantile(live, 0.35), np.quantile(live, 0.7)
    out = []
    for b in blocks:
        level = 1 if b <= low else (3 if b >= high else 2)
        out.extend([level] * 4)
    return out[:n]


def song_form(progression_bars):
    """Intro, verse, chorus, verse, chorus, outro, each a whole number of times the progression."""
    p = max(1, progression_bars)
    unit = lambda bars: max(p, int(np.ceil(bars / p)) * p)
    return [(1, unit(4)), (2, unit(8)), (3, unit(8)), (2, unit(8)), (3, unit(8)), (1, unit(4))]


# ---------------------------------------------------------------------------------------------------------------------
# the parts


class Bar:
    """One bar of the song, as the parts see it."""

    def __init__(self, index, start, spb, chords, level, first, last, final):
        self.index = index
        self.start = start
        self.spb = spb
        self.chords = chords  # [(beat, beats, Chord)]
        self.level = level
        self.first = first    # first bar of a section
        self.last = last      # last bar of a section
        self.final = final    # the song's last bar

    def t(self, beat):
        return self.start + beat * self.spb

    def chord_at(self, beat):
        for b, length, c in self.chords:
            if b <= beat < b + length:
                return c
        return self.chords[-1][2]

    def segment_end(self, beat):
        for b, length, _ in self.chords:
            if b <= beat < b + length:
                return b + length
        return 4.0


def nearest(pc, target):
    """The MIDI note of pitch class pc closest to target."""
    base = target - ((target - pc) % 12)
    return base if target - base <= 6 else base + 12


def voicing(pcs, low, high, previous=None):
    """Chord tones stacked from an inversion, within low..high, moving as little as possible."""
    candidates = []
    for inv in range(len(pcs)):
        order = pcs[inv:] + pcs[:inv]
        for start in range(low, low + 12):
            if start % 12 != order[0]:
                continue
            notes = [start]
            for pc in order[1:]:
                n = notes[-1] + 1
                while n % 12 != pc:
                    n += 1
                notes.append(n)
            if notes[-1] <= high:
                candidates.append(notes)
    if not candidates:
        return sorted(nearest(pc, (low + high) // 2) for pc in pcs)
    if previous:
        centre = np.mean(previous)
        return min(candidates, key=lambda v: abs(np.mean(v) - centre) + 0.1 * abs(v[-1] - previous[-1]))
    return min(candidates, key=lambda v: abs(np.mean(v) - (low + high) / 2))


class Part:
    def __init__(self, name, style, rng):
        self.name = name
        self.style = style
        self.rng = rng
        self.notes = []
        self.previous = None

    def add(self, start, end, pitch, vel, jitter=0.006):
        if jitter:
            start += self.rng.normal(0.0, jitter)
        vel = int(np.clip(vel * self.rng.uniform(0.92, 1.08), 1, 127))
        self.notes.append((max(0.0, start), max(start + 0.02, end), int(pitch), vel))


def drums(part, bar):
    s, L = part.style, bar.level
    t = bar.t
    hit = lambda beat, note, vel, jitter=0.004: part.add(t(beat), t(beat) + 0.1, note, vel, jitter)
    eighths = [i * 0.5 for i in range(8)]
    sixteenths = [i * 0.25 for i in range(16)]
    fill = bar.last and L >= 2 and not bar.final

    if bar.final:
        hit(0, KICK, 110)
        hit(0, CRASH, 110)
        return
    if bar.first and L >= 2:
        hit(0, CRASH, 100 if L == 3 else 80)

    groove_end = 2.0 if fill else 4.0
    if s == "electronic":
        if L == 1:
            for b in eighths:
                if b % 1 and b < groove_end:
                    hit(b, HH, 50)
        else:
            for b in (0, 1, 2, 3):
                if b < groove_end:
                    hit(b, KICK, 110)
            for b in (1, 3):
                if b < groove_end:
                    hit(b, CLAP, 100)
            for b in (0.5, 1.5, 2.5, 3.5):
                if b < groove_end:
                    hit(b, OPEN_HH, 75)
            if L == 3:
                for b in sixteenths:
                    if b < groove_end and b % 0.5:
                        hit(b, HH, 55)
    elif s in ("ballad", "chinese"):
        if L == 1:
            if bar.first:
                hit(0, RIDE, 50)
        else:
            kicks = [0, 2.5] if L == 2 else [0, 1.5, 2.5]
            for b in kicks:
                if b < groove_end:
                    hit(b, KICK, 90)
            for b in (1, 3):
                if b < groove_end:
                    hit(b, RIM if L == 2 else SNARE, 80 if L == 2 else 95)
            for b in (eighths if L == 3 else [0, 1, 2, 3]):
                if b < groove_end:
                    hit(b, HH, 55 if b % 1 else 65)
    elif s == "folk":
        for b in (sixteenths if L == 3 else eighths):
            if b < groove_end:
                hit(b, SHAKER, 50 if b % 0.5 else 62)
        if L >= 2:
            for b in ([0, 2] if L == 2 else [0, 2, 2.5]):
                if b < groove_end:
                    hit(b, KICK, 85)
            for b in (1, 3):
                if b < groove_end:
                    hit(b, RIM if L == 2 else SNARE, 80)
                    if L == 3:
                        hit(b, TAMBOURINE, 60)
    else:  # pop, rock
        if L == 1:
            for b in (0, 2):
                if b < groove_end:
                    hit(b, KICK, 80)
            for b in (0, 1, 2, 3):
                if b < groove_end:
                    hit(b, HH if s == "pop" else RIDE, 55)
        else:
            if s == "rock":
                kicks = [0, 2, 2.5] if L == 2 else [0, 0.5, 2, 2.5]
            else:
                kicks = [0, 2, 2.5] if L == 2 else [0, 0.75, 2, 2.5]
            for b in kicks:
                if b < groove_end:
                    hit(b, KICK, 105)
            for b in (1, 3):
                if b < groove_end:
                    hit(b, SNARE, 110 if L == 3 else 100)
            cymbal = OPEN_HH if (s == "rock" and L == 3) else HH
            for b in eighths:
                if b < groove_end:
                    hit(b, cymbal, 80 if b % 1 == 0 else 62)

    if fill:
        if s == "electronic":
            for b in sixteenths[8:]:
                hit(b, SNARE, int(60 + (b - 2) * 25))
        else:
            for i, b in enumerate(sixteenths[8:]):
                note = SNARE if i < 4 else TOMS[i - 4]
                hit(b, note, 85 + i * 2)
            hit(2, KICK, 95)


def bass(part, bar):
    s, L = part.style, bar.level
    low, high = 28, 47

    def note(beat, length, which="root", vel=95):
        c = bar.chord_at(beat)
        pc = {"root": c.bass, "fifth": c.fifth(), "third": c.third(), "octave": c.bass}[which]
        pitch = nearest(pc, 36)
        while pitch < low:
            pitch += 12
        while pitch > high:
            pitch -= 12
        if which == "octave" and pitch + 12 <= 59:
            pitch += 12
        end = min(beat + length, bar.segment_end(beat))
        part.add(bar.t(beat), bar.t(end) - 0.02, pitch, vel)

    if bar.final:
        note(0, 4, vel=100)
        return
    if L == 1 and s not in ("electronic", "rock"):
        for b, length, _ in bar.chords:
            note(b, length, vel=80)
        return
    if s == "electronic":
        if L == 1:
            note(0, 4, vel=75)
        else:
            for i, b in enumerate([0.5, 1.5, 2.5, 3.5] if L == 2 else [i * 0.5 for i in range(8)]):
                note(b, 0.4, "octave" if (L == 3 and i % 2) else "root", 100)
    elif s == "rock":
        for b in ([0, 2] if L == 1 else [i * 0.5 for i in range(8)]):
            note(b, 2 if L == 1 else 0.45, vel=100 if b % 1 == 0 else 88)
    elif s in ("ballad", "chinese"):
        if L == 2:
            note(0, 2)
            note(2, 2, "fifth" if len(bar.chords) == 1 else "root")
        else:
            note(0, 1.5)
            note(1.5, 0.5, "fifth", 85)
            note(2, 1.5)
            note(3.5, 0.5, "octave", 85)
    elif s == "folk":
        if L == 2:
            note(0, 2)
            note(2, 2, "fifth")
        else:
            for b, which in zip((0, 1, 2, 3), ("root", "fifth", "octave", "fifth")):
                note(b, 0.95, which)
    else:  # pop
        if L == 2:
            for b, length in ((0, 1.5), (1.5, 0.5), (2, 1.5), (3.5, 0.5)):
                note(b, length, vel=95 if b % 1 == 0 else 80)
        else:
            for b in [i * 0.5 for i in range(8)]:
                note(b, 0.45, "octave" if b == 2.5 else "root", 100 if b % 1 == 0 else 85)


def chord_notes(part, bar, beat, low=55, high=76):
    c = bar.chord_at(beat)
    v = voicing(c.pcs()[:4], low, high, part.previous)
    part.previous = v
    return v


def keys(part, bar):
    s, L = part.style, bar.level

    def block(beat, length, vel, low=55, high=76, bass_note=True):
        v = chord_notes(part, bar, beat, low, high)
        end = min(beat + length, bar.segment_end(beat))
        for n in v:
            part.add(bar.t(beat), bar.t(end) - 0.03, n, vel, 0.004)
        if bass_note:
            root = nearest(bar.chord_at(beat).bass, 43)
            part.add(bar.t(beat), bar.t(end) - 0.03, root, vel - 5)

    def arpeggio(step, vel, low=48, extra=False, pattern=None):
        beats = np.arange(0, 4, step)
        tones = {}
        for b in beats:
            c = bar.chord_at(b)
            if c not in tones:
                pcs = c.pcs()[:3] + ([(c.root + 2) % 12] if extra else [])  # the added second: pentatonic colour
                base = nearest(c.bass, low)
                if base < low - 2:
                    base += 12
                up = [base]
                for octave in (0, 12):
                    for pc in sorted(pcs, key=lambda p: (p - c.bass) % 12):
                        n = nearest(pc, base + octave + 6)
                        while n <= up[-1]:
                            n += 12
                        up.append(n)
                tones[c] = up
            seq = tones[c]
            order = pattern or list(range(len(seq))) + list(range(len(seq) - 2, 0, -1))
            i = int(round(b / step))
            n = seq[order[i % len(order)] % len(seq)]
            while n > 88:  # keep the top of a fast arpeggio out of the whistle range
                n -= 12
            part.add(bar.t(b), bar.t(min(b + step * 2.5, bar.segment_end(b))), n, vel - (6 if i % 2 else 0))

    if bar.final:
        block(0, 4, 80)
        return
    if s == "electronic":
        if L >= 2:
            arpeggio(0.25, 75 if L == 2 else 85, low=55)
        return
    if s == "chinese":
        if L == 1:
            arpeggio(1.0, 65, low=50, extra=True)
        else:
            arpeggio(0.5 if L == 2 else 0.25, 72 if L == 2 else 80, low=50, extra=True)
        return
    if s == "ballad":
        if L == 1:
            for b, length, _ in bar.chords:
                block(b, length, 60)
        else:
            arpeggio(0.5, 68 if L == 2 else 76, low=43, pattern=[0, 2, 3, 4, 5, 4, 3, 2])
            if L == 3:
                for b, length, _ in bar.chords:
                    for n in chord_notes(part, bar, b, 67, 84):
                        part.add(bar.t(b), bar.t(b + length) - 0.05, n, 55)
        return
    # pop, rock, folk
    if L == 1:
        for b, length, _ in bar.chords:
            block(b, length, 58)
    elif L == 2:
        for b, length in ((0, 1.5), (1.5, 0.5), (2, 2)):
            block(b, length, 70 if b % 1 == 0 else 62, bass_note=b == 0)
    else:
        for b, length in ((0, 1), (1, 0.5), (1.5, 1), (2.5, 0.5), (3, 1)):
            block(b, length, 80 if b % 1 == 0 else 70, bass_note=b in (0, 2.5))


def guitar_shape(chord, previous=None):
    """Six strings' worth of a chord: the bass note from the low E string up, open-position like."""
    root = nearest(chord.bass, 45)
    if root < 40:
        root += 12
    if root > 52:
        root -= 12
    notes = [root]
    pcs = chord.triad() + chord.pcs()[3:4]
    target = [root + 7, root + 12, root + 16, root + 19, root + 24]
    for t in target:
        n = min((nearest(pc, t) for pc in pcs), key=lambda x: abs(x - t))
        while n <= notes[-1]:
            n += 12
        if n <= 79:
            notes.append(n)
    return notes


def guitar(part, bar):
    s, L = part.style, bar.level

    def strum(beat, length, vel, down=True, strings=6):
        shape = guitar_shape(bar.chord_at(beat))
        shape = shape[:strings] if down else shape[-strings:]
        order = shape if down else list(reversed(shape))
        end = min(beat + length, bar.segment_end(beat))
        for i, n in enumerate(order):
            part.add(bar.t(beat) + i * 0.012, bar.t(end) - 0.02, n, vel - i * 2, 0.003)

    def power(beat, length, vel):
        c = bar.chord_at(beat)
        root = nearest(c.root, 45)  # a power chord is root and fifth, whatever the bass plays
        if root < 40:
            root += 12
        end = min(beat + length, bar.segment_end(beat))
        for n in (root, root + (c.fifth() - c.root) % 12, root + 12):
            part.add(bar.t(beat), bar.t(end) - 0.02, n, vel, 0.003)

    def pick(step_vel):
        # Travis picking: the thumb alternates the bass on the beats, the fingers fill the offbeats
        for i in range(8):
            b = i * 0.5
            shape = guitar_shape(bar.chord_at(b))
            if i % 2 == 0:
                n = shape[0] if i % 4 == 0 else shape[min(1, len(shape) - 1)]
            else:
                n = shape[[3, 4, 2, 5][i // 2 % 4] % len(shape)]
            part.add(bar.t(b), bar.t(min(b + 1.5, bar.segment_end(b))), n, step_vel - (8 if i % 2 else 0))

    if bar.final:
        if s == "rock":
            power(0, 4, 105)
        else:
            strum(0, 4, 85)
        return
    if s == "rock":
        if L == 1:
            for b, length, _ in bar.chords:
                power(b, length, 85)
        elif L == 2:
            for b in [i * 0.5 for i in range(8)]:
                power(b, 0.4, 92 if b % 1 == 0 else 80)
        else:
            for b, length in ((0, 1.5), (1.5, 0.5), (2, 1), (3, 0.5), (3.5, 0.5)):
                power(b, length, 105 if b % 1 == 0 else 92)
    elif s in ("folk", "ballad", "chinese"):
        if L == 1:
            if s == "folk":
                pick(62)
            else:
                strum(0, 4, 60)
        elif s == "folk" and L == 2:
            pick(75)
        else:
            for b, down in ((0, True), (1, True), (1.5, False), (2.5, False), (3, True), (3.5, False)):
                strum(b, 0.5 if not down else 1.0, 82 if down else 68, down, 6 if down else 4)
    else:  # pop, electronic
        if L == 1:
            for b, length, _ in bar.chords:
                strum(b, length, 62)
        else:
            vel = 76 if L == 2 else 88
            for b, down in ((0, True), (1, True), (1.5, False), (2.5, False), (3, True), (3.5, False)):
                strum(b, 0.5 if not down else 1.0, vel if down else vel - 14, down, 6 if down else 4)


def strings(part, bar):
    s, L = part.style, bar.level
    if s == "rock" and L < 3 and not bar.final:
        return
    low, high = (55, 72) if L < 3 else (62, 81)
    vel = {1: 38, 2: 46, 3: 58}[L]  # sustained strings sound loud: they sit under the rest
    for b, length, c in bar.chords:
        v = voicing(c.pcs()[:3 if L < 3 else 4], low, high, part.previous)
        part.previous = v
        for n in v:
            part.add(bar.t(b), bar.t(b + length) - 0.02, n, vel, 0.0)
        if L == 3:
            # the low strings double the bass an octave up
            part.add(bar.t(b), bar.t(b + length) - 0.02, nearest(c.bass, 48), vel - 8, 0.0)


GENERATORS = {"drums": drums, "bass": bass, "keys": keys, "guitar": guitar, "strings": strings}


# ---------------------------------------------------------------------------------------------------------------------


def arrange(bars_chords, levels, start, tempo, style, parts, seed):
    spb = 60.0 / tempo
    rng = np.random.default_rng(seed)
    n = len(bars_chords)
    out = {name: Part(name, style, rng) for name in parts}
    for i in range(n):
        chords = bars_chords[i]
        share = 4.0 / len(chords)
        bar = Bar(i, start + i * 4 * spb, spb, [(k * share, share, c) for k, c in enumerate(chords)], levels[i],
                  i == 0 or levels[i - 1] != levels[i], i == n - 1 or levels[i + 1] != levels[i], i == n - 1)
        for name in parts:
            GENERATORS[name](out[name], bar)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--tempo", type=float, default=120.0)
    parser.add_argument("--style", default="pop", choices=STYLES)
    parser.add_argument("--chords", default="")
    parser.add_argument("--audio", default="")
    parser.add_argument("--midi", default="")
    parser.add_argument("--parts", default="")
    parser.add_argument("--structure", default="", choices=["", "song", "loop", "follow"])
    parser.add_argument("--name", default="arrangement")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    try:
        tempo = float(np.clip(args.tempo, 30.0, 300.0))
        parts = [p for p in args.parts.split(",") if p] or DEFAULT_PARTS[args.style]
        unknown = [p for p in parts if p not in GENERATORS]
        if unknown:
            raise ValueError("unknown part: " + ", ".join(unknown))
        progress(2, "reading")

        if args.chords:
            progression = parse_progression(args.chords)
            if args.structure == "loop":
                form = [(2, max(8, len(progression)))]
            else:
                form = song_form(len(progression))
            levels = [level for level, bars in form for _ in range(bars)]
            bars_chords = [progression[i % len(progression)] for i in range(len(levels))]
            start = 0.0
        elif args.audio or args.midi:
            if args.audio:
                start, bars_chords, loudness = from_audio(args.audio, tempo)
            else:
                start, bars_chords, loudness = from_midi(args.midi, tempo)
            levels = sections_from_levels(np.asarray(loudness)) if args.structure != "loop" else [2] * len(bars_chords)
            # a song recognised from audio starts and ends quietly whatever its loudness says
            if len(levels) > 8:
                levels[0] = min(levels[0], 2)
        else:
            raise ValueError("give --chords, --audio or --midi")
        print("CHORDS " + " | ".join(" ".join(c.name for c in bar) for bar in bars_chords), flush=True)

        progress(60, "arranging")
        result = arrange(bars_chords, levels, start, tempo, args.style, parts, args.seed)
        os.makedirs(args.output_dir, exist_ok=True)
        for i, name in enumerate(parts):
            part = result[name]
            path = os.path.join(args.output_dir, "%s_%s.mid" % (args.name, name))
            program = 0 if name == "drums" else PROGRAMS[name].get(args.style, PROGRAMS[name]["*"])
            label = PART_NAMES[name]
            write_midi(path, sorted(part.notes), program, name == "drums", tempo, label)
            print("MIDI %s\t%s\t%s\t%d" % (os.path.abspath(path), ROLES[name], label, len(part.notes)), flush=True)
            progress(60 + 40 * (i + 1) / len(parts), "writing " + name)
        print("DONE", flush=True)
        return 0
    except Exception as e:
        traceback.print_exc()
        print("ERROR", str(e) or e.__class__.__name__, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
