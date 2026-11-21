#!/usr/bin/env python3
"""Sticky Hub: a local web page for flashing a Seeed reTerminal Sticky and loading a study pack.

Started by `scripts/flash-sticky.sh` with no arguments. It serves one page on 127.0.0.1 and does
two jobs, one at a time, streaming progress to the page:

  Flash firmware   Runs flash-sticky.sh non-interactively over USB (app slot + otadata only).
  Study pack       Copies a Study-App pack folder to /study/ on the SD card through the reader's
                   Wi-Fi File Transfer web server (/api/files, /mkdir, /delete, /upload).

Only the Python standard library is used, so it runs from the flasher's private venv.
"""

from __future__ import annotations

import argparse
import glob
import html
import json
import os
import secrets
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DEFAULT_REPO = "jamesctippets-ui/crossblink"
STUDY_DEST = "/study"
# Device-owned files that must never be replaced from the computer: the reader's progress log
# (imported back into the app) and any dot-prefixed state such as /study/.srs/.
STUDY_SKIP_NAMES = {"progress.jsonl"}
HTTP_TIMEOUT = 30


# --- Job runner --------------------------------------------------------------------------------


class Job:
    def __init__(self, kind: str):
        self.kind = kind
        self.lines: list[str] = []
        self.state = "running"  # running | ok | failed
        self.lock = threading.Lock()

    def log(self, text: str) -> None:
        with self.lock:
            for line in text.splitlines() or [""]:
                self.lines.append(line)

    def finish(self, ok: bool, message: str) -> None:
        self.log(message)
        with self.lock:
            self.state = "ok" if ok else "failed"

    def snapshot(self, offset: int) -> dict:
        with self.lock:
            return {"kind": self.kind, "state": self.state, "lines": self.lines[offset:], "next": len(self.lines)}


class Hub:
    def __init__(self, script: Path, repo: str):
        self.script = script
        self.repo = repo
        self.job: Job | None = None
        self.lock = threading.Lock()

    def start(self, kind: str, target) -> Job:
        with self.lock:
            if self.job and self.job.state == "running":
                raise RuntimeError("Another job is still running. Wait for it to finish.")
            job = Job(kind)
            self.job = job
        threading.Thread(target=self._run, args=(job, target), daemon=True).start()
        return job

    @staticmethod
    def _run(job: Job, target) -> None:
        try:
            target(job)
        except Exception as exc:  # report anything unexpected on the page instead of killing the thread
            job.finish(False, f"Error: {exc}")

    # --- flashing ---

    def flash(self, job: Job, source: str, pr: str, file: str, port: str) -> None:
        args = [str(self.script), "--yes"]
        if source == "pr":
            if not pr.isdigit():
                return job.finish(False, "Pick a pull request first.")
            args += ["--pr", pr]
        elif source == "file":
            if not file or not os.path.isfile(file):
                return job.finish(False, "Choose a firmware file first.")
            args += ["--file", file]
        else:
            args += ["--main"]
        if port:
            args += ["--port", port]
        env = dict(os.environ, CROSSBLINK_NONINTERACTIVE="1", CROSSBLINK_REPO=self.repo)
        job.log("$ flash-sticky.sh " + " ".join(args[1:]))
        proc = subprocess.Popen(
            ["bash", *args],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,  # no controlling terminal, so nothing can stop to prompt
        )
        assert proc.stdout is not None
        buf = b""
        while True:
            chunk = proc.stdout.read1(4096) if hasattr(proc.stdout, "read1") else proc.stdout.read(4096)
            if not chunk:
                break
            buf += chunk
            # esptool redraws its progress line with \r; treat that as a line break for the page.
            parts = buf.replace(b"\r", b"\n").split(b"\n")
            buf = parts.pop()
            for part in parts:
                if part.strip():
                    job.log(part.decode("utf-8", "replace"))
        if buf.strip():
            job.log(buf.decode("utf-8", "replace"))
        code = proc.wait()
        if code == 0:
            job.finish(True, "Flash finished. The Sticky is restarting.")
        else:
            job.finish(False, f"Flash failed (exit code {code}). See the log above.")

    # --- study pack ---

    def upload_study(self, job: Job, host: str, folder: str) -> None:
        base = device_base(host)
        root = resolve_pack_folder(folder)
        files = pack_files(root)
        if not files:
            return job.finish(False, f"No files to send in {root}.")
        total = sum(p.stat().st_size for _, p in files)
        job.log(f"Pack: {root} ({len(files)} files, {total / 1024:.0f} KB)")
        job.log(f"Reader: {base}")
        status = http_get_json(base + "/api/status")
        job.log(f"Connected to the reader (firmware {status.get('version', '?')}).")

        listings: dict[str, dict[str, bool]] = {}

        def listing(path: str) -> dict[str, bool]:
            if path not in listings:
                entries = http_get_json(base + "/api/files?" + urllib.parse.urlencode({"path": path}))
                listings[path] = {e["name"]: bool(e.get("isDirectory")) for e in entries}
            return listings[path]

        def ensure_dir(path: str) -> None:
            parent, name = path.rsplit("/", 1)
            parent = parent or "/"
            if parent != "/":
                ensure_dir(parent)
            entries = listing(parent)
            if name in entries:
                if not entries[name]:
                    raise RuntimeError(f"{path} exists on the SD card as a file, not a folder.")
                return
            http_post_form(base + "/mkdir", {"name": name, "path": parent})
            entries[name] = True
            listings[path] = {}
            job.log(f"Created folder {path}")

        sent = 0
        started = time.time()
        for i, (rel, local) in enumerate(files, 1):
            dest_dir = STUDY_DEST + ("/" + rel.parent.as_posix() if rel.parent.as_posix() != "." else "")
            ensure_dir(dest_dir)
            name = rel.name
            if name in listing(dest_dir):
                # The reader refuses to overwrite, so remove the old copy of this one file first.
                http_post_form(base + "/delete", {"path": f"{dest_dir}/{name}"})
            http_upload(base + "/upload?" + urllib.parse.urlencode({"path": dest_dir}), local)
            listing(dest_dir)[name] = False
            sent += local.stat().st_size
            job.log(f"[{i}/{len(files)}] {dest_dir}/{name} ({local.stat().st_size / 1024:.0f} KB)")
        secs = max(time.time() - started, 0.1)
        job.finish(True, f"Study pack copied: {len(files)} files, {sent / 1024:.0f} KB in {secs:.0f} s. "
                         "Your progress file on the reader was left as it was.")


# --- Study pack helpers ----------------------------------------------------------------------


def resolve_pack_folder(folder: str) -> Path:
    if not folder:
        raise RuntimeError("Choose the study pack folder first.")
    root = Path(os.path.expanduser(folder)).resolve()
    if not root.is_dir():
        raise RuntimeError(f"Folder not found: {root}")
    # Accept the pack folder itself, or a parent such as Study-App/ or eink-out/ that contains it.
    for candidate in (root, root / "study", root / "eink-out" / "study"):
        if (candidate / "manifest.json").is_file():
            return candidate
    raise RuntimeError(f"{root} does not look like a study pack (no manifest.json). Pick the 'study' folder "
                       "that Study-App's tools/eink_pack.py wrote.")


def pack_files(root: Path) -> list[tuple[Path, Path]]:
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if name.startswith(".") or name in STUDY_SKIP_NAMES:
                continue
            local = Path(dirpath) / name
            out.append((local.relative_to(root), local))
    # manifest.json last, so the reader never sees a manifest that points at files not yet copied.
    out.sort(key=lambda item: (item[0].as_posix() == "manifest.json", item[0].as_posix()))
    return out


def guess_pack_folder() -> str:
    home = Path.home()
    for base in ("Study-App", "Projects/Study-App", "Developer/Study-App", "Documents/Study-App",
                 "Documents/GitHub/Study-App", "code/Study-App", "src/Study-App"):
        candidate = home / base / "eink-out" / "study"
        if (candidate / "manifest.json").is_file():
            return str(candidate)
    return ""


# --- Reader HTTP client ----------------------------------------------------------------------


def device_base(host: str) -> str:
    host = (host or "").strip().rstrip("/")
    if not host:
        raise RuntimeError("Enter the reader's address shown on its File Transfer screen.")
    if not host.startswith(("http://", "https://")):
        host = "http://" + host
    return host


# The reader is on the local network: never send its requests through a proxy from the environment.
_DIRECT = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _open(req: urllib.request.Request):
    try:
        return _DIRECT.open(req, timeout=HTTP_TIMEOUT)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace").strip()
        raise RuntimeError(f"The reader said {exc.code}: {body or exc.reason}") from None
    except (urllib.error.URLError, OSError) as exc:
        reason = getattr(exc, "reason", exc)
        raise RuntimeError(f"Could not reach the reader ({reason}). On the Sticky, open File Transfer and "
                           "make sure it is on the same Wi-Fi as this Mac, then check the address.") from None


def http_get_json(url: str):
    with _open(urllib.request.Request(url)) as resp:
        return json.loads(resp.read().decode("utf-8"))


def http_post_form(url: str, fields: dict) -> str:
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/x-www-form-urlencoded"})
    with _open(req) as resp:
        return resp.read().decode("utf-8", "replace")


def http_upload(url: str, path: Path) -> str:
    boundary = "----stickyhub" + uuid.uuid4().hex
    head = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{path.name}\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    body = head + path.read_bytes() + tail
    req = urllib.request.Request(url, data=body, headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    with _open(req) as resp:
        return resp.read().decode("utf-8", "replace")


# --- Mac helpers -----------------------------------------------------------------------------


def list_ports() -> list[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))


def choose_with_finder(kind: str) -> str:
    if sys.platform != "darwin" or not shutil.which("osascript"):
        raise RuntimeError("The Finder picker only works on macOS. Type the path instead.")
    script = ('POSIX path of (choose folder with prompt "Choose the study pack folder")' if kind == "folder" else
              'POSIX path of (choose file with prompt "Choose the Sticky firmware (.bin or .zip)")')
    # Bring the dialog to the front; it otherwise opens behind the browser.
    full = f'tell application "System Events" to activate\n{script}'
    res = subprocess.run(["osascript", "-e", full], capture_output=True, text=True)
    if res.returncode != 0:
        return ""  # cancelled
    return res.stdout.strip()


def gh_ready() -> bool:
    if not shutil.which("gh"):
        return False
    return subprocess.run(["gh", "auth", "status"], capture_output=True).returncode == 0


def open_prs(repo: str) -> list[dict]:
    if not gh_ready():
        return []
    res = subprocess.run(["gh", "api", f"repos/{repo}/pulls?state=open&per_page=50", "--jq",
                          "[.[] | {number, title}]"], capture_output=True, text=True)
    if res.returncode != 0:
        return []
    try:
        return json.loads(res.stdout or "[]")
    except json.JSONDecodeError:
        return []


# --- Web server ------------------------------------------------------------------------------


def make_handler(hub: Hub, token: str, port: int):
    allowed_hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}

    class Handler(BaseHTTPRequestHandler):
        server_version = "StickyHub/1"

        def log_message(self, fmt, *args):  # keep the Terminal quiet
            pass

        def _send(self, code: int, body: bytes, ctype: str) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code: int, obj) -> None:
            self._send(code, json.dumps(obj).encode(), "application/json")

        def _guard(self) -> bool:
            # Only this page may drive the hub: reject other Host names (DNS rebinding) and any
            # request without the per-run token (other sites posting to localhost).
            if self.headers.get("Host") not in allowed_hosts:
                self._json(403, {"error": "forbidden host"})
                return False
            if self.path.startswith("/api/") and self.headers.get("X-Hub-Token") != token:
                self._json(403, {"error": "bad token"})
                return False
            return True

        def do_GET(self):
            if not self._guard():
                return
            url = urllib.parse.urlparse(self.path)
            if url.path == "/":
                page = PAGE.replace("__TOKEN__", token).replace("__REPO__", html.escape(hub.repo))
                return self._send(200, page.encode(), "text/html; charset=utf-8")
            if url.path == "/api/state":
                return self._json(200, {"ports": list_ports(), "gh": gh_ready(), "studyFolder": guess_pack_folder()})
            if url.path == "/api/prs":
                return self._json(200, {"prs": open_prs(hub.repo), "gh": gh_ready()})
            if url.path == "/api/job":
                offset = int(urllib.parse.parse_qs(url.query).get("from", ["0"])[0] or 0)
                return self._json(200, hub.job.snapshot(offset) if hub.job else {"state": "idle", "lines": [], "next": 0})
            self._json(404, {"error": "not found"})

        def do_POST(self):
            if not self._guard():
                return
            length = int(self.headers.get("Content-Length") or 0)
            try:
                data = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError:
                return self._json(400, {"error": "bad json"})
            path = urllib.parse.urlparse(self.path).path
            try:
                if path == "/api/flash":
                    hub.start("flash", lambda job: hub.flash(job, str(data.get("source", "main")),
                                                             str(data.get("pr", "")), str(data.get("file", "")),
                                                             str(data.get("port", ""))))
                    return self._json(200, {"ok": True})
                if path == "/api/study":
                    hub.start("study", lambda job: hub.upload_study(job, str(data.get("host", "")),
                                                                    str(data.get("folder", ""))))
                    return self._json(200, {"ok": True})
                if path == "/api/check-reader":
                    status = http_get_json(device_base(str(data.get("host", ""))) + "/api/status")
                    return self._json(200, {"ok": True, "status": status})
                if path == "/api/check-pack":
                    root = resolve_pack_folder(str(data.get("folder", "")))
                    files = pack_files(root)
                    size = sum(p.stat().st_size for _, p in files)
                    return self._json(200, {"ok": True, "folder": str(root), "files": len(files), "bytes": size})
                if path == "/api/choose":
                    return self._json(200, {"path": choose_with_finder(str(data.get("kind", "file")))})
            except RuntimeError as exc:
                return self._json(400, {"error": str(exc)})
            self._json(404, {"error": "not found"})

    return Handler


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--script", required=True, help="path to flash-sticky.sh")
    ap.add_argument("--repo", default=os.environ.get("CROSSBLINK_REPO", DEFAULT_REPO))
    ap.add_argument("--port", type=int, default=0, help="local port (default: a free one)")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    hub = Hub(Path(args.script).resolve(), args.repo)
    token = secrets.token_urlsafe(24)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), None)  # type: ignore[arg-type]
    port = server.server_address[1]
    server.RequestHandlerClass = make_handler(hub, token, port)
    url = f"http://127.0.0.1:{port}/"
    print(f"Sticky Hub is running at {url}", flush=True)
    print("Leave this window open while you use the page. Press Ctrl+C here to stop.", flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sticky Hub</title>
<style>
:root { --bg:#f6f5f2; --card:#fff; --ink:#1d1d1f; --muted:#6b6b70; --line:#dedcd6; --accent:#2f5d50;
        --ok:#2e7d32; --bad:#c62828; --log:#111; --logink:#e6e6e6; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#161617; --card:#202022; --ink:#ececec; --muted:#a0a0a6; --line:#34343a; --accent:#7fb8a6;
          --ok:#81c784; --bad:#ef9a9a; --log:#0b0b0c; }
}
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--ink); font:15px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
main { max-width:760px; margin:0 auto; padding:28px 16px 48px; }
h1 { font-size:24px; margin:0 0 4px; }
.sub { color:var(--muted); margin:0 0 24px; }
section { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:18px 18px 20px; margin-bottom:18px; }
h2 { font-size:17px; margin:0 0 4px; }
.hint { color:var(--muted); font-size:13px; margin:0 0 14px; }
label.row { display:flex; gap:10px; align-items:center; margin:8px 0; flex-wrap:wrap; }
.field { display:flex; gap:8px; margin:8px 0 0 26px; flex-wrap:wrap; }
input[type=text], select { flex:1; min-width:0; padding:8px 10px; border:1px solid var(--line); border-radius:8px;
  background:var(--bg); color:var(--ink); font:inherit; }
button { padding:8px 14px; border-radius:8px; border:1px solid var(--line); background:var(--bg); color:var(--ink);
  font:inherit; cursor:pointer; }
button.primary { background:var(--accent); border-color:var(--accent); color:#fff; font-weight:600; padding:10px 18px; }
button:disabled { opacity:.5; cursor:default; }
.actions { display:flex; gap:12px; align-items:center; margin-top:16px; flex-wrap:wrap; }
.status { font-size:13px; color:var(--muted); }
.status.ok { color:var(--ok); } .status.bad { color:var(--bad); }
pre { background:var(--log); color:var(--logink); border-radius:10px; padding:12px; height:260px; overflow:auto;
  font:12px/1.4 ui-monospace, Menlo, monospace; white-space:pre-wrap; margin:0; }
.logtitle { display:flex; justify-content:space-between; align-items:baseline; margin-bottom:8px; }
</style>
</head>
<body>
<main>
  <h1>Sticky Hub</h1>
  <p class="sub">Flash CrossBlink over USB and load your study pack. Your SD card and settings are kept.</p>

  <section>
    <h2>1. Flash firmware (USB)</h2>
    <p class="hint">Plug the Sticky into this Mac with a USB-C data cable and wake it. Only the app firmware is replaced.</p>
    <label class="row"><input type="radio" name="src" value="main" checked> Latest build from main</label>
    <label class="row"><input type="radio" name="src" value="pr"> A pull request build</label>
    <div class="field"><select id="pr"><option value="">Loading pull requests...</option></select></div>
    <label class="row"><input type="radio" name="src" value="file"> A file I downloaded (.bin or .zip)</label>
    <div class="field"><input type="text" id="file" placeholder="/Users/you/Downloads/firmware.bin"><button id="pickFile">Choose...</button></div>
    <label class="row" style="margin-top:14px">USB port
      <select id="port"></select><button id="refresh">Refresh</button></label>
    <div class="actions"><button class="primary" id="flash">Flash Sticky</button><span class="status" id="flashStatus"></span></div>
  </section>

  <section>
    <h2>2. Load study pack (Wi-Fi)</h2>
    <p class="hint">On the Sticky open File Transfer and join the same Wi-Fi as this Mac. Files go to /study/ on the SD card.
      Matching files are replaced, everything else (books, settings, your study progress) is left alone.</p>
    <label class="row">Study pack folder</label>
    <div class="field" style="margin-left:0"><input type="text" id="folder" placeholder="~/Study-App/eink-out/study"><button id="pickFolder">Choose...</button></div>
    <div class="status" id="packStatus" style="margin-top:6px"></div>
    <label class="row" style="margin-top:12px">Reader address</label>
    <div class="field" style="margin-left:0"><input type="text" id="host" value="crosspoint.local"><button id="check">Test connection</button></div>
    <div class="status" id="hostStatus" style="margin-top:6px"></div>
    <div class="actions"><button class="primary" id="study">Send study pack</button><span class="status" id="studyStatus"></span></div>
  </section>

  <section>
    <div class="logtitle"><h2>Progress</h2><span class="status" id="jobState">Idle</span></div>
    <pre id="log">Nothing running yet.</pre>
  </section>
</main>
<script>
const TOKEN = "__TOKEN__";
const $ = (id) => document.getElementById(id);
const store = { get(k){ try { return localStorage.getItem(k) || ""; } catch { return ""; } },
                set(k,v){ try { localStorage.setItem(k, v); } catch {} } };

async function api(path, body) {
  const opts = { headers: { "X-Hub-Token": TOKEN } };
  if (body !== undefined) { opts.method = "POST"; opts.body = JSON.stringify(body); opts.headers["Content-Type"] = "application/json"; }
  const res = await fetch(path, opts);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || ("HTTP " + res.status));
  return data;
}
function setStatus(id, text, cls) { const el = $(id); el.textContent = text; el.className = "status" + (cls ? " " + cls : ""); }

async function loadState() {
  const s = await api("/api/state");
  const port = $("port"), prev = port.value;
  port.innerHTML = "";
  if (!s.ports.length) port.innerHTML = '<option value="">No device found: plug in and wake the Sticky, then Refresh</option>';
  s.ports.forEach(p => port.add(new Option(p, p)));
  if (s.ports.includes(prev)) port.value = prev;
  if (!$("folder").value) $("folder").value = store.get("folder") || s.studyFolder || "";
  if (!s.gh) setStatus("flashStatus", "GitHub CLI not signed in: run 'gh auth login' in Terminal to download builds, or flash a file.", "bad");
  if ($("folder").value) checkPack();
}
async function loadPrs() {
  const sel = $("pr");
  try {
    const { prs, gh } = await api("/api/prs");
    sel.innerHTML = "";
    if (!prs.length) sel.add(new Option(gh ? "No open pull requests found" : "Sign in to GitHub to list pull requests", ""));
    prs.forEach(p => sel.add(new Option("#" + p.number + "  " + p.title, p.number)));
  } catch { sel.innerHTML = '<option value="">Could not load pull requests</option>'; }
}
async function checkPack() {
  const folder = $("folder").value.trim();
  if (!folder) return setStatus("packStatus", "");
  try {
    const r = await api("/api/check-pack", { folder });
    setStatus("packStatus", r.files + " files, " + Math.round(r.bytes / 1024) + " KB in " + r.folder, "ok");
    store.set("folder", folder);
  } catch (e) { setStatus("packStatus", e.message, "bad"); }
}

let polling = null, offset = 0;
function busy(on) { ["flash", "study"].forEach(id => $(id).disabled = on); }
function startPolling(title) {
  offset = 0; $("log").textContent = ""; busy(true);
  setStatus("jobState", title + "...", "");
  clearInterval(polling);
  polling = setInterval(async () => {
    const j = await api("/api/job?from=" + offset).catch(() => null);
    if (!j) return;
    if (j.lines.length) { const log = $("log"); log.textContent += j.lines.join("\n") + "\n"; log.scrollTop = log.scrollHeight; }
    offset = j.next;
    if (j.state !== "running") {
      clearInterval(polling); busy(false);
      setStatus("jobState", j.state === "ok" ? "Done" : "Failed", j.state === "ok" ? "ok" : "bad");
    }
  }, 500);
}

$("refresh").onclick = (e) => { e.preventDefault(); loadState(); };
$("pickFile").onclick = async (e) => { e.preventDefault();
  const r = await api("/api/choose", { kind: "file" }); if (r.path) { $("file").value = r.path; document.querySelector('input[value=file]').checked = true; } };
$("pickFolder").onclick = async (e) => { e.preventDefault();
  const r = await api("/api/choose", { kind: "folder" }); if (r.path) { $("folder").value = r.path; checkPack(); } };
$("folder").onchange = checkPack;
$("pr").onchange = () => { document.querySelector('input[value=pr]').checked = true; };
$("host").value = store.get("host") || "crosspoint.local";
$("check").onclick = async (e) => { e.preventDefault();
  setStatus("hostStatus", "Checking...");
  try { const r = await api("/api/check-reader", { host: $("host").value });
        store.set("host", $("host").value);
        setStatus("hostStatus", "Connected" + (r.status.version ? " (firmware " + r.status.version + ")" : ""), "ok"); }
  catch (err) { setStatus("hostStatus", err.message, "bad"); } };
$("flash").onclick = async () => {
  const source = document.querySelector('input[name=src]:checked').value;
  if (!$("port").value && !confirm("No USB device is listed. Try anyway? The flasher will wait 30 seconds for one.")) return;
  try { await api("/api/flash", { source, pr: $("pr").value, file: $("file").value.trim(), port: $("port").value });
        setStatus("flashStatus", ""); startPolling("Flashing"); }
  catch (e) { setStatus("flashStatus", e.message, "bad"); }
};
$("study").onclick = async () => {
  try { store.set("host", $("host").value);
        await api("/api/study", { host: $("host").value, folder: $("folder").value.trim() });
        setStatus("studyStatus", ""); startPolling("Sending study pack"); }
  catch (e) { setStatus("studyStatus", e.message, "bad"); }
};
loadState(); loadPrs();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    sys.exit(main())
