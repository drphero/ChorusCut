"""How often do independent line-start estimates agree, and is the agreement right?"""
import csv, json, statistics, sys
from pathlib import Path
D = Path("jamendolyrics")
songs = [r for r in csv.DictReader(open(D / "JamendoLyrics.csv")) if r["Language"] == "English"]
tot = dict(lines=0, anchored=0, anchor_right=0, any_right=0)
for s in songs:
    stem = Path(s["Filepath"]).stem
    human = [float(r["start_time"]) for r in csv.DictReader(open(D / "annotations/lines" / f"{stem}.csv"))]
    order = [n for n, l in enumerate((D / "lyrics" / f"{stem}.txt").read_text().splitlines()) if l.strip()]
    first = {}
    for m in ("shipped", "mms_global", "mms_win"):
        for w in json.load(open(f"results/{m}/{stem}.json"))["words"]:
            first.setdefault(m, {}).setdefault(w["line"], w)
    a_cnt = a_ok = any_ok = 0
    for k, h in enumerate(human):
        n = order[k]
        est = [first["shipped"][n]["start"] / 1000, first["mms_global"][n]["start"] / 1000,
               first["mms_win"][n]["whisper_start"] / 1000]
        agree = [(i, j) for i in range(3) for j in range(i + 1, 3) if abs(est[i] - est[j]) <= 0.3]
        any_ok += any(abs(e - h) <= 0.3 for e in est)
        if agree:
            a_cnt += 1
            i, j = agree[0]
            a_ok += abs((est[i] + est[j]) / 2 - h) <= 0.3
    tot["lines"] += len(human); tot["anchored"] += a_cnt; tot["anchor_right"] += a_ok; tot["any_right"] += any_ok
    print(f"{stem[:34]:34s} lines {len(human):3d}  anchored {100*a_cnt/len(human):5.1f}%  anchors right {100*a_ok/max(1,a_cnt):5.1f}%  some estimate right {100*any_ok/len(human):5.1f}%")
t = tot
print(f"\nALL: {t['lines']} lines, anchored {100*t['anchored']/t['lines']:.1f}%, anchors right {100*t['anchor_right']/t['anchored']:.1f}%, at least one estimate right {100*t['any_right']/t['lines']:.1f}%")
