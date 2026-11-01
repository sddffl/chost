# Models bundled with chost

`basic_pitch_nmp.onnx` is the ICASSP 2022 note transcription model from Spotify's
[Basic Pitch](https://github.com/spotify/basic-pitch) 0.4.0, under the Apache License 2.0
(`LICENSE-basic-pitch.txt`). `tools/chost_transcribe.py` runs it with onnxruntime; its note decoding
is adapted from Basic Pitch's `note_creation.py`.
