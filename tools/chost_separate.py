"""chost stem separation helper.

chost runs this script with a Python that has audio-separator installed, reads its stdout line by line
and turns every stem into a new track. The protocol, one line each:

    PROGRESS <0-100> <message>
    STEM <stem id>\t<label>\t<path>
    ERROR <message>
    DONE

Levels:
    basic   4 stems: vocals, drums, bass, other (Demucs htdemucs_ft)
    fine    6 stems: adds guitar and piano (Demucs htdemucs_6s)
    finest  fine, then the drums split into kick/snare/toms/hi-hat/ride/crash (MDX23C DrumSep)
            and the vocals into lead and backing vocals (Mel-Roformer karaoke)

    python chost_separate.py --check
    python chost_separate.py --input song.wav --out stems/ --level finest [--model-dir models/]
    python chost_separate.py --input song.wav --out stems/ --level finest --selftest
"""

import argparse
import os
import re
import shutil
import sys
import traceback

LABELS = {
    "vocals": "人声",
    "drums": "鼓",
    "bass": "贝斯",
    "guitar": "吉他",
    "piano": "钢琴",
    "other": "其他",
    "kick": "底鼓",
    "snare": "军鼓",
    "toms": "通鼓",
    "hh": "踩镲",
    "ride": "叮叮镲",
    "crash": "吊镲",
    "lead": "主唱",
    "backing": "和声",
}

DEMUCS_4 = "htdemucs_ft.yaml"
DEMUCS_6 = "htdemucs_6s.yaml"
DRUMSEP = "MDX23C-DrumSep-aufr33-jarredou.ckpt"
KARAOKE = "mel_band_roformer_karaoke_aufr33_viperx_sdr_10.1956.ckpt"


def say(*parts):
    print(*parts, flush=True)


def progress(pct, message):
    say("PROGRESS", int(pct), message)


def stem(stem_id, path):
    say("STEM " + stem_id + "\t" + LABELS.get(stem_id, stem_id) + "\t" + os.path.abspath(path))


def stem_name(path):
    # audio-separator names its files "<input>_(<Stem>)_<model>.wav"
    match = re.search(r"_\(([^)]+)\)", os.path.basename(path))
    return match.group(1).lower() if match else os.path.splitext(os.path.basename(path))[0].lower()


def device_name():
    try:
        import torch

        if torch.cuda.is_available():
            return "cuda:" + torch.cuda.get_device_name(0)
        if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
            return "mps"
    except Exception:
        pass
    return "cpu"


def check():
    try:
        from importlib.metadata import version

        import audio_separator  # noqa: F401

        say("OK", version("audio-separator"), device_name())
        return 0
    except Exception as e:
        say("ERROR", "audio-separator is not installed: " + str(e))
        return 1


class Runner:
    def __init__(self, out_dir, model_dir, base):
        from audio_separator.separator import Separator

        self.out_dir = out_dir
        self.base = base
        self.separator = Separator(output_dir=out_dir, model_file_dir=model_dir, output_format="WAV")
        self.loaded = None

    def run(self, model, path):
        """Separates path with model, returns {stem name: file path}."""
        if self.loaded != model:
            self.separator.load_model(model_filename=model)
            self.loaded = model
        files = self.separator.separate(path)
        result = {}
        for f in files:
            full = f if os.path.isabs(f) else os.path.join(self.out_dir, f)
            result[stem_name(full)] = full
        return result

    def keep(self, stem_id, path):
        """Moves a stem to a plain ASCII name and reports it."""
        target = os.path.join(self.out_dir, self.base + "_" + stem_id + ".wav")
        if os.path.abspath(path) != os.path.abspath(target):
            if os.path.exists(target):
                os.remove(target)
            shutil.move(path, target)
        stem(stem_id, target)
        return target


def separate(args):
    os.makedirs(args.out, exist_ok=True)
    base = re.sub(r"[^A-Za-z0-9_-]+", "_", os.path.splitext(os.path.basename(args.input))[0]).strip("_") or "song"
    model_dir = args.model_dir or os.path.join(os.path.expanduser("~"), ".chost", "models")
    os.makedirs(model_dir, exist_ok=True)

    progress(2, "loading the separation model (the first run downloads it)")
    runner = Runner(args.out, model_dir, base)
    progress(5, "separating instruments")
    main = runner.run(DEMUCS_4 if args.level == "basic" else DEMUCS_6, args.input)

    if args.level != "finest":
        for name, path in main.items():
            runner.keep(name, path)
        progress(100, "done")
        return

    progress(50, "splitting the drums into kick, snare, toms and cymbals")
    for name, path in main.items():
        if name not in ("drums", "vocals"):
            runner.keep(name, path)
    drums = runner.keep("drums", main["drums"]) if "drums" in main else None
    vocals = runner.keep("vocals", main["vocals"]) if "vocals" in main else None

    if drums:
        for name, path in runner.run(DRUMSEP, drums).items():
            runner.keep(name, path)
    progress(80, "splitting lead and backing vocals")
    if vocals:
        for name, path in runner.run(KARAOKE, vocals).items():
            runner.keep("lead" if name == "vocals" else "backing", path)
    progress(100, "done")


def selftest(args):
    """Writes fake stems to test chost without the models: scaled copies of the input, or the files
    <id>.wav from the folder in CHOST_SELFTEST_STEMS when that is set."""
    import time
    import wave

    os.makedirs(args.out, exist_ok=True)
    names = ["vocals", "drums", "bass", "other"]
    if args.level != "basic":
        names[3:3] = ["guitar", "piano"]
    if args.level == "finest":
        names += ["kick", "snare", "toms", "hh", "ride", "crash", "lead", "backing"]
    with wave.open(args.input, "rb") as src:
        params = src.getparams()
        frames = src.readframes(params.nframes)
    if params.sampwidth != 2:
        raise RuntimeError("selftest needs a 16-bit wav")
    import array

    samples = array.array("h", frames)
    prepared = os.environ.get("CHOST_SELFTEST_STEMS")
    for i, name in enumerate(names):
        progress(100 * i / len(names), "fake " + name)
        if prepared and os.path.exists(os.path.join(prepared, name + ".wav")):
            path = os.path.join(args.out, "selftest_" + name + ".wav")
            shutil.copy(os.path.join(prepared, name + ".wav"), path)
            stem(name, path)
            time.sleep(0.05)
            continue
        gain = 1.0 / (i + 1)
        out = array.array("h", (int(s * gain) for s in samples))
        path = os.path.join(args.out, "selftest_" + name + ".wav")
        with wave.open(path, "wb") as dst:
            dst.setparams(params)
            dst.writeframes(out.tobytes())
        stem(name, path)
        time.sleep(0.05)
    progress(100, "done")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--input")
    parser.add_argument("--out")
    parser.add_argument("--level", choices=["basic", "fine", "finest"], default="finest")
    parser.add_argument("--model-dir")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    # stems and labels are UTF-8 whatever the console's code page is
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    if args.check:
        if args.selftest:
            say("OK selftest cpu")
            return 0
        return check()
    if not args.input or not args.out:
        parser.error("--input and --out are required")
    try:
        (selftest if args.selftest else separate)(args)
        say("DONE")
        return 0
    except Exception as e:
        traceback.print_exc()
        say("ERROR", str(e) or e.__class__.__name__)
        return 1


if __name__ == "__main__":
    sys.exit(main())
