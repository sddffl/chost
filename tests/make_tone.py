"""Writes a 5 second stereo 440 Hz test tone (16-bit WAV) for the smoke tests."""
import math
import struct
import sys
import wave

path = sys.argv[1]
rate = int(sys.argv[2]) if len(sys.argv) > 2 else 48000

with wave.open(path, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    for i in range(rate * 5):
        v = int(16000 * math.sin(2 * math.pi * 440 * i / rate))
        frames += struct.pack("<hh", v, v)
    w.writeframes(bytes(frames))
