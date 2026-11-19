"""Offline English lyric alignment. Read lyrics on stdin and emit word spans as JSON."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import unicodedata


def parse(text):
    words = []
    for line_number, line in enumerate(text.splitlines()):
        if re.fullmatch(r"\s*\[[^\]]+\]\s*", line):
            continue
        for match in re.finditer(r"[^\W_]+(?:['’][^\W_]+)*", line):
            original = match.group()
            plain = unicodedata.normalize("NFKD", original).replace("’", "'")
            plain = "".join(c for c in plain if not unicodedata.combining(c)).upper()
            if not re.fullmatch("[A-Z']+", plain):
                raise ValueError(f"Spell out {original!r} in sung English on line {line_number + 1}.")
            words.append((line_number, original, plain))
    if not words:
        raise ValueError("Paste at least one line of sung words.")
    return words


def ctc_spans(logp, tokens, separator):
    """Viterbi over alternating blanks and required letters; preserve repeated letters."""
    import numpy as np
    state = np.zeros(2*len(tokens)+1, dtype=np.intp)
    state[1::2] = tokens
    previous = np.full(len(state), -np.inf, dtype=np.float32)
    previous[0] = 0
    # Music between words can be long. Inside a spelling, give sustained blanks a
    # small cost so distant instrumental transients do not complete a word.
    cost = np.zeros(len(state), dtype=np.float32)
    internal = (state[1:-2:2] != separator) & (state[3::2] != separator)
    cost[2:-1:2] = 0.12 * internal
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
        previous = best + emission
    pos = len(state)-2 + int(previous[-1] > previous[-2])
    if not np.isfinite(previous[pos]):
        raise ValueError("Cannot fit the supplied words to this recording.")
    path = np.empty(len(logp), dtype=np.intp)
    for t in range(len(path)-1,-1,-1):
        path[t] = pos
        pos -= int(trace[t,pos])
    spans = []
    for i, token in enumerate(tokens):
        # The monotonic state path lets us locate a letter with two binary searches.
        a, b = np.searchsorted(path, [2*i+1, 2*i+2])
        spans.append((int(a), int(b), float(np.exp(logp[a:b,token]).mean())))
    return spans


def align(audio, stream, text):
    import torch
    import torchaudio
    words = parse(text)
    torch.set_num_threads(min(4, os.cpu_count() or 1))
    bundle = torchaudio.pipelines.WAV2VEC2_ASR_BASE_960H
    checkpoint = Path(torch.hub.get_dir()) / "checkpoints" / "wav2vec2_fairseq_base_ls960_asr_ls960.pth"
    if not checkpoint.is_file():
        raise ValueError(f"Local English model is missing at {checkpoint}. See the alignment setup in README.")
    ffmpeg = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
    pcm = subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-i", audio, "-map", f"0:{stream}",
                          "-ac", "1", "-ar", "16000", "-f", "f32le", "pipe:1"],
                         stdout=subprocess.PIPE, check=True).stdout
    if len(pcm) < 1600:
        raise ValueError("The song is too short to align.")
    wave = torch.frombuffer(bytearray(pcm), dtype=torch.float32)
    del pcm
    model = bundle.get_model().eval()
    # A global CTC path distinguishes repeated passages. Overlapping acoustic windows
    # bound attention memory. This model emits one frame per 320 samples, receptive field 400.
    stride, field = 320, 400
    count = (len(wave) - field) // stride + 1
    chunks = []
    with torch.inference_mode():
        for first in range(0, count, 600):
            last = min(first + 600, count)
            left, right = max(0, first-75), min(count, last+75)
            clip = wave[left*stride : (right-1)*stride+field]
            logits, _ = model(clip.unsqueeze(0))
            chunks.append(logits[0, first-left:last-left].log_softmax(-1))
            print(f"Analyzing song {round(100*last/count)}%", file=sys.stderr, flush=True)
    emissions = torch.cat(chunks)
    labels = {label: i for i, label in enumerate(bundle.get_labels())}
    tokens = [labels[ch] for ch in "|".join(w[2] for w in words)]
    if len(tokens) + sum(a == b for a,b in zip(tokens,tokens[1:])) > count:
        raise ValueError("The song is too short for these lyrics.")
    spans = ctc_spans(emissions.numpy(), tokens, labels["|"])
    result = []
    cursor = 0
    for line, original, normalized in words:
        letters = spans[cursor:cursor+len(normalized)]
        result.append(dict(line=line, word=original, start=20*letters[0][0], end=20*letters[-1][1],
                           score=round(sum(v[2] for v in letters)/len(letters), 4)))
        cursor += len(normalized)+1
    return {"model":"WAV2VEC2_ASR_BASE_960H", "words":result}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("audio")
    parser.add_argument("--stream", type=int, default=0)
    args = parser.parse_args()
    try:
        import resource
        import time
        started = time.monotonic()
        print(json.dumps(align(args.audio, args.stream, sys.stdin.read()), ensure_ascii=False))
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        mib = rss / (1048576 if sys.platform == "darwin" else 1024)
        print(f"Finished in {time.monotonic()-started:.1f}s, peak resident memory {mib:.0f} MiB", file=sys.stderr)
    except Exception as exc:
        print(f"Alignment failed: {exc}", file=sys.stderr)
        sys.exit(1)
