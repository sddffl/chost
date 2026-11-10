"""chost MuScriptor Large 35-Instrument Conductor MIDI Transcriber.

This script uses MuScriptor Large (~1.4B parameters) running directly on RTX 3090 GPU
to transcribe audio into multi-track Conductor-aligned MIDI, splitting by 35 instruments.

Outputs:
    <out_dir>/all_parts.mid   - Master Conductor Type-1 MIDI containing all tracks
    <out_dir>/lead.mid        - Voice / Vocals melody track
    <out_dir>/bass.mid        - Electric/Acoustic Bass
    <out_dir>/piano.mid       - Acoustic & Electric Piano
    <out_dir>/guitar.mid      - Electric & Acoustic Guitars
    <out_dir>/drums.mid       - Drum kit & Percussion
    <out_dir>/strings.mid     - Violins, Cellos, Strings ensemble
    <out_dir>/synth.mid       - Synth Leads & Pads

Stdout protocol for chost:
    PROGRESS <0-100> <message>
    MIDI <role>\t<path>\t<note count>
    DONE
"""

import argparse
import os
import sys
import time
import mido

MODEL_WEIGHTS_DIR = r"D:\AI_Models\muscriptor-large"

def log_progress(pct, msg):
    print(f"PROGRESS {pct} {msg}", flush=True)

def split_midi_by_instruments(master_midi_path, out_dir):
    mid = mido.MidiFile(master_midi_path)
    os.makedirs(out_dir, exist_ok=True)
    
    # Map track names/program numbers to standard chost roles
    # MuScriptor exports tracks named by instrument group
    tracks_by_role = {
        "lead": [],
        "bass": [],
        "piano": [],
        "guitar": [],
        "drums": [],
        "other": []
    }
    
    conductor_track = None
    for track in mid.tracks:
        is_tempo_track = False
        track_name = track.name.lower() if track.name else ""
        
        # Check if it's the conductor/tempo track (track 0 with no notes)
        has_notes = any(msg.type in ("note_on", "note_off") for msg in track)
        if not has_notes:
            conductor_track = track
            continue
            
        if any(v in track_name for v in ["voice", "vocal", "lead"]):
            tracks_by_role["lead"].append(track)
        elif any(b in track_name for b in ["bass"]):
            tracks_by_role["bass"].append(track)
        elif any(p in track_name for p in ["piano", "organ", "keyboard"]):
            tracks_by_role["piano"].append(track)
        elif any(g in track_name for g in ["guitar"]):
            tracks_by_role["guitar"].append(track)
        elif any(d in track_name for d in ["drum", "percussion"]):
            tracks_by_role["drums"].append(track)
        else:
            tracks_by_role["other"].append(track)
            
    exported_files = {}
    for role, tracks in tracks_by_role.items():
        if not tracks:
            continue
        role_mid = mido.MidiFile(ticks_per_beat=mid.ticks_per_beat)
        if conductor_track is not None:
            role_mid.tracks.append(conductor_track)
        note_count = 0
        for t in tracks:
            role_mid.tracks.append(t)
            for msg in t:
                if msg.type == "note_on" and msg.velocity > 0:
                    note_count += 1
        if note_count > 0:
            out_file = os.path.join(out_dir, f"{role}.mid")
            role_mid.save(out_file)
            exported_files[role] = (out_file, note_count)
            print(f"MIDI {role}\t{out_file}\t{note_count}", flush=True)

    return exported_files

def main():
    parser = argparse.ArgumentParser(description="MuScriptor Large Transcriber for chost")
    parser.add_argument("--input", required=True, help="Input audio file")
    parser.add_argument("--output-dir", required=True, help="Output directory for generated MIDI files")
    parser.add_argument("--model", default=MODEL_WEIGHTS_DIR, help="Path to weights or model size")
    parser.add_argument("--quantize", action="store_true", help="Snap notes to detected beat grid")
    args = parser.parse_args()

    input_audio = os.path.abspath(args.input)
    out_dir = os.path.abspath(args.output_dir)
    os.makedirs(out_dir, exist_ok=True)
    master_midi = os.path.join(out_dir, "all_parts.mid")

    log_progress(5, "正在初始化 RTX 3090 深度学习推理核心与 MuScriptor Large 1.4B 架构...")
    
    from muscriptor.transcription_model import TranscriptionModel
    from muscriptor.utils.midi import ExportOptions

    weights_file = os.path.join(args.model, "model.safetensors") if os.path.isdir(args.model) else args.model
    if not os.path.exists(weights_file):
        # Fall back to model keyword or report error
        weights_file = args.model

    log_progress(15, "正在加载 MuScriptor Large 权重至 RTX 3090 24GB 显存...")
    model = TranscriptionModel.load_model(weights_file)

    log_progress(35, "正在通过 Beat-This 神经网络精准侦测全局节拍与拍号速度网格...")
    grid = model.detect_beat_grid_for(input_audio)

    log_progress(50, "正在运行 1.4B 巨量参数自回归声学-MIDI 跨模态解码（35 种乐器声部细分）...")
    export_opts = ExportOptions(quantize=args.quantize)
    midi_bytes, _ = model.transcribe_and_postprocess(
        input_audio,
        export=export_opts,
        detect_tempo=True
    )

    log_progress(85, "正在导出 Type-1 Conductor 高精度多轨乐谱...")
    with open(master_midi, "wb") as f:
        f.write(midi_bytes)

    log_progress(92, "正在根据乐器声部分解各轨道专属 MIDI 并注入 chost 工程...")
    split_midi_by_instruments(master_midi, out_dir)

    log_progress(100, "MuScriptor Large 终极采谱与声部切分完成！")
    print("DONE", flush=True)

if __name__ == "__main__":
    main()
