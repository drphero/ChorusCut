"""Run align_v2.py end to end on every JamendoLyrics song (reusing Mel-RoFormer stems) and score it.

usage: bench_v2.py run      # writes results/v2/<stem>.json
       bench_v2.py score    # starts, ends, flags, instrumental sections
"""
import csv, json, subprocess, sys, time
from pathlib import Path
import numpy as np
import bench, gpu_bench as g
OUT = Path("results/v2")

def run():
    OUT.mkdir(parents=True, exist_ok=True)
    for s in g.songs():
        out = OUT / f"{s['stem']}.json"
        if out.exists(): continue
        lyr = g.DATA / "lyrics" / f"{s['stem']}.txt"
        t = time.monotonic()
        r = subprocess.run([sys.executable, "-B", "-W", "ignore", str(Path(__file__).resolve().parent.parent / "experiments/align_v2.py"),
                            s["audio"], str(lyr), str(out), "--language", s["code"],
                            "--vocals", str(Path("results/stems/melroformer") / f"{s['stem']}.wav")], capture_output=True, text=True)
        print(f"{s['stem'][:36]:36s} {r.stdout.strip()[-110:] if r.returncode == 0 else 'FAILED ' + r.stderr[-200:]} ({time.monotonic()-t:.0f}s)", flush=True)

def score():
    import instrumental as ins
    starts, ok, flagged_err, ends, line_ends = [], [], [], [], []
    lines_total = lines_flagged = 0
    per_lang = {}
    tp = fp = fn = 0
    for s in g.songs():
        f = OUT / f"{s['stem']}.json"
        if not f.exists(): continue
        d = json.load(open(f)); w = d["words"]
        rows = list(csv.DictReader(open(g.DATA / "annotations/words" / f"{s['stem']}.csv")))
        toks = (g.DATA / "lyrics" / f"{s['stem']}.words.txt").read_text(encoding="utf-8").split()
        import difflib
        a = [bench.romanize(x["word"]) for x in w]; b = [bench.romanize(t) for t in toks]
        for blk in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
            for k in range(blk.size):
                x, r = w[blk.a + k], rows[blk.b + k]
                e = abs(x["start"] / 1000 - float(r["word_start"]))
                starts.append(e); per_lang.setdefault(s["language"], []).append(e)
                (flagged_err if x.get("flag") else ok).append(e)
                ends.append(abs(x["end"] / 1000 - float(r["word_end"])))
                if r["line_end"] not in ("", "nan"): line_ends.append(abs(x["end"] / 1000 - float(r["line_end"])))
        lines = {x["line"]: x.get("flag", False) for x in w}
        lines_total += len(lines); lines_flagged += sum(lines.values())
        human = [(float(r["start_time"]), float(r["end_time"])) for r in csv.DictReader(open(g.DATA / "annotations/lines" / f"{s['stem']}.csv"))]
        total = max(max(e for _, e in human), max(x["end"] for x in w) / 1000) + 5
        truth = ins.mask_from_spans(human, total, 2.0)
        pred = np.zeros_like(truth)
        for sec in d["sections"]: pred[int(sec["start"] / 1000 / ins.HOP):int(sec["end"] / 1000 / ins.HOP)] = True
        tp += np.sum(pred & truth); fp += np.sum(pred & ~truth); fn += np.sum(~pred & truth)
    pct = lambda v, t: 100 * np.mean(np.array(v) <= t)
    print(f"songs scored: word starts ≤0.1 {pct(starts,0.1):.1f}%  ≤0.3 {pct(starts,0.3):.1f}%  >1s {sum(np.array(starts)>1)}  "
          + "  ".join(f"{k[:2]} {pct(v,0.3):.1f}" for k, v in sorted(per_lang.items())))
    print(f"flags: {100*lines_flagged/lines_total:.1f}% of lines; unflagged words ≤0.3 {pct(ok,0.3):.1f}% (>1s {sum(np.array(ok)>1)}); flagged words ≤0.3 {pct(flagged_err,0.3):.1f}%")
    print(f"ends: word ends ≤0.3 {pct(ends,0.3):.1f}%  line ends ≤0.3 {pct(line_ends,0.3):.1f}%  ≤0.1 {pct(line_ends,0.1):.1f}%")
    print(f"instrumental ≥2s: precision {100*tp/max(1,tp+fp):.1f}%  recall {100*tp/max(1,tp+fn):.1f}%")

if __name__ == "__main__":
    run() if sys.argv[1] == "run" else score()
