"""Run the example through the SDL disk driver and inspect the samples it receives."""
from array import array
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave

project = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else (
    project / "ChorusCut.app/Contents/MacOS/ChorusCut"
)

with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    example = work / "Example"
    shutil.copytree(project / "Example/Media", example / "Media")
    with wave.open(str(example / "Media/Master.wav"), "rb") as source:
        assert source.getsampwidth() == 2 and source.getframerate() == 48000
        assert source.getnchannels() == 2
        samples = array("h")
        samples.frombytes(source.readframes(source.getnframes()))
    source_peak = max(map(abs, samples)) / 32768
    assert 0.001 < source_peak <= 0.013
    # Launch plays at unity (0 dB). Mute and gain are explicit choices.
    for name, gain in (("muted", "mute"), ("default launch", None), ("-24 dB", -24), ("unity", 0)):
        capture = work / (name + ".raw")
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", SDL_AUDIODRIVER="disk",
                   SDL_AUDIO_DRIVER="disk", SDL_AUDIO_DISK_OUTPUT_FILE=str(capture))
        args = [str(binary), "--project", str(example), "--check-playback"]
        if gain == "mute":
            args.append("--check-mute")
        elif gain is not None:
            args.extend(["--check-monitor-db", str(gain)])
        result = subprocess.run(args, env=env, cwd=work, capture_output=True, text=True, timeout=35)
        assert result.returncode == 0, result.stderr
        assert "format=F32LE channels=2 freq=48000" in result.stderr, result.stderr
        pcm = array("f")
        pcm.frombytes(capture.read_bytes())
        assert len(pcm) >= 7 * 48000 * 2 and all(map(math.isfinite, pcm))
        peak = max(map(abs, pcm))
        expected = 0 if gain == "mute" else source_peak * 10 ** ((gain or 0) / 20)
        assert math.isclose(peak, expected, rel_tol=0.0001), (name, peak, expected)
        print(f"{name}: {len(pcm)} samples, peak {peak:.8f}")
