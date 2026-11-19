"""Vocal stem with Mel-Band RoFormer: overlapping 8 s chunks with linear crossfades."""
import torch

from .melband import KIM_VOCALS, MelBandRoformer

KIM_REPO = "KimberleyJSN/melbandroformer"  # MIT
KIM_FILE = "MelBandRoformer.ckpt"
SAMPLE_RATE = 44100
CHUNK = 352800
OVERLAP = 2


def load_model(device, cache_dir=None):
    from huggingface_hub import hf_hub_download
    path = hf_hub_download(KIM_REPO, KIM_FILE, cache_dir=cache_dir)
    state = torch.load(path, map_location="cpu", weights_only=True)
    if "state_dict" in state:
        state = state["state_dict"]
    model = MelBandRoformer(**KIM_VOCALS)
    model.load_state_dict(state, strict=True)
    return model.to(device).eval()


def _window(size, fade):
    w = torch.ones(size)
    w[:fade] = torch.linspace(0, 1, fade)
    w[-fade:] = torch.linspace(1, 0, fade)
    return w


@torch.inference_mode()
def vocals(model, mix, device, batch=1, progress=None):
    """mix: (2, samples) float tensor at 44.1 kHz. Returns the vocal stem with the same shape, on the CPU."""
    step = CHUNK // OVERLAP
    border = CHUNK - step
    length = mix.shape[-1]
    padded = torch.nn.functional.pad(mix[None], (border, border), mode="reflect" if length > 2 * border else "constant")[0]
    total = padded.shape[-1]
    window = _window(CHUNK, CHUNK // 10)
    out = torch.zeros_like(padded)
    weight = torch.zeros(total)
    starts = list(range(0, total, step))
    use_half = device == "cuda"
    for k in range(0, len(starts), batch):
        group = starts[k:k + batch]
        chunks = []
        for s in group:
            c = padded[:, s:s + CHUNK]
            if c.shape[-1] < CHUNK:
                c = torch.nn.functional.pad(c, (0, CHUNK - c.shape[-1]))
            chunks.append(c)
        x = torch.stack(chunks).to(device)
        with torch.autocast(device_type="cuda", dtype=torch.float16, enabled=use_half):
            y = model(x).float().cpu()
        for s, part in zip(group, y):
            n = min(CHUNK, total - s, part.shape[-1])
            out[:, s:s + n] += part[:, :n] * window[:n]
            weight[s:s + n] += window[:n]
        if progress:
            progress(min(k + batch, len(starts)) / len(starts))
    out = out / weight.clamp(min=1e-8)
    return out[:, border:border + length]
