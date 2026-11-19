"""Parakeet CTC (subword) forced alignment on Mel-RoFormer vocals, English songs.

usage: parakeet_align.py [--repo nvidia/parakeet-ctc-1.1b] [--source melroformer]

Writes results/gpu/<config>/<stem>.json in the same word format as gpu_bench.py, with
config "lines=small_model=parakeet_mode=global_source=<source>".
"""
import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
import torch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "experiments"))
import bench  # noqa: E402
import gpu_bench as g  # noqa: E402
import hybrid  # noqa: E402


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--repo", default="nvidia/parakeet-ctc-1.1b")
    p.add_argument("--source", default="melroformer")
    args = p.parse_args()
    from transformers import AutoProcessor, ParakeetForCTC
    processor = AutoProcessor.from_pretrained(args.repo)
    # fp16 overflows to NaN on parakeet-ctc-0.6b; it is small enough for fp32.
    dtype = torch.float16 if "1.1b" in args.repo else torch.float32
    model = ParakeetForCTC.from_pretrained(args.repo, torch_dtype=dtype).to("cuda").eval()
    tok = processor.tokenizer
    blank = model.config.pad_token_id if model.config.pad_token_id is not None else len(tok)
    name = "parakeet" if "1.1b" in args.repo else "parakeet06"
    out_dir = HERE / "results/gpu" / f"lines=small_model={name}_mode=global_source={args.source}"
    out_dir.mkdir(parents=True, exist_ok=True)
    for song in g.songs(["English"]):
        started = time.monotonic()
        wave = hybrid.load_wave(g.stem_audio(song, args.source)).numpy()
        feats = processor.feature_extractor(wave, sampling_rate=16000, return_tensors="pt")
        with torch.inference_mode():
            logits = model(input_features=feats.input_features.to("cuda", dtype),
                           attention_mask=feats.attention_mask.to("cuda")).logits[0].float()
        logp = logits.log_softmax(-1).cpu().numpy()
        # Put the blank in column 0 so hybrid.viterbi's convention holds.
        order = [blank] + [i for i in range(logp.shape[1]) if i != blank]
        logp = logp[:, order]
        remap = {old: new for new, old in enumerate(order)}
        frame = len(wave) / 16000 / len(logp)  # seconds per output frame (~0.08)
        words = bench.parse_any(song["text"])
        tokens, owner = [], []
        for w, (_, _, plain) in enumerate(words):
            ids = tok.encode(" " + plain.lower(), add_special_tokens=False)
            for t in ids:
                tokens.append(remap[t])
                owner.append(w)
        internal = [owner[i] == owner[i + 1] for i in range(len(tokens) - 1)]
        spans = hybrid.viterbi(logp, tokens, internal)
        result = []
        for w, (line, original, _) in enumerate(words):
            mine = [s for s, o in zip(spans, owner) if o == w]
            if not mine:
                continue
            result.append(dict(line=line, word=original, start=round(1000 * frame * mine[0][0]),
                               end=round(1000 * frame * mine[-1][1]), score=0.0))
        json.dump({"words": result}, open(out_dir / f"{song['stem']}.json", "w"))
        errors, _ = bench.score(result, bench.human_onsets(song["stem"]))
        s = bench.summarize(errors)
        print(f"{song['stem'][:36]:36s} ≤0.3 {s['le03']:5.1f}%  frame {1000 * frame:.0f}ms  {time.monotonic() - started:.1f}s",
              flush=True)


if __name__ == "__main__":
    main()
