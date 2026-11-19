"""Cache model outputs once, then score many pipeline settings quickly.

usage: tune.py cache [--whisper base,turbo]   # emissions + Whisper line positions per English song
       tune.py sweep                          # score settings from the cache

The cache lives in results/cache/. MMS emissions are stored as float16 log-probabilities.
"""
import csv
import itertools
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402
from bench import DATA, hybrid, whisper_lines  # noqa: E402

CACHE = HERE / "results" / "cache"
TURBO_DIR = "/Users/brent/Haute Cuisine/production/models"  # existing large-v3-turbo.pt, read only


def english_songs():
    return [r for r in csv.DictReader(open(DATA / "JamendoLyrics.csv")) if r["Language"] == "English"]


def cache(whisper_names):
    CACHE.mkdir(parents=True, exist_ok=True)
    mms = hybrid.acoustic_model("mms")
    for name in whisper_names:
        import stable_whisper
        import stable_whisper.timing
        import torch
        import whisper.timing
        full = {"turbo": "large-v3-turbo"}.get(name, name)
        root = TURBO_DIR if name == "turbo" else os.path.expanduser("~/.cache/whisper")
        model = stable_whisper.load_model(full, device="cpu", download_root=root)
        if name == "turbo" and torch.backends.mps.is_available():
            original = whisper.timing.dtw
            stable_whisper.timing.dtw = lambda x: original(x.cpu())
            model.encoder.to("mps")
            model.decoder.to("mps")
        for song in english_songs():
            stem = Path(song["Filepath"]).stem
            out = CACHE / f"{stem}.lines_{name}.json"
            if out.exists():
                continue
            audio = str(DATA / "subsets" / "en" / "mp3" / song["Filepath"])
            text = (DATA / "lyrics" / f"{stem}.txt").read_text(encoding="utf-8")
            started = time.monotonic()
            try:
                lines = whisper_lines.find_lines(model, audio, whisper_lines.lyric_lines(text), "en")
            except Exception as exc:
                print(f"{name} {stem}: FAILED {exc}", flush=True)
                continue
            json.dump(lines, open(out, "w"))
            print(f"{name} {stem}: {time.monotonic() - started:.1f}s", flush=True)
        del model
    for song in english_songs():
        stem = Path(song["Filepath"]).stem
        sources = {"mix": DATA / "subsets" / "en" / "mp3" / song["Filepath"],
                   "voc": HERE / "results" / "stems" / "htdemucs" / stem / "vocals.wav"}
        for kind, path in sources.items():
            out = CACHE / f"{stem}.mms_{kind}.npy"
            if out.exists() or not path.exists():
                continue
            np.save(out, hybrid.emissions(mms[0], hybrid.load_wave(str(path))).astype(np.float16))
            print(f"mms {kind} {stem}", flush=True)
    if "--base" in sys.argv:
        base = hybrid.acoustic_model("base")
        for song in english_songs():
            stem = Path(song["Filepath"]).stem
            out = CACHE / f"{stem}.base_mix.npy"
            if not out.exists():
                path = DATA / "subsets" / "en" / "mp3" / song["Filepath"]
                np.save(out, hybrid.emissions(base[0], hybrid.load_wave(str(path))).astype(np.float16))
                print(f"base mix {stem}", flush=True)


def ensemble(mms_logp, base_logp, weight=0.5):
    """Mix base-model letter probabilities into MMS's vocabulary, frame by frame."""
    import torchaudio
    mms_ids = torchaudio.pipelines.MMS_FA.get_dict(star=None)
    base_labels = torchaudio.pipelines.WAV2VEC2_ASR_BASE_960H.get_labels()
    T = min(len(mms_logp), len(base_logp))
    pm = np.exp(mms_logp[:T].astype(np.float64))
    pb = np.exp(base_logp[:T].astype(np.float64))
    mapped = np.zeros_like(pm)
    for i, label in enumerate(base_labels):
        if label in ("-", "|", "<s>", "<pad>", "</s>", "<unk>"):
            mapped[:, 0] += pb[:, i]          # base blank and word separator count as MMS blank
        elif label.lower() in mms_ids:
            mapped[:, mms_ids[label.lower()]] += pb[:, i]
    mixed = (1 - weight) * pm + weight * mapped / mapped.sum(1, keepdims=True)
    return np.log(np.maximum(mixed, 1e-12)).astype(np.float32)


def sweep():
    _, tokenize, sep = hybrid.acoustic_model("mms")
    songs = english_songs()
    data = {}
    for song in songs:
        stem = Path(song["Filepath"]).stem
        text = (DATA / "lyrics" / f"{stem}.txt").read_text(encoding="utf-8")
        data[stem] = dict(words=bench.parse_any(text), human=bench.human_onsets(stem),
                          logp=np.load(CACHE / f"{stem}.mms_mix.npy").astype(np.float32),
                          lines={n: json.load(open(CACHE / f"{stem}.lines_{n}.json"))
                                 for n in ("base", "turbo") if (CACHE / f"{stem}.lines_{n}.json").exists()})
    weights = [float(w) for w in os.environ.get("ENSEMBLE", "0").split(",")]
    for d, song in zip(data.values(), songs):
        stem = Path(song["Filepath"]).stem
        d["base"] = (np.load(CACHE / f"{stem}.base_mix.npy").astype(np.float32)
                     if (CACHE / f"{stem}.base_mix.npy").exists() else None)
    grid = itertools.product(("base", "turbo"), weights)
    for lines_name, weight in grid:
        late_cost, slack = 0.0, 0.25
        pooled, worst = [], []
        for stem, d in data.items():
            if lines_name not in d["lines"]:
                continue
            logp = ensemble(d["logp"], d["base"], weight) if weight and d["base"] is not None else d["logp"]
            predicted = hybrid.align_windowed(logp, d["words"], d["lines"][lines_name], tokenize, sep)
            errors, _ = bench.score(predicted, d["human"])
            pooled += errors
            worst.append(bench.summarize(errors)["le03"])
        if pooled:
            s = bench.summarize(pooled)
            print(f"lines {lines_name:5s} base-model weight {weight:4.2f}: ≤0.1s {s['le01']:5.1f}%  "
                  f"≤0.3s {s['le03']:5.1f}%  >1s {s['over1']:4d}  mean {s['mean']:.3f}  worst song {min(worst):5.1f}%",
                  flush=True)


if __name__ == "__main__":
    if sys.argv[1] == "cache":
        names = sys.argv[sys.argv.index("--whisper") + 1].split(",") if "--whisper" in sys.argv else ["base"]
        cache(names)
    else:
        sweep()
