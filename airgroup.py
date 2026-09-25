#!/usr/bin/env python3
"""airgroup: play AirPlay audio (e.g. Apple Music on an iPhone) on a Google Cast speaker group.

    iPhone --AirPlay--> shairport-sync --raw PCM via FIFO--> airgroup
    airgroup: real-time pacer -> ffmpeg (MP3) -> http://<pi>:8090/stream.mp3
    Google Home group <--HTTP-- pulls the stream after airgroup tells it to play the URL

shairport-sync's session hooks call the small HTTP API below (/api/start, /api/stop,
/api/volume) so casting starts when you pick the speaker on the phone, stops a while
after you disconnect, and the phone's volume slider controls the speaker group.

Run with --list to print the Cast devices and groups visible on the network.
"""

from __future__ import annotations

import json
import logging
import os
import queue
import signal
import socket
import stat
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import pychromecast
from pychromecast.config import APP_MEDIA_RECEIVER

log = logging.getLogger("airgroup")


def _env_float(name: str, default: float) -> float:
    return float(os.environ.get(name, default))


CAST_TARGET = os.environ.get("CAST_TARGET", "")
HTTP_PORT = int(os.environ.get("HTTP_PORT", "8090"))
ADVERTISE_HOST = os.environ.get("ADVERTISE_HOST", "")  # blank = auto-detect LAN IP
FIFO_PATH = os.environ.get("FIFO_PATH", "/run/airgroup/audio.fifo")
MP3_BITRATE = os.environ.get("MP3_BITRATE", "320k")
STREAM_TITLE = os.environ.get("STREAM_TITLE", "AirPlay")
SYNC_VOLUME = os.environ.get("SYNC_VOLUME", "1").lower() not in ("0", "false", "no")
MAX_VOLUME = _env_float("MAX_VOLUME", 1.0)
STOP_GRACE_SEC = _env_float("STOP_GRACE_SEC", 30)
SILENCE_TIMEOUT_SEC = _env_float("SILENCE_TIMEOUT_SEC", 600)
DISCOVERY_TIMEOUT_SEC = _env_float("DISCOVERY_TIMEOUT_SEC", 10)

# shairport-sync's pipe backend emits signed 16-bit little-endian stereo at 44.1 kHz.
SAMPLE_RATE = 44100
FRAME_BYTES = 4
TICK_SEC = 0.02
CHUNK_BYTES = int(SAMPLE_RATE * TICK_SEC) * FRAME_BYTES
PREBUFFER_BYTES = int(SAMPLE_RATE * 0.15) * FRAME_BYTES  # jitter buffer before (re)starting audio
MAX_BUFFER_BYTES = int(SAMPLE_RATE * 0.6) * FRAME_BYTES  # beyond this we drop old audio to cap latency
SILENCE = bytes(CHUNK_BYTES)


def lan_ip() -> str:
    """The address the speakers should use to reach us (no packets are sent)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]


class PcmSource:
    """Reads PCM from shairport-sync's FIFO and hands it out in fixed real-time chunks.

    When the phone is paused or not connected there is no PCM, so next_chunk() returns
    silence. That keeps the MP3 stream flowing continuously, which the Cast receiver needs.
    """

    def __init__(self, path: str):
        self.path = path
        self.last_audio = 0.0  # monotonic time real audio last arrived
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._primed = False

    def open(self) -> None:
        if os.path.lexists(self.path) and not stat.S_ISFIFO(os.lstat(self.path).st_mode):
            os.remove(self.path)
        if not os.path.exists(self.path):
            os.mkfifo(self.path)
        os.chmod(self.path, 0o666)  # shairport-sync runs as its own user
        # O_RDWR keeps a writer attached, so reads never hit EOF between AirPlay sessions.
        fd = os.open(self.path, os.O_RDWR)
        threading.Thread(target=self._read_loop, args=(fd,), name="fifo", daemon=True).start()

    def _read_loop(self, fd: int) -> None:
        while True:
            data = os.read(fd, 65536)
            with self._lock:
                self._buf += data
                self.last_audio = time.monotonic()
                if len(self._buf) > MAX_BUFFER_BYTES:
                    # Nobody is consuming, or we've drifted behind: keep only the newest audio.
                    drop = (len(self._buf) - PREBUFFER_BYTES) // FRAME_BYTES * FRAME_BYTES
                    del self._buf[:drop]

    def next_chunk(self) -> bytes:
        with self._lock:
            if not self._primed and len(self._buf) >= PREBUFFER_BYTES:
                self._primed = True
            if self._primed and len(self._buf) >= CHUNK_BYTES:
                chunk = bytes(self._buf[:CHUNK_BYTES])
                del self._buf[:CHUNK_BYTES]
                return chunk
            self._primed = False  # underrun (pause/stop): wait for a fresh prebuffer
            return SILENCE


class Listener:
    def __init__(self):
        self.q: queue.Queue[bytes] = queue.Queue(maxsize=256)
        self.closed = False


class Streamer:
    """Feeds paced PCM into ffmpeg and fans the MP3 output out to HTTP listeners."""

    def __init__(self, source: PcmSource):
        self.source = source
        self.last_listener = 0.0  # monotonic time we last had at least one listener
        self._proc: subprocess.Popen | None = None
        self._stop = threading.Event()
        self._listeners: set[Listener] = set()
        self._lock = threading.Lock()

    @property
    def running(self) -> bool:
        return self._proc is not None

    @property
    def listener_count(self) -> int:
        with self._lock:
            return len(self._listeners)

    def start(self) -> None:
        if self._proc is not None:
            return
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "2", "-i", "pipe:0",
            "-c:a", "libmp3lame", "-b:a", MP3_BITRATE, "-write_xing", "0",
            "-flush_packets", "1", "-f", "mp3", "pipe:1",
        ]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self._proc, self._stop = proc, threading.Event()
        self.last_listener = time.monotonic()
        threading.Thread(target=self._pace, args=(proc, self._stop), name="pacer", daemon=True).start()
        threading.Thread(target=self._fan_out, args=(proc,), name="fanout", daemon=True).start()
        log.info("Encoder started (MP3 %s)", MP3_BITRATE)

    def stop(self) -> None:
        proc, self._proc = self._proc, None
        if proc is None:
            return
        self._stop.set()
        try:
            proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        self._close_listeners()
        log.info("Encoder stopped")

    def subscribe(self) -> Listener | None:
        if self._proc is None:
            return None
        listener = Listener()
        with self._lock:
            self._listeners.add(listener)
        return listener

    def unsubscribe(self, listener: Listener) -> None:
        listener.closed = True
        with self._lock:
            self._listeners.discard(listener)

    def _close_listeners(self) -> None:
        with self._lock:
            listeners, self._listeners = self._listeners, set()
        for listener in listeners:
            listener.closed = True

    def _pace(self, proc: subprocess.Popen, stop: threading.Event) -> None:
        deadline = time.monotonic()
        while not stop.is_set():
            try:
                proc.stdin.write(self.source.next_chunk())
            except (OSError, ValueError):
                break
            deadline += TICK_SEC
            delay = deadline - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            elif delay < -0.5:
                deadline = time.monotonic()  # we stalled; don't try to catch up in a burst

    def _fan_out(self, proc: subprocess.Popen) -> None:
        while True:
            data = proc.stdout.read(4096)
            if not data:
                break
            with self._lock:
                listeners = list(self._listeners)
            if listeners:
                self.last_listener = time.monotonic()
            for listener in listeners:
                try:
                    listener.q.put_nowait(data)
                except queue.Full:
                    log.warning("Dropping a listener that stopped reading")
                    self.unsubscribe(listener)
        if self._proc is proc:
            log.error("ffmpeg exited unexpectedly (code %s)", proc.poll())
            self._proc = None
            self._close_listeners()


class Caster:
    """Thin wrapper around pychromecast for one named Cast device or speaker group."""

    def __init__(self, name: str):
        self.name = name
        self._cast = None
        self._browser = None

    def connect(self):
        if self._cast is not None:
            if self._cast.socket_client.is_connected:
                return self._cast
            log.info("Lost connection to %r; rediscovering", self.name)
            self.disconnect()
        casts, browser = pychromecast.get_listed_chromecasts(
            friendly_names=[self.name], discovery_timeout=DISCOVERY_TIMEOUT_SEC
        )
        if not casts:
            browser.stop_discovery()
            raise RuntimeError(f"No Cast device or group named {self.name!r} found on the network")
        cast = casts[0]
        cast.wait(timeout=10)
        self._cast, self._browser = cast, browser
        log.info("Connected to %r (%s)", self.name, cast.cast_info.cast_type)
        return cast

    def disconnect(self) -> None:
        cast, browser = self._cast, self._browser
        self._cast = self._browser = None
        try:
            if cast is not None:
                cast.disconnect(timeout=2)
        finally:
            if browser is not None:
                browser.stop_discovery()

    def play(self, url: str) -> None:
        mc = self.connect().media_controller
        mc.play_media(url, "audio/mpeg", title=STREAM_TITLE, stream_type="LIVE")
        mc.block_until_active(timeout=15)
        log.info("Casting %s to %r", url, self.name)

    def stop(self, url: str) -> None:
        if self.owner(url) == "us":
            self._cast.quit_app()
            log.info("Stopped casting to %r", self.name)

    def set_volume(self, level: float) -> None:
        self.connect().set_volume(level)

    def owner(self, url: str) -> str:
        """Who is using the speakers: "us", "other" (another app/stream), "none", or "unknown"."""
        cast = self._cast
        if cast is None or not cast.socket_client.is_connected:
            return "unknown"
        if cast.app_id is None:
            return "none"
        if cast.app_id != APP_MEDIA_RECEIVER:
            return "other"
        content_id = cast.media_controller.status.content_id
        return "other" if content_id and content_id != url else "us"


class Bridge:
    """Owns the session state. All Cast I/O happens on this one worker thread, in order."""

    def __init__(self, source: PcmSource, streamer: Streamer, caster: Caster):
        self.source, self.streamer, self.caster = source, streamer, caster
        self.state = "idle"  # idle | starting | casting | preempted | error
        self.last_error: str | None = None
        self.url = ""
        self.volume: float | None = None
        self._cmds: queue.Queue = queue.Queue()
        self._session_active = False
        self._stop_at: float | None = None
        self._cast_started = 0.0
        self._last_attempt = 0.0
        self._recasts = 0

    # Called from HTTP threads.
    def request(self, cmd: str, **kwargs) -> None:
        self._cmds.put((cmd, kwargs))

    def request_volume(self, db: float) -> None:
        # AirPlay volume is -30.0 (quietest) .. 0.0 dB, with -144.0 meaning mute.
        self.volume = max(0.0, min(1.0, (db + 30) / 30)) * MAX_VOLUME
        self.request("volume")

    def status(self) -> dict:
        now = time.monotonic()
        return {
            "state": self.state,
            "target": self.caster.name,
            "stream_url": self.url,
            "listeners": self.streamer.listener_count,
            "audio_flowing": now - self.source.last_audio < 1,
            "volume": None if self.volume is None else round(self.volume, 2),
            "stop_in_sec": None if self._stop_at is None else max(0, round(self._stop_at - now)),
            "last_error": self.last_error,
        }

    # Worker thread.
    def run(self) -> None:
        self.request("warmup")
        last_tick = 0.0
        while True:
            try:
                cmd, kwargs = self._cmds.get(timeout=2)
            except queue.Empty:
                cmd, kwargs = "tick", {}
            try:
                getattr(self, "_on_" + cmd)(**kwargs)
            except Exception as e:
                log.exception("%s failed", cmd)
                self.last_error = f"{cmd}: {e}"
                if cmd == "start":
                    self.state = "error"
            if cmd != "tick" and time.monotonic() - last_tick >= 2:
                self.request("tick")
            if cmd == "tick":
                last_tick = time.monotonic()

    def _on_warmup(self) -> None:
        # Discover the group up front so the first "play" from the phone starts quickly.
        self.caster.connect()

    def _on_start(self, force: bool = False) -> None:
        self._session_active = True
        self._stop_at = None
        if self.state == "casting" and not force:
            return
        self.state = "starting"
        self._last_attempt = time.monotonic()
        self.url = f"http://{ADVERTISE_HOST or lan_ip()}:{HTTP_PORT}/stream.mp3"
        self.streamer.start()
        self.caster.play(self.url)
        self.state = "casting"
        self.last_error = None
        self._cast_started = time.monotonic()
        if not force:
            self._recasts = 0
        if SYNC_VOLUME and self.volume is not None:
            self.caster.set_volume(self.volume)

    def _on_stop(self, now: bool = False) -> None:
        self._session_active = False
        if now or STOP_GRACE_SEC <= 0:
            self._teardown()
        elif self.state != "idle":
            # Wait a bit: switching tracks or re-selecting the speaker shouldn't drop the group.
            self._stop_at = time.monotonic() + STOP_GRACE_SEC

    def _on_volume(self) -> None:
        if SYNC_VOLUME and self.state == "casting" and self.volume is not None:
            self.caster.set_volume(self.volume)

    def _on_tick(self) -> None:
        now = time.monotonic()
        if self._stop_at is not None and now >= self._stop_at:
            log.info("AirPlay session ended %.0fs ago; releasing the speakers", STOP_GRACE_SEC)
            self._teardown()
        elif self.state == "casting":
            self._check_cast(now)
        elif self.state == "error" and self._session_active and now - self._last_attempt > 15:
            log.info("Retrying cast")
            self._on_start()

    def _check_cast(self, now: float) -> None:
        since_cast = now - self._cast_started
        if since_cast > SILENCE_TIMEOUT_SEC and now - self.source.last_audio > SILENCE_TIMEOUT_SEC:
            log.info("No audio for %.0fs; releasing the speakers", SILENCE_TIMEOUT_SEC)
            self._teardown()
            return
        owner = self.caster.owner(self.url) if since_cast > 10 else "unknown"
        if owner in ("other", "none"):
            # Someone cast something else or said "Hey Google, stop": respect that.
            log.info("Speakers were taken over or stopped from elsewhere; standing down")
            self.streamer.stop()
            self.state = "preempted"
        elif now - self.streamer.last_listener > 20:
            if self._recasts >= 5:
                self.state = "error"
                self.last_error = "The speakers keep dropping the stream"
                return
            self._recasts += 1
            log.warning("Speakers stopped pulling the stream; recasting (attempt %d)", self._recasts)
            self._on_start(force=True)

    def _teardown(self) -> None:
        self._stop_at = None
        try:
            if self.url:
                self.caster.stop(self.url)
        finally:
            self.streamer.stop()
            self.state = "idle"


STATUS_PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>airgroup</title>
<style>
:root{color-scheme:light dark;--bg:#f6f6f4;--card:#fff;--fg:#1b1b1b;--muted:#6b6b6b;--accent:#1a73e8;--line:#e3e3e0}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--card:#1e1e1e;--fg:#ececec;--muted:#9a9a9a;--accent:#8ab4f8;--line:#2e2e2e}}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,sans-serif}
main{max-width:28rem;margin:0 auto;padding:2rem 1rem}
h1{font-size:1.25rem;margin:0 0 1rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1rem 1.25rem}
dl{display:grid;grid-template-columns:auto 1fr;gap:.35rem 1rem;margin:0}
dt{color:var(--muted)}dd{margin:0;overflow-wrap:anywhere}
.row{display:flex;gap:.5rem;margin-top:1rem}
button{flex:1;padding:.7rem;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg);font:inherit;cursor:pointer}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--bg)}
</style></head><body><main>
<h1>AirPlay &rarr; Cast bridge</h1>
<div class="card"><dl id="s"></dl>
<div class="row"><button class="primary" onclick="post('/api/start?force=1')">Cast now</button>
<button onclick="post('/api/stop?now=1')">Stop</button></div></div>
</main><script>
const labels={state:"State",target:"Speakers",listeners:"Listeners",audio_flowing:"Audio from phone",volume:"Volume",stop_in_sec:"Stopping in (s)",last_error:"Last error",stream_url:"Stream"};
async function refresh(){try{const s=await (await fetch('/api/status')).json();
document.getElementById('s').innerHTML=Object.entries(labels).filter(([k])=>s[k]!==null&&s[k]!=="").map(([k,l])=>`<dt>${l}</dt><dd>${String(s[k]).replace(/</g,'&lt;')}</dd>`).join('')}catch(e){}}
async function post(p){await fetch(p,{method:'POST'});setTimeout(refresh,300)}
refresh();setInterval(refresh,2000);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    server_version = "airgroup"

    @property
    def bridge(self) -> Bridge:
        return self.server.bridge

    def log_message(self, fmt, *args):
        log.debug("%s %s", self.address_string(), fmt % args)

    def do_HEAD(self):
        if urlparse(self.path).path == "/stream.mp3":
            self._stream_headers()
        else:
            self.send_error(404)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/stream.mp3":
            self._stream()
        elif path == "/api/status":
            self._send(200, "application/json", json.dumps(self.bridge.status()).encode())
        elif path == "/":
            self._send(200, "text/html; charset=utf-8", STATUS_PAGE.encode())
        else:
            self.send_error(404)

    def do_POST(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        flag = lambda name: query.get(name, ["0"])[0] in ("1", "true", "yes")
        if url.path == "/api/start":
            self.bridge.request("start", force=flag("force"))
        elif url.path == "/api/stop":
            self.bridge.request("stop", now=flag("now"))
        elif url.path == "/api/volume":
            try:
                self.bridge.request_volume(float(query["db"][0]))
            except (KeyError, ValueError):
                self.send_error(400, "expected ?db=<AirPlay volume in dB>")
                return
        else:
            self.send_error(404)
            return
        self._send(200, "application/json", b'{"ok":true}')

    def _send(self, code: int, ctype: str, body: bytes) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _stream_headers(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Cache-Control", "no-cache, no-store")
        self.send_header("Connection", "close")
        self.end_headers()

    def _stream(self) -> None:
        listener = self.bridge.streamer.subscribe()
        if listener is None:
            self.send_error(503, "Not streaming")
            return
        log.info("Listener connected: %s", self.client_address[0])
        self._stream_headers()
        try:
            while not listener.closed:
                try:
                    data = listener.q.get(timeout=1)
                except queue.Empty:
                    continue
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        finally:
            self.bridge.streamer.unsubscribe(listener)
            log.info("Listener disconnected: %s", self.client_address[0])


def list_devices() -> None:
    print(f"Looking for Cast devices for {DISCOVERY_TIMEOUT_SEC:.0f}s...")
    devices, browser = pychromecast.discovery.discover_chromecasts(timeout=DISCOVERY_TIMEOUT_SEC)
    browser.stop_discovery()
    for d in sorted(devices, key=lambda d: (d.cast_type != "group", d.friendly_name.lower())):
        kind = "GROUP " if d.cast_type == "group" else "device"
        print(f"  {kind}  {d.friendly_name!r:32}  {d.model_name}  {d.host}")
    if not devices:
        print("  None found. Is the Pi on the same network/VLAN as the speakers?")


def main() -> None:
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO").upper(),
        format="%(levelname)s %(name)s: %(message)s",
    )
    logging.getLogger("pychromecast").setLevel(logging.WARNING)
    if "--list" in sys.argv:
        list_devices()
        return
    if not CAST_TARGET:
        sys.exit("CAST_TARGET is not set. Put your speaker group's name in /etc/default/airgroup.")

    source = PcmSource(FIFO_PATH)
    source.open()
    bridge = Bridge(source, Streamer(source), Caster(CAST_TARGET))

    server = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    server.daemon_threads = True
    server.bridge = bridge
    threading.Thread(target=bridge.run, name="bridge", daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown).start())

    log.info("Ready: AirPlay audio will be cast to %r (status page on port %d)", CAST_TARGET, HTTP_PORT)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if bridge.state in ("starting", "casting"):
            try:
                bridge.caster.stop(bridge.url)
            except Exception:
                log.exception("Could not stop casting on shutdown")
        bridge.streamer.stop()


if __name__ == "__main__":
    main()
