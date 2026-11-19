"""A stand-in ComfyUI server for testing Chorus Cut's job handling without a GPU.

usage: comfy_stub.py PORT ALIGNMENT_JSON

Implements the endpoints Chorus Cut uses (/system_stats, /object_info/<node>, /upload/image,
/prompt, /history/<id>, /queue, /interrupt). A queued job answers with ALIGNMENT_JSON,
every word shifted by the stub's current "shift" in milliseconds. Control endpoints:
  POST /_stub/mode  {"mode": "finish" | "hold" | "error", "shift": ms}
  POST /_stub/release   finish held jobs
  GET  /_stub/log       what the client sent
"""
import json
import sys
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, FIXTURE = int(sys.argv[1]), sys.argv[2]
lock = threading.Lock()
state = {"mode": "finish", "shift": 0, "jobs": {}, "uploads": [], "prompts": [], "deleted": [], "interrupted": 0}


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

    def do_GET(self):
        with lock:
            if self.path == "/system_stats":
                return self.send({"system": {"comfyui_version": "stub"}, "devices": [{"name": "cuda:0 Stub GPU"}]})
            if self.path == "/object_info/ChorusCutAlignLyrics":
                return self.send({"ChorusCutAlignLyrics": {"input": {}}})
            if self.path == "/queue":
                running = [[0, i, {}, {}, []] for i, j in state["jobs"].items() if j["state"] == "running"]
                return self.send({"queue_running": running, "queue_pending": []})
            if self.path.startswith("/history/"):
                job_id = self.path.split("/")[-1]
                job = state["jobs"].get(job_id)
                if not job or job["state"] in ("running", "interrupted"):
                    return self.send({})
                if job["state"] == "error":
                    return self.send({job_id: {"status": {"status_str": "error", "completed": False, "messages": [
                        ["execution_error", {"exception_message": "Stub failure for testing"}]]}, "outputs": {}}})
                return self.send({job_id: {"status": {"status_str": "success", "completed": True, "messages": []},
                                           "outputs": {"2": {"chorus_cut_alignment": [job["result"]]}}}})
            if self.path == "/_stub/log":
                return self.send({k: v for k, v in state.items() if k != "jobs"} |
                                 {"jobs": {i: j["state"] for i, j in state["jobs"].items()}})
        self.send({"error": "not found"}, 404)

    def do_POST(self):
        raw = self.body()
        with lock:
            if self.path == "/upload/image":
                name = raw.split(b'filename="', 1)[1].split(b'"', 1)[0].decode()
                state["uploads"].append({"name": name, "bytes": len(raw),
                                         "auth": self.headers.get("Authorization", "")})
                return self.send({"name": name, "subfolder": "", "type": "input"})
            if self.path == "/prompt":
                prompt = json.loads(raw)["prompt"]
                job_id = str(uuid.uuid4())
                state["prompts"].append(prompt)
                mode = state["mode"]
                state["jobs"][job_id] = {"state": "running" if mode == "hold" else mode if mode == "error" else "done",
                                         "result": result(state["shift"])}
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
                    if j["state"] == "running":
                        j["state"] = "done"
                return self.send({"ok": True})
        self.send({"error": "not found"}, 404)


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
