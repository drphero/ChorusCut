"""Score lyric alignment variants against JamendoLyrics human word timings.

usage: bench.py [--languages English,French,...] [--methods shipped,mms_global,mms_win] [--limit N]

Writes per-song word timings to results/<method>/<song>.json and prints onset accuracy.
Onset error is |predicted word start − human word start|, pooled over all scored words.
"""
import argparse
import csv
import difflib
import json
import os
import re
import statistics
import sys
import time
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "experiments"))
import align  # noqa: E402  (the shipped worker's parser)
import hybrid  # noqa: E402
import whisper_lines  # noqa: E402

DATA = HERE / "jamendolyrics"
CODES = {"English": "en", "French": "fr", "German": "de", "Spanish": "es"}
WORD = re.compile(r"[^\W_]+(?:['’][^\W_]+)*")


def romanize(word):
    w = word.replace("’", "'").replace("ß", "ss").replace("æ", "ae").replace("œ", "oe")
    w = "".join(c for c in unicodedata.normalize("NFKD", w) if not unicodedata.combining(c))
    return re.sub(r"[^A-Za-z']", "", w).upper().strip("'")


def parse_any(text):
    """Like align.parse, but romanizes accented and non-English letters for MMS."""
    words = []
    for n, line in enumerate(text.splitlines()):
        if re.fullmatch(r"\s*\[[^\]]+\]\s*", line):
            continue
        for m in WORD.finditer(line):
            plain = romanize(m.group())
            if plain:
                words.append((n, m.group(), plain))
    return words


def human_onsets(stem):
    rows = list(csv.DictReader(open(DATA / "annotations" / "words" / f"{stem}.csv")))
    tokens = (DATA / "lyrics" / f"{stem}.words.txt").read_text(encoding="utf-8").split()
    if len(rows) != len(tokens):
        raise ValueError(f"{stem}: {len(rows)} timings for {len(tokens)} words")
    return [(romanize(t), float(r["word_start"])) for t, r in zip(tokens, rows)]


def score(predicted, human):
    """Pair predicted words (dicts) with human words by spelling, then measure onset errors."""
    a = [romanize(w["word"]) for w in predicted]
    b = [t for t, _ in human]
    errors = []
    for block in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
        for k in range(block.size):
            errors.append(abs(predicted[block.a + k]["start"] / 1000 - human[block.b + k][1]))
    return errors, len(b) - len(errors)


def summarize(errors):
    n = len(errors)
    within = lambda t: 100 * sum(e <= t for e in errors) / n  # noqa: E731
    return dict(words=n, mean=round(statistics.fmean(errors), 3), median=round(statistics.median(errors), 3),
                le01=round(within(0.1), 1), le03=round(within(0.3), 1), over1=sum(e > 1 for e in errors),
                worst=round(max(errors), 2))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--languages", default="English")
    p.add_argument("--methods", default="shipped,mms_global,mms_win")
    p.add_argument("--limit", type=int)
    p.add_argument("--songs", help="comma-separated file stems")
    args = p.parse_args()
    languages = args.languages.split(",")
    methods = args.methods.split(",")
    songs = [r for r in csv.DictReader(open(DATA / "JamendoLyrics.csv")) if r["Language"] in languages]
    songs = songs[:args.limit] if args.limit else songs
    if args.songs:
        songs = [r for r in songs if Path(r["Filepath"]).stem in args.songs.split(",")]
    models = {}
    if "shipped" in methods:
        models["base"] = hybrid.acoustic_model("base")
    if any(m.startswith("mms") for m in methods):
        models["mms"] = hybrid.acoustic_model("mms")
    whisper = None
    if any(m.startswith("mms_win") for m in methods):
        import stable_whisper
        whisper = stable_whisper.load_model("base", device="cpu",
                                            download_root=os.path.expanduser("~/.cache/whisper"))
    pooled = {m: [] for m in methods}
    per_song = {m: {} for m in methods}
    for song in songs:
        stem = Path(song["Filepath"]).stem
        audio = str(DATA / "subsets" / CODES[song["Language"]] / "mp3" / song["Filepath"])
        text = (DATA / "lyrics" / f"{stem}.txt").read_text(encoding="utf-8")
        human = human_onsets(stem)
        wave = hybrid.load_wave(audio)
        timings = {}
        for method in methods:
            started = time.monotonic()
            try:
                if method == "shipped":
                    if song["Language"] != "English":
                        continue
                    model, tokenize, sep = models["base"]
                    words = align.parse(text)
                    predicted = hybrid.align_words(hybrid.emissions(model, wave), 0, words, tokenize, sep)
                else:
                    model, tokenize, sep = models["mms"]
                    words = parse_any(text)
                    if "mms_logp" not in timings:
                        timings["mms_logp"] = hybrid.emissions(model, wave)
                    logp = timings["mms_logp"]
                    if method == "mms_win_voc":
                        # Whisper finds lines on the mix; MMS times words on the Demucs vocal stem.
                        vocals = HERE / "results" / "stems" / "htdemucs" / stem / "vocals.wav"
                        logp = hybrid.emissions(model, hybrid.load_wave(str(vocals)))
                    if method == "mms_global":
                        predicted = hybrid.align_words(logp, 0, words, tokenize, sep)
                    else:
                        lines = whisper_lines.lyric_lines(text)
                        windows = whisper_lines.find_lines(whisper, audio, lines, CODES[song["Language"]])
                        predicted = hybrid.align_windowed(logp, words, windows, tokenize, sep)
            except Exception as exc:  # Record the failure and keep benchmarking.
                print(f"  {method:10s} {stem}: FAILED {exc}", flush=True)
                per_song[method][stem] = {"error": str(exc)}
                continue
            out = HERE / "results" / method
            out.mkdir(parents=True, exist_ok=True)
            json.dump({"words": predicted}, open(out / f"{stem}.json", "w"), ensure_ascii=False)
            errors, unmatched = score(predicted, human)
            pooled[method] += errors
            s = summarize(errors)
            s.update(unmatched=unmatched, seconds=round(time.monotonic() - started, 1), language=song["Language"])
            per_song[method][stem] = s
            print(f"  {method:10s} {stem[:38]:38s} ≤0.3s {s['le03']:5.1f}%  median {s['median']:.3f}  "
                  f">1s {s['over1']:3d}  unmatched {unmatched}  {s['seconds']}s", flush=True)
    print()
    for method in methods:
        if pooled[method]:
            s = summarize(pooled[method])
            songs_ok = [v["le03"] for v in per_song[method].values() if "le03" in v]
            print(f"{method:10s} words {s['words']}  mean {s['mean']}s  median {s['median']}s  "
                  f"≤0.1s {s['le01']}%  ≤0.3s {s['le03']}%  >1s {s['over1']}  worst song ≤0.3s {min(songs_ok)}%")
    json.dump(per_song, open(HERE / "results" / f"summary_{'_'.join(languages)}.json", "w"), indent=1)


if __name__ == "__main__":
    main()
