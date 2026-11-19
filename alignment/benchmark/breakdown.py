"""Split mms_win word errors into 'line misplaced' vs 'line right, word wrong'."""
import csv, json, sys, difflib
from pathlib import Path
from bench import DATA, romanize, human_onsets
songs = [r for r in csv.DictReader(open(DATA / "JamendoLyrics.csv")) if r["Language"] == "English"]
method = sys.argv[1] if len(sys.argv) > 1 else "mms_win"
tot = dict(words=0, bad=0, bad_line=0, bad_inline=0)
for s in songs:
    stem = Path(s["Filepath"]).stem
    pred = json.load(open(f"results/{method}/{stem}.json"))["words"]
    human = human_onsets(stem)
    hl = [float(r["start_time"]) for r in csv.DictReader(open(DATA / "annotations/lines" / f"{stem}.csv"))]
    order = [n for n, l in enumerate((DATA / "lyrics" / f"{stem}.txt").read_text().splitlines()) if l.strip()]
    line_of = {n: k for k, n in enumerate(order)}
    first = {}
    for w in pred: first.setdefault(w["line"], w["start"] / 1000)
    a = [romanize(w["word"]) for w in pred]; b = [t for t, _ in human]
    bad = bad_line = 0
    for blk in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
        for k in range(blk.size):
            w = pred[blk.a + k]; e = abs(w["start"] / 1000 - human[blk.b + k][1])
            tot["words"] += 1
            if e > 0.3:
                bad += 1
                if abs(first[w["line"]] - hl[line_of[w["line"]]]) > 0.5: bad_line += 1
    tot["bad"] += bad; tot["bad_line"] += bad_line; tot["bad_inline"] += bad - bad_line
    print(f"{stem[:34]:34s} words>0.3s {bad:4d}  in misplaced lines {bad_line:4d}  in well-placed lines {bad-bad_line:4d}")
t = tot
print(f"\nALL {t['words']} words: {t['bad']} off by >0.3s ({100*t['bad']/t['words']:.1f}%) — "
      f"{t['bad_line']} in misplaced lines ({100*t['bad_line']/t['words']:.1f}% of words), "
      f"{t['bad_inline']} inside well-placed lines ({100*t['bad_inline']/t['words']:.1f}% of words)")
