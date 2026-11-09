"""chost NVIDIA NemotronLabs RE-USE voice enhancement & dereverberation adapter.

Pure PyTorch implementation derived from SoundtSeparator / NVIDIA NemotronLabs RE-USE.
Eliminates room reverberation, boxiness, and background noise from vocal / speech stems.

Protocol:
    PROGRESS <0-100> <message>
    DONE <output file>
    ERROR <message>
"""

import argparse
import json
import math
import os
import sys
import urllib.request
import numpy as np
import soundfile as sf
import torch
import torch.nn.functional as F
from safetensors.torch import load_file

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)

from reuse_runtime.generator_SEMamba_time_d4 import SEMamba
from reuse_runtime.stfts import mag_phase_stft, mag_phase_istft

CONFIG_PATH = os.path.join(TOOLS_DIR, "reuse_runtime", "config.json")
MODEL_URL = ("https://huggingface.co/nvidia/NVIDIA-NemotronLabs-RE-USE/resolve/"
             "31f82f2efad1095034492f236e8dac9239ba713b/model.safetensors")
MODEL_BYTES = 38592940


def progress(pct, msg):
    print(f"PROGRESS {int(pct)} {msg}", flush=True)


def get_model_path():
    candidates = [
        os.path.join(TOOLS_DIR, "models", "nemotron-reuse", "model.safetensors"),
        os.path.join(os.path.dirname(TOOLS_DIR), "models", "nemotron-reuse", "model.safetensors"),
        r"D:\AI_Models\audio_separator\nemotron-reuse\model.safetensors",
        r"D:\AI_Models\nemotron-reuse\model.safetensors",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.path.getsize(c) == MODEL_BYTES:
            return c
    return candidates[0]


def ensure_model_downloaded(target_path):
    if os.path.isfile(target_path) and os.path.getsize(target_path) == MODEL_BYTES:
        return target_path

    os.makedirs(os.path.dirname(target_path), exist_ok=True)
    temp_path = target_path + ".tmp"
    progress(5, "正在下载 NVIDIA Nemotron RE-USE 语音增强与去混响模型 (约 38MB)...")

    urls = [
        MODEL_URL,
        "https://ghproxy.net/" + MODEL_URL,
        "https://mirror.ghproxy.com/" + MODEL_URL,
    ]

    last_err = None
    for url in urls:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "chost/1.0"})
            with urllib.request.urlopen(req, timeout=60) as resp, open(temp_path, "wb") as f:
                total = int(resp.headers.get("content-length", MODEL_BYTES))
                downloaded = 0
                while True:
                    chunk = resp.read(256 * 1024)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    pct = 5 + int(35 * downloaded / total)
                    progress(pct, f"正在下载模型... ({downloaded // 1024} KB / {total // 1024} KB)")
            if os.path.getsize(temp_path) == MODEL_BYTES:
                os.replace(temp_path, target_path)
                return target_path
        except Exception as e:
            last_err = e
            if os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except OSError:
                    pass

    if last_err:
        raise RuntimeError(f"下载 RE-USE 模型失败: {last_err}")
    return target_path


def load_model(weights_path, device=None):
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        config = json.load(f)
    model = SEMamba(config)
    model.load_state_dict(load_file(weights_path, device="cpu"), strict=True)
    device = device or ("cuda:0" if torch.cuda.is_available() else "cpu")
    return model.to(device).eval(), config, device


def _even(v):
    v = int(round(v))
    return v if v % 2 == 0 else v + 1


def _run_chunk(model, config, samples, sr):
    stft = config["stft_cfg"]
    ratio = stft["sampling_rate"]
    n_fft = _even(stft["n_fft"] * sr // ratio)
    hop = _even(stft["hop_size"] * sr // ratio)
    win = _even(stft["win_size"] * sr // ratio)
    compress = config["model_cfg"]["compress_factor"]
    orig_len = samples.shape[-1]

    if orig_len <= n_fft // 2:
        samples = F.pad(samples, (0, n_fft // 2 + 1 - orig_len))

    mag, phase, _ = mag_phase_stft(samples, n_fft, hop, win, compress_factor=compress, center=True, addeps=False)
    out_mag, out_phase, _ = model(mag, phase)

    zero_portion = (torch.expm1(F.relu(out_mag)) == 0).float().mean(dim=1)
    out_mag = out_mag.masked_fill((zero_portion > 0.5).unsqueeze(1), 0)

    output = mag_phase_istft(out_mag, out_phase, n_fft, hop, win, compress_factor=compress, length=samples.shape[-1])
    if output.shape[-1] < orig_len:
        output = F.pad(output, (0, orig_len - output.shape[-1]))
    return output[..., :orig_len]


def enhance_audio(audio, sr=44100, weights_path=None, chunk_seconds=2.0, overlap=0.5):
    """audio: (samples, channels) float32 -> (samples, channels) float32"""
    if weights_path is None:
        weights_path = get_model_path()
    weights_path = ensure_model_downloaded(weights_path)

    progress(45, "正在加载 NVIDIA Nemotron RE-USE 神经声学引擎...")
    model, config, device = load_model(weights_path)

    # Process each channel or average mono
    orig_channels = audio.shape[1]
    out_channels = []

    progress(50, f"正在进行神经去混响与人声超清净化 (设备: {device})...")

    with torch.inference_mode():
        for ch in range(orig_channels):
            mono = torch.from_numpy(audio[:, ch:ch+1].T).to(device) # (1, samples)
            length = mono.shape[-1]
            chunk = max(1, int(chunk_seconds * sr))
            stride = max(1, int(chunk * (1.0 - overlap)))
            
            result = torch.zeros_like(mono)
            weights = torch.zeros_like(mono)
            starts = list(range(0, max(1, length - chunk + 1), stride))
            if starts[-1] + chunk < length:
                starts.append(max(0, length - chunk))

            total_chunks = len(starts)
            for i, start in enumerate(starts):
                end = min(length, start + chunk)
                enhanced = _run_chunk(model, config, mono[:, start:end], sr)
                window = torch.hann_window(end - start, device=device, periodic=False).clamp_min(1e-5)
                result[:, start:end] += enhanced * window
                weights[:, start:end] += window

                if ch == 0 and i % 5 == 0:
                    pct = 50 + int(45 * (i + 1) / total_chunks)
                    progress(pct, f"去混响增强处理中... ({pct}%)")

            res_ch = (result / weights.clamp_min(1e-5)).cpu().numpy().T
            out_channels.append(res_ch)

    enhanced_audio = np.concatenate(out_channels, axis=1).astype(np.float32)
    return enhanced_audio


def main():
    parser = argparse.ArgumentParser(description="chost NVIDIA Nemotron RE-USE voice enhancement")
    parser.add_argument("--input", required=True, help="Input WAV file")
    parser.add_argument("--output", required=True, help="Output WAV file")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"ERROR Input file not found: {args.input}", flush=True)
        sys.exit(1)

    try:
        progress(5, f"读取音频文件: {os.path.basename(args.input)}")
        audio, sr = sf.read(args.input, dtype="float32", always_2d=True)

        res = enhance_audio(audio, sr=sr)

        progress(95, f"正在保存净化后的音频至: {os.path.basename(args.output)}")
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        sf.write(args.output, res, sr, subtype="PCM_24")

        progress(100, "语音增强与去混响完成")
        print(f"DONE {os.path.abspath(args.output)}", flush=True)
    except Exception as e:
        print(f"ERROR {e}", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
