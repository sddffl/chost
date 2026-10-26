"""chost audio-to-MIDI helper.

chost runs this script with the Python of its AI environment and reads stdout line by line:

    PROGRESS <0-100> <message>
    MIDI <path>\t<note count>
    ERROR <message>
    DONE

Pitched parts go through Spotify's Basic Pitch model (tools/models/basic_pitch_nmp.onnx, run with
onnxruntime, nothing to download). Drums are found by onset detection: one General MIDI drum note per
separated part (kick, snare, ...), or kick/snare/hi-hat by frequency band for a whole drum stem.

    python chost_transcribe.py --input bass.wav --output bass.mid --role bass [--tempo 120]

Roles: vocals lead backing bass guitar piano other mix, drums kick snare toms hh ride crash.
Requires numpy, scipy, librosa and onnxruntime (all part of audio-separator's dependencies).
"""

import argparse
import os
import struct
import sys
import traceback

import numpy as np

# ---------------------------------------------------------------------------------------------------------------------
# Basic Pitch: model constants and note decoding.
# Adapted from basic_pitch/constants.py and basic_pitch/note_creation.py, Copyright 2022 Spotify AB,
# Apache License 2.0 (see tools/models/LICENSE-basic-pitch.txt).

FFT_HOP = 256
AUDIO_SAMPLE_RATE = 22050
AUDIO_WINDOW_LENGTH = 2
ANNOTATIONS_FPS = AUDIO_SAMPLE_RATE // FFT_HOP
ANNOT_N_FRAMES = ANNOTATIONS_FPS * AUDIO_WINDOW_LENGTH
AUDIO_N_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_WINDOW_LENGTH - FFT_HOP
N_OVERLAPPING_FRAMES = 30
MIDI_OFFSET = 21
MAX_FREQ_IDX = 87

MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "basic_pitch_nmp.onnx")


def run_model(audio):
    """Runs Basic Pitch over mono 22.05 kHz audio, returns (frames, onsets) of shape (n_times, 88)."""
    import onnxruntime as ort

    session = ort.InferenceSession(MODEL_PATH, providers=["CPUExecutionProvider"])
    overlap_len = N_OVERLAPPING_FRAMES * FFT_HOP
    hop_size = AUDIO_N_SAMPLES - overlap_len
    original_length = audio.shape[0]
    padded = np.concatenate([np.zeros(overlap_len // 2, dtype=np.float32), audio.astype(np.float32)])

    notes, onsets = [], []
    starts = list(range(0, padded.shape[0], hop_size))
    for n, i in enumerate(starts):
        window = padded[i : i + AUDIO_N_SAMPLES]
        if len(window) < AUDIO_N_SAMPLES:
            window = np.pad(window, (0, AUDIO_N_SAMPLES - len(window)))
        note, onset = session.run(
            ["StatefulPartitionedCall:1", "StatefulPartitionedCall:2"],
            {"serving_default_input_2:0": window[np.newaxis, :, np.newaxis]},
        )
        notes.append(note)
        onsets.append(onset)
        if n % 8 == 0:
            progress(10 + 70 * n / len(starts), "listening for notes")

    def unwrap(output):
        n_olap = N_OVERLAPPING_FRAMES // 2
        output = np.concatenate(output)[:, n_olap:-n_olap, :]
        frames = int(np.floor(original_length * (ANNOTATIONS_FPS / AUDIO_SAMPLE_RATE)))
        return output.reshape(-1, output.shape[2])[:frames, :]

    return unwrap(notes), unwrap(onsets)


def model_frames_to_time(n_frames):
    original_times = np.arange(n_frames) * FFT_HOP / AUDIO_SAMPLE_RATE
    window_numbers = np.floor(np.arange(n_frames) / ANNOT_N_FRAMES)
    window_offset = (FFT_HOP / AUDIO_SAMPLE_RATE) * (ANNOT_N_FRAMES - (AUDIO_N_SAMPLES / FFT_HOP)) + 0.0018
    return original_times - (window_offset * window_numbers)


def hz_to_index(hz):
    return int(np.round(12 * np.log2(hz / 440.0) + 69 - MIDI_OFFSET))


def inferred_onsets(onsets, frames, n_diff=2):
    """Adds onsets where the frame activation jumps, the model misses some repeated notes."""
    diffs = []
    for n in range(1, n_diff + 1):
        appended = np.concatenate([np.zeros((n, frames.shape[1])), frames])
        diffs.append(appended[n:, :] - appended[:-n, :])
    frame_diff = np.min(diffs, axis=0)
    frame_diff[frame_diff < 0] = 0
    frame_diff[:n_diff, :] = 0
    if np.max(frame_diff) > 0:
        frame_diff = np.max(onsets) * frame_diff / np.max(frame_diff)
    return np.max([onsets, frame_diff], axis=0)


def decode_notes(frames, onsets, onset_thresh, frame_thresh, min_note_len, min_freq, max_freq, energy_tol=11):
    """Polyphonic note decoding with the "melodia trick", returns [(start_frame, end_frame, pitch, amplitude)]."""
    import scipy.signal

    frames = frames.copy()
    onsets = onsets.copy()
    n_frames = frames.shape[0]
    if max_freq is not None:
        onsets[:, hz_to_index(max_freq) :] = 0
        frames[:, hz_to_index(max_freq) :] = 0
    if min_freq is not None:
        onsets[:, : hz_to_index(min_freq)] = 0
        frames[:, : hz_to_index(min_freq)] = 0
    onsets = inferred_onsets(onsets, frames)

    peak_thresh = np.zeros(onsets.shape)
    peaks = scipy.signal.argrelmax(onsets, axis=0)
    peak_thresh[peaks] = onsets[peaks]
    onset_idx = np.where(peak_thresh >= onset_thresh)
    remaining = frames.copy()

    def clear_neighbours(t0, t1, f):
        remaining[t0:t1, f] = 0
        if f < MAX_FREQ_IDX:
            remaining[t0:t1, f + 1] = 0
        if f > 0:
            remaining[t0:t1, f - 1] = 0

    events = []
    # from the last onset backwards, each onset holds its note until the energy stays low
    for start, f in zip(onset_idx[0][::-1], onset_idx[1][::-1]):
        if start >= n_frames - 1:
            continue
        i, k = start + 1, 0
        while i < n_frames - 1 and k < energy_tol:
            k = k + 1 if remaining[i, f] < frame_thresh else 0
            i += 1
        i -= k
        if i - start <= min_note_len:
            continue
        clear_neighbours(start, i, f)
        events.append((start, i, f + MIDI_OFFSET, float(np.mean(frames[start:i, f]))))

    # melodia trick: notes without a detected onset, grown from the strongest remaining energy
    while np.max(remaining) > frame_thresh:
        mid, f = np.unravel_index(np.argmax(remaining), remaining.shape)
        remaining[mid, f] = 0
        i, k = mid + 1, 0
        while i < n_frames - 1 and k < energy_tol:
            k = k + 1 if remaining[i, f] < frame_thresh else 0
            clear_neighbours(i, i + 1, f)
            i += 1
        end = i - 1 - k
        i, k = mid - 1, 0
        while i > 0 and k < energy_tol:
            k = k + 1 if remaining[i, f] < frame_thresh else 0
            clear_neighbours(i, i + 1, f)
            i -= 1
        start = i + 1 + k
        if end - start <= min_note_len:
            continue
        events.append((start, end, f + MIDI_OFFSET, float(np.mean(frames[start:end, f]))))
    return events


# ---------------------------------------------------------------------------------------------------------------------
# roles

# min/max frequency (Hz), monophonic, General MIDI program; drums play on channel 10
PITCHED = {
    "lead": (70, 1400, True, 53),  # voice oohs
    "vocals": (70, 1400, True, 53),
    "backing": (70, 1400, False, 52),  # choir aahs
    "bass": (28, 420, True, 33),  # electric bass (finger)
    "guitar": (70, 1500, False, 25),  # acoustic guitar (steel)
    "piano": (27, 4200, False, 0),  # acoustic grand
    "other": (40, 4200, False, 48),  # strings
    "mix": (27, 4200, False, 0),
}
DRUM_NOTES = {"kick": 36, "snare": 38, "hh": 42, "ride": 51, "crash": 49}
TOM_NOTES = [41, 43, 45, 47, 48, 50]  # low floor tom .. high tom


def progress(pct, message):
    print("PROGRESS", int(pct), message, flush=True)


def load(path, sr):
    import librosa

    audio, _ = librosa.load(path, sr=sr, mono=True)
    return audio


def transcribe_pitched(path, role):
    low, high, mono, program = PITCHED[role]
    progress(3, "loading audio")
    audio = load(path, AUDIO_SAMPLE_RATE)
    frames, onsets = run_model(audio)
    progress(85, "writing notes")
    min_note_len = int(np.round(0.06 * AUDIO_SAMPLE_RATE / FFT_HOP))  # 60 ms
    events = decode_notes(frames, onsets, onset_thresh=0.5, frame_thresh=0.3, min_note_len=min_note_len,
                          min_freq=low, max_freq=high)
    times = model_frames_to_time(frames.shape[0])
    notes = [(float(times[s]), float(times[e]), int(p), amp) for s, e, p, amp in events]
    if mono:
        # the model holds a repeated note as one long note, the attacks in the audio split it again
        attacks = [t for t in onsets_of(audio, AUDIO_SAMPLE_RATE, gate=0.25)[0] if is_reattack(audio, AUDIO_SAMPLE_RATE, t)]
        notes = split_at_attacks(monophonic(notes), attacks)
    else:
        notes = drop_ghost_notes(notes)
    notes = [(s, e, p, velocity(a)) for s, e, p, a in notes if e > s]
    return notes, program, False


def monophonic(notes):
    """One note at a time for voices and bass: of two notes starting together the stronger one stays,
    a note hidden inside a stronger one is dropped, otherwise the earlier note is cut short."""
    kept = []
    for note in sorted(notes, key=lambda n: n[0]):
        start, end, _, amp = note
        if kept and kept[-1][1] > start:
            p_start, p_end, p_pitch, p_amp = kept[-1]
            if start - p_start < 0.03:
                if amp > p_amp:
                    kept[-1] = note
                continue
            if p_end >= end and p_amp * (p_end - p_start) >= amp * (end - start):
                continue
            kept[-1] = (p_start, start, p_pitch, p_amp)
        kept.append(note)
    return [n for n in kept if n[1] - n[0] >= 0.05]


def is_reattack(audio, sr, t, window=0.03):
    """A played note gets louder after a dip; vibrato and consonants that trip the onset detector don't."""
    a = int(t * sr)
    w = int(window * sr)
    before = audio[max(0, a - w) : a]
    after = audio[a : a + 2 * w]
    if len(before) == 0 or len(after) == 0:
        return True
    rms = lambda x: float(np.sqrt(np.mean(x.astype(np.float64) ** 2)) + 1e-9)
    return rms(after) >= 1.4 * rms(before)


def split_at_attacks(notes, attacks, margin=0.06):
    out = []
    attacks = sorted(attacks)
    for start, end, pitch, amp in notes:
        cut = start
        for t in attacks:
            if cut + margin < t < end - margin:
                out.append((cut, t, pitch, amp))
                cut = t
        out.append((cut, end, pitch, amp))
    return out


HARMONICS = {12, 19, 24, 28, 31, -12}  # overtones of a louder note, and the octave below it


def drop_ghost_notes(notes):
    """Removes the weak short notes and overtones that polyphonic transcription adds around loud notes."""
    kept = []
    for i, (start, end, pitch, amp) in enumerate(notes):
        length = end - start
        ghost = False
        for j, (o_start, o_end, o_pitch, o_amp) in enumerate(notes):
            if i == j:
                continue
            overlap = min(end, o_end) - max(start, o_start)
            if overlap < 0.5 * length:
                continue
            if (pitch - o_pitch) in HARMONICS and o_amp >= 1.6 * amp:
                ghost = True
            elif length < 0.12 and o_amp >= 1.4 * amp:
                ghost = True
            if ghost:
                break
        if not ghost:
            kept.append((start, end, pitch, amp))
    return kept


def velocity(amplitude):
    return int(np.clip(np.round(127 * min(1.0, amplitude * 1.6)), 20, 127))


def onsets_of(audio, sr, min_gap=0.05, gate=0.12):
    """Hit times and peak levels; hits quieter than gate x the loudest (separation bleed) are dropped."""
    import librosa

    hop = 128
    # a little silence in front, so that a hit right at the start still counts as an onset
    lead_in = int(0.1 * sr)
    audio = np.concatenate([np.zeros(lead_in, dtype=audio.dtype), audio])
    envelope = librosa.onset.onset_strength(y=audio, sr=sr, hop_length=hop)
    frames = librosa.onset.onset_detect(onset_envelope=envelope, sr=sr, hop_length=hop, backtrack=True,
                                        pre_max=3, post_max=3, pre_avg=10, post_avg=10, delta=0.08, wait=int(min_gap * sr / hop))
    times = librosa.frames_to_time(frames, sr=sr, hop_length=hop)
    times = times[times * sr >= lead_in - hop]
    peaks = []
    for t in times:
        a = int(t * sr)
        peaks.append(float(np.max(np.abs(audio[a : a + int(0.05 * sr)])) if a < len(audio) else 0.0))
    peaks = np.array(peaks)
    if len(peaks) == 0 or peaks.max() <= 0:
        return [], []
    keep = peaks >= gate * peaks.max()
    times = np.maximum(0.0, times - lead_in / sr)
    return list(times[keep]), list(peaks[keep] / peaks.max())


def hit_velocity(level):
    return int(np.clip(np.round(30 + 97 * level ** 0.6), 1, 127))


def transcribe_drum_part(path, role):
    sr = 44100
    progress(5, "loading audio")
    audio = load(path, sr)
    progress(40, "finding hits")
    times, levels = onsets_of(audio, sr)
    notes = []
    if role == "toms":
        # toms are told apart by pitch: rank the hits' fundamentals into as many toms as there are clusters
        freqs = [dominant_frequency(audio, sr, t, 50, 400) for t in times]
        mapping = tom_mapping(freqs)
        notes = [(t, t + 0.1, mapping(f), hit_velocity(v)) for t, v, f in zip(times, levels, freqs)]
    elif role == "hh":
        # an open hi-hat rings much longer than a closed one
        # an open hi-hat rings much longer than a closed one; the ring is measured up to the next hit
        for i, (t, v) in enumerate(zip(times, levels)):
            gap = (times[i + 1] - t) if i + 1 < len(times) else 0.5
            ring = ring_time(audio, sr, t, min(0.5, gap - 0.01))
            is_open = ring > 0.18 or (gap <= 0.2 and ring >= 0.9 * (gap - 0.01))
            notes.append((t, t + 0.1, 46 if is_open else 42, hit_velocity(v)))
    else:
        notes = [(t, t + 0.1, DRUM_NOTES[role], hit_velocity(v)) for t, v in zip(times, levels)]
    return notes, 0, True


def transcribe_drum_kit(path):
    """A whole drum stem: kick, snare and hi-hat from three frequency bands."""
    import scipy.signal

    sr = 44100
    progress(5, "loading audio")
    audio = load(path, sr)
    bands = [("kick", 36, ("lowpass", 120)), ("snare", 38, ("bandpass", (180, 2500))), ("hh", 42, ("highpass", 7000))]
    notes = []
    for n, (name, note, (kind, cutoff)) in enumerate(bands):
        progress(20 + 25 * n, "finding " + name + " hits")
        sos = scipy.signal.butter(4, cutoff, btype=kind, fs=sr, output="sos")
        times, levels = onsets_of(scipy.signal.sosfiltfilt(sos, audio), sr)
        notes += [(t, t + 0.1, note, hit_velocity(v)) for t, v in zip(times, levels)]
    return sorted(notes), 0, True


def dominant_frequency(audio, sr, t, low, high):
    a = int((t + 0.01) * sr)
    seg = audio[a : a + int(0.15 * sr)]
    if len(seg) < 256:
        return 0.0
    spectrum = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n=8192))
    freqs = np.fft.rfftfreq(8192, 1 / sr)
    band = (freqs >= low) & (freqs <= high)
    return float(freqs[band][np.argmax(spectrum[band])])


def tom_mapping(freqs):
    found = sorted(set(round(12 * np.log2(f / 440.0)) for f in freqs if f > 0))  # semitone clusters
    groups = []
    for semitone in found:
        if groups and semitone - groups[-1][-1] <= 2:
            groups[-1].append(semitone)
        else:
            groups.append([semitone])
    groups = groups[: len(TOM_NOTES)]
    # spread the clusters over the GM toms, lowest to highest
    picks = [TOM_NOTES[int(round(i * (len(TOM_NOTES) - 1) / max(1, len(groups) - 1)))] for i in range(len(groups))]
    if len(groups) == 1:
        picks = [45]

    def mapping(f):
        if f <= 0 or not groups:
            return 45
        s = round(12 * np.log2(f / 440.0))
        best = min(range(len(groups)), key=lambda i: min(abs(s - g) for g in groups[i]))
        return picks[best]

    return mapping


def ring_time(audio, sr, t, window=0.5):
    a = int(t * sr)
    seg = np.abs(audio[a : a + max(1, int(window * sr))])
    if len(seg) == 0 or seg.max() <= 0:
        return 0.0
    above = np.where(seg > 0.1 * seg.max())[0]
    return float(above[-1]) / sr


# ---------------------------------------------------------------------------------------------------------------------
# MIDI file writing

PPQ = 480


def var_len(value):
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def track_chunk(events):
    """events: [(tick, bytes)] in any order; note offs sort before note ons at the same tick."""
    events = sorted(events, key=lambda e: (e[0], e[1][0] & 0xF0 == 0x90))
    data = bytearray()
    last = 0
    for tick, message in events:
        data += var_len(tick - last) + message
        last = tick
    data += var_len(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + bytes(data)


def write_midi(path, notes, program, drums, tempo, name):
    to_tick = lambda seconds: max(0, int(round(seconds * tempo / 60.0 * PPQ)))
    channel = 9 if drums else 0
    microseconds = int(round(60_000_000 / tempo))
    meta = [
        (0, b"\xff\x51\x03" + microseconds.to_bytes(3, "big")),
        (0, b"\xff\x58\x04\x04\x02\x18\x08"),  # 4/4
    ]
    title = name.encode("utf-8")[:120]
    events = [(0, b"\xff\x03" + var_len(len(title)) + title)]
    if not drums:
        events.append((0, bytes([0xC0 | channel, program])))
    for start, end, pitch, vel in notes:
        on, off = to_tick(start), to_tick(end)
        if off <= on:
            off = on + 1
        events.append((on, bytes([0x90 | channel, pitch, vel])))
        events.append((off, bytes([0x80 | channel, pitch, 0])))
    header = b"MThd" + struct.pack(">IHHH", 6, 1, 2, PPQ)
    with open(path, "wb") as f:
        f.write(header + track_chunk(meta) + track_chunk(events))


# ---------------------------------------------------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--role", default="mix", choices=sorted(set(PITCHED) | set(DRUM_NOTES) | {"toms", "drums"}))
    parser.add_argument("--tempo", type=float, default=120.0, help="tempo written to the MIDI file")
    parser.add_argument("--name", default="")
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    try:
        if args.role == "drums":
            notes, program, drums = transcribe_drum_kit(args.input)
        elif args.role in DRUM_NOTES or args.role == "toms":
            notes, program, drums = transcribe_drum_part(args.input, args.role)
        else:
            notes, program, drums = transcribe_pitched(args.input, args.role)
        write_midi(args.output, notes, program, drums, args.tempo, args.name or args.role)
        progress(100, "done")
        print("MIDI " + os.path.abspath(args.output) + "\t" + str(len(notes)), flush=True)
        print("DONE", flush=True)
        return 0
    except Exception as e:
        traceback.print_exc()
        print("ERROR", str(e) or e.__class__.__name__, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
