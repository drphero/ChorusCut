"""Command line: python -m chorus_align AUDIO LYRICS.txt OUT.json [--language en] [--device auto]

Runs the same pipeline as the ComfyUI node, outside ComfyUI (for benchmarks and scripts).
Decoding needs ffmpeg on PATH.
"""
import argparse
import json
import shutil
import subprocess

import numpy as np
import torch

from .pipeline import DEFAULT_VOTERS, LANGUAGES, align


def decode(path, rate=44100):
    ffmpeg = shutil.which("ffmpeg") or "ffmpeg"
    pcm = subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-i", path, "-ac", "2", "-ar", str(rate),
                          "-f", "f32le", "pipe:1"], stdout=subprocess.PIPE, check=True).stdout
    return torch.from_numpy(np.frombuffer(pcm, dtype=np.float32).reshape(-1, 2).T.copy()), rate


def main():
    p = argparse.ArgumentParser()
    p.add_argument("audio")
    p.add_argument("lyrics")
    p.add_argument("out")
    p.add_argument("--language", default="en", choices=LANGUAGES)
    p.add_argument("--device", default="auto", choices=["auto", "cuda", "mps"])
    p.add_argument("--voters", default=DEFAULT_VOTERS)
    p.add_argument("--vocals", help="precomputed vocal stem, to skip separation")
    p.add_argument("--cache-dir")
    args = p.parse_args()
    wave, rate = decode(args.audio)
    vocals = decode(args.vocals)[0] if args.vocals else None
    result = align(wave, rate, open(args.lyrics, encoding="utf-8").read(), args.language, args.device,
                   args.voters, args.cache_dir, vocals=vocals,
                   progress=lambda f, label: label and print(f"{round(100 * f):3d}% {label}", flush=True))
    json.dump(result, open(args.out, "w", encoding="utf-8"), ensure_ascii=False)
    flagged = len({w["line"] for w in result["words"] if w.get("flag")})
    print(f"{len(result['words'])} words, {flagged} lines flagged, {len(result['sections'])} instrumental sections,"
          f" {result['device']}, timings {result['timings_s']}")


if __name__ == "__main__":
    main()
