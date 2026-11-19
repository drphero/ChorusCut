"""Experiment: compare word onsets of an alignment with a reference.

usage: compare.py RESULT.json REFERENCE.json [--lines]

RESULT is hybrid.py/align.py output. REFERENCE is either the same format or a
stable-ts JSON (segments with words, times in seconds) such as Haute Cuisine's
production/alignment_final.json. Hyphenated words are split as the CTC parser does.
"""
import json
import re
import statistics
import sys


def norm(word):
    return re.sub(r"[^a-z0-9]", "", word.lower())


def onsets(path):
    data = json.load(open(path))
    out = []
    if "segments" in data:
        for line, segment in enumerate(data["segments"]):
            for w in segment["words"]:
                parts = [p for p in re.split(r"[\s\-—]+", w["word"].strip()) if norm(p)]
                for k, part in enumerate(parts):
                    out.append((line, norm(part), w["start"] if k == 0 else None))
        return out, False
    for w in data["words"]:
        out.append((w["line"], norm(w["word"]), w["start"] / 1000))
    return out, True


def main():
    result, _ = onsets(sys.argv[1])
    reference, raw_lines = onsets(sys.argv[2])
    if [r[1] for r in result] != [r[1] for r in reference]:
        raise SystemExit("word sequences differ")
    pairs = [(abs(a[2] - b[2]), a, b) for a, b in zip(result, reference) if b[2] is not None]
    errors = [p[0] for p in pairs]
    bands = "  ".join(f"≤{t}s {100 * sum(e <= t for e in errors) / len(errors):5.1f}%"
                      for t in (0.05, 0.1, 0.2, 0.3, 0.5))
    # Line starts: the first word of each line in the result.
    first = {}
    for e, a, b in pairs:
        first.setdefault(a[0], e)
    lines_ok = sum(e <= 0.3 for e in first.values())
    print(f"median {statistics.median(errors):.3f}s  {bands}  lines≤0.3s {lines_ok}/{len(first)}  "
          f"worst {max(errors):.2f}s  words>1s {sum(e > 1 for e in errors)}")
    if "--lines" in sys.argv:
        for line, e in first.items():
            if e > 0.3:
                print(f"  line {line}: first word off by {e:.2f}s")


if __name__ == "__main__":
    main()
