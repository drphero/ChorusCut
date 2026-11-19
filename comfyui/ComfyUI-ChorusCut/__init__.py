"""Chorus Cut nodes for ComfyUI.

Chorus Cut (the music-video editor) queues these nodes through ComfyUI's API, on the user's
own machine or on a rented GPU. They also work in hand-built graphs.
"""
import json
import os
import time

from .chorus_align.pipeline import LANGUAGES, align

try:
    import folder_paths
except ImportError:  # imported outside ComfyUI, e.g. by tests
    folder_paths = None


def _models_dir():
    base = folder_paths.models_dir if folder_paths else os.path.expanduser("~/.cache")
    return os.path.join(base, "chorus_cut")


class ChorusCutAlignLyrics:
    """Times every sung word of the lyrics against the song, flags doubtful lines and finds instrumentals."""

    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "audio": ("AUDIO",),
                "lyrics": ("STRING", {"multiline": True, "default": "",
                                      "tooltip": "Lyrics as sung, one line per line; [Section] lines are skipped."}),
                "language": (LANGUAGES, {"default": "en"}),
            },
            "optional": {
                "device": (["auto", "cuda", "mps"], {"default": "auto"}),
            },
        }

    RETURN_TYPES = ("STRING",)
    RETURN_NAMES = ("alignment_json",)
    FUNCTION = "run"
    CATEGORY = "Chorus Cut"
    OUTPUT_NODE = True
    DESCRIPTION = ("Lyric alignment for Chorus Cut: Mel-Band RoFormer vocals, then Parakeet, wav2vec2 and XLSR "
                   "forced alignment combined by a per-line vote. Models download on first use (about 5.5 GB).")

    def run(self, audio, lyrics, language, device="auto"):
        import comfy.model_management as mm
        import comfy.utils
        mm.unload_all_models()
        mm.soft_empty_cache()
        bar = comfy.utils.ProgressBar(1000)

        def progress(fraction, label):
            mm.throw_exception_if_processing_interrupted()
            bar.update_absolute(int(1000 * fraction))
            print(f"[Chorus Cut] {round(100 * fraction)}% {label}")

        result = align(audio["waveform"][0], audio["sample_rate"], lyrics, language, device,
                       cache_dir=_models_dir(), progress=progress)
        text = json.dumps(result, ensure_ascii=False)
        folder = os.path.join(folder_paths.get_output_directory(), "chorus_cut")
        os.makedirs(folder, exist_ok=True)
        name = f"alignment_{time.strftime('%Y%m%d-%H%M%S')}.json"
        with open(os.path.join(folder, name), "w", encoding="utf-8") as f:
            f.write(text)
        flagged = len({w["line"] for w in result["words"] if w.get("flag")})
        summary = (f"{len(result['words'])} words, {flagged} lines to check, "
                   f"{len(result['sections'])} instrumental sections ({result['device']})")
        return {"ui": {"chorus_cut_alignment": [text], "text": [summary],
                       "files": [{"filename": name, "subfolder": "chorus_cut", "type": "output"}]},
                "result": (text,)}


NODE_CLASS_MAPPINGS = {"ChorusCutAlignLyrics": ChorusCutAlignLyrics}
NODE_DISPLAY_NAME_MAPPINGS = {"ChorusCutAlignLyrics": "Chorus Cut: Align Lyrics"}
