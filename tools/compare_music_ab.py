#!/usr/bin/env python3
"""Rank native module renders against an exact DeSmuME internal-WAV capture."""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np
from scipy.signal import correlate, resample_poly


def load_mono(path: Path, target_rate: int) -> np.ndarray:
    with wave.open(str(path), "rb") as source:
        if source.getsampwidth() != 2:
            raise ValueError(f"{path}: expected signed 16-bit PCM")
        channels = source.getnchannels()
        rate = source.getframerate()
        samples = np.frombuffer(
            source.readframes(source.getnframes()), dtype="<i2"
        ).reshape(-1, channels).astype(np.float64).mean(axis=1)
    samples -= samples.mean()
    return resample_poly(samples, target_rate, rate)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("renders", type=Path, nargs="?", default=Path("out/snd/music"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rate", type=int, default=8000)
    parser.add_argument("--seconds", type=float, default=8.0)
    args = parser.parse_args()

    reference = load_mono(args.reference, args.rate)
    nonzero = np.flatnonzero(np.abs(reference) > 20)
    if nonzero.size:
        reference = reference[nonzero[0]:]
    reference = reference[: int(args.rate * args.seconds)]
    reference /= np.linalg.norm(reference) + 1e-9
    rankings = []
    for path in sorted(args.renders.glob("module_*.wav")):
        candidate = load_mono(path, args.rate)
        correlation = correlate(candidate, reference, mode="valid", method="fft")
        cumulative = np.concatenate(([0.0], np.cumsum(candidate * candidate)))
        energy = np.sqrt(np.maximum(
            1e-9,
            cumulative[len(reference):] - cumulative[:-len(reference)],
        ))
        normalized = correlation / energy
        index = int(np.argmax(normalized))
        rankings.append({
            "module": path.stem,
            "normalized_correlation": float(normalized[index]),
            "offset_seconds": index / args.rate,
        })
    rankings.sort(key=lambda row: row["normalized_correlation"], reverse=True)
    result = {
        "reference": str(args.reference),
        "comparison_seconds": len(reference) / args.rate,
        "rankings": rankings,
        "interpretation": (
            "A high rank identifies the likely module; correlation is not an "
            "acceptance test because DeSmuME and the native renderer differ in "
            "mixing, resampling, envelopes, and capture start offset."
        ),
    }
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
