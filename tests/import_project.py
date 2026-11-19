"""Exercise real-media creation, persistence, XML and disk-only monitoring on generated inputs."""
import argparse
from array import array
import math
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
from urllib.parse import unquote, urlparse
from xml.etree import ElementTree as ET

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, default=root / "build")
parser.add_argument("--keep", type=Path, help="create a new folder and keep the NLE fixture there")
args = parser.parse_args()
app = args.build.resolve() / "ChorusCut.app/Contents/MacOS/ChorusCut"
check = args.build.resolve() / "ChorusCutImportCheck"
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", SDL_AUDIODRIVER="disk", SDL_AUDIO_DRIVER="disk")


def run(*cmd):
    subprocess.run(cmd, env=env, check=True, timeout=90)


def verify(work):
    for role, source, frames, size in (("Performance", "testsrc2", 270, "384x216"),
                                       ("Cutaway", "testsrc", 90, "320x180")):
        (work / role).mkdir()
        run("ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"{source}=s={size}:r=30000/1001",
            "-f", "lavfi", "-i", "sine=f=1300:r=48000", "-frames:v", str(frames), "-c:v", "libx264",
            "-pix_fmt", "yuv420p", "-c:a", "aac", work / role / "take.mp4")
    run("ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=f=350:r=44100:d=10.01",
        "-af", "volume=0.08", work / "Song & voice.wav")
    run(check, work)
    imported = work / "Imported cut"
    # Reopen in a new app process after moving the folder away from its original path.
    project = work / "Relocated cut"
    imported.rename(project)
    run(app, "--project", project, "--export")
    sequence = ET.parse(project / "Chorus.xml").find("sequence")
    assert sequence.findtext("duration") == "300"
    assert sequence.findtext("rate/timebase") == "30" and sequence.findtext("rate/ntsc") == "TRUE"
    clips = sequence.findall("media/video/track/clipitem") + sequence.findall("media/audio/track/clipitem")
    assert len(clips) == 3
    for clip, expected, relpath in zip(clips, [(0,270,0,270,270), (100,168,12,80,90), (0,300,0,300,300)],
                                       ["Performance/take.mp4", "Cutaway/take.mp4", "Master/Song & voice.wav"]):
        assert tuple(int(clip.findtext(key)) for key in ("start","end","in","out","file/duration")) == expected
        path = Path(unquote(urlparse(clip.findtext("file/pathurl")).path))
        assert path == project / "Media" / relpath and path.is_file()
    assert clips[0].findtext("file/media/video/samplecharacteristics/width") == "384"
    assert clips[1].findtext("file/media/video/samplecharacteristics/width") == "320"
    assert clips[2].findtext("file/media/audio/samplecharacteristics/samplerate") == "44100"
    # Camera sound is over ten times louder than the selected song. Check the actual SDL mix.
    capture = work / "imported-monitor.raw"
    subprocess.run([app, "--project", project, "--check-playback", "--check-monitor-db", "0"],
                   env=dict(env, SDL_AUDIO_DISK_OUTPUT_FILE=str(capture)), check=True, timeout=35)
    samples = array("f")
    samples.frombytes(capture.read_bytes())
    assert len(samples) >= 9 * 48000 * 2 and all(map(math.isfinite, samples))
    peak = max(map(abs, samples))
    assert 0.006 < peak < 0.012, peak
    capture.unlink()
    legacy = work / "Legacy copy"
    shutil.copytree(root / "Example/Media", legacy / "Media")
    # A legacy user's saved placement, distinct from the default fixture trim.
    with sqlite3.connect(legacy / "project.sqlite") as db:
        db.execute("CREATE TABLE cutaway(id INTEGER PRIMARY KEY, start INTEGER, end INTEGER, source INTEGER)")
        db.execute("INSERT INTO cutaway VALUES(1,83,133,41)")
    run(app, "--project", legacy, "--export")
    cut = ET.parse(legacy / "Chorus.xml").findall(".//video/track/clipitem")[1]
    assert tuple(map(int, (cut.findtext("start"), cut.findtext("end"), cut.findtext("in")))) == (83,133,41)
    print(f"PASS — relocated XML, cut and handles, legacy trim, imported monitor peak {peak:.8f}", flush=True)


if args.keep:
    work = args.keep.resolve()
    work.mkdir()
    verify(work)
else:
    with tempfile.TemporaryDirectory() as tmp:
        verify(Path(tmp))
