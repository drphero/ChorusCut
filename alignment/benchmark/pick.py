"""Per-line choice between mms_global and mms_win: oracle ceiling vs simple rules."""
import csv, json, difflib
from pathlib import Path
from bench import DATA, romanize, human_onsets, summarize
songs = list(csv.DictReader(open(DATA / "JamendoLyrics.csv")))
res = {k: [] for k in ("global", "win", "oracle", "score", "agree_or_score")}
for s in songs:
    stem = Path(s["Filepath"]).stem
    try:
        g = json.load(open(f"results/mms_global/{stem}.json"))["words"]
        w = json.load(open(f"results/mms_win/{stem}.json"))["words"]
    except FileNotFoundError:
        continue
    human = human_onsets(stem)
    a = [romanize(x["word"]) for x in g]; b = [t for t, _ in human]
    match = {}
    for blk in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
        for k in range(blk.size): match[blk.a + k] = human[blk.b + k][1]
    lines = {}
    for i, (x, y) in enumerate(zip(g, w)):
        lines.setdefault(x["line"], []).append(i)
    for idx in lines.values():
        eg = [abs(g[i]["start"] / 1000 - match[i]) for i in idx if i in match]
        ew = [abs(w[i]["start"] / 1000 - match[i]) for i in idx if i in match]
        if not eg: continue
        res["global"] += eg; res["win"] += ew
        res["oracle"] += eg if sum(eg) < sum(ew) else ew
        sg = sum(g[i]["score"] for i in idx) / len(idx); sw = sum(w[i]["score"] for i in idx) / len(idx)
        res["score"] += eg if sg > sw else ew
        agree = abs(g[idx[0]]["start"] - w[idx[0]]["start"]) <= 300
        res["agree_or_score"] += ew if agree else (eg if sg > sw else ew)
for k, v in res.items():
    s = summarize(v); print(f"{k:15s} ≤0.1s {s['le01']:5.1f}%  ≤0.3s {s['le03']:5.1f}%  >1s {s['over1']:5d}")
