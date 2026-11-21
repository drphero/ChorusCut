"""A stand-in ComfyUI server for testing Chorus Cut's job handling without a GPU.

usage: comfy_stub.py PORT ALIGNMENT_JSON

Implements the endpoints Chorus Cut uses (/system_stats, /object_info/<node>, /upload/image,
/prompt, /history/<id>, /queue, /interrupt, /view). An alignment job answers with ALIGNMENT_JSON,
every word shifted by the stub's current "shift" in milliseconds. Any other graph is a picture
job: each SaveImage node outputs one PNG (PreviewImage a temporary one), served by /view. A face
refine (H3FaceSelect) saves a video the size and length of the uploaded take it names, plus
"faceExtra" frames (to test a refusal). An upscale (an H3 latent upscaler) saves one of the uploaded
take's length plus "upscaleExtra" frames, at the megapixels its graph asks for in steps of 32.
Control endpoints:
  POST /_stub/mode  {"mode": "finish" | "hold" | "wait" | "error", "shift": ms}   (wait: pending in the queue)
  POST /_stub/release   finish held jobs
  GET  /_stub/log       what the client sent
  /ws?clientId=...      WebSocket: sends "progress" for held jobs queued by that client, and for a held
                        job with a ModelPreviewOverrideKJ node, KJ Nodes' "kj_preview_override" step
                        clips (its preview_frames long, at its preview_fps) as "previewMime" says:
                        "webp" (animated WebP) or "mp4" (fragmented H.264, as NVENC servers send)
  "previewNode" in /_stub/mode: "missing" (no KJ Nodes, the default), "old" (no tiny_vae), "novae"
  (no H3 tiny VAE), "older" (only an older H3 tiny VAE) or "ok" (taeh3.safetensors)
"""
import base64
import struct
import zlib
import hashlib
import json
import sys
import time
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, FIXTURE = int(sys.argv[1]), sys.argv[2]
lock = threading.Lock()
state = {"mode": "finish", "previewNode": "missing", "previewMime": "webp", "shift": 0, "jobs": {}, "uploads": [], "prompts": [], "deleted": [], "interrupted": 0}


def png(name):
    """An 8x8 PNG in a colour taken from the name, so each picture is different."""
    r, g, b = hashlib.sha1(name.encode()).digest()[:3]
    raw = b"".join(b"\x00" + bytes([r, g, b]) * 8 for _ in range(8))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


VIDEOS = {}
UPLOADED = {}


def probe(data):
    """Width, height and frame count of an uploaded video, by ffprobe."""
    import subprocess
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".mp4") as f:
        f.write(data)
        f.flush()
        out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
                              "stream=width,height,nb_read_frames", "-of", "csv=p=0", f.name], capture_output=True, text=True).stdout
    w, h, n = (int(x) for x in out.strip().split(",")[:3])
    return w, h, n


def video_frames(prompt):
    """The length a video graph asks for: a frame count, or seconds through an expression (H3's 17n+5)."""
    for spec in prompt.values():
        length = spec.get("inputs", {}).get("length")
        if isinstance(length, (int, float)):
            return int(length)
    for spec in prompt.values():
        if spec.get("class_type") == "ComfyMathExpression":
            for key, value in spec["inputs"].items():
                if key.startswith("values.") and isinstance(value, list):
                    seconds = prompt[value[0]]["inputs"]["value"]
                    frames = max(5, round(seconds * 24))
                    return frames + (5 - frames % 17) % 17
    return 24


def video(name, frames, size="96x54"):
    """A small H.264 MP4 of that many frames at 24 fps, colour taken from the name, made with ffmpeg."""
    import subprocess
    import tempfile
    r, g, b = hashlib.sha1(name.encode()).digest()[:3]
    with tempfile.TemporaryDirectory() as tmp:
        out = tmp + "/v.mp4"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"color=c=0x{r:02x}{g:02x}{b:02x}:s={size}:r=24",
                        "-frames:v", str(frames), "-pix_fmt", "yuv420p", "-c:v", "libx264", out], check=True)
        return open(out, "rb").read()


CLIPS = {}


def preview_clip(mime, frames, fps, variant):
    """A small clip like KJ Nodes' step previews: animated WebP, or fragmented MP4 (NVENC servers)."""
    import subprocess
    import tempfile
    key = (mime, frames, fps, variant)
    if key not in CLIPS:
        colours = ["0x203040", "0x506070", "0x90a0b0"]
        with tempfile.TemporaryDirectory() as tmp:
            out = tmp + ("/c.webp" if mime == "webp" else "/c.mp4")
            codec = ["-c:v", "libwebp_anim", "-loop", "0"] if mime == "webp" else \
                    ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "frag_keyframe+empty_moov+default_base_moof"]
            subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"color=c={colours[variant % 3]}:s=160x90:r={fps}",
                            "-frames:v", str(frames)] + codec + [out], check=True)
            CLIPS[key] = open(out, "rb").read()
    return CLIPS[key]


def ws_send(wfile, payload):
    data = json.dumps(payload).encode()
    if len(data) < 126:
        head = bytes([0x81, len(data)])
    elif len(data) < 65536:
        head = bytes([0x81, 126]) + len(data).to_bytes(2, "big")
    else:
        head = bytes([0x81, 127]) + len(data).to_bytes(8, "big")
    wfile.write(head + data)
    wfile.flush()


def picture_outputs(prompt, number):
    outputs = {}
    face = next((s["inputs"].get("video") for s in prompt.values() if s.get("class_type") == "H3FaceSelect"), None)
    upscaler = next((s for s in prompt.values() if "LatentUpscaler" in s.get("class_type", "")), None)
    if upscaler is not None:
        source = next(s["inputs"].get("video") for s in prompt.values() if s.get("class_type") == "VHS_LoadVideoFFmpeg")
        mp = upscaler["inputs"].get("mode.megapixels")
        if isinstance(mp, list):
            mp = prompt[mp[0]]["inputs"]["value"]
    for node, spec in prompt.items():
        kind = spec.get("class_type", "")
        if kind == "SaveVideo":
            prefix = spec["inputs"].get("filename_prefix", "video/ComfyUI")
            folder, _, base = prefix.rpartition("/")
            name = f"{base}_{number:05}_.mp4"
            if upscaler is not None:
                w, h, n = probe(UPLOADED[source])
                # As the real upscaler did: the scale rounded to a tenth, each side up to a step of 32.
                import math
                k = round((mp * 1e6 / (w * h)) ** 0.5, 1)
                VIDEOS[name] = video(name, n + state.get("upscaleExtra", 0), f"{math.ceil(w / 32 * k - 1e-6) * 32}x{math.ceil(h / 32 * k - 1e-6) * 32}")
            elif face is not None:
                w, h, n = probe(UPLOADED[face])
                VIDEOS[name] = video(name, n + state.get("faceExtra", 0), f"{w}x{h}")
            else:
                VIDEOS[name] = video(name, video_frames(prompt))
            outputs[node] = {"images": [{"filename": name, "subfolder": folder, "type": "output"}], "animated": [True]}
        elif kind == "SaveImage":
            prefix = spec["inputs"].get("filename_prefix", "ComfyUI")
            folder, _, base = prefix.rpartition("/")
            outputs[node] = {"images": [{"filename": f"{base}_{number:05}_.png", "subfolder": folder, "type": "output"}]}
        elif kind == "PreviewImage":
            outputs[node] = {"images": [{"filename": f"preview_{number:05}_.png", "subfolder": "", "type": "temp"}]}
    return outputs


def result(shift):
    data = json.load(open(FIXTURE, encoding="utf-8"))
    for w in data["words"]:
        w["start"] += shift
        w["end"] += shift
    return json.dumps(data, ensure_ascii=False)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, payload, code=200):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def websocket(self):
        """Just enough RFC 6455 to push text frames to one client."""
        client = self.path.partition("clientId=")[2]
        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] +
                                                 "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.protocol_version = "HTTP/1.1"  # WebSocket upgrades need HTTP/1.1
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        with lock:
            state.setdefault("sockets", []).append(client)
        step = 0
        steps = {}
        try:
            while True:
                time.sleep(0.2)
                with lock:
                    held = [(i, j["prompt"]) for i, j in state["jobs"].items() if j["state"] == "running" and j["client"] == client]
                    mime = state["previewMime"]
                for job_id, prompt in held:
                    step = min(step + 1, 9)
                    ws_send(self.wfile, {"type": "progress", "data": {"value": step, "max": 10, "prompt_id": job_id, "node": "2"}})
                    live = next(((n, s["inputs"]) for n, s in prompt.items() if s.get("class_type") == "ModelPreviewOverrideKJ"), None)
                    if live is None:
                        continue
                    node, inputs = live
                    at = steps.get(job_id, -1) + 1
                    if at > 9:
                        continue
                    steps[job_id] = at
                    if at == 0:
                        ws_send(self.wfile, {"type": "execution_start", "data": {"prompt_id": job_id}})
                        ws_send(self.wfile, {"type": "kj_preview_override", "data": {"node_id": node, "step": 0, "total": 10, "sigmas": [1.0 - k / 10 for k in range(11)]}})
                        continue
                    clip = preview_clip(mime, inputs["preview_frames"], inputs["preview_fps"], at)
                    ws_send(self.wfile, {"type": "kj_preview_override", "data": {
                        "node_id": node, "image": base64.b64encode(clip).decode(), "mime": "image/webp" if mime == "webp" else "video/mp4",
                        "w": 160, "h": 90, "step": at, "total": 10, "sigma": 1.0 - at / 10, "sigmas": None, "delta": 0.1,
                        "step_ms": 200.0, "avg_step_ms": 200.0, "fps": inputs["preview_fps"]}})
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def do_GET(self):
        if self.path.startswith("/ws"):
            return self.websocket()
        with lock:
            if self.path == "/system_stats":
                return self.send({"system": {"comfyui_version": "stub"}, "devices": [{"name": "cuda:0 Stub GPU"}]})
            if self.path == "/object_info/ChorusCutAlignLyrics":
                return self.send({"ChorusCutAlignLyrics": {"input": {}}})
            if self.path == "/object_info/ModelPreviewOverrideKJ":
                kind = state["previewNode"]
                if kind == "missing":
                    return self.send({})
                optional = {"vae": ["VAE", {}], "audio_vae": ["VAE", {}]}
                if kind != "old":
                    vaes = {"novae": ["none", "taesd.safetensors"], "older": ["none", "taeh3_v1.safetensors"]}.get(kind, ["none", "taeh3.safetensors"])
                    optional["tiny_vae"] = ["COMBO", {"default": "none", "options": vaes}]
                return self.send({"ModelPreviewOverrideKJ": {"input": {"required": {
                    "model": ["MODEL", {}], "max_resolution": ["INT", {"default": 1024}], "jpeg_quality": ["INT", {"default": 80}],
                    "suppress_default_preview": ["BOOLEAN", {"default": True}], "preview_frames": ["INT", {"default": 1, "max": 1024}],
                    "preview_fps": ["INT", {"default": 12}]}, "optional": optional}}})
            # Node types of the test workflows: which inputs are optional, and how one names its pictures.
            infos = {
                "TextEncodeQwenImageEditPlus": {"input": {"required": {"clip": ["CLIP", {}], "prompt": ["STRING", {}]},
                                                           "optional": {"vae": ["VAE", {}], "image1": ["IMAGE", {}], "image2": ["IMAGE", {}], "image3": ["IMAGE", {}]}}},
                "ImageConditioning": {"description": "Text to image, or an edit guided by pictures. The prompt refers to them as <Picture 1>, <Picture 2>, ...",
                                      "input": {"required": {"clip": ["CLIP", {}], "prompt": ["STRING", {}], "width": ["INT", {}], "height": ["INT", {}]},
                                                "optional": {"vae": ["VAE", {}], "images": ["COMFY_AUTOGROW_V3", {"template": {"input": {"required": {"image": ["IMAGE", {}]}}}, "prefix": "image_", "min": 0, "max": 100}]}}},
                "ComfySwitchNode": {"input": {"required": {"switch": ["BOOLEAN", {}]}, "optional": {"on_false": ["*", {}], "on_true": ["*", {}]}}},
                "MiniMaxH3ReferenceToVideo": {"description": "<Picture i> / <Video k> / <Audio j> reference conditioning for MiniMax H3. Use the same tags when prompting.",
                                              "input": {"required": {"clip": ["CLIP", {}], "prompt": ["STRING", {}], "width": ["INT", {}], "height": ["INT", {}],
                                                                     "length": ["INT", {"default": 124, "min": 5, "max": 3600, "step": 17}],
                                                                     "ref_image_size": ["COMBO", {"options": ["match", "max"]}]},
                                                        "optional": {"vae": ["VAE", {}], "audio_vae": ["VAE", {}],
                                                                     "ref_images": ["COMFY_AUTOGROW_V3", {"template": {"input": {"required": {"ref_image": ["IMAGE", {}]}}}, "prefix": "ref_image_", "min": 0, "max": 16}],
                                                                     "ref_audios": ["COMFY_AUTOGROW_V3", {}]}}},
                "H3FaceRefineStrength": {"input": {"required": {"preset": [["0.20 (light)", "0.30 (default: re-draws the face toward the references)", "0.45 (strong)"], {}],
                                                                "custom_denoise": ["FLOAT", {}]}}},
                "H3FaceSelect": {"input": {"required": {"video": ["STRING", {}], "select": [["manual", "identity_reference", "largest_face"], {}]},
                                           "optional": {"identity_reference": ["IMAGE", {}], "identity_threshold": ["FLOAT", {}]}}},
                "PixaromaH3AudioSync": {"input": {"required": {"model": ["MODEL", {}], "latent": ["LATENT", {}], "audio_vae": ["VAE", {}], "track": ["AUDIO", {}]}}},
                "ResolutionSelector": {"input": {"required": {"aspect_ratio": [["1:1 (Square)", "3:4 (Portrait Standard)", "4:3 (Landscape Standard)", "16:9 (Widescreen)"], {}],
                                                              "megapixels": ["FLOAT", {}], "multiple": ["INT", {}]}}},
            }
            name = self.path[len("/object_info/"):] if self.path.startswith("/object_info/") else ""
            if name in infos:
                state.setdefault("object_info", []).append(name)
                return self.send({name: infos[name]})
            if self.path == "/object_info/KSampler":
                state.setdefault("object_info", []).append("KSampler")
                return self.send({"KSampler": {"input": {"required": {
                    "seed": ["INT", {"default": 0}], "steps": ["INT", {"default": 20}], "cfg": ["FLOAT", {"default": 8.0}],
                    "sampler_name": [["euler", "dpmpp_2m", "uni_pc"], {}],
                    "scheduler": ["COMBO", {"options": ["normal", "karras", "simple"]}],
                    "denoise": ["FLOAT", {"default": 1.0}]}}}})
            if self.path == "/queue":
                running = [[0, i, {}, {}, []] for i, j in state["jobs"].items() if j["state"] == "running"]
                pending = [[0, i, {}, {}, []] for i, j in state["jobs"].items() if j["state"] == "waiting"]
                return self.send({"queue_running": running, "queue_pending": pending})
            if self.path.startswith("/view?"):
                from urllib.parse import parse_qs, urlparse
                q = parse_qs(urlparse(self.path).query)
                name = q.get("filename", [""])[0]
                state.setdefault("views", []).append({"filename": name, "subfolder": q.get("subfolder", [""])[0], "type": q.get("type", [""])[0]})
                body = VIDEOS.get(name) or png(name)
                self.send_response(200)
                self.send_header("Content-Type", "video/mp4" if name in VIDEOS else "image/png")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path.startswith("/history/"):
                job_id = self.path.split("/")[-1]
                job = state["jobs"].get(job_id)
                if not job or job["state"] in ("running", "interrupted", "waiting"):
                    return self.send({})
                if job.get("outputs") is not None and job["state"] == "done":
                    return self.send({job_id: {"status": {"status_str": "success", "completed": True, "messages": []},
                                               "outputs": job["outputs"]}})
                if job["state"] == "error":
                    return self.send({job_id: {"status": {"status_str": "error", "completed": False, "messages": [
                        ["execution_error", {"exception_message": "Stub failure for testing"}]]}, "outputs": {}}})
                return self.send({job_id: {"status": {"status_str": "success", "completed": True, "messages": []},
                                           "outputs": {"2": {"chorus_cut_alignment": [job["result"]]}}}})
            if self.path == "/_stub/log":
                return self.send({k: v for k, v in state.items() if k not in ("jobs",)} |
                                 {"jobs": {i: j["state"] for i, j in state["jobs"].items()}})
        self.send({"error": "not found"}, 404)

    def do_POST(self):
        raw = self.body()
        with lock:
            if self.path == "/upload/image":
                name = raw.split(b'filename="', 1)[1].split(b'"', 1)[0].decode()
                entry = {"name": name, "bytes": len(raw), "auth": self.headers.get("Authorization", "")}
                # A WAV's own size, from its RIFF header (the request also carries the form around it).
                at = raw.find(b"RIFF")
                if at >= 0 and raw[at + 8:at + 12] == b"WAVE":
                    entry["wavBytes"] = int.from_bytes(raw[at + 4:at + 8], "little") + 8
                    data = raw[at + 44:at + entry["wavBytes"]]
                    entry["silent"] = not data.strip(b"\0")
                # The file itself, for a face refine's take.
                start = raw.find(b"\r\n\r\n", raw.find(b'filename="')) + 4
                boundary = raw[:raw.find(b"\r\n")]
                UPLOADED[name] = raw[start:raw.find(b"\r\n" + boundary, start)]
                state["uploads"].append(entry)
                return self.send({"name": name, "subfolder": "", "type": "input"})
            if self.path == "/prompt":
                request = json.loads(raw)
                prompt = request["prompt"]
                job_id = str(uuid.uuid4())
                state["prompts"].append(prompt)
                mode = state["mode"]
                picture = not any(n.get("class_type") == "ChorusCutAlignLyrics" for n in prompt.values())
                state["jobs"][job_id] = {"state": {"hold": "running", "error": "error", "wait": "waiting"}.get(mode, "done"),
                                         "result": "" if picture else result(state["shift"]), "client": request.get("client_id", ""), "prompt": prompt,
                                         "outputs": picture_outputs(prompt, len(state["prompts"])) if picture else None}
                return self.send({"prompt_id": job_id, "number": len(state["prompts"]), "node_errors": {}})
            if self.path == "/queue":
                state["deleted"] += json.loads(raw).get("delete", [])
                return self.send({})
            if self.path == "/interrupt":
                state["interrupted"] += 1
                for j in state["jobs"].values():
                    if j["state"] == "running":
                        j["state"] = "interrupted"
                return self.send({})
            if self.path == "/_stub/mode":
                state.update(json.loads(raw))
                return self.send({"ok": True})
            if self.path == "/_stub/release":
                for j in state["jobs"].values():
                    if j["state"] in ("running", "waiting"):
                        j["state"] = "done"
                return self.send({"ok": True})
        self.send({"error": "not found"}, 404)


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
