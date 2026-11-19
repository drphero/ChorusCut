"""Score instrumental-section detection against JamendoLyrics human line timings.

usage: instrumental.py [--gap 2.0] [--languages English,...]

Truth: time outside human-annotated lines, kept only where the gap is >= --gap seconds
(intro, outro and breaks). Predictions come from the best single alignment per language
(wav2vec2-large for English, XLSR otherwise, both whole-song on Mel-RoFormer vocals):

  aligner   gaps >= --gap between aligned lines
  refined   line ends extended while the vocal stem stays loud (held notes), and gaps
            dropped where the vocal stem is mostly active

Reports frame-level precision/recall/F1 of the instrumental mask (50 ms frames) and,
for each true instrumental section, whether a predicted one starts and ends within 0.5 s.
"""
import argparse
import csv
import json
from pathlib import Path

import numpy as np
import soundfile as sf

import gpu_bench as g

HOP = 0.05


def mask_from_spans(spans, total, gap):
    """Instrumental mask: frames outside spans, keeping only gaps of at least `gap` seconds."""
    n = int(total / HOP) + 1
    sung = np.zeros(n, bool)
    for a, b in spans:
        sung[int(a / HOP):int(b / HOP) + 1] = True
    inst = ~sung
    # Remove short gaps.
    out = inst.copy()
    i = 0
    while i < n:
        if inst[i]:
            j = i
            while j < n and inst[j]:
                j += 1
            if (j - i) * HOP < gap:
                out[i:j] = False
            i = j
        else:
            i += 1
    return out


def sections(mask):
    out, i, n = [], 0, len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            out.append((i * HOP, j * HOP))
            i = j
        else:
            i += 1
    return out


def vocal_activity(path, total):
    x, sr = sf.read(path, always_2d=True)
    x = x.mean(1)
    hop = int(sr * HOP)
    n = min(len(x) // hop, int(total / HOP) + 1)
    rms = np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(1))
    db = 20 * np.log10(rms + 1e-9)
    return db


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--gap", type=float, default=2.0)
    p.add_argument("--languages")
    p.add_argument("--quiet-db", type=float, default=-24, help="vocal level below the song's singing median counted as silent")
    args = p.parse_args()
    langs = args.languages.split(",") if args.languages else None
    totals = {k: np.zeros(3) for k in ("aligner", "refined")}  # tp, fp, fn frames
    sec_hits = {k: 0 for k in totals}
    sec_total = 0
    for song in g.songs(langs):
        stem = song["stem"]
        cfg = "lines=small_model=w2v_large_mode=global_source=melroformer" if song["language"] == "English" \
            else "lines=small_model=xlsr_mode=global_source=melroformer"
        pred_path = Path("results/gpu") / cfg / f"{stem}.json"
        vocals = Path("results/stems/melroformer") / f"{stem}.wav"
        if not pred_path.exists() or not vocals.exists():
            continue
        total = sf.info(str(vocals)).duration
        human = [(float(r["start_time"]), float(r["end_time"]))
                 for r in csv.DictReader(open(g.DATA / "annotations/lines" / f"{stem}.csv"))]
        truth = mask_from_spans(human, total, args.gap)
        words = json.load(open(pred_path))["words"]
        lines = {}
        for w in words:
            lines.setdefault(w["line"], []).append(w)
        spans = sorted((v[0]["start"] / 1000, v[-1]["end"] / 1000) for v in lines.values())
        preds = {"aligner": mask_from_spans(spans, total, args.gap)}
        # Refinement with the vocal stem.
        db = vocal_activity(str(vocals), total)
        sung = np.zeros(len(truth), bool)
        for a, b in spans:
            sung[int(a / HOP):int(b / HOP) + 1] = True
        ref = np.median(db[sung[:len(db)]]) if sung[:len(db)].any() else db.max()
        loud = np.zeros(len(truth), bool)
        loud[:len(db)] = db > ref + args.quiet_db
        extended = []
        for a, b in spans:
            j = int(b / HOP)
            while j + 1 < len(loud) and loud[j + 1] and (j + 1 - int(b / HOP)) * HOP < 3.0:
                j += 1
            extended.append((a, j * HOP))
        refined = mask_from_spans(extended, total, args.gap)
        for a, b in sections(refined):
            seg = loud[int(a / HOP):int(b / HOP)]
            if len(seg) and seg.mean() > 0.6:
                refined[int(a / HOP):int(b / HOP)] = False
        preds["refined"] = refined
        true_secs = sections(truth)
        sec_total += len(true_secs)
        for k, m in preds.items():
            totals[k] += [np.sum(m & truth), np.sum(m & ~truth), np.sum(~m & truth)]
            ps = sections(m)
            for a, b in true_secs:
                if any(abs(a - c) <= 0.5 and abs(b - d) <= 0.5 for c, d in ps):
                    sec_hits[k] += 1
    for k, (tp, fp, fn) in totals.items():
        prec, rec = tp / max(1, tp + fp), tp / max(1, tp + fn)
        f1 = 2 * prec * rec / max(1e-9, prec + rec)
        print(f"{k:8s} gap≥{args.gap}s  precision {100 * prec:5.1f}%  recall {100 * rec:5.1f}%  F1 {100 * f1:5.1f}%  "
              f"sections with both edges ≤0.5s: {sec_hits[k]}/{sec_total} ({100 * sec_hits[k] / max(1, sec_total):.0f}%)")


if __name__ == "__main__":
    main()
