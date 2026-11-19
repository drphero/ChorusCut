"""Chorus Cut lyric alignment: vocal stem → whole-song CTC with several models → per-line vote.

This is alignment/experiments/align_v2.py made portable: vocal separation runs in PyTorch,
so every step runs on an NVIDIA GPU (CUDA) or Apple silicon (MPS), on any OS ComfyUI supports. The models are licensed for commercial
use: Mel-Band RoFormer vocals (MIT), Parakeet CTC 0.6B (CC-BY-4.0), wav2vec2-large-960h and
the XLSR-53 fine-tunes (Apache-2.0).

The result has the format Chorus Cut imports: {"words": [...], "sections": [...]} where each
word is {line, word, start, end (ms), flag?} and sections are instrumental gaps.
"""
import json
import re
import statistics
import time
import unicodedata

import numpy as np
import torch

CANON = ["<blank>"] + list("abcdefghijklmnopqrstuvwxyz'")
FRAME = 0.02  # seconds per wav2vec2 emission frame
MODELS = {
    "w2v_large": {"en": "facebook/wav2vec2-large-960h-lv60-self"},
    # parakeet-ctc-1.1b scored the same in this four-voter pipeline (2026-11-19) and is slower.
    "parakeet": {"en": "nvidia/parakeet-ctc-0.6b"},
    "xlsr": {"en": "jonatasgrosman/wav2vec2-large-xlsr-53-english",
             "fr": "jonatasgrosman/wav2vec2-large-xlsr-53-french",
             "de": "jonatasgrosman/wav2vec2-large-xlsr-53-german",
             "es": "jonatasgrosman/wav2vec2-large-xlsr-53-spanish"},
}
DEFAULT_VOTERS = "parakeet,w2v_large,xlsr,xlsr@mix"
LANGUAGES = ["en", "fr", "de", "es"]


def pick_device(preference="auto"):
    """ComfyUI always runs on a GPU here: NVIDIA (CUDA) or Apple silicon (MPS). There is no CPU path."""
    cuda = torch.cuda.is_available()
    mps = bool(getattr(torch.backends, "mps", None) and torch.backends.mps.is_available())
    if preference == "auto":
        preference = "cuda" if cuda else "mps" if mps else ""
    if preference == "cuda" and cuda or preference == "mps" and mps:
        return preference
    raise RuntimeError("Chorus Cut alignment needs a GPU: an NVIDIA card with CUDA, or Apple silicon.")


# ---------------------------------------------------------------- lyrics

def romanize(word):
    w = word.replace("’", "'").replace("ß", "ss").replace("æ", "ae").replace("œ", "oe")
    w = "".join(c for c in unicodedata.normalize("NFKD", w) if not unicodedata.combining(c))
    return "".join(c for c in w.lower() if c in CANON[1:]).strip("'")


def parse(text):
    """(line number, word as written, romanized) for every sung word; [Section] lines are skipped."""
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


# ---------------------------------------------------------------- forced alignment

def viterbi(logp, tokens, internal):
    """Best monotonic path through alternating blanks and letters.

    Edge blanks are free (lead-in and lead-out). A blank between two letters of the same
    word costs a little, so a word cannot be stretched across an instrumental gap.
    Returns (first_frame, end_frame) for each token.
    """
    state = np.zeros(2 * len(tokens) + 1, dtype=np.intp)
    state[1::2] = tokens
    previous = np.full(len(state), -np.inf, dtype=np.float32)
    previous[0] = 0
    cost = np.zeros(len(state), dtype=np.float32)
    cost[2:-1:2] = 0.12 * np.asarray(internal, dtype=np.float32)
    can_skip = (state[2:] != 0) & (state[2:] != state[:-2])
    trace = np.empty((len(logp), len(state)), dtype=np.uint8)
    for t, frame in enumerate(logp):
        best = previous.copy()
        direction = trace[t]
        direction.fill(0)
        advance = previous[:-1] > best[1:]
        best[1:] = np.maximum(best[1:], previous[:-1])
        direction[1:][advance] = 1
        skip = can_skip & (previous[:-2] > best[2:])
        best[2:][skip] = previous[:-2][skip]
        direction[2:][skip] = 2
        emission = frame[state] - cost
        emission[0] = emission[-1] = frame.max()
        previous = best + emission
    pos = len(state) - 2 + int(previous[-1] > previous[-2])
    if not np.isfinite(previous[pos]):
        raise ValueError("The song is too short for these lyrics.")
    path = np.empty(len(logp), dtype=np.intp)
    for t in range(len(path) - 1, -1, -1):
        path[t] = pos
        pos -= int(trace[t, pos])
    spans = []
    for i in range(len(tokens)):
        a, b = np.searchsorted(path, [2 * i + 1, 2 * i + 2])
        spans.append((int(a), int(b)))
    return spans


def align_letters(logp, words, frame):
    tokens, owner = [], []
    for w, (_, _, plain) in enumerate(words):
        for token in tokenize(plain):
            tokens.append(token)
            owner.append(w)
    spans = viterbi(logp, tokens, [owner[i] == owner[i + 1] for i in range(len(tokens) - 1)])
    return _word_spans(spans, owner, words, frame)


def _word_spans(spans, owner, words, frame):
    first, last = {}, {}
    for sp, o in zip(spans, owner):
        first.setdefault(o, sp)
        last[o] = sp
    return [dict(line=line, word=original, start=round(1000 * frame * first[w][0]),
                 end=round(1000 * frame * last[w][1])) for w, (line, original, _) in enumerate(words)]


def wav2vec2_emissions(repo, wave, device, cache_dir):
    from huggingface_hub import hf_hub_download
    from transformers import AutoFeatureExtractor, Wav2Vec2ForCTC
    model = Wav2Vec2ForCTC.from_pretrained(repo, cache_dir=cache_dir).to(device).eval()
    if device == "cuda":
        model = model.half()
    fe = AutoFeatureExtractor.from_pretrained(repo, cache_dir=cache_dir)
    vocab = json.load(open(hf_hub_download(repo, "vocab.json", cache_dir=cache_dir), encoding="utf-8"))
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
    del model
    logp = np.concatenate(chunks)
    # Fold each model's vocabulary onto the shared 28-symbol alphabet.
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


def parakeet_words(repo, wave, words, device, cache_dir):
    """Subword CTC forced alignment with Parakeet.

    The 1.1B model runs in float16 on CUDA. The 0.6B model gives NaN logits in float16, so it
    runs in float32, and any NaN result is retried in float32.
    """
    from transformers import AutoProcessor, ParakeetForCTC
    processor = AutoProcessor.from_pretrained(repo, cache_dir=cache_dir)
    feats = processor.feature_extractor(wave, sampling_rate=16000, return_tensors="pt")
    half = device == "cuda" and "1.1b" in repo
    for dtype in ([torch.float16] if half else []) + [torch.float32]:
        model = ParakeetForCTC.from_pretrained(repo, torch_dtype=dtype, cache_dir=cache_dir).to(device).eval()
        with torch.inference_mode():
            logits = model(input_features=feats.input_features.to(device, dtype),
                           attention_mask=feats.attention_mask.to(device)).logits[0].float()
        if torch.isfinite(logits).all():
            break
        del model
    blank = model.config.pad_token_id
    del model
    logp = logits.log_softmax(-1).cpu().numpy()
    order = [blank] + [i for i in range(logp.shape[1]) if i != blank]
    logp, remap = logp[:, order], {old: new for new, old in enumerate(order)}
    frame = len(wave) / 16000 / len(logp)
    tokens, owner = [], []
    for w, (_, _, plain) in enumerate(words):
        for t in processor.tokenizer.encode(" " + plain, add_special_tokens=False):
            tokens.append(remap[t])
            owner.append(w)
    spans = viterbi(logp, tokens, [owner[i] == owner[i + 1] for i in range(len(tokens) - 1)])
    return _word_spans(spans, owner, words, frame)


# ---------------------------------------------------------------- combining

def vote(runs, agree, need):
    """Per line, take the start most runs agree on; flag lines where too few runs agree."""
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


def vocal_level(vocals, sample_rate, hop=0.02):
    x = vocals.mean(0).numpy().astype(np.float64)
    h = int(sample_rate * hop)
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


def _resample(wave, rate, target):
    import torchaudio
    return wave if rate == target else torchaudio.functional.resample(wave, rate, target)


def align(waveform, sample_rate, lyrics, language="en", device="auto", voters=DEFAULT_VOTERS, cache_dir=None,
          vocals=None, agree=0.25, need=2, hold_db=-12.0, gap=2.0, progress=None, log=print):
    """waveform: (channels, samples) float tensor. Returns the alignment dict Chorus Cut imports.

    progress(fraction, label) is called as the steps finish; vocals may supply a precomputed
    44.1 kHz stereo stem to skip separation.
    """
    from . import separate
    device = pick_device(device)
    words = parse(lyrics)
    if not words:
        raise ValueError("Paste at least one line of sung words.")
    if language not in LANGUAGES:
        raise ValueError(f"Language must be one of {', '.join(LANGUAGES)}.")
    specs = [s for s in voters.split(",") if MODELS[s.partition("@")[0]].get(language)]
    steps = 1 + len(specs) + 1
    done = [0]

    def step(label):
        done[0] += 1
        if progress:
            progress(done[0] / steps, label)

    timings = {}
    started = time.monotonic()
    waveform = waveform.float()
    if waveform.shape[0] == 1:
        waveform = waveform.repeat(2, 1)
    waveform = waveform[:2]
    if vocals is None:
        model = separate.load_model(device, cache_dir)
        vocals = separate.vocals(model, _resample(waveform, sample_rate, separate.SAMPLE_RATE), device,
                                 batch=4 if device == "cuda" else 1)
        del model
        if device == "cuda":
            torch.cuda.empty_cache()
    timings["separate"] = round(time.monotonic() - started, 1)
    step("Separated the vocals")
    waves = {"voc": _resample(vocals.mean(0, keepdim=True), separate.SAMPLE_RATE, 16000)[0].numpy(),
             "mix": _resample(waveform.mean(0, keepdim=True), sample_rate, 16000)[0].numpy()}
    db, hop = vocal_level(vocals, separate.SAMPLE_RATE)
    runs = []
    for spec in specs:
        name, _, source = spec.partition("@")
        repo = MODELS[name][language]
        t = time.monotonic()
        wave = waves[source or "voc"]
        try:
            if name == "parakeet":
                runs.append(parakeet_words(repo, wave, words, device, cache_dir))
            else:
                runs.append(align_letters(wav2vec2_emissions(repo, wave, device, cache_dir), words, FRAME))
        except ImportError as e:  # e.g. a transformers release without Parakeet
            log(f"Chorus Cut: skipping {spec}: {e}")
        if device == "cuda":
            torch.cuda.empty_cache()
        timings[spec] = round(time.monotonic() - t, 1)
        step(f"Aligned with {spec}")
    if not runs:
        raise RuntimeError("No aligner could run.")
    result = vote(runs, agree, need) if len(runs) > 1 else runs[0]
    loud = extend_ends(result, db, hop, hold_db)
    sections = instrumental(result, loud, hop, len(db) * hop, gap)
    step("Combined the results")
    return {"pipeline": "chorus_align", "voters": [s for s in specs], "device": device, "language": language,
            "timings_s": timings, "words": result, "sections": sections}
