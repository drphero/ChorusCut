"""Word-end and line-end accuracy against JamendoLyrics human word_end / line end times."""
import csv, difflib, json, sys
from pathlib import Path
import numpy as np
import bench, gpu_bench as g

def run(cfg_for, langs=None):
    we, le, ws = [], [], []
    for s in g.songs(langs):
        d = Path("results/gpu") / cfg_for(s) / f"{s['stem']}.json"
        if not d.exists(): continue
        pred = json.load(open(d))["words"]
        rows = list(csv.DictReader(open(g.DATA / "annotations/words" / f"{s['stem']}.csv")))
        toks = (g.DATA / "lyrics" / f"{s['stem']}.words.txt").read_text(encoding="utf-8").split()
        a = [bench.romanize(w["word"]) for w in pred]; b = [bench.romanize(t) for t in toks]
        for blk in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
            for k in range(blk.size):
                w, r = pred[blk.a + k], rows[blk.b + k]
                e = w["end"] / 1000 - float(r["word_end"])
                we.append(e); ws.append(w["start"] / 1000 - float(r["word_start"]))
                if r["line_end"] not in ("", "nan"): le.append(w["end"] / 1000 - float(r["line_end"]))
    for name, e in (("word starts", ws), ("word ends", we), ("line ends", le)):
        e = np.array(e)
        print(f"  {name:11s} n={len(e):5d}  ≤0.1 {100*np.mean(np.abs(e)<=0.1):5.1f}%  ≤0.3 {100*np.mean(np.abs(e)<=0.3):5.1f}%  "
              f"signed median {np.median(e):+.3f}s  early>0.3 {100*np.mean(e<-0.3):4.1f}%  late>0.3 {100*np.mean(e>0.3):4.1f}%")

best = lambda s: ("lines=small_model=w2v_large_mode=global_source=melroformer" if s["language"] == "English"
                  else "lines=small_model=xlsr_mode=global_source=melroformer")
print("best per language (whole song, vocals):"); run(best)

# ---- end refinement with the Mel-RoFormer vocal stem
import soundfile as sf
def refine(pred, vocals, quiet_db, cap=3.0, hop=0.02):
    x, sr = sf.read(vocals, always_2d=True); x = x.mean(1); h = int(sr * hop); n = len(x) // h
    db = 20 * np.log10(np.sqrt((x[:n * h].reshape(n, h) ** 2).mean(1)) + 1e-9)
    starts = [w["start"] for w in pred]
    sung = np.zeros(n, bool)
    for w in pred: sung[int(w["start"] / 1000 / hop):int(w["end"] / 1000 / hop) + 1] = True
    loud = db > np.median(db[sung[:n]]) + quiet_db
    out = []
    for i, w in enumerate(pred):
        w = dict(w); limit = (starts[i + 1] / 1000 - 0.02) if i + 1 < len(pred) else n * hop
        j = int(w["end"] / 1000 / hop)
        while j + 1 < n and loud[j + 1] and (j + 1) * hop < min(limit, w["end"] / 1000 + cap): j += 1
        w["end"] = max(w["end"], round(1000 * j * hop)); out.append(w)
    return out

if len(sys.argv) > 1:
    import os
    for q in [float(v) for v in sys.argv[1].split(",")]:
        tmpdir = Path(f"results/gpu/_refined_{q}")
        for s in g.songs():
            cfg = best(s); src = Path("results/gpu") / cfg / f"{s['stem']}.json"
            if not src.exists(): continue
            (tmpdir / cfg).mkdir(parents=True, exist_ok=True)
            pred = refine(json.load(open(src))["words"], f"results/stems/melroformer/{s['stem']}.wav", q)
            json.dump({"words": pred}, open(tmpdir / cfg / f"{s['stem']}.json", "w"))
        print(f"refined ends, vocal louder than singing median {q:+.0f} dB:")
        run(lambda s, t=tmpdir: str(Path(t).relative_to("results/gpu") / best(s)))
