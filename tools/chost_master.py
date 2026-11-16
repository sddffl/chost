"""chost AI mastering.

chost runs this script with the Python of its AI environment and reads stdout line by line:

    PROGRESS <0-100> <message>
    BEFORE <integrated LUFS>\t<true peak dBTP>
    AFTER <integrated LUFS>\t<true peak dBTP>\t<most limiting, dB>\t<most compression, dB>
    EQ <Hz>:<dB>,<Hz>:<dB>,...          the tone changes it made
    CLARITY <average mud cut dB>\t<most mud cut dB>\t<air dB>
    WIDTH <side to mid before, dB>\t<after, dB>
    ERROR <message>
    DONE <output file>

    python chost_master.py --input mix.wav --output master.wav [--reference song.wav]
                           [--style natural|warm|bright|punchy] [--target -14 | --target reference]
                           [--ceiling -1] [--no-mono-bass] [--demud 2.5] [--air 2] [--width 1.3]

The chain, as a mastering engineer would set it up:
  1. tone: a linear phase EQ that nudges the mix's long-term spectrum toward a reference song's, or
     without one toward a balanced tilt, then the style's flavour on top; gentle, at most a few dB
  2. clarity: a dynamic cut of up to --demud dB between 250 and 400 Hz, only while that band stands out
     of the music around it (the boxy, muffled "mud"), and an air shelf of --air dB above 10 kHz
  3. width: the side signal (what differs between left and right) above 200 Hz up by --width, kept
     below the mid so the mix still holds up in mono
  4. bass in mono below 120 Hz, so the low end stays solid on club systems and phones
  5. glue: a slow stereo-linked compressor taking a dB or two off the loudest moments
  6. loudness: gain to the target integrated loudness (ITU-R BS.1770 / EBU R128, the measure Spotify,
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
    if style == "natural":
        # clean lower midrange mud, open vocal presence, smooth air
        return bell(f, 320.0, -1.0, 0.7) + bell(f, 3500.0, 0.8, 1.0) + shelf(f, 9000.0, 1.5, True)
    if style == "warm":
        return shelf(f, 150.0, 1.5, False) + shelf(f, 7000.0, -1.5, True)
    if style == "bright":
        return shelf(f, 8000.0, 2.5, True) + bell(f, 300.0, -1.2, 0.7) + bell(f, 3000.0, 1.0, 1.0)
    if style == "punchy":
        return bell(f, 70.0, 1.2, 0.6) + bell(f, 350.0, -1.2, 0.7) + bell(f, 4000.0, 1.0, 1.0)
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
    diff = np.where(BANDS > 14000.0, np.minimum(diff, 3.5), diff)
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
# 2. clarity: the mud dynamically out, air in


def zero_phase(x, sr, kind, freq, order=2):
    """x through a Butterworth filter run forwards and backwards: no phase shift, so it can be added to or
    taken from x (a peak or a shelf without the comb a phase shift would make)."""
    from scipy.signal import butter, sosfiltfilt

    return sosfiltfilt(butter(order, freq, kind, fs=sr, output="sos"), x, axis=0)


MUD = (250.0, 400.0)
MUD_AROUND = (120.0, 2000.0)
_balanced = {}


def mud_share(x, sr):
    """How much of the 120 Hz - 2 kHz power the 250-400 Hz band holds, in dB, per 10 ms step."""
    mud = zero_phase(x, sr, "bandpass", MUD)
    around = zero_phase(x, sr, "bandpass", MUD_AROUND)
    step = max(1, int(sr * 0.01))
    n = len(x) // step
    def power(y):
        p = (y[: n * step] ** 2).reshape(n, step, -1).mean(axis=(1, 2))
        # a 100 ms average: the cut follows the music's phrases, not each cycle of a bass note
        return np.convolve(p, np.ones(10) / 10.0, mode="same")
    pm, pa = power(mud), power(around)
    with np.errstate(divide="ignore", invalid="ignore"):
        share = 10.0 * np.log10(np.maximum(pm, 1e-20) / np.maximum(pa, 1e-20))
    return share, mud, step, pa


def balanced_share(sr):
    """The share of a balanced spectrum (pink noise, as much power in every octave): the reference."""
    if sr not in _balanced:
        rng = np.random.default_rng(1)
        n = int(sr * 4)
        spectrum = np.fft.rfft(rng.standard_normal(n))
        f = np.fft.rfftfreq(n, 1.0 / sr)
        spectrum[1:] /= np.sqrt(f[1:])
        spectrum[0] = 0.0
        pink = np.fft.irfft(spectrum, n)[:, None]
        share, _, _, _ = mud_share(pink, sr)
        _balanced[sr] = float(np.median(share[20:-20]))
    return _balanced[sr]


def demud(x, sr, depth_db=3.0, margin_db=0.2):
    """Takes up to depth_db out of 250-400 Hz while that band stands out of its neighbourhood by more than a
    balanced mix's share (plus margin_db); a lean passage is left alone. Returns (signal, average cut dB
    over the music, most cut dB)."""
    if depth_db <= 0.0 or len(x) < sr // 5:
        return x, 0.0, 0.0
    share, mud, step, around = mud_share(x, sr)
    excess = share - balanced_share(sr) - margin_db
    cut = np.clip(excess, 0.0, depth_db) # a dB of cut for every dB of excess, up to the depth
    cut[around < 1e-10] = 0.0 # silence: nothing to judge
    # the gain glides: 20 ms into a cut, 200 ms back out of it
    smooth = np.empty_like(cut)
    level = 0.0
    attack, release = np.exp(-1.0 / 2.0), np.exp(-1.0 / 20.0)
    for i, c in enumerate(cut):
        k = attack if c > level else release
        level = k * level + (1.0 - k) * c
        smooth[i] = level
    centres = (np.arange(len(smooth)) + 0.5) * step
    gain = 10.0 ** (-np.interp(np.arange(len(x)), centres, smooth) / 20.0)
    music = around > 1e-10
    return x + (gain - 1.0)[:, None] * mud, float(smooth[music].mean()) if music.any() else 0.0, float(smooth.max())


def air(x, sr, gain_db=2.0, corner=8500.0):
    """A high shelf for air: most of gain_db at 10 kHz, all of it from 12 kHz up, nothing below 5 kHz."""
    if gain_db == 0.0 or corner >= sr * 0.45:
        return x
    return x + (10.0 ** (gain_db / 20.0) - 1.0) * zero_phase(x, sr, "highpass", corner, order=4)


# ---------------------------------------------------------------------------------------------------------------------
# 3. width


def side_to_mid(x):
    """The side signal's level against the mid's, in dB (-inf for mono)."""
    mid = (x[:, 0] + x[:, 1]) / 2.0
    side = (x[:, 0] - x[:, 1]) / 2.0
    pm, ps = float(np.mean(mid ** 2)), float(np.mean(side ** 2))
    return 10.0 * np.log10(max(ps, 1e-20) / max(pm, 1e-20))


def widen(x, sr, amount=1.3, most=-3.0, corner=200.0):
    """Mid/side widening: the side above corner Hz times amount (the bass stays as it is, mono_bass then
    centres it), but never past `most` dB under the mid, so nothing gets phasey or falls apart in mono."""
    if x.shape[1] != 2 or amount == 1.0:
        return x
    mid = (x[:, 0] + x[:, 1]) / 2.0
    side = (x[:, 0] - x[:, 1]) / 2.0
    high = zero_phase(side, sr, "highpass", corner)
    pm = float(np.mean(mid ** 2))
    # the most the side may grow by: the whole side at `most` dB under the mid
    allowed = np.sqrt(pm * 10.0 ** (most / 10.0) / max(float(np.mean(side ** 2)), 1e-20))
    amount = float(np.clip(amount, 0.0, max(1.0, allowed))) if amount > 1.0 else amount
    side = side + (amount - 1.0) * high
    return np.stack([mid + side, mid - side], axis=1)


# ---------------------------------------------------------------------------------------------------------------------
# 4. bass in mono, 5. glue


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
# 6. loudness


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


def master(x, sr, reference, style, target, ceiling, bass_mono, demud_db=2.5, air_db=2.0, width=1.3):
    curve = eq_curve(x, sr, reference, style)
    progress(25, "tone")
    y = apply_eq(x, sr, curve)
    progress(30, "clarity")
    y, mud_avg, mud_most = demud(y, sr, demud_db)
    y = air(y, sr, air_db)
    before_width = side_to_mid(y)
    y = widen(y, sr, width)
    print("CLARITY %.1f\t%.1f\t%.1f" % (mud_avg, mud_most, air_db), flush=True)
    print("WIDTH %.1f\t%.1f" % (before_width, side_to_mid(y)), flush=True)
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
    parser.add_argument("--demud", type=float, default=2.5, help="most dB the dynamic 250-400 Hz cut takes, 0: off")
    parser.add_argument("--air", type=float, default=2.0, help="dB of the high shelf above 10 kHz, 0: off")
    parser.add_argument("--width", type=float, default=1.3, help="side gain above 200 Hz, 1: as it is")
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

        out, limited, glue_db, curve = master(x, sr, reference, args.style, target, args.ceiling, not args.no_mono_bass,
                                              max(0.0, args.demud), args.air, max(0.0, args.width))
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
