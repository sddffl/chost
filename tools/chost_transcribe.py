"""chost audio-to-MIDI helper powered by MuScriptor Large 1.4B.

This script runs with the Python environment of chost and reports progress over stdout:

    PROGRESS <0-100> <message>
    MIDI <path>\t<note count>
    ERROR <message>
    DONE

Usage:
    python chost_transcribe.py --input <audio_path> --output <midi_path> --role <role> [--tempo 120] [--name <track_name>]
"""

import argparse
import os
import sys
import traceback
import mido

MODEL_WEIGHTS_DIR = r"D:\AI_Models\muscriptor-large"

def progress(pct, msg):
    print(f"PROGRESS {int(pct)} {msg}", flush=True)

def error(msg):
    print(f"ERROR {msg}", flush=True)

def count_notes(midi_path):
    try:
        mid = mido.MidiFile(midi_path)
        notes = 0
        for track in mid.tracks:
            for msg in track:
                if msg.type == "note_on" and msg.velocity > 0:
                    notes += 1
        return notes
    except Exception:
        return 0

def transcribe_with_muscriptor(input_audio, output_midi, role="mix", tempo=120.0):
    weights_path = os.path.join(MODEL_WEIGHTS_DIR, "model.safetensors")
    if not os.path.exists(weights_path):
        weights_path = MODEL_WEIGHTS_DIR

    progress(10, "正在装载 MuScriptor Large 1.4B 神经网络模型至 RTX 3090...")
    from muscriptor.transcription_model import TranscriptionModel
    from muscriptor.utils.midi import ExportOptions

    model = TranscriptionModel.load_model(weights_path)
    progress(30, "正在运行 Beat-This 神经网络精准侦测音乐网格与速度...")
    
    # Map role to instrument list if specific
    instrument_map = {
        "vocals": ["voice"],
        "lead": ["voice"],
        "backing": ["voice"],
        "bass": ["electric_bass", "acoustic_bass"],
        "guitar": ["acoustic_guitar", "clean_electric_guitar", "distorted_electric_guitar"],
        "piano": ["acoustic_piano", "electric_piano"],
        "drums": ["drums"],
        "kick": ["drums"],
        "snare": ["drums"],
        "other": ["string_ensemble", "synth_lead", "synth_pad", "violin", "cello"]
    }
    instruments = instrument_map.get(role.lower())

    progress(45, "正在使用 1.4B 大模型进行全多音/多声部神经解码...")
    export_opts = ExportOptions(quantize=False)
    midi_bytes, _ = model.transcribe_and_postprocess(
        input_audio,
        instruments=instruments,
        export=export_opts,
        detect_tempo=True
    )

    progress(90, "正在封装导出高精度 MIDI 轨...")
    with open(output_midi, "wb") as f:
        f.write(midi_bytes)

    notes = count_notes(output_midi)
    progress(100, f"转换完成，共生成 {notes} 个音符")
    print(f"MIDI {output_midi}\t{notes}", flush=True)

def main():
    parser = argparse.ArgumentParser(description="chost audio-to-MIDI powered by MuScriptor Large")
    parser.add_argument("--input", required=True, help="Input audio file")
    parser.add_argument("--output", required=True, help="Output MIDI file")
    parser.add_argument("--role", default="mix", help="Stem role (vocals, bass, guitar, piano, drums, other, mix)")
    parser.add_argument("--tempo", type=float, default=120.0, help="Estimated tempo BPM")
    parser.add_argument("--name", default="", help="Track name")
    args = parser.parse_args()

    input_file = os.path.abspath(args.input)
    output_file = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output_file), exist_ok=True)

    if not os.path.exists(input_file):
        error(f"找不到输入音频文件: {input_file}")
        sys.exit(1)

    try:
        transcribe_with_muscriptor(input_file, output_file, role=args.role, tempo=args.tempo)
        print("DONE", flush=True)
    except Exception as e:
        traceback.print_exc(file=sys.stderr)
        error(f"转谱失败: {str(e)}")
        sys.exit(1)

if __name__ == "__main__":
    main()
