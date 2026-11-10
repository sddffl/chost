"""chost AI mastering.

chost runs this script with the Python of its AI environment and reads stdout line by line:

    PROGRESS <0-100> <message>
    BEFORE <integrated LUFS>\t<true peak dBTP>
    AFTER <integrated LUFS>\t<true peak dBTP>\t<most limiting, dB>\t<most compression, dB>
    EQ <Hz>:<dB>,<Hz>:<dB>,...          the tone changes it made
    ERROR <message>
    DONE <output file>

    python chost_master.py --input mix.wav --output master.wav [--reference song.wav]
                           [--style natural|warm|bright|punchy] [--target -14 | --target reference]
                           [--ceiling -1] [--no-mono-bass]

The chain, as a mastering engineer would set it up:
  1. tone: a linear phase EQ that nudges the mix's long-term spectrum toward a reference song's, or
     without one toward a balanced tilt, then the style's flavour on top; gentle, at most a few dB
  2. bass in mono below 120 Hz, so the low end stays solid on club systems and phones
  3. glue: a slow stereo-linked compressor taking a dB or two off the loudest moments
  4. loudness: gain to the target integrated loudness (ITU-R BS.1770 / EBU R128, the measure Spotify,
     YouTube and Apple Music normalise with) into a true-peak limiter (4x oversampled) at the ceiling
Writes a 24-bit WAV at the input's sample rate. Requires numpy, scipy and soundfile.
"""

import argparse
import sys
import traceback

import numpy as np

STYLES = ("natural", "warm", "bright", "punchy")


def progress(pct, message):
    print("PROGRESS", int(pct), message, flush=True)


# ---------------------------------------------------------------------------------------------------------------------
# measuring


def k_weighting(sr):
    """The two BS.1770 pre-filters (high shelf, then high pass) as second-order sections for any rate."""
    sos = []
    # high shelf: +4 dB above about 1.7 kHz, the head's effect on what reaches the ear
    g, q, fc = 3.99984385397, 0.7071752369554193, 1681.9744509555319
    a = 10.0 ** (g / 40.0)
    w0 = 2.0 * np.pi * fc / sr
    alpha = np.sin(w0) / (2.0 * q)
    c = np.cos(w0)
    b = [a * ((a + 1) + (a - 1) * c + 2 * np.sqrt(a) * alpha), -2 * a * ((a - 1) + (a + 1) * c),
         a * ((a + 1) + (a - 1) * c - 2 * np.sqrt(a) * alpha)]
    den = [(a + 1) - (a - 1) * c + 2 * np.sqrt(a) * alpha, 2 * ((a - 1) - (a + 1) * c),
           (a + 1) - (a - 1) * c - 2 * np.sqrt(a) * alpha]
    sos.append([x / den[0] for x in b] + [x / den[0] for x in den])
    # high pass at 38 Hz
    q, fc = 0.5003270373253953, 38.13547087613982
    w0 = 2.0 * np.pi * fc / sr
    alpha = np.sin(w0) / (2.0 * q)
    c = np.cos(w0)
    b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    den = [1 + alpha, -2 * c, 1 - alpha]
    sos.append([x / den[0] for x in b] + [x / den[0] for x in den])
    return np.array(sos)


def loudness(x, sr):
    """Integrated loudness in LUFS of a (samples, channels) signal, -70 or below for silence."""
    from scipy.signal import sosfilt

    y = sosfilt(k_weighting(sr), x, axis=0)
    block, step = int(0.4 * sr), int(0.1 * sr)
    if len(y) < block:
        block = step = len(y)
    squares = np.concatenate([np.zeros((1, y.shape[1])), np.cumsum(y.astype(np.float64) ** 2, axis=0)])
    starts = np.arange(0, len(y) - block + 1, step)
    z = (squares[starts + block] - squares[starts]) / block # mean square per block and channel
    power = z.sum(axis=1)
    with np.errstate(divide="ignore"):
        levels = -0.691 + 10.0 * np.log10(power)
    gated = power[levels > -70.0]
    if len(gated) == 0:
        return -70.0
    relative = -0.691 + 10.0 * np.log10(gated.mean()) - 10.0
    gated = power[(levels > -70.0) & (levels > relative)]
    return float(-0.691 + 10.0 * np.log10(gated.mean()))


def peak_envelope(x, oversample=4):
    """The true (inter-sample) peak at every sample, the largest of all channels, by 4x oversampling."""
    from scipy.signal import resample_poly

    chunk, pad = 1 << 18, 64
    out = np.empty(len(x))
    for begin in range(0, len(x), chunk):
        end = min(len(x), begin + chunk)
        lo, hi = max(0, begin - pad), min(len(x), end + pad)
        up = np.abs(resample_poly(x[lo:hi], oversample, 1, axis=0)).max(axis=1)
        up = up[(begin - lo) * oversample : (begin - lo + end - begin) * oversample]
        out[begin:end] = np.maximum(up.reshape(-1, oversample).max(axis=1), np.abs(x[begin:end]).max(axis=1))
    return out


def db(v):
    return 20.0 * np.log10(max(float(v), 1e-9))


# ---------------------------------------------------------------------------------------------------------------------
# 1. tone


BANDS = 25.0 * 2.0 ** (np.arange(0, 30) / 3.0) # third octaves from 25 Hz to 20 kHz


def band_levels(x, sr):
    """The long-term spectrum in dB at the third-octave centres, each averaged over an octave."""
    from scipy.signal import welch

    mid = x.mean(axis=1)
    f, p = welch(mid, sr, nperseg=16384)
    levels = []
    for fc in BANDS:
        sel = (f >= fc / np.sqrt(2.0)) & (f < fc * np.sqrt(2.0))
        levels.append(10.0 * np.log10(p[sel].mean() + 1e-20) if sel.any() else -200.0)
    return np.array(levels)


def shelf(f, fc, gain, high):
    r = (fc / f) ** 2 if high else (f / fc) ** 2
    return gain / (1.0 + r)


def bell(f, fc, gain, octaves):
    return gain * np.exp(-0.5 * (np.log2(f / fc) / octaves) ** 2)


def style_curve(style, f):
    if style == "warm":
        return shelf(f, 150.0, 1.5, False) + shelf(f, 7000.0, -1.5, True)
    if style == "bright":
        return shelf(f, 8000.0, 2.0, True) + bell(f, 300.0, -1.0, 0.7) + bell(f, 3000.0, 0.5, 1.0)
    if style == "punchy":
        return bell(f, 70.0, 1.0, 0.6) + bell(f, 350.0, -1.0, 0.7) + bell(f, 4000.0, 1.0, 1.0)
    return np.zeros_like(f)


def smooth(v, passes=2):
    for _ in range(passes):
        v = np.convolve(np.pad(v, 1, mode="edge"), [0.25, 0.5, 0.25], mode="valid")
    return v


def eq_curve(x, sr, reference, style):
    """The tone change in dB at BANDS."""
    cur = band_levels(x, sr)
    mids = (BANDS >= 100.0) & (BANDS <= 8000.0)
    octave = np.log2(BANDS / 1000.0)
    if reference is not None:
        ref = band_levels(reference, sr)
        diff = (ref - ref[mids].mean()) - (cur - cur[mids].mean())
        diff = np.clip(smooth(diff) * 0.7, -6.0, 6.0)
    else:
        # a balanced master falls by about 4.5 dB per octave: move the mix's tilt halfway there and even
        # out the wider bumps a little, leaving its character alone
        fit = (BANDS >= 80.0) & (BANDS <= 12000.0)
        slope, offset = np.polyfit(octave[fit], cur[fit], 1)
        tilt = np.clip((-4.5 - slope) * 0.5, -1.0, 1.0) * octave
        residual = np.where(fit, (offset + slope * octave) - cur, 0.0)
        diff = np.clip(tilt + smooth(residual) * 0.35, -3.0, 3.0)
    diff = diff + style_curve(style, BANDS)
    diff = np.where(BANDS < 35.0, np.minimum(diff, 0.0), diff) # never boost the rumble
    diff = np.where(BANDS > 14000.0, np.minimum(diff, 2.0), diff)
    most = 6.0 if reference is not None else 4.0
    return np.clip(diff - diff[mids].mean(), -most, most) # tone only: the loudness stage sets the level


def apply_eq(x, sr, curve):
    from scipy.signal import firwin2, oaconvolve

    nyquist = sr / 2.0
    keep = BANDS < nyquist * 0.95
    freqs = np.concatenate([[0.0], BANDS[keep], [nyquist]])
    gains_db = np.concatenate([[curve[0]], curve[keep], [curve[keep][-1]]])
    taps = 4097 if sr <= 50000 else 8193
    h = firwin2(taps, freqs, 10.0 ** (gains_db / 20.0), fs=sr)
    delay = (taps - 1) // 2
    return oaconvolve(x, h[:, None], mode="full", axes=0)[delay : delay + len(x)]


# ---------------------------------------------------------------------------------------------------------------------
# 2. bass in mono, 3. glue


def mono_bass(x, sr, cutoff=120.0):
    from scipy.signal import butter, sosfiltfilt

    if x.shape[1] != 2:
        return x
    mid = (x[:, 0] + x[:, 1]) / 2.0
    side = (x[:, 0] - x[:, 1]) / 2.0
    side = sosfiltfilt(butter(2, cutoff, "highpass", fs=sr, output="sos"), side)
    return np.stack([mid + side, mid - side], axis=1)


def glue(x, sr, style):
    """A slow, stereo-linked RMS compressor working in 1 ms steps; returns (signal, most reduction dB)."""
    step = max(1, int(sr / 1000))
    n = len(x) // step
    if n < 10:
        return x, 0.0
    power = (x[: n * step] ** 2).mean(axis=1).reshape(n, step).mean(axis=1)
    attack, release = np.exp(-1.0 / 20.0), np.exp(-1.0 / 200.0) # per 1 ms step
    env = np.empty(n)
    level = power[0]
    for i, p in enumerate(power):
        level = (attack if p > level else release) * level + (1.0 - (attack if p > level else release)) * p
        env[i] = level
    env_db = 10.0 * np.log10(env + 1e-12)
    loud = env_db[env_db > env_db.max() - 40.0]
    ratio, below, most = {"punchy": (2.5, 4.0, 6.0), "warm": (1.8, 2.0, 4.0)}.get(style, (1.5, 2.0, 4.0))
    threshold = np.percentile(loud, 90) - below
    knee = 6.0
    over = env_db - threshold
    reduction = np.where(over <= -knee / 2, 0.0,
                         np.where(over >= knee / 2, over * (1.0 - 1.0 / ratio),
                                  (1.0 - 1.0 / ratio) * (over + knee / 2) ** 2 / (2 * knee)))
    reduction = np.minimum(reduction, most)
    centres = (np.arange(n) + 0.5) * step
    gain = 10.0 ** (-np.interp(np.arange(len(x)), centres, reduction) / 20.0)
    return x * gain[:, None], float(reduction.max())


# ---------------------------------------------------------------------------------------------------------------------
# 4. loudness


def limit(x, sr, ceiling_db):
    """A look-ahead true-peak limiter; returns (signal, most reduction dB)."""
    from scipy.ndimage import minimum_filter1d, uniform_filter1d

    ceiling = 10.0 ** ((ceiling_db - 0.15) / 20.0) # a little headroom for meters that oversample differently
    y = x
    most = 0.0
    for _ in range(3): # the smoothed gain can leave an inter-sample overshoot, a second pass catches it
        peaks = peak_envelope(y)
        need = np.minimum(1.0, ceiling / np.maximum(peaks, 1e-9))
        if need.min() >= 1.0:
            break
        # the gain may only be lower than needed: hold the deepest reduction around every peak, then
        # smooth over half that window, so the gain glides down before the peak and back up after it
        fast = int(0.0015 * sr) | 1
        slow = int(0.010 * sr) | 1
        g = uniform_filter1d(minimum_filter1d(need, 2 * fast + 1), fast)
        g = np.minimum(g, uniform_filter1d(minimum_filter1d(need, 2 * slow + 1), slow))
        g = np.minimum(g, need)
        most = max(most, -db(g.min()))
        y = y * g[:, None]
    return np.clip(y, -ceiling, ceiling), most


def master(x, sr, reference, style, target, ceiling, bass_mono):
    curve = eq_curve(x, sr, reference, style)
    progress(25, "tone")
    y = apply_eq(x, sr, curve)
    if bass_mono:
        y = mono_bass(y, sr)
    progress(35, "glue")
    y, glue_db = glue(y, sr, style)

    gain_db = target - loudness(y, sr)
    best = None
    for attempt in range(6):
        progress(45 + attempt * 8, "loudness")
        out, limited = limit(y * 10.0 ** (gain_db / 20.0), sr, ceiling)
        reached = loudness(out, sr)
        if best is None or abs(reached - target) < abs(best[1] - target):
            best = (out, reached, limited)
        if abs(reached - target) < 0.1:
            break
        gain_db += target - reached # limiting eats some of the gain: ask for the difference again
        if gain_db > 30.0:
            break
    return best[0], best[2], glue_db, curve


def read(path):
    import soundfile as sf

    x, sr = sf.read(path, dtype="float64", always_2d=True)
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2], sr


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--reference", default="")
    parser.add_argument("--style", default="natural", choices=STYLES)
    parser.add_argument("--target", default="-14", help="integrated loudness in LUFS, or 'reference'")
    parser.add_argument("--ceiling", type=float, default=-1.0, help="true peak ceiling in dBTP")
    parser.add_argument("--no-mono-bass", action="store_true")
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    try:
        import soundfile as sf

        progress(2, "reading the mix")
        x, sr = read(args.input)
        before = loudness(x, sr)
        if before <= -69.0:
            raise RuntimeError("the mix is silent")
        print("BEFORE %.1f\t%.1f" % (before, db(peak_envelope(x).max())), flush=True)

        reference = None
        if args.reference:
            progress(8, "reading the reference")
            reference, rsr = read(args.reference)
            if rsr != sr:
                from math import gcd

                from scipy.signal import resample_poly

                g = gcd(int(sr), int(rsr))
                reference = resample_poly(reference, int(sr) // g, int(rsr) // g, axis=0)
        if args.target == "reference":
            if reference is None:
                raise RuntimeError("--target reference needs --reference")
            target = loudness(reference, sr)
        else:
            target = float(args.target)
        target = float(np.clip(target, -24.0, -6.0))

        out, limited, glue_db, curve = master(x, sr, reference, args.style, target, args.ceiling, not args.no_mono_bass)
        progress(95, "writing")
        sf.write(args.output, out, sr, subtype="PCM_24")
        print("AFTER %.1f\t%.1f\t%.1f\t%.1f" % (loudness(out, sr), db(peak_envelope(out).max()), limited, glue_db), flush=True)
        print("EQ " + ",".join("%d:%.1f" % (f, g) for f, g in zip(BANDS, curve)), flush=True)
        progress(100, "done")
        print("DONE " + args.output, flush=True)
        return 0
    except Exception as e:
        traceback.print_exc()
        print("ERROR", str(e) or e.__class__.__name__, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
