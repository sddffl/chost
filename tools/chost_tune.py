"""chost AI pitch correction (修音) for a solo voice or instrument.

chost runs this script with the Python of its AI environment and reads stdout line by line:

    PROGRESS <0-100> <message>
    KEY <tonic> <major|minor>                                  with --key auto: the key the melody is in
    NOTE <start s>\t<end s>\t<sung pitch>\t<corrected pitch>    pitches as MIDI numbers, 60.5 = C4 + 50 cents
    CURVE <hop s>\t<pitch,pitch,...>                           what was sung, 0 where nothing is pitched
    TUNED <hop s>\t<pitch,pitch,...>                           the same after the correction
    SUMMARY <notes>\t<notes moved>\t<average cents off before>\t<after>
    WARN notsolo                                               it doesn't sound like one voice alone
    ERROR <message>
    DONE <output file>

    python chost_tune.py --input vocals.wav --output tuned.wav [--key "A minor" | --key auto] [--scale smart|strict|chromatic]
                         [--strength 0-1] [--drift 0-1] [--speed <ms>] [--hard]

How it works, like Melodyne's note-based correction rather than a plain auto-tune:
  1. the pitch of every 10 ms (probabilistic YIN), cut into notes where the singer moves to another pitch
  2. each note's centre goes to the nearest note of the key: a note sung clearly off the key but in tune
     (a passing note, a blue note) stays, only the ambiguous ones are pulled into the key
  3. the note is shifted as a whole, so vibrato, scoops and the singer's expression stay; --drift also
     straightens some of the slow wandering inside a note (below 2 Hz, so a vibrato is never touched, and
     less of it in a note that has one: it is the singer's expression, not a mistake). From one note's
     correction to the next it glides on an S-curve over --speed ms (35 by default), never a step
  4. consonants, "s" sounds and breaths (noise, not a pitch) are found by their spectrum and left exactly
     as they are, and a vowel that pitch tracking loses for a moment keeps its note's correction instead
     of dropping out of it and back (what made tuned singing stutter)
  5. the shift itself is pitch-synchronous overlap-add (TD-PSOLA): one grain per vocal cord pulse, the
     grains closer together or further apart. The vowels keep their colour (no chipmunk), and wherever
     nothing moves the output is the input, sample for sample.
--hard is the Auto-Tune effect: every moment straight onto the key's notes, no glide.
Writes a 24-bit WAV with the input's rate and channels. Requires numpy, scipy, soundfile and librosa.
"""

import argparse
import sys
import traceback

import numpy as np

NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
ALIASES = {"Db": 1, "D#": 3, "Gb": 6, "G#": 8, "A#": 10, "Cb": 11, "Fb": 4, "E#": 5, "B#": 0}
MAJOR = [0, 2, 4, 5, 7, 9, 11]
MINOR = [0, 2, 3, 5, 7, 8, 10, 11] # natural minor and the raised 7th singers use over the dominant
OFF_KEY_COST = {"smart": 0.25, "strict": 10.0, "chromatic": 0.0} # semitones a note may be off before the key wins
ANALYSIS_SR = 16000
HOP = 160 # 10 ms


def progress(pct, message):
    print("PROGRESS", int(pct), message, flush=True)


def parse_key(text):
    """'A minor' -> the pitch classes of its scale, None when no key is given."""
    if not text:
        return None
    parts = text.replace("-", " ").split()
    name = parts[0][:1].upper() + parts[0][1:]
    tonic = NAMES.index(name) if name in NAMES else ALIASES.get(name)
    if tonic is None:
        raise RuntimeError("unknown key: " + text)
    mode = parts[1].lower() if len(parts) > 1 else "major"
    return sorted({(tonic + d) % 12 for d in (MINOR if mode.startswith("min") else MAJOR)})


def melody_key(pitch, notes):
    """The key a melody is in: how long each pitch class sounds against the Krumhansl-Kessler profiles."""
    from chost_analyze import MAJOR, MINOR

    weight = np.zeros(12)
    for s, e in notes:
        weight[int(round(note_centre(pitch[s:e]))) % 12] += e - s
    best = (-2.0, 0, "major")
    for mode, profile in (("major", MAJOR), ("minor", MINOR)):
        for tonic in range(12):
            r = np.corrcoef(weight, np.roll(profile, tonic))[0, 1]
            if np.isfinite(r) and r > best[0]:
                best = (r, tonic, mode)
    return "%s %s" % (NAMES[best[1]], best[2])


def nearest_note(pitch, scale, off_key_cost):
    """The note a pitch belongs to: the closest one, off-key notes costing off_key_cost semitones extra."""
    base = int(np.floor(pitch))
    best, best_cost = base, 1e9
    for note in range(base - 1, base + 3):
        cost = abs(pitch - note) + (0.0 if scale is None or note % 12 in scale else off_key_cost)
        if cost < best_cost:
            best, best_cost = note, cost
    return float(best)


# ---------------------------------------------------------------------------------------------------------------------
# what was sung


def track_pitch(mono, sr):
    """Pitch in MIDI numbers per 10 ms frame (NaN where nothing is pitched), and how sure pYIN was of it."""
    import librosa

    y = librosa.resample(mono, orig_sr=sr, target_sr=ANALYSIS_SR) if sr != ANALYSIS_SR else mono
    frames = 1 + len(y) // HOP
    pitch = np.full(frames, np.nan)
    sure = []
    chunk, pad = 20 * ANALYSIS_SR, ANALYSIS_SR // 2 # 20 s at a time, with half a second of context each side
    for start in range(0, len(y), chunk):
        a, b = max(0, start - pad), min(len(y), start + chunk + pad)
        f0, voiced, probability = librosa.pyin(y[a:b], fmin=65.0, fmax=1100.0, sr=ANALYSIS_SR, frame_length=1024, hop_length=HOP)
        f0 = np.where(voiced, f0, np.nan)
        sure.extend(probability[voiced])
        first = (start - a) // HOP
        count = min(chunk // HOP, frames - start // HOP)
        part = f0[first:first + count]
        pitch[start // HOP:start // HOP + len(part)] = 69.0 + 12.0 * np.log2(part / 440.0)
        progress(5 + 45 * min(1.0, (start + chunk) / len(y)), "listening to the pitch")
    # a lone frame or two is a glitch, not a note
    voiced = np.isfinite(pitch)
    runs = np.flatnonzero(np.diff(np.concatenate([[0], voiced.astype(int), [0]])))
    for s, e in zip(runs[::2], runs[1::2]):
        if e - s < 4:
            pitch[s:e] = np.nan
        else:
            # remove isolated 1-2 frame octave jump spikes that cause audible stuttering
            from scipy.ndimage import median_filter
            seg = pitch[s:e].copy()
            filtered = median_filter(seg, size=5, mode="nearest")
            octave_err = np.abs(seg - filtered) > 8.0
            seg[octave_err] = filtered[octave_err]
            pitch[s:e] = seg
    return pitch, float(np.mean(sure)) if sure else 0.0


def find_notes(pitch):
    """(start, end) frame ranges of the notes: a note ends where the voice stops or settles on another pitch."""
    from scipy.ndimage import median_filter

    notes = []
    voiced = np.isfinite(pitch)
    runs = np.flatnonzero(np.diff(np.concatenate([[0], voiced.astype(int), [0]])))
    for s, e in zip(runs[::2], runs[1::2]):
        smooth = median_filter(pitch[s:e], size=5, mode="nearest")
        start, away = 0, 0
        for k in range(len(smooth)):
            # the note so far, its last 300 ms: wide enough to ride out a vibrato
            ref = np.median(smooth[max(start, k - 30):k]) if k > start else smooth[k]
            away = away + 1 if abs(smooth[k] - ref) > 0.8 else 0
            if away >= 6: # 60 ms somewhere else: a new note began where it left
                split = k - away + 1
                if split - start >= 3:
                    notes.append((s + start, s + split))
                    start = split
                away = 0
        notes.append((s + start, e))
    return notes


def note_centre(pitch):
    """The pitch a note is heard at: its middle, the scoop into it and the fall off it left out."""
    n = len(pitch)
    middle = pitch[int(n * 0.2):max(int(n * 0.2) + 1, int(n * 0.85))]
    return float(np.median(middle))


def smooth(values, frames):
    """Centred moving average over a number of frames (Hann-weighted)."""
    if frames < 2:
        return values.copy()
    w = np.hanning(frames + 2)[1:-1]
    w /= w.sum()
    padded = np.pad(values, (frames, frames), mode="edge")
    return np.convolve(padded, w, mode="same")[frames:-frames]


def noise_frames(mono, sr, frames, hop_s):
    """Frames that are noise rather than a voice's pitch: "s", "sh", "f", "t" and the like (most of their
    energy above 4 kHz), breaths (quiet and noise-like) and other noise (a flat spectrum). pYIN now and
    then hears a pitch in them; moving their "pitch" would only smear them."""
    win = max(64, int(0.02 * sr) // 2 * 2)
    half = win // 2
    padded = np.pad(mono.astype(np.float64), (half, half + win))
    starts = np.minimum(np.round(np.arange(frames) * hop_s * sr).astype(np.int64), len(mono))
    f = np.fft.rfftfreq(win, 1.0 / sr)
    high = f >= 4000.0
    voice = (f >= 100.0) & (f <= min(8000.0, sr / 2.0))
    hann = np.hanning(win)
    hf = np.zeros(frames)
    flat = np.zeros(frames)
    level = np.zeros(frames)
    for a in range(0, frames, 2048):
        idx = starts[a:a + 2048]
        block = padded[idx[:, None] + np.arange(win)[None, :]]
        level[a:a + len(idx)] = 10.0 * np.log10(np.mean(block ** 2, axis=1) + 1e-20)
        power = np.abs(np.fft.rfft(block * hann, axis=1)) ** 2 + 1e-20
        hf[a:a + len(idx)] = power[:, high].sum(axis=1) / power.sum(axis=1) if high.any() else 0.0
        band = power[:, voice]
        flat[a:a + len(idx)] = np.exp(np.mean(np.log(band), axis=1)) / np.mean(band, axis=1)
    loud = np.percentile(level, 95)
    sibilant = hf > 0.45
    breath = (flat > 0.25) & (level < loud - 24.0)
    noisy = flat > 0.5
    return sibilant | breath | noisy


def bridge_dropouts(pitch, noise, longest=6):
    """Fills short holes in the pitch where pYIN lost a vowel for a moment (up to longest frames, voiced on
    both sides, no noise in it) by joining the two sides: the note goes on through it, its correction too."""
    pitch = pitch.copy()
    voiced = np.isfinite(pitch)
    runs = np.flatnonzero(np.diff(np.concatenate([[1], voiced.astype(int), [1]])))
    for s, e in zip(runs[::2], runs[1::2]): # unvoiced runs [s, e)
        if s == 0 or e >= len(pitch) or e - s > longest or noise[s:e].any():
            continue
        if abs(pitch[e] - pitch[s - 1]) > 1.0: # a jump to another note is a new note, not a hole
            continue
        pitch[s:e] = np.interp(np.arange(s, e), [s - 1, e], [pitch[s - 1], pitch[e]])
    return pitch


def lowpass(values, cutoff, rate=100.0):
    """Zero-phase low pass of a pitch curve (frames at rate per second)."""
    from scipy.signal import butter, sosfiltfilt

    if len(values) < 16:
        return np.full_like(values, np.median(values))
    return sosfiltfilt(butter(2, cutoff, fs=rate, output="sos"), values, padlen=min(len(values) - 1, 30))


def vibrato_depth(values, rate=100.0):
    """How wide a note's vibrato (4-8 Hz) swings, in semitones (RMS times 2*sqrt(2): peak to peak for a sine)."""
    from scipy.signal import butter, sosfiltfilt

    if len(values) < 30:
        return 0.0
    band = sosfiltfilt(butter(2, [4.0, 8.0], "bandpass", fs=rate, output="sos"), values - np.mean(values), padlen=min(len(values) - 1, 30))
    inner = band[len(band) // 10:len(band) - len(band) // 10] if len(band) > 20 else band
    return float(np.sqrt(np.mean(inner ** 2)) * 2.0 * np.sqrt(2.0))


def note_drift(pitch, sung, drift):
    """The part of a note's wandering --drift straightens: below 2 Hz, so vibrato (4-8 Hz) passes untouched;
    in a note with a clear vibrato only half as much, and never more than a third of a semitone either way,
    so the singer's drifts into and around a note keep their shape; the scoop into it and the fall off it are
    left alone. Faded in and out over the note's first and last 50 ms."""
    if drift <= 0.0:
        return np.zeros(len(pitch))
    # the scoop into the note and the fall off it are expression too: held at the note's middle, so the
    # low pass doesn't spread them over the note (the same parts note_centre leaves out)
    n = len(pitch)
    a, b = int(n * 0.2), max(int(n * 0.2) + 1, int(n * 0.85))
    held = pitch.copy()
    held[:a] = pitch[a]
    held[b:] = pitch[b - 1]
    wander = lowpass(held, 2.0) - sung
    amount = drift * (0.5 if vibrato_depth(pitch) > 0.15 else 1.0)
    straighten = -np.clip(amount * wander, -0.33, 0.33)
    fade = np.minimum(1.0, np.minimum(np.arange(len(pitch)) + 1, np.arange(len(pitch))[::-1] + 1) / 5.0)
    return straighten * fade


def s_curve(times, steps, transition_ms):
    """A curve that starts at steps[0][1] and at every (time, value) of steps glides to that value on a
    logistic S-curve, 2 % to 98 % of the way over transition_ms around the time."""
    out = np.full(len(times), float(steps[0][1]) if steps else 0.0)
    if len(steps) < 2 or len(times) == 0:
        return out
    tau = max(transition_ms, 1e-3) / 1000.0 / (2.0 * np.log(49.0))
    reach = 8.0 * tau # beyond this the curve is within 0.04 % of where it goes
    for (_, before), (at, after) in zip(steps[:-1], steps[1:]):
        delta = after - before
        if delta == 0.0:
            continue
        lo, hi = np.searchsorted(times, [at - reach, at + reach])
        x = (times[lo:hi] - at) / tau
        out[lo:hi] += delta / (1.0 + np.exp(-x))
        out[hi:] += delta
    return out


def correction_at(times, plan, hop_s, transition_ms):
    """The shift in semitones at times (seconds): each note's move, gliding on an S-curve from one note to
    the next (in the middle of the gap between them, or where one note turns into the next), plus the
    note's own straightening of its drift."""
    notes, drift_frames = plan
    if not notes:
        return np.zeros(len(times))
    steps = [(0.0, notes[0][3])]
    for (s0, e0, _, m0), (s1, e1, _, m1) in zip(notes[:-1], notes[1:]):
        steps.append(((e0 + s1) / 2.0 * hop_s, m1))
    shift = s_curve(times, steps, transition_ms)
    frame_times = np.arange(len(drift_frames)) * hop_s
    return shift + np.interp(times, frame_times, drift_frames)


def edge_envelope(times, voiced_at, transition_ms):
    """1 inside the singing, 0 where the input passes through untouched (consonants, breaths, silence),
    the correction fading in on an S-curve over the first transition_ms of the voice and out over its last:
    where it starts and stops there is nothing for the correction to jump from or to."""
    if len(voiced_at) == 0:
        return np.zeros(0)
    edges = np.flatnonzero(np.diff(voiced_at.astype(np.int8)))
    half = transition_ms / 2000.0
    steps = [(0.0, 1.0 if voiced_at[0] else 0.0)]
    for i in edges:
        on = bool(voiced_at[i + 1])
        steps.append((times[i + 1] + (half if on else -half), 1.0 if on else 0.0))
    return np.clip(s_curve_edges(times, steps, transition_ms), 0.0, 1.0)


def s_curve_edges(times, steps, transition_ms):
    """Like s_curve, but each step is an edge of a voiced stretch: on (+1) or off (-1), so stretches that
    overlap after shifting their edges inwards add up instead of jumping."""
    out = np.full(len(times), float(steps[0][1]))
    tau = max(transition_ms, 1e-3) / 1000.0 / (2.0 * np.log(49.0))
    reach = 8.0 * tau
    for at, value in steps[1:]:
        delta = 1.0 if value > 0.5 else -1.0
        lo, hi = np.searchsorted(times, [at - reach, at + reach])
        out[lo:hi] += delta / (1.0 + np.exp(-(times[lo:hi] - at) / tau))
        out[hi:] += delta
    return out


def plan_correction(pitch, scale, off_key_cost, strength, drift, speed_ms, hard):
    """Semitones to move each frame by, and the notes as (start, end, sung, corrected) in frames."""
    n = len(pitch)
    voiced = np.isfinite(pitch)
    shift = np.zeros(n)
    notes = []
    if hard:
        # every moment of every note straight onto the note: scoops, vibrato and wandering all go
        for s, e in find_notes(pitch):
            sung = note_centre(pitch[s:e])
            target = nearest_note(sung, scale, off_key_cost)
            shift[s:e] = target - pitch[s:e]
            notes.append((s, e, sung, target))
        speed_frames = 1
    else:
        # each note moved as a whole, an S-curve from one to the next (correction_at), the slow drift
        # inside a note partly straightened; frame by frame here for the curve chost shows
        moves, drifts = [], np.zeros(n)
        for s, e in find_notes(pitch):
            sung = note_centre(pitch[s:e])
            target = nearest_note(sung, scale, off_key_cost)
            move = (target - sung) * strength
            drifts[s:e] = note_drift(pitch[s:e], sung, drift)
            moves.append((s, e, sung, move))
            notes.append((s, e, sung, sung + move))
        hop_s = HOP / ANALYSIS_SR
        plan = (moves, drifts)
        shift = correction_at(np.arange(n) * hop_s, plan, hop_s, speed_ms)
        shift[~voiced] = 0.0
        return shift, notes, plan
    # in the gaps the shift glides from one note's to the next, so the smoothing has nothing to jump over
    idx = np.flatnonzero(voiced)
    if len(idx):
        shift = np.interp(np.arange(n), idx, shift[idx])
        shift = smooth(shift, speed_frames)
    shift[~voiced] = 0.0 # nothing pitched there: left exactly as it is
    return shift, notes, None


# ---------------------------------------------------------------------------------------------------------------------
# moving it


def pitch_marks(mono, sr, period):
    """Analysis marks: one per pitch period on the waveform's peaks where voiced, every 5 ms elsewhere.

    period: samples per period at every sample, 0 where unvoiced."""
    from scipy.signal import butter, sosfiltfilt

    lowpassed = sosfiltfilt(butter(2, 900.0, fs=sr, output="sos"), mono)
    step = int(sr * 0.005)
    marks = []
    t = 0
    n = len(mono)
    while t < n:
        p = period[t]
        if p > 0:
            # the strongest peak near where one period on should be, so the grains all start on a pulse
            lo, hi = max(0, int(t - 0.2 * p)), min(n, int(t + 0.2 * p) + 1)
            if marks and marks[-1] < t:
                peak = lo + int(np.argmax(np.abs(lowpassed[lo:hi])))
                t = max(peak, marks[-1] + int(0.5 * p))
            marks.append(t)
            t = int(t + p)
        else:
            marks.append(t)
            t += step
    return np.asarray(marks, dtype=np.int64)


def psola(audio, sr, marks, voiced_at, ratio_at):
    """Time-domain PSOLA: the grains around the analysis marks, laid down every period / ratio.

    audio: (samples, channels). voiced_at, ratio_at: per sample."""
    n = len(audio)
    out = np.zeros_like(audio)
    weight = np.zeros(n)
    left = np.diff(marks, prepend=marks[0] - (marks[1] - marks[0] if len(marks) > 1 else 1))
    right = np.diff(marks, append=marks[-1] + (marks[-1] - marks[-2] if len(marks) > 1 else 1))
    left = np.maximum(left, 1)
    right = np.maximum(right, 1)
    s = float(marks[0])
    i = 0
    while s < n:
        # the analysis mark nearest to where this grain goes
        while i + 1 < len(marks) and abs(marks[i + 1] - s) <= abs(marks[i] - s):
            i += 1
        a = int(marks[i])
        if voiced_at[a]:
            r = float(ratio_at[min(int(s), n - 1)])
        else:
            # nothing pitched: the grain goes back where it came from, so this stays the input exactly
            r, s = 1.0, float(a)
        # half a Hann either side: with ratio 1 neighbouring grains add up to exactly 1
        widen = max(1.0, 1.0 / r)
        lw, rw = int(left[i] * widen), int(right[i] * widen)
        c = int(round(s))
        lo, hi = max(0, a - lw, a - c), min(n, a + rw, a + n - c)
        if hi > lo:
            k = np.arange(lo, hi) - a
            w = np.where(k < 0, 0.5 + 0.5 * np.cos(np.pi * k / lw), 0.5 + 0.5 * np.cos(np.pi * k / rw))
            out[lo - a + c:hi - a + c] += audio[lo:hi] * w[:, None]
            weight[lo - a + c:hi - a + c] += w
        s += right[i] / r if r > 0 else right[i]
        if s <= c: # never stand still
            s = c + 1.0
    covered = weight > 1e-3
    out[covered] /= weight[covered, None]
    out[~covered] = audio[~covered]
    return out


def encode(curve):
    return ",".join("0" if not np.isfinite(v) else "%.2f" % v for v in curve)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--key", default="", help='e.g. "A minor", or auto; without one every semitone is a note')
    parser.add_argument("--scale", choices=sorted(OFF_KEY_COST), default="smart")
    parser.add_argument("--strength", type=float, default=1.0, help="how far each note goes to its target, 0-1")
    parser.add_argument("--drift", type=float, default=0.3, help="how much of the wandering inside a note goes, 0-1")
    parser.add_argument("--speed", type=float, default=35.0, help="milliseconds of the S-curve from one note's correction to the next")
    parser.add_argument("--hard", action="store_true", help="the Auto-Tune effect")
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    try:
        import soundfile as sf

        progress(2, "reading the audio")
        audio, sr = sf.read(args.input, dtype="float64", always_2d=True)
        if len(audio) < sr // 2 or np.max(np.abs(audio)) < 1e-4:
            raise RuntimeError("the audio is too short or silent")
        mono = audio.mean(axis=1)
        strength = float(np.clip(args.strength, 0.0, 1.0))
        drift = float(np.clip(args.drift, 0.0, 1.0))

        pitch, sure = track_pitch(mono, sr)
        hop_s = HOP / ANALYSIS_SR
        # consonants and breaths are left as they are; a vowel lost for a moment keeps its note
        noise = noise_frames(mono, sr, len(pitch), hop_s)
        pitch[noise] = np.nan
        pitch = bridge_dropouts(pitch, noise)
        if sure < 0.3:
            # a single voice is clear (0.8 and up); chords, drums and whole mixes leave pYIN guessing
            print("WARN notsolo", flush=True)
        if np.isfinite(pitch).sum() < 20:
            raise RuntimeError("no singing found (is this a vocal or solo instrument track?)")
        progress(55, "finding the notes")
        key = args.key
        if key == "auto":
            key = melody_key(pitch, find_notes(pitch))
            print("KEY", key, flush=True)
        scale = parse_key(key)
        off_key_cost = OFF_KEY_COST[args.scale] if scale is not None else 0.0
        shift, notes, plan = plan_correction(pitch, scale, off_key_cost, strength, drift, args.speed, args.hard)

        progress(60, "moving the notes")
        times = np.arange(len(audio)) / sr
        frame_times = np.arange(len(pitch)) * hop_s
        hz = 440.0 * 2.0 ** ((np.nan_to_num(pitch, nan=-1000.0) - 69.0) / 12.0)
        voiced_frames = np.isfinite(pitch)
        # the frame each sample belongs to (pYIN frames are centred on their time)
        frame_of = np.minimum(np.round(times / hop_s).astype(np.int64), len(pitch) - 1)
        voiced_at = voiced_frames[frame_of]
        period = np.where(voiced_at, sr / np.maximum(hz[frame_of], 1.0), 0.0)
        # the S-curves at every sample, not just every 10 ms frame
        if plan is not None:
            semitones = correction_at(times, plan, hop_s, args.speed) * edge_envelope(times, voiced_at, args.speed)
        else:
            semitones = np.interp(times, frame_times, shift)
        ratio_at = 2.0 ** (semitones / 12.0)
        marks = pitch_marks(mono, sr, period)
        progress(75, "moving the notes")
        tuned = psola(audio, sr, marks, voiced_at, ratio_at)
        peak = np.max(np.abs(tuned))
        if peak > 0.999: # the grains can add up a hair over the original's peaks
            tuned *= 0.999 / peak

        progress(92, "writing the result")
        sf.write(args.output, tuned, sr, subtype="PCM_24")

        before, after, moved = [], [], 0
        for s, e, sung, corrected in notes:
            target = nearest_note(sung, scale, off_key_cost)
            before.append(abs(sung - target) * 100.0)
            after.append(abs(corrected - target) * 100.0)
            moved += abs(corrected - sung) >= 0.05
            print("NOTE %.3f\t%.3f\t%.2f\t%.2f" % (s * hop_s, e * hop_s, sung, corrected))
        print("CURVE %.4f\t%s" % (hop_s, encode(pitch)), flush=True)
        print("TUNED %.4f\t%s" % (hop_s, encode(np.where(voiced_frames, pitch + shift, np.nan))), flush=True)
        print("SUMMARY %d\t%d\t%.0f\t%.0f" % (len(notes), moved, np.mean(before) if before else 0.0,
                                               np.mean(after) if after else 0.0), flush=True)
        progress(100, "done")
        print("DONE", args.output, flush=True)
        return 0
    except Exception as e:
        traceback.print_exc()
        print("ERROR", str(e) or e.__class__.__name__, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
