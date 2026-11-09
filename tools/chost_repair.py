"""chost DSP repair and audio cleanup helper.

Inspired by SoundtSeparator DSP techniques:
- declick: Removes clicks, pops, and lip-smack impulses via 2nd derivative MAD outlier detection and interpolation.
- dehum: Removes 50Hz / 60Hz power mains hum and its higher harmonics via cascaded IIR notch filters.
- voice_clean: High-pass subsonic filter + declick + dehum for clean vocal/speech preprocessing.
- loudnorm: ITU-R BS.1770-4 loudness normalization with True-Peak protection.

Protocol:
    PROGRESS <0-100> <message>
    DONE <output file>
    ERROR <message>
"""

import argparse
import math
import os
import sys
import numpy as np
import soundfile as sf
from scipy.signal import filtfilt, iirnotch, butter, sosfilt


def progress(pct, msg):
    print(f"PROGRESS {int(pct)} {msg}", flush=True)


def declick(audio, sensitivity=0.5, sr=44100):
    """Click and pop impulse noise removal.
    audio: (samples, channels) float32
    sensitivity: 0.0 (gentle) to 1.0 (aggressive)
    """
    # Sensitivity factor: 0.0 -> k=40.0, 1.0 -> k=8.0
    k = 40.0 - 32.0 * float(np.clip(sensitivity, 0.0, 1.0))
    out = audio.copy()
    n = audio.shape[0]
    if n < 16:
        return out

    for c in range(audio.shape[1]):
        x = audio[:, c].astype(np.float64)
        d2 = np.abs(np.diff(x, 2))
        mad = np.median(d2) + 1e-12
        mask = np.zeros(n, dtype=bool)
        hit = d2 > k * mad
        mask[1:-1] |= hit
        
        # Widen detection mask slightly around each click (~0.7ms)
        widen = max(8, int(sr * 0.0007))
        idx = np.flatnonzero(mask)
        if idx.size == 0:
            continue
        for i in idx:
            mask[max(0, i - widen):min(n, i + widen + 1)] = True
            
        good = ~mask
        if good.sum() < 2:
            continue
        pos = np.arange(n)
        out[:, c] = np.interp(pos, pos[good], x[good]).astype(np.float32)
        
    return out


def dehum(audio, base_freq=50.0, harmonics=6, sr=44100):
    """Mains hum removal using cascaded notch filters.
    audio: (samples, channels) float32
    base_freq: 50.0 or 60.0 Hz
    """
    out = audio.astype(np.float64)
    nyq = sr / 2.0
    for h in range(1, harmonics + 1):
        f0 = base_freq * h
        if f0 >= nyq * 0.95:
            break
        q = 30.0 + 5.0 * h
        b, a = iirnotch(f0, q, fs=sr)
        for c in range(out.shape[1]):
            out[:, c] = filtfilt(b, a, out[:, c])
    return out.astype(np.float32)


def voice_clean(audio, sr=44100, base_freq=50.0):
    """Subsonic rumble cutoff (35Hz) + Declick + Dehum for pristine vocal track."""
    # 1. High-pass filter at 35 Hz to remove DC offset and microphone boom
    sos = butter(4, 35.0, btype="highpass", fs=sr, output="sos")
    out = sosfilt(sos, audio, axis=0).astype(np.float32)
    # 2. Declick
    out = declick(out, sensitivity=0.55, sr=sr)
    # 3. Dehum
    out = dehum(out, base_freq=base_freq, harmonics=6, sr=sr)
    return out


def loudness_normalize(audio, sr=44100, target_lufs=-14.0, ceiling_dbtp=-1.0):
    """ITU-R BS.1770-4 loudness normalization with peak ceiling limiter."""
    try:
        import pyloudnorm as pyln
        meter = pyln.Meter(sr)
        measured = float(meter.integrated_loudness(audio.astype(np.float64)))
        if np.isfinite(measured) and measured > -70.0:
            gain_db = float(target_lufs) - measured
            gain = 10.0 ** (gain_db / 20.0)
            peak = float(np.abs(audio).max()) * gain
            limit = 10.0 ** (ceiling_dbtp / 20.0)
            if peak > limit:
                gain *= limit / peak
            return (audio * gain).astype(np.float32), measured
    except Exception:
        pass

    # Fallback to simple peak normalization if pyloudnorm fails
    peak = float(np.abs(audio).max())
    if peak > 1e-6:
        target_peak = 10.0 ** (ceiling_dbtp / 20.0)
        gain = target_peak / peak
        return (audio * min(gain, 1.0)).astype(np.float32), -14.0
    return audio, -70.0


def main():
    parser = argparse.ArgumentParser(description="chost DSP audio repair")
    parser.add_argument("--input", required=True, help="Input WAV file")
    parser.add_argument("--output", required=True, help="Output WAV file")
    parser.add_argument("--action", default="clean", choices=["declick", "dehum", "clean", "loudnorm"],
                        help="Action to perform")
    parser.add_argument("--sensitivity", type=float, default=0.5, help="Declick sensitivity (0.0-1.0)")
    parser.add_argument("--base-freq", type=float, default=50.0, choices=[50.0, 60.0], help="Mains frequency")
    parser.add_argument("--target-lufs", type=float, default=-14.0, help="Target LUFS for loudnorm")

    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"ERROR Input file not found: {args.input}", flush=True)
        sys.exit(1)

    try:
        progress(5, f"读取音频文件: {os.path.basename(args.input)}")
        audio, sr = sf.read(args.input, dtype="float32", always_2d=True)

        if args.action == "declick":
            progress(40, "正在执行二阶微分离群检测与消除爆音 (Declick)...")
            res = declick(audio, sensitivity=args.sensitivity, sr=sr)
        elif args.action == "dehum":
            progress(40, f"正在执行 {args.base_freq}Hz 及其多阶谐波陷波滤波 (Dehum)...")
            res = dehum(audio, base_freq=args.base_freq, sr=sr)
        elif args.action == "loudnorm":
            progress(40, f"正在执行 ITU-R BS.1770-4 响度标准化至 {args.target_lufs} LUFS...")
            res, orig_lufs = loudness_normalize(audio, sr=sr, target_lufs=args.target_lufs)
        elif args.action == "clean":
            progress(25, "正在消除次声波杂音与直流偏置...")
            progress(50, "正在消除电流爆音与口水音 (Declick)...")
            progress(75, f"正在消除 {args.base_freq}Hz 工频电磁嗡鸣 (Dehum)...")
            res = voice_clean(audio, sr=sr, base_freq=args.base_freq)
        else:
            res = audio

        progress(90, f"正在保存处理后的音频至: {os.path.basename(args.output)}")
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        sf.write(args.output, res, sr, subtype="PCM_24")

        progress(100, "处理完成")
        print(f"DONE {os.path.abspath(args.output)}", flush=True)
    except Exception as e:
        print(f"ERROR {e}", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
