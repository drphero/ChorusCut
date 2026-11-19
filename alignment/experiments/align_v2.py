"""Candidate worker: Mel-RoFormer vocals → whole-song CTC alignment with several models → per-line vote.

usage: align_v2.py AUDIO LYRICS OUT.json [--language en] [--models parakeet,w2v_large,xlsr]
                   [--audiocpp DIR] [--vocals VOCALS.wav] [--device cuda|mps|cpu]

Models: Parakeet CTC 0.6B (CC-BY-4.0), wav2vec2 and XLSR-53 fine-tunes (Apache-2.0),
Mel-RoFormer (MIT): all usable commercially. A voter "name@mix" runs on the original mix
instead of the vocal stem. Output words carry "flag": true when any word in their line
has fewer than --need runs within --agree seconds of the chosen start. Word ends are
extended while the vocal stem stays within --hold-db of the song's singing level, and
"sections" lists instrumental gaps of at least --gap seconds.
"""
import argparse
import json
import statistics
import subprocess
import sys
import tempfile
import time
import unicodedata
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hybrid  # noqa: E402

CANON = ["<blank>"] + list("abcdefghijklmnopqrstuvwxyz'")
MODELS = {
    "w2v_large": {"en": "facebook/wav2vec2-large-960h-lv60-self"},
    "w2v_base": {"en": "facebook/wav2vec2-base-960h"},
    "parakeet": {"en": "nvidia/parakeet-ctc-0.6b"},
    "xlsr": {"en": "jonatasgrosman/wav2vec2-large-xlsr-53-english",
             "fr": "jonatasgrosman/wav2vec2-large-xlsr-53-french",
             "de": "jonatasgrosman/wav2vec2-large-xlsr-53-german",
             "es": "jonatasgrosman/wav2vec2-large-xlsr-53-spanish"},
}


def romanize(word):
    w = word.replace("’", "'").replace("ß", "ss").replace("æ", "ae").replace("œ", "oe")
    w = "".join(c for c in unicodedata.normalize("NFKD", w) if not unicodedata.combining(c))
    return "".join(c for c in w.lower() if c in CANON[1:]).strip("'")


def parse(text):
    import re
    words = []
    for n, line in enumerate(text.splitlines()):
        if re.fullmatch(r"\s*\[[^\]]+\]\s*", line):
            continue
        for m in re.finditer(r"[^\W_]+(?:['’][^\W_]+)*", line):
            plain = romanize(m.group())
            if plain:
                words.append((n, m.group(), plain))
    return words


def tokenize(plain):
    return [CANON.index(c) for c in plain if c in CANON[1:]]


def separate(audio, audiocpp, workdir):
    """Mel-Band RoFormer vocals via audio.cpp; returns the vocal WAV path."""
    cli = next(p for p in [Path(audiocpp) / "build/linux-cuda-release/bin/audiocpp_cli",
                           Path(audiocpp) / "build/macos-metal-release/bin/audiocpp_cli",
                           Path(audiocpp) / "build/bin/audiocpp_cli"] if p.exists())
    model = Path(audiocpp) / "models/Mel-Band-RoFormer-GGUF/mel-band-roformer-f16.gguf"
    backend = "cuda" if "cuda" in str(cli) else "metal" if "metal" in str(cli) else "cpu"
    mix = Path(workdir) / "mix.wav"
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", audio, "-ar", "44100", "-ac", "2", str(mix)], check=True)
    subprocess.run([str(cli), "--task", "sep", "--family", "mel_band_roformer", "--model", str(model), "--backend",
                    backend, "--audio", str(mix), "--out-dir", str(workdir)], check=True, capture_output=True)
    return str(Path(workdir) / "vocals.wav")


def emissions(repo, wave, device):
    import torch
    from huggingface_hub import hf_hub_download
    from transformers import AutoFeatureExtractor, Wav2Vec2ForCTC
    model = Wav2Vec2ForCTC.from_pretrained(repo).to(device).eval()
    if device == "cuda":
        model = model.half()
    fe = AutoFeatureExtractor.from_pretrained(repo)
    vocab = json.load(open(hf_hub_download(repo, "vocab.json")))
    x = wave.copy()
    if getattr(fe, "do_normalize", False):
        x = (x - x.mean()) / (x.std() + 1e-7)
    x = torch.from_numpy(x).to(device)
    if device == "cuda":
        x = x.half()
    stride, field = 320, 400
    count = (len(x) - field) // stride + 1
    chunks = []
    with torch.inference_mode():
        for first in range(0, count, 1500):
            last = min(first + 1500, count)
            left, right = max(0, first - 100), min(count, last + 100)
            logits = model(x[left * stride:(right - 1) * stride + field].unsqueeze(0)).logits[0].float()
            chunks.append(logits[first - left:first - left + (last - first)].log_softmax(-1).cpu().numpy())
    logp = np.concatenate(chunks)
    p = np.exp(logp.astype(np.float64))
    out = np.zeros((len(p), len(CANON)))
    for token, i in vocab.items():
        t = token.lower().replace("ß", "s").replace("’", "'")
        if len(t) == 1:
            t = "".join(c for c in unicodedata.normalize("NFKD", t) if not unicodedata.combining(c)) or t
        if i == 0 or token in ("<pad>", "<s>", "</s>", "<unk>", "|") or t not in CANON[1:]:
            out[:, 0] += p[:, i]
        else:
            out[:, CANON.index(t)] += p[:, i]
    out /= out.sum(1, keepdims=True)
    return np.log(np.maximum(out, 1e-12)).astype(np.float32)


def align_parakeet(repo, wave, words, device):
    """Subword CTC forced alignment with Parakeet; returns words in the same format as hybrid.align_words."""
    import torch
    from transformers import AutoProcessor, ParakeetForCTC
    processor = AutoProcessor.from_pretrained(repo)
    model = ParakeetForCTC.from_pretrained(repo, torch_dtype=torch.float32).to(device).eval()
    feats = processor.feature_extractor(wave, sampling_rate=16000, return_tensors="pt")
    with torch.inference_mode():
        logits = model(input_features=feats.input_features.to(device),
                       attention_mask=feats.attention_mask.to(device)).logits[0].float()
    logp = logits.log_softmax(-1).cpu().numpy()
    blank = model.config.pad_token_id
    order = [blank] + [i for i in range(logp.shape[1]) if i != blank]
    logp, remap = logp[:, order], {old: new for new, old in enumerate(order)}
    frame = len(wave) / 16000 / len(logp)
    tokens, owner = [], []
    for w, (_, _, plain) in enumerate(words):
        for t in processor.tokenizer.encode(" " + plain, add_special_tokens=False):
            tokens.append(remap[t])
            owner.append(w)
    spans = hybrid.viterbi(logp, tokens, [owner[i] == owner[i + 1] for i in range(len(tokens) - 1)])
    out = []
    for w, (line, original, _) in enumerate(words):
        mine = [sp for sp, o in zip(spans, owner) if o == w]
        out.append(dict(line=line, word=original, start=round(1000 * frame * mine[0][0]),
                        end=round(1000 * frame * mine[-1][1]), score=0.0))
    return out


def vocal_level(vocals, hop=0.02):
    import soundfile as sf
    x, sr = sf.read(vocals, always_2d=True)
    x = x.mean(1)
    h = int(sr * hop)
    n = len(x) // h
    return 20 * np.log10(np.sqrt((x[:n * h].reshape(n, h) ** 2).mean(1)) + 1e-9), hop


def extend_ends(words, db, hop, hold_db, cap=3.0):
    """Extend each word's end while the vocal stem stays loud (held notes), up to the next word."""
    n = len(db)
    sung = np.zeros(n, bool)
    for w in words:
        sung[int(w["start"] / 1000 / hop):int(w["end"] / 1000 / hop) + 1] = True
    loud = db > (np.median(db[sung[:n]]) if sung[:n].any() else db.max()) + hold_db
    for i, w in enumerate(words):
        limit = (words[i + 1]["start"] / 1000 - 0.02) if i + 1 < len(words) else n * hop
        j = int(w["end"] / 1000 / hop)
        while j + 1 < n and loud[j + 1] and (j + 1) * hop < min(limit, w["end"] / 1000 + cap):
            j += 1
        w["end"] = max(w["end"], round(1000 * j * hop))
    return loud


def instrumental(words, loud, hop, total, gap):
    """Gaps of at least `gap` seconds between lines where the vocal stem is mostly quiet."""
    lines = {}
    for w in words:
        lines.setdefault(w["line"], []).append(w)
    spans = sorted((v[0]["start"] / 1000, v[-1]["end"] / 1000) for v in lines.values())
    out, t = [], 0.0
    for a, b in spans + [(total, total)]:
        if a - t >= gap:
            seg = loud[int(t / hop):int(a / hop)]
            if not len(seg) or seg.mean() <= 0.6:
                out.append({"start": round(1000 * t), "end": round(1000 * a),
                            "kind": "intro" if t == 0 else "outro" if a >= total else "break"})
        t = max(t, b)
    return out


def vote(runs, agree, need):
    by_line = {}
    for i, w in enumerate(runs[0]):
        by_line.setdefault(w["line"], []).append(i)
    result = [None] * len(runs[0])
    for idx in by_line.values():
        starts = [r[idx[0]]["start"] for r in runs]
        support = [sum(abs(s - t) <= 1000 * agree for t in starts) for s in starts]
        best = max(range(len(runs)), key=lambda k: support[k])
        agreeing = [k for k in range(len(runs)) if abs(starts[k] - starts[best]) <= 1000 * agree]
        flag = support[best] < need
        chosen = []
        for i in idx:
            w = dict(runs[best][i])
            w["start"] = round(statistics.median(runs[k][i]["start"] for k in agreeing))
            w["end"] = max(w["start"] + 20, round(statistics.median(runs[k][i]["end"] for k in agreeing)))
            if sum(abs(r[i]["start"] - w["start"]) <= 1000 * agree for r in runs) < need:
                flag = True
            chosen.append((i, w))
        for i, w in chosen:
            w["flag"] = flag
            result[i] = w
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("audio")
    p.add_argument("lyrics")
    p.add_argument("out")
    p.add_argument("--language", default="en")
    p.add_argument("--models", default="parakeet,w2v_large,xlsr,xlsr@mix")
    p.add_argument("--audiocpp", default="/workspace/audio.cpp")
    p.add_argument("--vocals")
    p.add_argument("--device", default="cuda")
    p.add_argument("--agree", type=float, default=0.25)
    p.add_argument("--need", type=int, default=2)
    p.add_argument("--hold-db", type=float, default=-12)
    p.add_argument("--gap", type=float, default=2.0)
    args = p.parse_args()
    timings = {}
    started = time.monotonic()
    with tempfile.TemporaryDirectory() as tmp:
        vocals = args.vocals or separate(args.audio, args.audiocpp, tmp)
        timings["separate"] = round(time.monotonic() - started, 1)
        waves = {"voc": hybrid.load_wave(vocals).numpy(), "mix": hybrid.load_wave(args.audio).numpy()}
        db, hop = vocal_level(vocals)
    words = parse(open(args.lyrics, encoding="utf-8").read())
    runs = []
    for spec in args.models.split(","):
        name, _, source = spec.partition("@")
        repo = MODELS[name].get(args.language)
        if not repo:
            continue
        t = time.monotonic()
        wave = waves[source or "voc"]
        if name == "parakeet":
            runs.append(align_parakeet(repo, wave, words, args.device))
        else:
            runs.append(hybrid.align_words(emissions(repo, wave, args.device), 0, words, tokenize, None))
        timings[spec] = round(time.monotonic() - t, 1)
    result = vote(runs, args.agree, args.need) if len(runs) > 1 else runs[0]
    loud = extend_ends(result, db, hop, args.hold_db)
    sections = instrumental(result, loud, hop, len(db) * hop, args.gap)
    flagged = len({w["line"] for w in result if w.get("flag")})
    json.dump({"pipeline": "align_v2", "models": args.models, "timings_s": timings, "words": result,
               "sections": sections}, open(args.out, "w"), ensure_ascii=False)
    print(f"{len(result)} words, {flagged} of {len({w['line'] for w in result})} lines flagged, "
          f"{len(sections)} instrumental sections, timings {timings}")


if __name__ == "__main__":
    main()
