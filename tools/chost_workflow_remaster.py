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

# Auto configure local models directory if present
if os.path.exists(r"D:\AI_Models\audio_separator") and "AUDIO_SEPARATOR_MODEL_DIR" not in os.environ:
    os.environ["AUDIO_SEPARATOR_MODEL_DIR"] = r"D:\AI_Models\audio_separator"


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


def run_command(cmd, desc=""):
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
        if line_clean:
            lines.append(line_clean)
            if not (line_clean.startswith("MIDI ") or line_clean.startswith("STEM ") or line_clean.startswith("TUNED ")):
                print(line_clean, flush=True)
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


def separate_audio(input_file, out_dir, level="finest"):
    os.makedirs(out_dir, exist_ok=True)
    model_dir = os.environ.get("AUDIO_SEPARATOR_MODEL_DIR", r"D:\AI_Models\audio_separator")
    cmd = [
        PYTHON_EXE,
        os.path.join(TOOLS_DIR, "chost_separate.py"),
        "--input", input_file,
        "--out", out_dir,
        "--level", level
    ]
    if os.path.exists(model_dir):
        cmd.extend(["--model-dir", model_dir])

    code, lines = run_command(cmd, "正在执行 RTX 3090 极速 14 轨 AI 细分分轨...")
    stems = {}
    for l in lines:
        if l.startswith("STEM "):
            parts = l.split("\t")
            if len(parts) >= 3:
                stem_id = parts[0].replace("STEM ", "").strip()
                stems[stem_id] = parts[2].strip()
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


def tune_vocals(stems, key, out_dir):
    tuned_stems = dict(stems)
    # 核心排重：如果已成功分离出主唱(lead)与和声(backing)，则自动剔除冗余的总人声(vocals)
    # 彻底杜绝主唱与总人声两轨重叠叠加播放造成的严重相位抵消与机械双重杂音！
    roles_to_tune = []
    if "lead" in stems and os.path.exists(stems["lead"]):
        roles_to_tune.append("lead")
        if "backing" in stems and os.path.exists(stems["backing"]):
            roles_to_tune.append("backing")
        if "vocals" in tuned_stems:
            del tuned_stems["vocals"]
    elif "vocals" in stems and os.path.exists(stems["vocals"]):
        roles_to_tune.append("vocals")

    for role in roles_to_tune:
        if role in stems and os.path.exists(stems[role]):
            in_file = stems[role]

            out_file = os.path.join(out_dir, f"tuned_{role}.wav")
            # 神经分离人声本身无底噪与电音，切忌使用破坏性全轨时域差分 declick，避免切碎人声辅音咬字引发电流断续杂音
            # 自然人性化修音：温和纠偏刚度(主唱0.45/和声0.60)，增大过渡平滑时间(主唱50ms/和声40ms)，保留歌者自然颤音与真实情感
            strength = "0.60" if role == "backing" else "0.45"
            speed = "40" if role == "backing" else "50"
            cmd = [
                PYTHON_EXE,
                os.path.join(TOOLS_DIR, "chost_tune.py"),
                "--input", in_file,
                "--output", out_file,
                "--key", key if key else "auto",
                "--scale", "smart",
                "--strength", strength,
                "--speed", speed,
                "--drift", "0.15"
            ]
            code, lines = run_command(cmd, f"正在对 {role}（{'和声' if role == 'backing' else '主唱'}）进行智能修音（自然人性化调校，调性：{key}）...")
            if os.path.exists(out_file) and os.path.getsize(out_file) > 1000:
                tuned_stems[role] = out_file
                print(f"TUNED {role}\t{out_file}", flush=True)
            else:
                log(f"⚠️ {role} 智能修音未生成有效文件，自动采用高保真纯净原轨")
                tuned_stems[role] = in_file
    return tuned_stems


def transcribe_stems(stems, tempo, out_dir):
    """Transcribes separated stems into MIDI. Uses MuScriptor Large 1.4B if available, else Basic Pitch."""
    midi_files = {}
    muscriptor_script = os.path.join(TOOLS_DIR, "chost_muscriptor.py")
    large_weights = r"D:\AI_Models\muscriptor-large\model.safetensors"
    
    # If MuScriptor Large weights exist on disk, transcribe mix / stems directly with 1.4B transformer
    if os.path.exists(muscriptor_script) and os.path.exists(large_weights) and os.path.getsize(large_weights) > 1000 * 1024 * 1024:
        log("🚀 启用 RTX 3090 独占级 MuScriptor Large (1.4B 巨量大模型) 进行 35 乐器全声部终极采谱...")
        # Prioritize master audio if available, else reconstruct mix from stems
        source_mix = stems.get("mix") or stems.get("other") or stems.get("lead")
        for k in ["mix", "other", "vocals", "lead"]:
            if k in stems and os.path.exists(stems[k]):
                source_mix = stems[k]
                break
        if source_mix and os.path.exists(source_mix):
            cmd = [
                PYTHON_EXE,
                muscriptor_script,
                "--input", source_mix,
                "--output-dir", out_dir,
                "--quantize"
            ]
            code, lines = run_command(cmd, "正在执行 MuScriptor Large 35 乐器声部神经解码...")
            for l in lines:
                if l.startswith("MIDI "):
                    parts = l.split("\t")
                    if len(parts) >= 2:
                        role = parts[0].replace("MIDI ", "").strip()
                        path = parts[1].strip()
                        if os.path.exists(path):
                            midi_files[role] = path
            if midi_files:
                return midi_files

    # Fallback to per-stem transcription
    target_roles = ["lead", "bass", "piano", "guitar", "other"]
    role_labels = {
        "lead": "主旋律人声",
        "bass": "贝斯",
        "piano": "钢琴",
        "guitar": "吉他",
        "other": "弦乐/副旋律"
    }
    for role in target_roles:
        target_path = stems.get(role)
        if not target_path and role == "lead":
            target_path = stems.get("vocals")
        if target_path and os.path.exists(target_path):
            out_mid = os.path.join(out_dir, f"{role}.mid")
            cmd = [
                PYTHON_EXE,
                os.path.join(TOOLS_DIR, "chost_transcribe.py"),
                "--input", target_path,
                "--output", out_mid,
                "--role", role,
                "--tempo", str(tempo)
            ]
            label = role_labels.get(role, role)
            code, lines = run_command(cmd, f"正在将 {label} ({role}) 转为高精度 MIDI...")
            if os.path.exists(out_mid) and os.path.getsize(out_mid) > 50:
                midi_files[role] = out_mid
                print(f"MIDI {role}\t{out_mid}", flush=True)
    return midi_files


def mix_and_master(stems, tuned_stems, out_master_path):
    """Mixes all tuned/processed stems and masters the output to target LUFS."""
    import soundfile as sf
    import numpy as np

    # Choose active audio tracks for mixdown
    tracks_to_mix = []
    # Drums
    drum_parts = ["kick", "snare", "hh", "toms", "ride", "crash"]
    has_subdrums = any(p in tuned_stems for p in drum_parts)
    if has_subdrums:
        for p in drum_parts:
            if p in tuned_stems:
                tracks_to_mix.append((p, tuned_stems[p], 1.0, 0.0))  # (name, file, vol, pan)
    elif "drums" in tuned_stems:
        tracks_to_mix.append(("drums", tuned_stems["drums"], 1.0, 0.0))

    # Bass
    if "bass" in tuned_stems:
        tracks_to_mix.append(("bass", tuned_stems["bass"], 0.95, 0.0))

    # Accompaniment
    if "guitar" in tuned_stems:
        tracks_to_mix.append(("guitar", tuned_stems["guitar"], 0.85, -0.35))
    if "piano" in tuned_stems:
        tracks_to_mix.append(("piano", tuned_stems["piano"], 0.85, 0.35))
    if "other" in tuned_stems:
        tracks_to_mix.append(("other", tuned_stems["other"], 0.75, 0.0))

    # Vocals
    has_subvocals = "lead" in tuned_stems and "backing" in tuned_stems
    if has_subvocals:
        tracks_to_mix.append(("lead", tuned_stems["lead"], 1.15, 0.0))
        tracks_to_mix.append(("backing", tuned_stems["backing"], 0.78, 0.40))
    elif "vocals" in tuned_stems:
        tracks_to_mix.append(("vocals", tuned_stems["vocals"], 1.15, 0.0))

    if not tracks_to_mix:
        raise RuntimeError("No tracks available to mix")

    log(f"正在进行多轨混音求和，共 {len(tracks_to_mix)} 条音轨...")

    # Emit balance settings so the host mixer reflects them live
    for name, path, vol, pan in tracks_to_mix:
        vol_db = 20.0 * np.log10(max(vol, 1e-4))
        print(f"BALANCE {name}\t{vol_db:.1f}\t{pan:.2f}", flush=True)

    # Load audio files and sum
    sr = 44100
    max_len = 0
    audio_data = []

    for name, path, vol, pan in tracks_to_mix:
        data, file_sr = sf.read(path, always_2d=True)
        if file_sr != sr:
            sr = file_sr
        gain_l = np.cos((pan + 1.0) * np.pi / 4.0) * vol
        gain_r = np.sin((pan + 1.0) * np.pi / 4.0) * vol
        processed = np.zeros_like(data)
        processed[:, 0] = data[:, 0] * gain_l
        processed[:, 1] = data[:, 1] * gain_r if data.shape[1] > 1 else data[:, 0] * gain_r
        audio_data.append(processed)
        if len(processed) > max_len:
            max_len = len(processed)

    mix = np.zeros((max_len, 2), dtype=np.float32)
    for data in audio_data:
        mix[: len(data), :] += data

    # Prevent clipping before master
    peak = np.max(np.abs(mix))
    if peak > 0.90:
        mix *= (0.90 / peak)

    # Write raw mix
    temp_mix = out_master_path + ".temp_mix.wav"
    sf.write(temp_mix, mix, sr, subtype="PCM_24")

    # Run AI mastering
    progress(90, "正在进行总线顶级母带声学雕琢（-14 LUFS, True-Peak 限制）...")
    cmd = [
        PYTHON_EXE,
        os.path.join(TOOLS_DIR, "chost_master.py"),
        "--input", temp_mix,
        "--output", out_master_path,
        "--style", "warm",
        "--target", "-14",
        "--ceiling", "-1.0"
    ]
    code, lines = run_command(cmd, "执行商业级母带处理...")
    try:
        os.remove(temp_mix)
    except Exception:
        pass


def main():
    parser = argparse.ArgumentParser(description="chost 一键全自动 AI 音乐重制与母带工作流")
    parser.add_argument("--input", required=True, help="输入的原始音频文件路径")
    parser.add_argument("--output", help="输出的最终母带文件路径（默认在桌面生成）")
    parser.add_argument("--export-dir", help="导出目录，默认为桌面")
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
    progress(20, "正在启动 RTX 3090 显卡极速 14 轨 AI 细分分轨...")
    stems_dir = os.path.join(work_dir, "stems")
    stems = separate_audio(input_file, stems_dir, level="finest")
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
