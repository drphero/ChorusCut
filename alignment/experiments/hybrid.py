"""Experiment: CTC word timing, either across the whole song or inside line windows.

usage: hybrid.py AUDIO LYRICS OUT.json [--model base|mms] [--lines WHISPER_LINES.json]

Without --lines this reproduces the shipped global alignment (with a choice of
acoustic model). With --lines, each lyric line is aligned only inside a window
around the position Whisper found, so a mistake cannot spread to later lines.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np
import torch
import torchaudio

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from align import parse  # noqa: E402  (same lyric parsing as the shipped worker)

FRAME = 0.02  # Seconds per emission frame for both models (320 samples at 16 kHz).


def load_wave(path):
    ffmpeg = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
    pcm = subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-i", path, "-ac", "1", "-ar", "16000",
                          "-f", "f32le", "pipe:1"], stdout=subprocess.PIPE, check=True).stdout
    return torch.frombuffer(bytearray(pcm), dtype=torch.float32)


def acoustic_model(kind):
    if kind == "base":
        bundle = torchaudio.pipelines.WAV2VEC2_ASR_BASE_960H
        model = bundle.get_model()
        ids = {label: i for i, label in enumerate(bundle.get_labels())}
        return model.eval(), (lambda plain: [ids[c] for c in plain if c in ids]), ids["|"]
    bundle = torchaudio.pipelines.MMS_FA
    model = bundle.get_model(with_star=False)
    ids = bundle.get_dict(star=None)
    return model.eval(), (lambda plain: [ids[c] for c in plain.lower() if c in ids]), None


def emissions(model, wave):
    stride, field = 320, 400
    count = (len(wave) - field) // stride + 1
    chunks = []
    with torch.inference_mode():
        for first in range(0, count, 600):
            last = min(first + 600, count)
            left, right = max(0, first - 75), min(count, last + 75)
            logits, _ = model(wave[left * stride:(right - 1) * stride + field].unsqueeze(0))
            chunks.append(logits[0, first - left:last - left].log_softmax(-1))
    return torch.cat(chunks).numpy()


def viterbi(logp, tokens, internal, late_from=None, late_cost=0.0):
    """Best monotonic path through alternating blanks and letters.

    Edge blanks are free (lead-in and lead-out). A blank between two letters of the
    same word costs a little so a word cannot be stretched across an instrumental gap.
    With late_from, each lead-in frame from that index on costs late_cost, so the
    first word does not skip past an expected start (e.g. a repeated phrase).
    Returns (first_frame, end_frame, mean_probability) for each token.
    """
    state = np.zeros(2 * len(tokens) + 1, dtype=np.intp)
    state[1::2] = tokens
    previous = np.full(len(state), -np.inf, dtype=np.float32)
    previous[0] = 0
    cost = np.zeros(len(state), dtype=np.float32)
    cost[2:-1:2] = 0.12 * np.asarray(internal, dtype=np.float32)
    can_skip = (state[2:] != 0) & (state[2:] != state[:-2])
    trace = np.empty((len(logp), len(state)), dtype=np.uint8)
    for t, frame in enumerate(logp):
        best = previous.copy()
        direction = trace[t]
        direction.fill(0)
        advance = previous[:-1] > best[1:]
        best[1:] = np.maximum(best[1:], previous[:-1])
        direction[1:][advance] = 1
        skip = can_skip & (previous[:-2] > best[2:])
        best[2:][skip] = previous[:-2][skip]
        direction[2:][skip] = 2
        emission = frame[state] - cost
        emission[0] = emission[-1] = frame.max()
        if late_from is not None and t >= late_from:
            emission[0] -= late_cost
        previous = best + emission
    pos = len(state) - 2 + int(previous[-1] > previous[-2])
    if not np.isfinite(previous[pos]):
        raise ValueError("window too short for its words")
    path = np.empty(len(logp), dtype=np.intp)
    for t in range(len(path) - 1, -1, -1):
        path[t] = pos
        pos -= int(trace[t, pos])
    spans = []
    for i, token in enumerate(tokens):
        a, b = np.searchsorted(path, [2 * i + 1, 2 * i + 2])
        spans.append((int(a), int(b), float(np.exp(logp[a:b, token]).mean()) if b > a else 0.0))
    return spans


def align_words(logp, offset, words, tokenize, separator, late_from=None, late_cost=0.0):
    """Align (line, original, plain) words to logp[...]; offset is the first frame's index."""
    tokens, owner, internal = [], [], []
    for w, (_, _, plain) in enumerate(words):
        if w and separator is not None:
            tokens.append(separator)
            owner.append(None)
        for token in tokenize(plain):
            tokens.append(token)
            owner.append(w)
    for i in range(len(tokens) - 1):
        internal.append(owner[i] is not None and owner[i] == owner[i + 1])
    spans = viterbi(logp, tokens, internal, late_from, late_cost)
    result = []
    for w, (line, original, _) in enumerate(words):
        mine = [s for s, o in zip(spans, owner) if o == w]
        result.append(dict(line=line, word=original,
                           start=round(1000 * FRAME * (offset + mine[0][0])),
                           end=round(1000 * FRAME * (offset + mine[-1][1])),
                           score=round(sum(s[2] for s in mine) / len(mine), 4)))
    return result


def align_windowed(logp, words, windows, tokenize, separator, pad=0.75, late_cost=0.0, late_slack=0.25):
    """Align each line inside a window around its Whisper position (seconds).

    late_cost > 0 charges each lead-in frame more than late_slack seconds after
    Whisper's line start.
    """
    total = len(logp)
    by_line = {}
    for w in words:
        by_line.setdefault(w[0], []).append(w)
    result, floor = [], 0
    for k, info in enumerate(windows):
        line_words = by_line[info["line"]]
        need = sum(len(tokenize(w[2])) for w in line_words) * 2 + 2
        a = min(max(floor, int((info["start"] - pad) / FRAME)), max(0, total - need))
        # Whisper's line ends stretch across following instrumentals, so also cap
        # the window by a generous sung duration for the line's word count.
        longest = info["start"] + pad + 1.5 + 0.6 * len(line_words)
        b = int(min(info["end"] + pad, longest) / FRAME)
        if k + 1 < len(windows):
            b = min(b, int((windows[k + 1]["start"] + 0.5) / FRAME))
        b = min(total, max(b, a + need))
        late_from = max(0, int((info["start"] + late_slack) / FRAME) - a) if late_cost else None
        aligned = align_words(logp[a:b], a, line_words, tokenize, separator, late_from, late_cost)
        for w in aligned:
            w["whisper_start"] = round(1000 * info["start"])
        result += aligned
        floor = int(aligned[-1]["end"] / 1000 / FRAME)
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("audio")
    p.add_argument("lyrics")
    p.add_argument("out")
    p.add_argument("--model", choices=["base", "mms"], default="base")
    p.add_argument("--lines")
    p.add_argument("--pad", type=float, default=0.75, help="seconds of slack around each Whisper line")
    args = p.parse_args()
    started = time.monotonic()
    words = parse(open(args.lyrics, encoding="utf-8").read())
    model, tokenize, separator = acoustic_model(args.model)
    logp = emissions(model, load_wave(args.audio))
    windows = json.load(open(args.lines)) if args.lines else None
    if windows:
        result = align_windowed(logp, words, windows, tokenize, separator, args.pad)
    else:
        result = align_words(logp, 0, words, tokenize, separator)
    json.dump({"model": args.model, "windowed": bool(windows), "words": result},
              open(args.out, "w"), ensure_ascii=False)
    print(f"{args.model} {'windowed' if windows else 'global'}: {len(result)} words "
          f"in {time.monotonic() - started:.1f}s")


if __name__ == "__main__":
    main()
