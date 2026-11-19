"""Mel-Band RoFormer, inference only, for Kimberley Jensen's MIT-licensed vocal model.

Adapted from Music-Source-Separation-Training (models/bs_roformer/mel_band_roformer.py
and attend.py), which carries this notice:

    MIT License
    Copyright (c) 2024 Roman Solovyev (ZFTurbo)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

Differences from the original: training losses, checkpointing, linear attention and
PoPE are removed; the rotary embedding and the librosa mel filter bank are
reimplemented here so the node needs no packages beyond torch, einops and numpy.
Parameter names match the original, so its checkpoints load with strict=True.
"""
import numpy as np
import torch
import torch.nn.functional as F
from einops import pack, rearrange, repeat, unpack
from torch import nn

# Kimberley Jensen's vocal model (config_vocals_mel_band_roformer_kj.yaml).
KIM_VOCALS = dict(dim=384, depth=6, stereo=True, num_stems=1, time_transformer_depth=1,
                  freq_transformer_depth=1, num_bands=60, dim_head=64, heads=8, sample_rate=44100,
                  stft_n_fft=2048, stft_hop_length=441, stft_win_length=2048, mask_estimator_depth=2)


def slaney_mel(sr, n_fft, n_mels):
    """librosa.filters.mel(sr=sr, n_fft=n_fft, n_mels=n_mels) with its defaults (Slaney scale and norm)."""
    def hz_to_mel(f):
        f = np.asanyarray(f, dtype=np.float64)
        mel = f / (200.0 / 3)
        log_t = f >= 1000.0
        return np.where(log_t, 15.0 + np.log(np.maximum(f, 1e-10) / 1000.0) / (np.log(6.4) / 27.0), mel)

    def mel_to_hz(m):
        m = np.asanyarray(m, dtype=np.float64)
        f = m * (200.0 / 3)
        log_t = m >= 15.0
        return np.where(log_t, 1000.0 * np.exp((np.log(6.4) / 27.0) * (m - 15.0)), f)

    fftfreqs = np.fft.rfftfreq(n=n_fft, d=1.0 / sr)
    mel_f = mel_to_hz(np.linspace(hz_to_mel(0.0), hz_to_mel(sr / 2.0), n_mels + 2))
    fdiff = np.diff(mel_f)
    ramps = np.subtract.outer(mel_f, fftfreqs)
    weights = np.zeros((n_mels, 1 + n_fft // 2), dtype=np.float32)
    for i in range(n_mels):
        lower = -ramps[i] / fdiff[i]
        upper = ramps[i + 2] / fdiff[i + 1]
        weights[i] = np.maximum(0, np.minimum(lower, upper))
    weights *= (2.0 / (mel_f[2:n_mels + 2] - mel_f[:n_mels]))[:, None]
    return weights


class RotaryEmbedding(nn.Module):
    """The subset of rotary_embedding_torch.RotaryEmbedding used here (interleaved pairs, theta 10000)."""

    def __init__(self, dim, theta=10000):
        super().__init__()
        self.freqs = nn.Parameter(1.0 / (theta ** (torch.arange(0, dim, 2)[: dim // 2].float() / dim)),
                                  requires_grad=False)

    def rotate_queries_or_keys(self, t):
        n = t.shape[-2]
        pos = torch.arange(n, device=t.device, dtype=self.freqs.dtype)
        freqs = torch.einsum("i,j->ij", pos, self.freqs.to(t.device))
        freqs = repeat(freqs, "n d -> n (d r)", r=2)
        x = rearrange(t, "... (d r) -> ... d r", r=2)
        x1, x2 = x.unbind(dim=-1)
        rotated = rearrange(torch.stack((-x2, x1), dim=-1), "... d r -> ... (d r)")
        return (t * freqs.cos().to(t.dtype)) + (rotated * freqs.sin().to(t.dtype))


class RMSNorm(nn.Module):
    def __init__(self, dim):
        super().__init__()
        self.scale = dim ** 0.5
        self.gamma = nn.Parameter(torch.ones(dim))

    def forward(self, x):
        return F.normalize(x, dim=-1) * self.scale * self.gamma


class FeedForward(nn.Module):
    def __init__(self, dim, mult=4):
        super().__init__()
        inner = int(dim * mult)
        self.net = nn.Sequential(RMSNorm(dim), nn.Linear(dim, inner), nn.GELU(), nn.Dropout(0.0),
                                 nn.Linear(inner, dim), nn.Dropout(0.0))

    def forward(self, x):
        return self.net(x)


class Attention(nn.Module):
    def __init__(self, dim, heads, dim_head, rotary_embed):
        super().__init__()
        self.heads = heads
        self.rotary_embed = rotary_embed
        inner = heads * dim_head
        self.norm = RMSNorm(dim)
        self.to_qkv = nn.Linear(dim, inner * 3, bias=False)
        self.to_gates = nn.Linear(dim, heads)
        self.to_out = nn.Sequential(nn.Linear(inner, dim, bias=False), nn.Dropout(0.0))

    def forward(self, x):
        x = self.norm(x)
        q, k, v = rearrange(self.to_qkv(x), "b n (qkv h d) -> qkv b h n d", qkv=3, h=self.heads)
        q = self.rotary_embed.rotate_queries_or_keys(q)
        k = self.rotary_embed.rotate_queries_or_keys(k)
        out = F.scaled_dot_product_attention(q, k, v)
        out = out * rearrange(self.to_gates(x), "b n h -> b h n 1").sigmoid()
        return self.to_out(rearrange(out, "b h n d -> b n (h d)"))


class Transformer(nn.Module):
    def __init__(self, dim, depth, heads, dim_head, rotary_embed):
        super().__init__()
        self.layers = nn.ModuleList([nn.ModuleList([Attention(dim, heads, dim_head, rotary_embed), FeedForward(dim)])
                                     for _ in range(depth)])
        self.norm = RMSNorm(dim)

    def forward(self, x):
        for attn, ff in self.layers:
            x = attn(x) + x
            x = ff(x) + x
        return self.norm(x)


class BandSplit(nn.Module):
    def __init__(self, dim, dim_inputs):
        super().__init__()
        self.dim_inputs = dim_inputs
        self.to_features = nn.ModuleList([nn.Sequential(RMSNorm(d), nn.Linear(d, dim)) for d in dim_inputs])

    def forward(self, x):
        return torch.stack([net(part) for part, net in zip(x.split(self.dim_inputs, dim=-1), self.to_features)], dim=-2)


def mlp(dim_in, dim_out, dim_hidden, depth):
    dims = (dim_in, *((dim_hidden,) * depth), dim_out)
    layers = []
    for i, (a, b) in enumerate(zip(dims[:-1], dims[1:])):
        layers.append(nn.Linear(a, b))
        if i < len(dims) - 2:
            layers.append(nn.Tanh())
    return nn.Sequential(*layers)


class MaskEstimator(nn.Module):
    def __init__(self, dim, dim_inputs, depth, expansion=4):
        super().__init__()
        self.to_freqs = nn.ModuleList([nn.Sequential(mlp(dim, d * 2, dim * expansion, depth), nn.GLU(dim=-1))
                                       for d in dim_inputs])

    def forward(self, x):
        return torch.cat([net(band) for band, net in zip(x.unbind(dim=-2), self.to_freqs)], dim=-1)


class MelBandRoformer(nn.Module):
    def __init__(self, dim, depth, stereo, num_stems, time_transformer_depth, freq_transformer_depth, num_bands,
                 dim_head, heads, sample_rate, stft_n_fft, stft_hop_length, stft_win_length, mask_estimator_depth):
        super().__init__()
        self.audio_channels = 2 if stereo else 1
        time_rotary, freq_rotary = RotaryEmbedding(dim_head), RotaryEmbedding(dim_head)
        self.layers = nn.ModuleList([nn.ModuleList([
            Transformer(dim, time_transformer_depth, heads, dim_head, time_rotary),
            Transformer(dim, freq_transformer_depth, heads, dim_head, freq_rotary)]) for _ in range(depth)])
        self.win_length = stft_win_length
        self.stft_kwargs = dict(n_fft=stft_n_fft, hop_length=stft_hop_length, win_length=stft_win_length,
                                normalized=False)
        freqs = stft_n_fft // 2 + 1
        bank = torch.from_numpy(slaney_mel(sample_rate, stft_n_fft, num_bands))
        bank[0][0] = 1.0
        bank[-1, -1] = 1.0
        freqs_per_band = bank > 0
        freq_indices = repeat(torch.arange(freqs), "f -> b f", b=num_bands)[freqs_per_band]
        if stereo:
            freq_indices = rearrange(repeat(freq_indices, "f -> f s", s=2) * 2 + torch.arange(2), "f s -> (f s)")
        self.register_buffer("freq_indices", freq_indices, persistent=False)
        self.register_buffer("num_bands_per_freq", freqs_per_band.sum(0), persistent=False)
        dims = tuple(2 * f * self.audio_channels for f in freqs_per_band.sum(1).tolist())
        self.band_split = BandSplit(dim, dims)
        self.mask_estimators = nn.ModuleList([MaskEstimator(dim, dims, mask_estimator_depth) for _ in range(num_stems)])

    def forward(self, audio):
        """audio: (batch, channels, samples) → separated stem with the same shape (minus STFT rounding)."""
        device = audio.device
        batch, channels, _ = audio.shape
        window = torch.hann_window(self.win_length, device=device)
        flat, shape = pack([audio], "* t")
        spec = torch.view_as_real(torch.stft(flat, **self.stft_kwargs, window=window, return_complex=True))
        spec = unpack(spec, shape, "* f t c")[0]
        spec = rearrange(spec, "b s f t c -> b (f s) t c")
        x = spec[torch.arange(batch, device=device)[..., None], self.freq_indices]
        x = self.band_split(rearrange(x, "b f t c -> b t (f c)"))
        for time_transformer, freq_transformer in self.layers:
            x = rearrange(x, "b t f d -> b f t d")
            x, ps = pack([x], "* t d")
            x = unpack(time_transformer(x), ps, "* t d")[0]
            x = rearrange(x, "b f t d -> b t f d")
            x, ps = pack([x], "* f d")
            x = unpack(freq_transformer(x), ps, "* f d")[0]
        masks = torch.stack([fn(x) for fn in self.mask_estimators], dim=1)
        masks = torch.view_as_complex(rearrange(masks, "b n t (f c) -> b n f t c", c=2).float().contiguous())
        spec = torch.view_as_complex(rearrange(spec, "b f t c -> b 1 f t c").float().contiguous())
        n = masks.shape[1]
        index = repeat(self.freq_indices, "f -> b n f t", b=batch, n=n, t=spec.shape[-1])
        summed = torch.zeros_like(repeat(spec, "b 1 ... -> b n ...", n=n)).scatter_add_(2, index, masks)
        denom = repeat(self.num_bands_per_freq, "f -> (f r) 1", r=channels)
        spec = spec * (summed / denom.clamp(min=1e-8))
        spec = rearrange(spec, "b n (f s) t -> (b n s) f t", s=self.audio_channels)
        spec[:, 0] = 0.0  # zero DC, as the original does by default
        out = torch.istft(spec, **self.stft_kwargs, window=window, return_complex=False)
        out = rearrange(out, "(b n s) t -> b n s t", b=batch, s=self.audio_channels, n=n)
        return out[:, 0] if n == 1 else out
