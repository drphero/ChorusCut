"""Per-line diagnosis: human line start vs each method's first word."""
import csv, json, sys
from pathlib import Path
sys.path.insert(0, "."); from bench import romanize
stem = sys.argv[1]; methods = sys.argv[2].split(",")
D = Path("jamendolyrics")
lines = list(csv.DictReader(open(D / "annotations/lines" / f"{stem}.csv")))
preds = {m: json.load(open(f"results/{m}/{stem}.json"))["words"] for m in methods}
# Group predicted words by lyric line index (non-empty lines in order).
txt = [l for l in (D / "lyrics" / f"{stem}.txt").read_text().splitlines()]
order = [n for n, l in enumerate(txt) if l.strip()]
for k, row in enumerate(lines):
    n = order[k] if k < len(order) else None
    cells = []
    for m in methods:
        ws = [w for w in preds[m] if w["line"] == n]
        cells.append(f"{m} {ws[0]['start']/1000 - float(row['start_time']):+7.2f}" if ws else f"{m} —")
        if m == "mms_win" and ws and "whisper_start" in ws[0]:
            cells.append(f"whisper {ws[0]['whisper_start']/1000 - float(row['start_time']):+7.2f}")
    print(f"{k:3d} {float(row['start_time']):7.2f}  " + "  ".join(cells) + f"  | {row['lyrics_line'][:38]}")
