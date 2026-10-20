#!/usr/bin/env python3
"""SPACERIP test track generator.

Creates a 20 second stereo WAV at 44.1 kHz with a clear 120 BPM kick,
off-beat bass, hats and a pad: perfect to verify the beat detection of
the visualisation.

Usage: make_test_wav.py [output.wav] [seconds] [bpm]
"""
import math
import struct
import sys
import wave

RATE = 44100


def gen(path, seconds, bpm):
    beat = 60.0 / bpm
    n = int(RATE * seconds)
    frames = bytearray()

    for i in range(n):
        t = i / RATE
        # position inside the bar (4 beats)
        pos = t / beat
        beat_idx = int(pos)
        frac = pos - beat_idx

        s = 0.0

        # --- kick: short decaying sine on every beat ---
        k = math.exp(-frac * beat * 22.0)
        s += 0.95 * k * math.sin(2 * math.pi * 55.0 * frac * beat)

        # --- bass: on every half beat ---
        hpos = (t / (beat / 2)) % 1.0
        hb = math.exp(-hpos * (beat / 2) * 9.0)
        s += 0.35 * hb * math.sin(2 * math.pi * 110.0 * t)

        # --- hat: white-ish noise burst on off beats ---
        h2 = (t / (beat / 2) + 0.5) % 1.0
        if h2 < 0.08:
            noise = ((i * 1103515245 + 12345) >> 16 & 0x7fff) / 32768.0
            s += 0.22 * math.exp(-h2 * 60.0) * (noise * 2 - 1)

        # --- pad chord, gently pulsing on the bar ---
        bar = (t / (beat * 4)) % 1.0
        env = 0.6 + 0.4 * math.exp(-bar * 3.0)
        s += 0.10 * env * (
            math.sin(2 * math.pi * 220.0 * t)
            + math.sin(2 * math.pi * 277.18 * t)
            + math.sin(2 * math.pi * 329.63 * t)
        )

        s = max(-1.0, min(1.0, s))
        v = int(s * 32000)
        frames += struct.pack("<hh", v, v)

    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"wrote {path}: {seconds}s, {bpm} BPM, stereo {RATE} Hz")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "spacerip_test.wav"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    bpm = float(sys.argv[3]) if len(sys.argv) > 3 else 120.0
    gen(out, secs, bpm)
