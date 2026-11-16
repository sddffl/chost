"""chost automated AI music remastering pipeline.

This workflow orchestrates end-to-end AI music production:
1. Audio Analysis: Detects BPM, key, scale, and meter
2. Stem Separation: Separates audio into up to 14 stems using GPU
3. Vocal Tuning: Intelligently corrects lead and backing vocals in the detected key
4. Audio to MIDI: Transcribes pitched accompaniment stems into precise MIDI notes
5. Multitrack Mixing & Balancing: Balances stems, sets pan and spatial positions
6. Mastering: Master bus EQ, glue compression, and True Peak limiting (-14 LUFS)
7. Desktop Export: Directly saves "[SongName]_顶级母带.wav" to the user's Desktop.

Usage:
    python chost_workflow_remaster.py --input "path/to/song.wav" [--export-dir "path/to/dir"]
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PYTHON_EXE = sys.executable

STEM_LABELS = {
    "vocals": "人声",
    "lead": "主唱",
    "backing": "和声",
    "bass": "贝斯",
    "drums": "鼓组",
    "kick": "底鼓",
    "snare": "军鼓",
    "toms": "通鼓",
    "hh": "踩镲",
    "ride": "叮叮镲",
    "crash": "吊镲",
    "guitar": "吉他",
    "piano": "钢琴",
    "other": "其他伴奏",
}


def detect_and_set_proxy():
    for port in [10808, 7890, 10809]:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.3)
                if s.connect_ex(("127.0.0.1", port)) == 0:
                    proxy = f"http://127.0.0.1:{port}"
                    if "http_proxy" not in os.environ:
                        os.environ["http_proxy"] = proxy
                        os.environ["HTTP_PROXY"] = proxy
                        os.environ["https_proxy"] = proxy
                        os.environ["HTTPS_PROXY"] = proxy
                    break
        except Exception:
            pass


detect_and_set_proxy()


def log(msg):
    print(msg, flush=True)


def progress(pct, msg):
    print(f"PROGRESS {int(pct)} {msg}", flush=True)


def get_desktop_dir():
    desktop = os.path.join(os.path.expanduser("~"), "Desktop")
    if os.path.exists(desktop):
        return desktop
    onedrive_desktop = os.path.join(os.path.expanduser("~"), "OneDrive", "Desktop")
    if os.path.exists(onedrive_desktop):
        return onedrive_desktop
    return desktop


def run_command(cmd, desc="", span=None):
    """Runs one of the tools; returns (exit code, its lines). What it prints goes on to chost indented, as
    log lines only: its own PROGRESS, MIDI, TUNED... lines mean something else than the workflow's (chost_
    transcribe.py prints "MIDI <file>\t<note count>"), and chost reads only the workflow's. With span
    (lo, hi) the tool's progress becomes the workflow's, between lo and hi."""
    if desc:
        log(f">>> {desc}")
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace"
    )
    lines = []
    for line in iter(proc.stdout.readline, ""):
        line_clean = line.strip()
        if not line_clean:
            continue
        lines.append(line_clean)
        if line_clean.startswith("PROGRESS "):
            if span:
                parts = line_clean.split(" ", 2)
                try:
                    pct = float(parts[1])
                except (IndexError, ValueError):
                    continue
                progress(span[0] + (span[1] - span[0]) * min(max(pct, 0.0), 100.0) / 100.0,
                         f"{desc.rstrip('.。…')}：{parts[2] if len(parts) > 2 else ''}")
            continue
        print("    " + line_clean, flush=True)
    proc.wait()
    return proc.returncode, lines


def analyze_audio(input_file):
    cmd = [PYTHON_EXE, os.path.join(TOOLS_DIR, "chost_analyze.py"), "--input", input_file]
    code, lines = run_command(cmd, "正在分析歌曲速度与调性...")
    bpm = 120.0
    key = "C major"
    for l in lines:
        if l.startswith("TEMPO "):
            try:
                bpm = float(l.split()[1])
            except Exception:
                pass
        elif l.startswith("KEY "):
            parts = l.split(maxsplit=2)
            if len(parts) >= 2:
                key = parts[1]
    print(f"ANALYZE {bpm:.1f}\t{key}", flush=True)
    return bpm, key


def separate_audio(input_file, out_dir, level="finest", model_dir=None):
    """model_dir: where the separation models are (chost passes the folder its stem separation uses);
    without one, AUDIO_SEPARATOR_MODEL_DIR, else chost_separate.py's own default."""
    os.makedirs(out_dir, exist_ok=True)
    model_dir = model_dir or os.environ.get("AUDIO_SEPARATOR_MODEL_DIR")
    cmd = [
        PYTHON_EXE,
        os.path.join(TOOLS_DIR, "chost_separate.py"),
        "--input", input_file,
        "--out", out_dir,
        "--level", level
    ]
    if model_dir:
        cmd.extend(["--model-dir", model_dir])

    code, lines = run_command(cmd, "正在执行 AI 细分分轨（最多 14 轨）...", span=(20, 45))
    stems = {}
    for l in lines:
        if l.startswith("STEM "):
            parts = l.split("\t")
            if len(parts) >= 3:
                stem_id = parts[0].replace("STEM ", "").strip()
                stems[stem_id] = parts[2].strip()
                # the separation's line was passed on indented: say it again as the workflow's own
                print(f"STEM {stem_id}\t{parts[1].strip() or STEM_LABELS.get(stem_id, stem_id)}\t{stems[stem_id]}", flush=True)
    # also scan directory in case
    for f in os.listdir(out_dir):
        if f.endswith(".wav"):
            full = os.path.join(out_dir, f)
            for k in ["bass", "drums", "piano", "guitar", "other", "vocals", "lead", "backing",
                      "kick", "snare", "toms", "hh", "ride", "crash"]:
                if f"_{k}.wav" in f.lower():
                    if k not in stems:
                        stems[k] = full
                        print(f"STEM {k}\t{STEM_LABELS.get(k, k)}\t{full}", flush=True)
    return stems


def share(lo, hi, index, count):
    """The index-th of count equal parts of the progress from lo to hi."""
    width = (hi - lo) / max(count, 1)
    return (lo + width * index, lo + width * (index + 1))


def tune_vocals(stems, key, out_dir):
    tuned_stems = dict(stems)
    roles = [r for r in ["lead", "vocals", "backing"] if r in stems and os.path.exists(stems[r])]
    for n, role in enumerate(roles):
        in_file = stems[role]
        out_file = os.path.join(out_dir, f"tuned_{role}.wav")
        strength = "0.90" if role == "backing" else "0.85"
        speed = "35" # a 35 ms S-curve from one note's correction to the next: no steps
        cmd = [
            PYTHON_EXE,
            os.path.join(TOOLS_DIR, "chost_tune.py"),
            "--input", in_file,
            "--output", out_file,
            "--key", key,
            "--scale", "smart",
            "--strength", strength,
            "--speed", speed
        ]
        code, lines = run_command(cmd, f"正在对 {role}（{'和声' if role == 'backing' else '主唱'}）进行智能修音（调性：{key}）...", span=share(50, 65, n, len(roles)))
        if code == 0 and os.path.exists(out_file) and os.path.getsize(out_file) > 1000:
            tuned_stems[role] = out_file
            print(f"TUNED {role}\t{out_file}", flush=True)
    return tuned_stems


def transcribe_stems(stems, tempo, out_dir):
    midi_files = {}
    roles = [r for r in ["bass", "piano", "guitar"] if r in stems and os.path.exists(stems[r])]
    for n, role in enumerate(roles):
        in_file = stems[role]
        out_mid = os.path.join(out_dir, f"{role}.mid")
        cmd = [
            PYTHON_EXE,
            os.path.join(TOOLS_DIR, "chost_transcribe.py"),
            "--input", in_file,
            "--output", out_mid,
            "--role", role,
            "--tempo", str(tempo)
        ]
        code, lines = run_command(cmd, f"正在将伴奏 {role} 转为高精度 MIDI...", span=share(70, 80, n, len(roles)))
        # code 0: a file left from an earlier run is not this run's result
        if code == 0 and os.path.exists(out_mid) and os.path.getsize(out_mid) > 50:
            midi_files[role] = out_mid
            print(f"MIDI {role}\t{out_mid}", flush=True)
    return midi_files


def load_stereo(path, target_sr=None):
    """A stem as float32 stereo frames, and its rate. Mono is spread to both sides, more than two channels
    keep the first two, and with target_sr another rate is resampled to it, so stems separated or tuned at
    different rates still line up sample for sample when they are summed."""
    import numpy as np
    import soundfile as sf

    data, sr = sf.read(path, always_2d=True, dtype="float32")
    if data.shape[1] == 1:
        data = np.repeat(data, 2, axis=1)
    elif data.shape[1] > 2:
        data = data[:, :2]
    if target_sr and sr != target_sr:
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(int(sr), int(target_sr))
        data = resample_poly(data, int(target_sr) // g, int(sr) // g, axis=0).astype(np.float32)
        sr = target_sr
    return np.ascontiguousarray(data), sr


# Where each part sits, as (left channel's pan, right channel's pan), -1 hard left .. 1 hard right: the
# foundation (kick, snare, bass) and the lead vocal in the centre, the cymbals as the audience sees the kit,
# a guitar and a piano facing each other at L65 / R65 (alone, one spreads over both), the backing vocals
# wide at L80 / R80. Each channel of a stem is panned on its own, so a stereo part keeps its own image
# inside its place.
PLACES = {
    "kick": (0.0, 0.0), "snare": (0.0, 0.0), "bass": (0.0, 0.0),
    "lead": (-0.08, 0.08), "vocals": (-0.08, 0.08),
    "drums": (-1.0, 1.0), "toms": (-0.55, 0.55), "hh": (0.15, 0.45), "ride": (0.35, 0.70), "crash": (-0.70, -0.20),
    "backing": (-0.80, 0.80), "other": (-0.50, 0.50),
}
PAIR = {"guitar": (-0.80, -0.50), "piano": (0.50, 0.80)} # a guitar and a piano together: L65 and R65
ALONE = (-0.65, 0.65)                                     # either one on its own
VOLUMES = {"kick": 1.0, "snare": 1.0, "toms": 1.0, "hh": 1.0, "ride": 1.0, "crash": 1.0, "drums": 1.0,
           "bass": 0.95, "guitar": 0.85, "piano": 0.85, "other": 0.75, "lead": 1.15, "vocals": 1.15, "backing": 0.78}
# the parts that collect the boxy 250-400 Hz build-up of a dense arrangement
MUDDY = {"guitar", "piano", "other", "backing", "lead", "vocals", "toms"}


def places_for(names):
    """name -> (left pan, right pan) for the parts in the mix."""
    places = {}
    both = "guitar" in names and "piano" in names
    for name in names:
        if name in PAIR:
            places[name] = PAIR[name] if both else ALONE
        else:
            places[name] = PLACES.get(name, (-1.0, 1.0))
    return places


def place(data, left_pan, right_pan):
    """A stereo stem with each channel panned on its own (constant power), scaled so that what is the same
    in both channels (the part itself) comes out as loud as it went in."""
    import numpy as np

    def gains(p):
        a = (p + 1.0) * np.pi / 4.0
        return np.cos(a), np.sin(a)

    ll, lr = gains(left_pan)
    rl, rr = gains(right_pan)
    scale = np.sqrt(2.0 / ((ll + rl) ** 2 + (lr + rr) ** 2))
    out = np.empty_like(data)
    out[:, 0] = (data[:, 0] * ll + data[:, 1] * rl) * scale
    out[:, 1] = (data[:, 0] * lr + data[:, 1] * rr) * scale
    return out


def mix_and_master(stems, tuned_stems, out_master_path):
    """Mixes all tuned/processed stems and masters the output to target LUFS."""
    import soundfile as sf
    import numpy as np

    sys.path.insert(0, TOOLS_DIR)
    from chost_master import demud

    # Choose active audio tracks for mixdown
    names = []
    drum_parts = ["kick", "snare", "hh", "toms", "ride", "crash"]
    if any(p in tuned_stems for p in drum_parts):
        names += [p for p in drum_parts if p in tuned_stems]
    elif "drums" in tuned_stems:
        names.append("drums")
    names += [p for p in ["bass", "guitar", "piano", "other"] if p in tuned_stems]
    if "lead" in tuned_stems and "backing" in tuned_stems:
        names += ["lead", "backing"]
    elif "vocals" in tuned_stems:
        names.append("vocals")
    if not names:
        raise RuntimeError("No tracks available to mix")
    places = places_for(names)
    tracks_to_mix = [(name, tuned_stems[name], VOLUMES.get(name, 0.85), places[name]) for name in names]

    log(f"正在进行多轨混音求和，共 {len(tracks_to_mix)} 条音轨...")

    # Emit balance settings so the host mixer reflects them live: one pan for a track, the middle of its place
    for name, path, vol, (pl, pr) in tracks_to_mix:
        vol_db = 20.0 * np.log10(max(vol, 1e-4))
        print(f"BALANCE {name}\t{vol_db:.1f}\t{(pl + pr) / 2.0:.2f}", flush=True)

    # Load audio files and sum, all at the first stem's rate
    sr = None
    mix = None
    for name, path, vol, (pl, pr) in tracks_to_mix:
        data, sr = load_stereo(path, sr)
        if name in MUDDY:
            data, avg_cut, most_cut = demud(data.astype(np.float64), sr, 2.5)
            log(f"  {name}: 250-400 Hz 动态减浑浊 平均 -{avg_cut:.1f} dB，最多 -{most_cut:.1f} dB")
        processed = place(data, pl, pr) * vol
        log(f"  {name}: 声像 L/R {pl:+.2f} / {pr:+.2f}")
        if mix is None:
            mix = np.zeros((len(processed), 2), dtype=np.float64)
        if len(processed) > len(mix):
            mix = np.concatenate([mix, np.zeros((len(processed) - len(mix), 2))])
        mix[: len(processed)] += processed
    mix = mix.astype(np.float32)

    # Prevent clipping before master
    peak = np.max(np.abs(mix))
    if peak > 0.90:
        mix *= (0.90 / peak)

    # Write raw mix
    temp_mix = out_master_path + ".temp_mix.wav"
    sf.write(temp_mix, mix, sr, subtype="PCM_24")

    # Run AI mastering: the tone it needs, the mud out (where it still builds up), air, Mid/Side width
    progress(90, "正在进行总线顶级母带声学雕琢（-14 LUFS, True-Peak 限制）...")
    cmd = [
        PYTHON_EXE,
        os.path.join(TOOLS_DIR, "chost_master.py"),
        "--input", temp_mix,
        "--output", out_master_path,
        "--style", "natural",
        "--target", "-14",
        "--ceiling", "-1.0",
        "--demud", "3.0",
        "--air", "2.5",
        "--width", "1.35",
    ]
    code, lines = run_command(cmd, "执行商业级母带处理...", span=(90, 99))
    try:
        os.remove(temp_mix)
    except Exception:
        pass


def main():
    parser = argparse.ArgumentParser(description="chost 一键全自动 AI 音乐重制与母带工作流")
    parser.add_argument("--input", required=True, help="输入的原始音频文件路径")
    parser.add_argument("--output", help="输出的最终母带文件路径（默认在桌面生成）")
    parser.add_argument("--export-dir", help="导出目录，默认为桌面")
    parser.add_argument("--model-dir", help="AI 分轨模型所在的文件夹（默认用 AUDIO_SEPARATOR_MODEL_DIR，或 chost_separate.py 自己的默认位置）")
    args = parser.parse_args()

    input_file = os.path.abspath(args.input)
    if not os.path.exists(input_file):
        log(f"ERROR: 输入文件不存在: {input_file}")
        return 1

    base_name = os.path.splitext(os.path.basename(input_file))[0]
    desktop_dir = args.export_dir or get_desktop_dir()
    os.makedirs(desktop_dir, exist_ok=True)

    final_master_path = args.output or os.path.join(desktop_dir, f"{base_name}_顶级母带.wav")
    work_dir = os.path.join(os.path.dirname(input_file), f"{base_name}_remaster_work")
    os.makedirs(work_dir, exist_ok=True)

    log("=" * 60)
    log(f"🎧 开始全自动 AI 音乐重制与顶级母带工作流")
    log(f"输入音频: {input_file}")
    log(f"目标导出: {final_master_path}")
    log("=" * 60)

    # 1. Analyze
    progress(5, "正在分析音频速度、调性与节拍...")
    bpm, key = analyze_audio(input_file)
    progress(15, f"音频分析完成: 速度 {bpm:.1f} BPM, 调性 {key}")

    # 2. Separate
    progress(20, "正在启动 AI 细分分轨（最多 14 轨）...")
    stems_dir = os.path.join(work_dir, "stems")
    stems = separate_audio(input_file, stems_dir, level="finest", model_dir=args.model_dir)
    progress(45, f"14 轨细分完成，提取轨道: {', '.join(stems.keys())}")

    # 3. Tune
    progress(50, f"正在基于 {key} 对主唱与和声进行智能音高修正与跑调纠偏...")
    tuned_stems = tune_vocals(stems, key, work_dir)
    progress(65, "人声与和声智能修音完成，保留自然演唱颤音")

    # 4. Transcribe Accompaniment to MIDI
    progress(70, "正在将伴奏分轨转为精准高保真 MIDI 音符谱...")
    midi_files = transcribe_stems(stems, bpm, work_dir)
    progress(80, f"MIDI 转谱完成: {', '.join(midi_files.keys())}")

    # 5. Mix & Master
    progress(85, "正在执行智能多轨响度平衡、空间声像排布与总线母带限制...")
    mix_and_master(stems, tuned_stems, final_master_path)

    progress(100, f"全套重制与顶级母带制作完成！已保存到桌面: {final_master_path}")
    log("=" * 60)
    log(f"🎉 成功生成顶级母带音频: {final_master_path}")
    log("=" * 60)
    print(f"MASTER {final_master_path}", flush=True)
    print("DONE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
