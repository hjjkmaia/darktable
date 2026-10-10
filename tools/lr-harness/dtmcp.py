#!/usr/bin/env python3
"""Lightroom verification client for this worktree's darktable-mcp.

The "no change" render is darktable's workflow default (exposure +0.7 EV on
a raw), not a flat pipeline. See README.md.
"""

from __future__ import annotations

import argparse
import base64
import fcntl
import json
import os
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get("DTMCP_BIN", str(REPO / "build" / "bin" / "darktable-mcp")))
ROOT = Path("/tmp/lr-harness")
SOCKET = ROOT / "mcp.sock"
PIDFILE = ROOT / "mcp.pid"
STAMP = ROOT / "mcp.stamp"
LOG = ROOT / "mcp.stderr"
LOCK = ROOT / "client.lock"

STARTUP_S = 90
RPC_S = 180
MAX_LINE = 64 * 1024 * 1024

# History renders only to force dt_dev_load_image -> dt_dev_read_history_ext.
# The pixels are discarded. 128px is enough for that load to finish.
HISTORY_WIDTH = 128

CHANNELS = ("r", "g", "b")
METRICS = ("min", "max", "mean", "p1", "p50", "p99", "clip_lo", "clip_hi")

_lock_fh = None


def die(msg: str) -> None:
    print(f"dtmcp: {msg}", file=sys.stderr)
    raise SystemExit(1)


def dump(obj: object) -> str:
    return json.dumps(obj, indent=2, sort_keys=True, allow_nan=False) + "\n"


def num(value: object) -> str:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, int):
        return str(value)
    return format(value, ".10g")


def signed(value: object) -> str:
    text = num(value)
    if text.startswith("-"):
        return text
    return "+" + text


def measurement(stack_loaded: bool, tone_mapper_off: bool) -> str:
    base = "no-tonemapper" if tone_mapper_off else "workflow-default"
    if stack_loaded:
        return base + "+stack"
    return base


def banned_roots() -> list[Path]:
    home = Path.home()
    return [
        home / "Desktop" / "sony",
        home / ".config" / "darktable",
        home / "photo-flavor",
    ]


def under_banned(path: Path) -> Path | None:
    try:
        resolved = path.resolve()
    except OSError as exc:
        die(f"cannot resolve {path}: {exc}")
    for root in banned_roots():
        try:
            resolved.relative_to(root.resolve())
        except (ValueError, OSError):
            continue
        return root
    return None


def guard(path: Path, what: str) -> None:
    root = under_banned(path)
    if root is not None:
        die(f"refusing to {what} {path}: {root} is off limits for this harness")


def code_token() -> str:
    parts = []
    for path in (Path(__file__).resolve(), BIN):
        st = path.stat()
        parts.append(f"{path}:{st.st_mtime_ns}:{st.st_size}")
    return "\n".join(parts) + "\n"


class LineBuffer:
    def __init__(self, fd: int):
        self.fd = fd
        self.buf = b""

    def readline(self, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise TimeoutError("timed out waiting for darktable-mcp")
            ready, _, _ = select.select([self.fd], [], [], remain)
            if not ready:
                raise TimeoutError("timed out waiting for darktable-mcp")
            chunk = os.read(self.fd, 1 << 20)
            if not chunk:
                raise EOFError("darktable-mcp closed the protocol stream")
            self.buf += chunk
            if len(self.buf) > MAX_LINE:
                raise RuntimeError("protocol line exceeded 64MiB")
        line, self.buf = self.buf.split(b"\n", 1)
        return line


def write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        n = os.write(fd, view)
        view = view[n:]


def read_json_line(reader: LineBuffer, timeout: float) -> bytes:
    while True:
        line = reader.readline(timeout).strip()
        if not line:
            continue
        if line.startswith(b"{"):
            return line
        sys.stderr.buffer.write(b"mcp-noise: " + line + b"\n")


def server_cmd() -> list[str]:
    return [
        str(BIN),
        "--core",
        "--configdir", str(ROOT / "cfg"),
        "--cachedir", str(ROOT / "cache"),
        "--library", ":memory:",
        "--conf", "write_sidecar_files=never",
    ]


def server_env() -> dict[str, str]:
    env = os.environ.copy()
    env["GSETTINGS_SCHEMA_DIR"] = "/opt/homebrew/share/glib-2.0/schemas"
    env["XDG_DATA_DIRS"] = "/opt/homebrew/share"
    env["LC_ALL"] = "C"
    env["LANG"] = "C"
    env.pop("DTMCP_SERVE", None)
    return env


def _lock() -> None:
    global _lock_fh
    ROOT.mkdir(parents=True, exist_ok=True)
    _lock_fh = open(LOCK, "a")
    fcntl.flock(_lock_fh.fileno(), fcntl.LOCK_EX)


def _unlock() -> None:
    if _lock_fh is not None:
        fcntl.flock(_lock_fh.fileno(), fcntl.LOCK_UN)


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _read_pid() -> int | None:
    try:
        text = PIDFILE.read_text().strip()
    except OSError:
        return None
    if not text.isdigit():
        return None
    return int(text)


def _stop_pid(pid: int) -> None:
    if not _pid_alive(pid):
        return
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        return
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and _pid_alive(pid):
        time.sleep(0.1)
    if _pid_alive(pid):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def _log_tail() -> str:
    try:
        lines = LOG.read_text(errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(lines[-30:])


def _ping(timeout: float = 5) -> bool:
    if not SOCKET.exists():
        return False
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect(str(SOCKET))
        sock.settimeout(None)
        sock.sendall(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
        reader = LineBuffer(sock.fileno())
        raw = read_json_line(reader, timeout)
        msg = json.loads(raw)
        return "result" in msg and msg.get("id") == 1
    except (OSError, TimeoutError, EOFError, json.JSONDecodeError, RuntimeError):
        return False
    finally:
        sock.close()


def _stamp_ok() -> bool:
    try:
        return STAMP.read_text() == code_token()
    except OSError:
        return False


def serve() -> None:
    if not BIN.is_file():
        print(f"dtmcp: missing server binary {BIN}", file=sys.stderr)
        raise SystemExit(1)
    (ROOT / "cfg").mkdir(parents=True, exist_ok=True)
    (ROOT / "cache").mkdir(parents=True, exist_ok=True)
    if SOCKET.exists():
        SOCKET.unlink()

    log = open(LOG, "ab", buffering=0)
    cmd = server_cmd()
    log.write((" ".join(cmd) + "\n").encode())
    mcp = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=log,
        env=server_env(),
        bufsize=0,
        cwd=str(REPO),
    )
    assert mcp.stdin is not None and mcp.stdout is not None
    reader = LineBuffer(mcp.stdout.fileno())

    def shutdown(signum: int, _frame: object) -> None:
        _stop_mcp(mcp)
        if SOCKET.exists():
            SOCKET.unlink()
        if _read_pid() == os.getpid():
            PIDFILE.unlink(missing_ok=True)
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    init = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "lr-harness", "version": "1"},
        },
    }
    write_all(mcp.stdin.fileno(), json.dumps(init, separators=(",", ":")).encode() + b"\n")
    try:
        raw = read_json_line(reader, STARTUP_S)
    except (TimeoutError, EOFError, RuntimeError) as exc:
        _stop_mcp(mcp)
        print(f"dtmcp: server failed during startup: {exc}", file=sys.stderr)
        raise SystemExit(1)
    msg = json.loads(raw)
    if "error" in msg:
        _stop_mcp(mcp)
        print(f"dtmcp: initialize failed: {msg['error']}", file=sys.stderr)
        raise SystemExit(1)
    note = {"jsonrpc": "2.0", "method": "notifications/initialized"}
    write_all(mcp.stdin.fileno(), json.dumps(note, separators=(",", ":")).encode() + b"\n")

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(str(SOCKET))
    os.chmod(SOCKET, 0o600)
    srv.listen(4)
    PIDFILE.write_text(str(os.getpid()) + "\n")
    STAMP.write_text(code_token())

    while True:
        conn, _addr = srv.accept()
        try:
            _proxy(conn, mcp, reader)
        except (TimeoutError, EOFError, OSError, RuntimeError) as exc:
            print(f"dtmcp: proxy stopped: {exc}", file=sys.stderr)
            _stop_mcp(mcp)
            break
        finally:
            conn.close()
    if SOCKET.exists():
        SOCKET.unlink()
    if _read_pid() == os.getpid():
        PIDFILE.unlink(missing_ok=True)


def _stop_mcp(mcp: subprocess.Popen) -> None:
    if mcp.poll() is not None:
        return
    try:
        if mcp.stdin is not None:
            mcp.stdin.close()
    except OSError:
        pass
    try:
        mcp.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        pass
    mcp.terminate()
    try:
        mcp.wait(timeout=5)
    except subprocess.TimeoutExpired:
        mcp.kill()


def _proxy(conn: socket.socket, mcp: subprocess.Popen, reader: LineBuffer) -> None:
    client = LineBuffer(conn.fileno())
    assert mcp.stdin is not None
    while True:
        try:
            req = client.readline(RPC_S)
        except EOFError:
            return
        if not req.strip():
            continue
        if mcp.poll() is not None:
            raise EOFError(f"darktable-mcp exited {mcp.returncode}")
        write_all(mcp.stdin.fileno(), req + b"\n")
        resp = read_json_line(reader, RPC_S)
        write_all(conn.fileno(), resp + b"\n")


def ensure_server() -> None:
    if not BIN.is_file():
        die(f"missing server binary {BIN}")
    _lock()
    try:
        pid = _read_pid()
        alive = pid is not None and _pid_alive(pid)
        # A live server may be inside a render. Wait for it; do not kill it.
        if alive and _stamp_ok():
            if _ping(RPC_S):
                return
            die(f"darktable-mcp pid {pid} is not answering; see {LOG}")
        if alive and pid is not None:
            _stop_pid(pid)
        if SOCKET.exists():
            SOCKET.unlink()
        env = os.environ.copy()
        env["DTMCP_SERVE"] = "1"
        env["PYTHONUNBUFFERED"] = "1"
        log = open(LOG, "ab", buffering=0)
        proc = subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve())],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=log,
            start_new_session=True,
            cwd=str(REPO),
        )
        deadline = time.monotonic() + STARTUP_S
        while time.monotonic() < deadline:
            if _ping(timeout=2):
                return
            if proc.poll() is not None:
                tail = _log_tail()
                die(f"darktable-mcp exited {proc.returncode} during startup\n{tail}")
            time.sleep(0.2)
        _stop_pid(proc.pid)
        tail = _log_tail()
        die(f"darktable-mcp was not ready within {STARTUP_S}s\n{tail}")
    finally:
        _unlock()


class Session:
    def __init__(self) -> None:
        ensure_server()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # The daemon serves one connection at a time. Connect waits out a
        # render another client already started.
        self.sock.settimeout(RPC_S)
        self.sock.connect(str(SOCKET))
        self.sock.settimeout(None)
        self.reader = LineBuffer(self.sock.fileno())
        self.nid = 0

    def close(self) -> None:
        self.sock.close()

    def call(self, method: str, params: dict | None = None, timeout: float = RPC_S) -> dict:
        self.nid += 1
        req: dict = {"jsonrpc": "2.0", "id": self.nid, "method": method}
        if params is not None:
            req["params"] = params
        self.sock.sendall(json.dumps(req, separators=(",", ":")).encode() + b"\n")
        raw = read_json_line(self.reader, timeout)
        msg = json.loads(raw)
        if "error" in msg:
            err = msg["error"]
            message = err.get("message", err) if isinstance(err, dict) else err
            die(str(message))
        if "result" not in msg:
            die(f"response had no result: {raw[:200]!r}")
        return msg["result"]

    def tool(self, name: str, arguments: dict | None = None) -> dict:
        result = self.call(
            "tools/call",
            {"name": name, "arguments": arguments or {}},
        )
        if result.get("isError"):
            die(_content_text(result) or f"{name} failed")
        return result


def _content_text(result: dict) -> str:
    parts = []
    for block in result.get("content") or []:
        if isinstance(block, dict) and block.get("type") == "text":
            parts.append(block.get("text") or "")
    return "\n".join(parts)


def content_json(result: dict) -> object:
    text = _content_text(result)
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        die(text or "tool returned non-JSON text")


def content_png(result: dict) -> bytes:
    for block in result.get("content") or []:
        if isinstance(block, dict) and block.get("type") == "image":
            data = block.get("data") or ""
            try:
                return base64.b64decode(data, validate=False)
            except (ValueError, TypeError) as exc:
                die(f"render image was not base64: {exc}")
    die("render returned no PNG")


def png_size(data: bytes) -> tuple[int, int]:
    sig = b"\x89PNG\r\n\x1a\n"
    if len(data) < 24 or data[:8] != sig or data[12:16] != b"IHDR":
        die("render did not return a PNG")
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    return width, height


def raw_path(text: str) -> Path:
    path = Path(text).expanduser().resolve()
    if not path.is_file():
        die(f"raw not found: {path}")
    guard(path, "open")
    return path


def load_stack(text: str) -> list:
    path = Path(text).expanduser().resolve()
    if not path.is_file():
        die(f"stack not found: {path}")
    try:
        data = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        die(f"{path}: {exc}")
    if isinstance(data, dict) and isinstance(data.get("stack"), list):
        data = data["stack"]
    elif isinstance(data, dict) and "operation" in data:
        data = [data]
    if not isinstance(data, list):
        die(f"{path}: expected a JSON array of {{operation, params}} entries")
    for i, entry in enumerate(data):
        if not isinstance(entry, dict) or not entry.get("operation"):
            die(f"{path}: entry {i} needs an operation string")
    return data


def stats_args(raw: Path, width: int, stack: list | None, tone_mapper_off: bool) -> dict:
    args: dict = {"input": {"path": str(raw)}, "width": width}
    if stack:
        args["stack"] = stack
    if tone_mapper_off:
        args["disable_tone_mappers"] = True
    return args


def fetch_stats(session: Session, raw: Path, width: int, stack: list | None,
                tone_mapper_off: bool) -> dict:
    result = session.tool("image_stats", stats_args(raw, width, stack, tone_mapper_off))
    stats = content_json(result)
    if not isinstance(stats, dict) or "channels" not in stats:
        die(f"image_stats returned unexpected JSON: {stats!r}"[:500])
    return stats


def stats_document(raw: Path, measure: str, stats: dict) -> dict:
    return {
        "file": raw.name,
        "measurement": measure,
        "width": stats.get("width"),
        "height": stats.get("height"),
        "channels": stats.get("channels"),
    }


def print_stats(doc: dict) -> None:
    print(f"{doc['file']}  {doc['width']}x{doc['height']}  {doc['measurement']}")
    channels = doc["channels"]
    for name in CHANNELS:
        ch = channels[name]
        bits = " ".join(f"{key}={num(ch[key])}" for key in METRICS)
        print(f"{name}  {bits}")


def channel_delta(a: dict, b: dict) -> dict:
    out = {}
    for key in METRICS:
        if key not in a or key not in b:
            die(f"image_stats channel missing {key}")
        av, bv = a[key], b[key]
        if isinstance(av, int) and isinstance(bv, int) and not isinstance(av, bool):
            delta = bv - av
        else:
            delta = float(bv) - float(av)
        out[key] = {"a": av, "b": bv, "delta": delta}
    return out


def cmd_tools(session: Session) -> None:
    result = session.call("tools/list")
    tools = result.get("tools") if isinstance(result, dict) else None
    if not isinstance(tools, list):
        die(f"tools/list returned unexpected JSON: {result!r}"[:500])
    for tool in tools:
        name = tool.get("name") if isinstance(tool, dict) else None
        if name:
            print(name)


def cmd_schema(session: Session, op: str) -> None:
    result = session.tool("module_schema", {"operation": op})
    print(dump(content_json(result)), end="")


def cmd_stats(session: Session, raw: Path, width: int, stack_path: str | None,
              tone_mapper_off: bool, as_json: bool) -> None:
    stack = load_stack(stack_path) if stack_path else None
    measure = measurement(bool(stack), tone_mapper_off)
    doc = stats_document(raw, measure, fetch_stats(session, raw, width, stack, tone_mapper_off))
    if as_json:
        print(dump(doc), end="")
    else:
        print_stats(doc)


def cmd_render(session: Session, raw: Path, stack_path: str | None, out: Path) -> None:
    guard(out, "write")
    stack = load_stack(stack_path) if stack_path else None
    args: dict = {"input": {"path": str(raw)}}
    if stack:
        args["stack"] = stack
    png = content_png(session.tool("render", args))
    width, height = png_size(png)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".part")
    try:
        tmp.write_bytes(png)
        os.replace(tmp, out)
    finally:
        if tmp.exists():
            tmp.unlink()
    print(f"wrote {out} ({width}x{height})")


def cmd_compare(session: Session, raw: Path, stack_a: str, stack_b: str, as_json: bool) -> None:
    a_stack = load_stack(stack_a)
    b_stack = load_stack(stack_b)
    a = fetch_stats(session, raw, 512, a_stack, False)
    b = fetch_stats(session, raw, 512, b_stack, False)
    if a.get("width") != b.get("width") or a.get("height") != b.get("height"):
        die(
            "renders differ in size: "
            f"{a.get('width')}x{a.get('height')} vs {b.get('width')}x{b.get('height')}"
        )
    channels = {}
    for name in CHANNELS:
        if name not in a["channels"] or name not in b["channels"]:
            die(f"image_stats missing channel {name}")
        channels[name] = channel_delta(a["channels"][name], b["channels"][name])
    doc = {
        "file": raw.name,
        "measurement": "workflow-default+stack",
        "delta": "B-A",
        "stack_a": Path(stack_a).name,
        "stack_b": Path(stack_b).name,
        "width": a.get("width"),
        "height": a.get("height"),
        "channels": channels,
    }
    if as_json:
        print(dump(doc), end="")
        return
    print(
        f"{doc['file']}  {doc['width']}x{doc['height']}  "
        f"{doc['stack_a']} -> {doc['stack_b']}  delta = B - A"
    )
    print("each stack is applied on the workflow-default history")
    for name in CHANNELS:
        for key in METRICS:
            row = channels[name][key]
            print(f"{name}.{key}  a={num(row['a'])}  b={num(row['b'])}  delta={signed(row['delta'])}")


def photo_sidecar(raw: Path) -> Path:
    # DSC04299.ARW -> DSC04299.xmp. Never the darktable DSC04299.ARW.xmp.
    sidecar = raw.with_suffix(".xmp")
    if not sidecar.is_file():
        die(
            f"{sidecar.name} must sit beside {raw.name} "
            "(photo-flavor sidecar, not the darktable .ARW.xmp)"
        )
    guard(sidecar, "open")
    return sidecar


def cmd_history(session: Session, raw: Path) -> None:
    """First-open import of a scratch copy, then the decoded history.

    import_images alone is the library path (dev == NULL): metadata only.
    image_stats on that imgid is what runs dt_dev_load_image and therefore
    dt_dev_read_history_ext. The copy is unique so auto-presets have not
    been applied yet, and the caller's file is never the path darktable opens.
    """
    sidecar = photo_sidecar(raw)
    hist_root = ROOT / "history"
    hist_root.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="open-", dir=str(hist_root)))
    scratch_raw = scratch / raw.name
    shutil.copy2(raw, scratch_raw)
    shutil.copy2(sidecar, scratch / sidecar.name)

    imported = content_json(session.tool("import_images", {"paths": [str(scratch_raw)]}))
    if not isinstance(imported, dict):
        die(f"import_images returned unexpected JSON: {imported!r}"[:500])
    images = imported.get("images") or []
    if not images or images[0].get("status") == "failed" or "imgid" not in images[0]:
        err = images[0].get("error") if images else imported
        die(f"import failed: {err}")
    imgid = images[0]["imgid"]

    # This render is the headless stand-in for the first darkroom open.
    session.tool("image_stats", {"input": {"imgid": imgid}, "width": HISTORY_WIDTH})
    history = content_json(session.tool("get_history", {"imgid": imgid}))
    if not isinstance(history, list):
        die(f"get_history returned unexpected JSON: {history!r}"[:500])
    doc = {
        "file": raw.name,
        "sidecar": sidecar.name,
        "history": history,
    }
    print(dump(doc), end="")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dtmcp.py",
        description=(
            "Measure Lightroom-style edits through darktable-mcp. "
            "The no-change render is the workflow default (exposure +0.7 EV), "
            "not a flat render."
        ),
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("tools", help="list server tools")

    schema = sub.add_parser("schema", help="print one module's parameter schema")
    schema.add_argument("--op", required=True, help="module operation, e.g. exposure")

    stats = sub.add_parser("stats", help="per-channel stats of a developed preview")
    stats.add_argument("--raw", required=True)
    stats.add_argument("--stack", help="JSON stack layered on the workflow default")
    stats.add_argument("--width", type=int, default=512)
    stats.add_argument("--no-tonemapper", action="store_true",
                       help="disable the workflow tone mapper")
    stats.add_argument("--json", action="store_true")

    render = sub.add_parser("render", help="write a PNG preview")
    render.add_argument("--raw", required=True)
    render.add_argument("--stack", help="JSON stack layered on the workflow default")
    render.add_argument("--out", required=True)

    compare = sub.add_parser("compare", help="per-channel deltas of stack B minus stack A")
    compare.add_argument("--raw", required=True)
    compare.add_argument("--stack", dest="stacks", action="append", required=True,
                         metavar="S.json",
                         help="pass twice: --stack A.json --stack B.json")
    compare.add_argument("--json", action="store_true")

    history = sub.add_parser("history", help="first-open import on a scratch copy, then history")
    history.add_argument("--raw", required=True,
                         help="a raw whose photo-flavor basename.xmp sits beside it")
    return parser


def main() -> None:
    if os.environ.get("DTMCP_SERVE") == "1":
        serve()
        return
    args = build_parser().parse_args()
    if args.cmd == "stats" and args.width <= 0:
        die("--width must be a positive pixel count")
    if args.cmd == "compare" and len(args.stacks) != 2:
        die("compare needs exactly two --stack files, A then B")

    raw = None
    out = None
    if args.cmd in ("stats", "render", "compare", "history"):
        raw = raw_path(args.raw)
    if args.cmd == "render":
        out = Path(args.out).expanduser()
        guard(out, "write")

    session = Session()
    try:
        if args.cmd == "tools":
            cmd_tools(session)
        elif args.cmd == "schema":
            cmd_schema(session, args.op)
        elif args.cmd == "stats":
            cmd_stats(session, raw, args.width, args.stack, args.no_tonemapper, args.json)
        elif args.cmd == "render":
            cmd_render(session, raw, args.stack, out)
        elif args.cmd == "compare":
            cmd_compare(session, raw, args.stacks[0], args.stacks[1], args.json)
        elif args.cmd == "history":
            cmd_history(session, raw)
        else:
            die(f"unknown command {args.cmd}")
    finally:
        session.close()


if __name__ == "__main__":
    try:
        main()
    except (TimeoutError, EOFError, ConnectionError, json.JSONDecodeError, OSError) as exc:
        die(str(exc))
