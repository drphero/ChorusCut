"""GPU benchmark: cache Whisper line positions and CTC emissions, then score many pipelines.

usage: gpu_bench.py lines  --whisper openai/whisper-base,openai/whisper-large-v3-turbo,...
       gpu_bench.py ctc    --models mms,w2v_base,w2v_large [--source mix|voc]
       gpu_bench.py qwen   --lines NAME [--source mix|voc]   # audio.cpp Qwen3-ForcedAligner per line window
       gpu_bench.py score  --config "lines=large-v3 model=ens:w2v_large+w2v_base mode=win" ...

All models load through huggingface_hub (transformers / stable-ts load_hf_whisper) or
audio.cpp's model manager. CTC models are mapped to one canonical alphabet
(blank, a–z, apostrophe), so any weighted mixture of models can be aligned the same way.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "experiments"))
import bench  # noqa: E402
import hybrid  # noqa: E402
import whisper_lines  # noqa: E402

DATA = bench.DATA
CACHE = HERE / "results" / "gpu_cache"
CODES = bench.CODES
CANON = ["<blank>"] + list("abcdefghijklmnopqrstuvwxyz'")
CTC_REPOS = {
    "mms": "MahmoudAshraf/mms-300m-1130-forced-aligner",       # CC-BY-NC-4.0
    "w2v_base": "facebook/wav2vec2-base-960h",                  # Apache-2.0, English
    "w2v_large": "facebook/wav2vec2-large-960h-lv60-self",      # Apache-2.0, English
    # Apache-2.0 XLSR-53 fine-tunes, one per language; cached under the name "xlsr".
    "xlsr": {"en": "jonatasgrosman/wav2vec2-large-xlsr-53-english",
             "fr": "jonatasgrosman/wav2vec2-large-xlsr-53-french",
             "de": "jonatasgrosman/wav2vec2-large-xlsr-53-german",
             "es": "jonatasgrosman/wav2vec2-large-xlsr-53-spanish"},
}
AUDIOCPP = Path("/workspace/audio.cpp")
QWEN_MODEL = AUDIOCPP / "models/Qwen3-ForcedAligner-0.6B-GGUF/qwen3-forced-aligner-0.6b-f16.gguf"


def songs(languages=None):
    rows = [r for r in csv.DictReader(open(DATA / "JamendoLyrics.csv"))]
    if languages:
        rows = [r for r in rows if r["Language"] in languages]
    out = []
    for r in rows:
        stem = Path(r["Filepath"]).stem
        out.append(dict(stem=stem, language=r["Language"], code=CODES[r["Language"]],
                        audio=str(DATA / "subsets" / CODES[r["Language"]] / "mp3" / r["Filepath"]),
                        text=(DATA / "lyrics" / f"{stem}.txt").read_text(encoding="utf-8")))
    return out


def stem_audio(song, source):
    if source == "mix":
        return song["audio"]
    return str(HERE / "results" / "stems" / source / f"{song['stem']}.wav")


def tokenize(plain):
    return [CANON.index(c) for c in plain.lower() if c in CANON[1:]]


# ---------------------------------------------------------------- caching stages
def cache_lines(names):
    import stable_whisper
    for repo in names:
        short = repo.split("/")[-1].replace("whisper-", "")
        model = stable_whisper.load_hf_whisper(repo, device="cuda")
        for song in songs():
            out = CACHE / f"{song['stem']}.lines_{short}.json"
            if out.exists():
                continue
            started = time.monotonic()
            try:
                lines = whisper_lines.find_lines(model, song["audio"], whisper_lines.lyric_lines(song["text"]),
                                                 song["code"])
            except Exception as exc:
                print(f"lines {short} {song['stem']}: FAILED {exc}", flush=True)
                continue
            json.dump(lines, open(out, "w"))
            print(f"lines {short} {song['stem']}: {time.monotonic() - started:.1f}s", flush=True)
        del model


def canonical(logp, vocab):
    """Fold a model's CTC log-probabilities into CANON (specials and word separators count as blank)."""
    p = np.exp(logp.astype(np.float64))
    out = np.zeros((len(p), len(CANON)))
    import unicodedata
    for token, i in vocab.items():
        t = token.lower().replace("ß", "s").replace("’", "'")
        if len(t) == 1:
            t = "".join(c for c in unicodedata.normalize("NFKD", t) if not unicodedata.combining(c)) or t
        if i == 0 or token in ("<pad>", "<s>", "</s>", "<unk>", "<blank>", "|"):
            out[:, 0] += p[:, i]
        elif t in CANON[1:]:
            out[:, CANON.index(t)] += p[:, i]
        else:
            out[:, 0] += p[:, i]
    out /= out.sum(1, keepdims=True)
    return np.log(np.maximum(out, 1e-12)).astype(np.float16)


def cache_ctc(names, source):
    import torch
    from transformers import AutoFeatureExtractor, Wav2Vec2ForCTC
    from huggingface_hub import hf_hub_download
    jobs = []
    for name in names:
        repos = CTC_REPOS[name]
        if isinstance(repos, dict):
            for code, repo in repos.items():
                jobs.append((name, repo, [l for l, c in CODES.items() if c == code]))
        else:
            jobs.append((name, repos, None if name == "mms" else ["English"]))
    for name, repo, langs in jobs:
        model = Wav2Vec2ForCTC.from_pretrained(repo).to("cuda").eval().half()
        fe = AutoFeatureExtractor.from_pretrained(repo)
        vocab = json.load(open(hf_hub_download(repo, "vocab.json")))
        if vocab and isinstance(next(iter(vocab.values())), dict):
            vocab = vocab.get("en", next(iter(vocab.values())))
        for song in songs(langs):
            out = CACHE / f"{song['stem']}.{name}_{source}.npy"
            path = stem_audio(song, source)
            if out.exists() or not Path(path).exists():
                continue
            wave = hybrid.load_wave(path).numpy()
            if getattr(fe, "do_normalize", False):
                wave = (wave - wave.mean()) / (wave.std() + 1e-7)
            x = torch.from_numpy(wave).to("cuda").half()
            stride, field = 320, 400
            count = (len(x) - field) // stride + 1
            chunks = []
            with torch.inference_mode():
                for first in range(0, count, 1500):
                    last = min(first + 1500, count)
                    left, right = max(0, first - 100), min(count, last + 100)
                    logits = model(x[left * stride:(right - 1) * stride + field].unsqueeze(0)).logits[0].float()
                    chunks.append(logits[first - left:first - left + (last - first)].log_softmax(-1).cpu().numpy())
            np.save(out, canonical(np.concatenate(chunks), vocab))
            print(f"ctc {name} {source} {song['stem']}", flush=True)
        del model
        torch.cuda.empty_cache()


def windows_for(lines_list, pad=0.75):
    """Independent per-line windows (seconds), same bounds as hybrid.align_windowed."""
    out = []
    for k, info in enumerate(lines_list):
        n = len(info.get("text", "").split()) or 8
        a = max(0.0, info["start"] - pad)
        b = min(info["end"] + pad, info["start"] + pad + 1.5 + 0.6 * n)
        if k + 1 < len(lines_list):
            b = min(b, lines_list[k + 1]["start"] + 0.5)
        out.append((a, max(b, a + 0.5)))
    return out


def cache_qwen(lines_name, source):
    """Qwen3-ForcedAligner (audio.cpp, CUDA) on each Whisper line window; one CLI session per song."""
    cli = AUDIOCPP / "build/linux-cuda-release/bin/audiocpp_cli"
    for song in songs():
        out = CACHE / f"{song['stem']}.qwen_{lines_name}_{source}.json"
        lines_path = CACHE / f"{song['stem']}.lines_{lines_name}.json"
        if out.exists() or not lines_path.exists():
            continue
        lines_list = json.load(open(lines_path))
        started = time.monotonic()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            pcm = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", stem_audio(song, source), "-ac", "1",
                                  "-ar", "16000", "-f", "s16le", "pipe:1"], stdout=subprocess.PIPE, check=True).stdout
            audio = np.frombuffer(pcm, dtype=np.int16)
            requests = []
            import wave as wavmod
            for k, ((a, b), info) in enumerate(zip(windows_for(lines_list), lines_list)):
                seg = audio[int(a * 16000):int(b * 16000)]
                with wavmod.open(str(tmp / f"w{k}.wav"), "wb") as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(16000)
                    w.writeframes(seg.tobytes())
                requests.append({"id": f"w{k}", "audio": f"w{k}.wav", "text": info["text"], "language": song["code"]})
            (tmp / "seq.json").write_text(json.dumps({"requests": requests}))
            run = subprocess.run([str(cli), "--task", "align", "--family", "qwen3_forced_aligner", "--model",
                                  str(QWEN_MODEL), "--backend", "cuda", "--request-sequence", str(tmp / "seq.json"),
                                  "--words-out", str(tmp / "words.json")], capture_output=True, text=True, cwd=tmp)
            words = []
            for k, ((a, _), info) in enumerate(zip(windows_for(lines_list), lines_list)):
                f = tmp / f"words_w{k}.json"
                if not f.exists():
                    continue
                for w in json.load(open(f)):
                    words.append(dict(line=info["line"], word=w["word"], start=round(1000 * (a + w["start_sample"] / 16000)),
                                      end=round(1000 * (a + w["end_sample"] / 16000)), score=0.0))
            if not words:
                print(f"qwen {song['stem']}: FAILED {run.stderr[-300:]}", flush=True)
                continue
        json.dump({"words": words}, open(out, "w"))
        print(f"qwen {lines_name} {source} {song['stem']}: {len(words)} words {time.monotonic() - started:.1f}s",
              flush=True)


# ---------------------------------------------------------------- scoring
def load_logp(stem, spec, source):
    """spec: 'mms' or 'ens:w2v_large*0.75+w2v_base*0.25' (weights default to equal)."""
    parts = spec[4:].split("+") if spec.startswith("ens:") else [spec]
    mats, weights = [], []
    for part in parts:
        name, _, weight = part.partition("*")
        path = CACHE / f"{stem}.{name}_{source}.npy"
        if not path.exists():
            return None
        mats.append(np.exp(np.load(path).astype(np.float64)))
        weights.append(float(weight) if weight else 1.0)
    T = min(len(m) for m in mats)
    mix = sum(w * m[:T] for w, m in zip(weights, mats)) / sum(weights)
    return np.log(np.maximum(mix, 1e-12)).astype(np.float32)


def predict(song, cfg):
    words = bench.parse_any(song["text"])
    if cfg["model"].startswith("qwen"):
        path = CACHE / f"{song['stem']}.qwen_{cfg['lines']}_{cfg.get('source', 'mix')}.json"
        return json.load(open(path))["words"] if path.exists() else None
    logp = load_logp(song["stem"], cfg["model"], cfg.get("source", "mix"))
    if logp is None:
        return None
    if cfg["mode"] == "global":
        return hybrid.align_words(logp, 0, words, tokenize, None)
    path = CACHE / f"{song['stem']}.lines_{cfg['lines']}.json"
    if not path.exists():
        return None
    return hybrid.align_windowed(logp, words, json.load(open(path)), tokenize, None)


def score(configs, languages):
    for text in configs:
        cfg = dict(kv.split("=", 1) for kv in text.split())
        per_lang, pooled, worst, missing = {}, [], [], 0
        for song in songs(languages):
            try:
                predicted = predict(song, cfg)
            except Exception as exc:
                print(f"  {song['stem']}: {exc}")
                predicted = None
            if predicted is None:
                missing += 1
                continue
            errors, _ = bench.score(predicted, bench.human_onsets(song["stem"]))
            pooled += errors
            per_lang.setdefault(song["language"], []).extend(errors)
            worst.append(bench.summarize(errors)["le03"])
            out = HERE / "results" / "gpu" / text.replace(" ", "_").replace(":", "-").replace("*", "x")
            out.mkdir(parents=True, exist_ok=True)
            json.dump({"words": predicted}, open(out / f"{song['stem']}.json", "w"))
        if not pooled:
            print(f"{text}: no songs scored")
            continue
        s = bench.summarize(pooled)
        langs = "  ".join(f"{k[:2]} {bench.summarize(v)['le03']:.1f}" for k, v in sorted(per_lang.items()))
        print(f"{text:58s} ≤0.1 {s['le01']:5.1f}%  ≤0.3 {s['le03']:5.1f}%  >1s {s['over1']:5d}  "
              f"worst {min(worst):5.1f}%  songs {len(worst)} (missing {missing})  [{langs}]", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("stage", choices=["lines", "ctc", "qwen", "score"])
    p.add_argument("--whisper", default="openai/whisper-base")
    p.add_argument("--models", default="mms,w2v_base,w2v_large")
    p.add_argument("--source", default="mix")
    p.add_argument("--lines", default="base")
    p.add_argument("--languages")
    p.add_argument("--config", action="append", default=[])
    args = p.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    languages = args.languages.split(",") if args.languages else None
    if args.stage == "lines":
        cache_lines(args.whisper.split(","))
    elif args.stage == "ctc":
        cache_ctc(args.models.split(","), args.source)
    elif args.stage == "qwen":
        cache_qwen(args.lines, args.source)
    else:
        score(args.config, languages)


if __name__ == "__main__":
    main()
