"""Per-line consensus across alignment runs, with review flags.

usage: consensus.py [--agree 0.25] [--need 2] CONFIG_DIR [CONFIG_DIR ...]

Each CONFIG_DIR is a results/gpu/<config> folder written by gpu_bench.py score.
All configs must come from the same lyric parse (CTC variants), so words line up by
index. For each line, the run whose first-word start is supported by the most other
runs (within --agree seconds) wins; each word's start is the median over the
supporting runs. A line is flagged for review when fewer than --need runs agree.

Reports overall onset accuracy, the share of lines flagged, and accuracy on flagged
and unflagged lines separately, which measures how much a user can trust what is
not highlighted.
"""
import argparse
import difflib
import json
import statistics
from pathlib import Path

import gpu_bench as g
import bench


def main():
    p = argparse.ArgumentParser()
    p.add_argument("configs", nargs="+")
    p.add_argument("--agree", type=float, default=0.25)
    p.add_argument("--need", type=int, default=2)
    p.add_argument("--languages")
    p.add_argument("--wordlevel", action="store_true", help="also flag a line when any word lacks agreement")
    p.add_argument("--per-song", action="store_true")
    args = p.parse_args()
    root = Path("results/gpu")
    langs = args.languages.split(",") if args.languages else None
    all_err, ok_err, flag_err = [], [], []
    lines_total = lines_flagged = 0
    per_lang = {}
    for song in g.songs(langs):
        runs = []
        for c in args.configs:
            f = root / c / f"{song['stem']}.json"
            if f.exists():
                runs.append(json.load(open(f))["words"])
        if len(runs) < 2 or len({len(r) for r in runs}) != 1:
            continue
        n = len(runs[0])
        by_line = {}
        for i, w in enumerate(runs[0]):
            by_line.setdefault(w["line"], []).append(i)
        chosen, flagged = [None] * n, [False] * n
        for idx in by_line.values():
            starts = [r[idx[0]]["start"] for r in runs]
            support = [sum(abs(s - t) <= 1000 * args.agree for t in starts) for s in starts]
            best = max(range(len(runs)), key=lambda k: support[k])  # ties keep the earlier config
            agreeing = [k for k in range(len(runs)) if abs(starts[k] - starts[best]) <= 1000 * args.agree]
            flag = support[best] < args.need
            words = []
            for i in idx:
                w = dict(runs[best][i])
                w["start"] = statistics.median(runs[k][i]["start"] for k in agreeing)
                words.append((i, w))
                # Word-level check: every word needs enough runs near the chosen start.
                if args.wordlevel and sum(abs(r[i]["start"] - w["start"]) <= 1000 * args.agree for r in runs) < args.need:
                    flag = True
            lines_total += 1
            lines_flagged += flag
            for i, w in words:
                chosen[i], flagged[i] = w, flag
        human = bench.human_onsets(song["stem"])
        a = [bench.romanize(w["word"]) for w in chosen]
        b = [t for t, _ in human]
        song_all, song_ok, song_lines = [], [], len(by_line)
        song_flagged = sum(flagged[idx[0]] for idx in by_line.values())
        for blk in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
            for k in range(blk.size):
                e = abs(chosen[blk.a + k]["start"] / 1000 - human[blk.b + k][1])
                all_err.append(e)
                song_all.append(e)
                per_lang.setdefault(song["language"], []).append(e)
                (flag_err if flagged[blk.a + k] else ok_err).append(e)
                if not flagged[blk.a + k]:
                    song_ok.append(e)
        if args.per_song:
            ok_le = 100 * sum(x <= 0.3 for x in song_ok) / max(1, len(song_ok))
            print(f"  {song['stem'][:34]:34s} overall {100 * sum(x <= 0.3 for x in song_all) / len(song_all):5.1f}%  "
                  f"flagged {song_flagged:2d}/{song_lines:2d} lines  unflagged {ok_le:5.1f}%  "
                  f"unflagged >1s {sum(x > 1 for x in song_ok)}")
    s = bench.summarize(all_err)
    ok = bench.summarize(ok_err) if ok_err else {"le03": 0, "words": 0, "over1": 0}
    fl = bench.summarize(flag_err) if flag_err else {"le03": 0, "words": 0}
    langs_text = "  ".join(f"{k[:2]} {bench.summarize(v)['le03']:.1f}" for k, v in sorted(per_lang.items()))
    print(f"agree≤{args.agree}s need {args.need}: overall ≤0.3 {s['le03']:.1f}% (>1s {s['over1']})  "
          f"flagged lines {100 * lines_flagged / max(1, lines_total):.1f}%  "
          f"unflagged words ≤0.3 {ok['le03']:.1f}% (>1s {ok['over1']}, {ok['words']} words)  "
          f"flagged words ≤0.3 {fl['le03']:.1f}%  [{langs_text}]")


if __name__ == "__main__":
    main()
