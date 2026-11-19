"""Experiment: find each lyric line's rough position with stable-ts (Whisper).

usage: whisper_lines.py AUDIO LYRICS OUT.json [--model base|turbo] [--model-dir DIR]

Writes [{"line": raw line index, "text", "start", "end"}] in seconds. Only the line
positions are used downstream; word timing comes from CTC in hybrid.py.
"""
import argparse
import json
import re
import time

import stable_whisper
import stable_whisper.timing
import torch
import whisper.timing


def lyric_lines(text):
    lines = []
    for n, raw in enumerate(text.splitlines()):
        if re.fullmatch(r"\s*\[[^\]]+\]\s*", raw):
            continue
        if re.search(r"[^\W_]", raw):
            lines.append((n, raw.strip()))
    return lines


def find_lines(model, audio, lines, language="en"):
    """Return [{"line", "text", "start", "end"}] for (raw index, text) lyric lines."""
    result = model.align(audio, "\n".join(t for _, t in lines), language=language,
                         original_split=True, regroup=False, suppress_silence=False, verbose=None)
    segments = result.to_dict()["segments"]
    if len(segments) != len(lines):
        raise ValueError(f"stable-ts returned {len(segments)} segments for {len(lines)} lines")
    return [dict(line=n, text=t, start=round(s["start"], 3), end=round(s["end"], 3))
            for (n, t), s in zip(lines, segments)]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("audio")
    p.add_argument("lyrics")
    p.add_argument("out")
    p.add_argument("--model", default="base")
    p.add_argument("--model-dir")
    args = p.parse_args()
    lines = lyric_lines(open(args.lyrics, encoding="utf-8").read())
    started = time.monotonic()
    name = {"turbo": "large-v3-turbo"}.get(args.model, args.model)
    model = stable_whisper.load_model(name, device="cpu", download_root=args.model_dir)
    if args.model != "base" and torch.backends.mps.is_available():
        # DTW runs on CPU; the encoder and decoder use the Apple GPU.
        original = whisper.timing.dtw
        stable_whisper.timing.dtw = lambda x: original(x.cpu())
        model.encoder.to("mps")
        model.decoder.to("mps")
    out = find_lines(model, args.audio, lines)
    json.dump(out, open(args.out, "w"), indent=1, ensure_ascii=False)
    print(f"{args.model}: {len(out)} lines in {time.monotonic() - started:.1f}s")


if __name__ == "__main__":
    main()
