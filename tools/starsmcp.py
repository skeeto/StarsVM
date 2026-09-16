#!/usr/bin/env python3
"""starsmcp.py - MCP stdio server for the StarsVM LLM harness.

The emulator's harness build (StarsVM-harness.exe) serves a named pipe; this
script is the other end.  It is a dev tool: it is never shipped, and it exists
so an agent can drive the game with structured tool calls instead of a
PowerShell process per observation.

Run with no arguments for the MCP stdio server (which launches the game itself
and owns its lifetime).  Run with --cli for a one-shot command against a
running or freshly launched instance:

    python tools/starsmcp.py --cli observe
    python tools/starsmcp.py --cli click 0x001309ba 201
    python tools/starsmcp.py --cli --attach observe

Every request has a timeout.  The pipe is a game-loop safe point, so a reply
arrives only when the game next pumps; if it is stuck in a long computation the
call must fail cleanly rather than hang.  The client nudges the game with a
harmless WM_NULL so the pump runs even when the game is idle in GetMessage.
"""

import base64
import ctypes
import ctypes.wintypes as wt
import json
import msvcrt
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "StarsVM-harness.exe")
DEFAULT_PIPE = r"\\.\pipe\StarsVM-harness"

_u = ctypes.windll.user32
_k = ctypes.windll.kernel32
_WM_NULL = 0x0000

def find_pid(name):
    out = subprocess.check_output(
        ["tasklist", "/FI", "IMAGENAME eq " + name, "/FO", "CSV", "/NH"],
        stderr=subprocess.DEVNULL).decode(errors="replace")
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == name.lower():
            return int(parts[1])
    return None


def top_windows(pid):
    tops = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def fn(h, l):
        p = wt.DWORD()
        _u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            tops.append(h)
        return True

    _u.EnumWindows(cb(fn), 0)
    return tops


def _open_with_timeout(pipe, timeout):
    """os.open blocks until the server instance accepts; do it off-thread so a
    game that never reaches its message loop fails instead of hanging."""
    result = []

    def work():
        try:
            result.append(os.open(pipe, os.O_RDWR | os.O_BINARY))
        except OSError as e:
            result.append(e)

    t = threading.Thread(target=work, daemon=True)
    t.start()
    t.join(timeout)
    if not result:
        return None
    r = result[0]
    if isinstance(r, OSError):
        raise r
    return r


class Harness:
    def __init__(self, pipe=DEFAULT_PIPE, launch=True, timeout=15.0, log=None):
        self.pipe = pipe
        self.timeout = timeout
        self.proc = None
        self.pid = None
        self.fd = None
        self.buf = b""
        self._last_nudge = 0.0

        if launch:
            args = [EXE, "--harness", pipe]
            if log:
                args += ["--log", log]
            self.proc = subprocess.Popen(args, cwd=ROOT)
            self.pid = self.proc.pid
        self._connect()

    def _connect(self):
        # The server closes its pipe instance when a client goes away and makes
        # a new one from the message-loop pump, so a game sitting idle has no
        # instance to open.  Nudging it while we retry is what makes attaching to
        # an already-running game work at all.
        deadline = time.time() + self.timeout
        last = None
        while time.time() < deadline:
            if self.pid is None:
                self.pid = find_pid("StarsVM-harness.exe")
            if self.pid is not None:
                self.nudge()
            try:
                fd = _open_with_timeout(self.pipe, 1.0)
                if fd is not None:
                    self.fd = fd
                    return
            except OSError as e:
                last = e
            time.sleep(0.1)
        raise RuntimeError("cannot connect to %s: %s" % (self.pipe, last))

    def nudge(self):
        if self.pid is None:
            self.pid = find_pid("StarsVM-harness.exe")
        if self.pid is None:
            return
        for h in top_windows(self.pid):
            _u.PostMessageW(h, _WM_NULL, 0, 0)

    def _avail(self):
        h = msvcrt.get_osfhandle(self.fd)
        n = wt.DWORD()
        if not _k.PeekNamedPipe(h, None, 0, None, ctypes.byref(n), None):
            return None
        return n.value

    def request(self, cmd, timeout=None):
        timeout = self.timeout if timeout is None else timeout
        os.write(self.fd, (cmd + "\n").encode())
        self.nudge()
        deadline = time.time() + timeout
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl]
                self.buf = self.buf[nl + 1:]
                return line.decode(errors="replace")
            if time.time() > deadline:
                raise TimeoutError("no reply to %r within %.1fs" % (cmd, timeout))
            avail = self._avail()
            if avail is None:
                raise RuntimeError("pipe closed")
            if avail > 0:
                self.buf += os.read(self.fd, avail)
            else:
                now = time.time()
                if now - self._last_nudge > 0.05:
                    self.nudge()
                    self._last_nudge = now
                time.sleep(0.01)

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None
        if self.proc is not None:
            try:
                self.proc.terminate()
            except Exception:
                pass
            self.proc = None


def bmp_to_png(data):
    """Convert a 24- or 32-bit BI_RGB BMP to an 8-bit RGB PNG.

    zlib is stdlib, so this costs nothing and keeps the emulator free of an
    image library; it only ever produces uncompressed RGB."""
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if bpp not in (24, 32) or comp != 0:
        raise ValueError("unsupported BMP: %d bpp, compression %d" % (bpp, comp))
    topdown = h < 0
    h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    rows = []
    for y in range(h):
        src = off + (y if topdown else h - 1 - y) * stride
        line = data[src:src + stride]
        out = bytearray()
        for x in range(w):
            if bpp == 32:
                b, g, r = line[x * 4], line[x * 4 + 1], line[x * 4 + 2]
            else:
                b, g, r = line[x * 3], line[x * 3 + 1], line[x * 3 + 2]
            out += bytes((r, g, b))
        rows.append(b"\x00" + bytes(out))
    raw = b"".join(rows)

    def chunk(typ, payload):
        return (struct.pack(">I", len(payload)) + typ + payload +
                struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def _screenshot(h, args):
    hwnd = args["window"]
    path = os.path.join(tempfile.gettempdir(), "starsvm-shot-%d.bmp" % os.getpid())
    try:
        os.remove(path)
    except OSError:
        pass
    raw = h.request("SHOT %s %s" % (hwnd, path), timeout=30.0)
    if not os.path.exists(path):
        return [{"type": "text", "text": raw}]
    with open(path, "rb") as f:
        data = f.read()
    try:
        os.remove(path)
    except OSError:
        pass
    png = bmp_to_png(data)
    return [
        {"type": "image", "mimeType": "image/png",
         "data": base64.b64encode(png).decode()},
        {"type": "text", "text": json.dumps(
            {"ok": True, "window": hwnd, "width": struct.unpack_from("<i", data, 18)[0],
             "height": abs(struct.unpack_from("<i", data, 22)[0])})},
    ]


def _wait_idle(h, args):
    timeout = float(args.get("timeout", 15.0))
    stable = float(args.get("stable", 0.15))
    start = time.time()
    deadline = start + timeout
    stable_since = None
    while time.time() < deadline:
        try:
            st = json.loads(h.request("IDLE", timeout=max(0.5, min(3.0, deadline - time.time()))))
        except TimeoutError:
            st = {"idle": False}
        if st.get("idle"):
            if stable_since is None:
                stable_since = time.time()
            elif time.time() - stable_since >= stable:
                return [{"type": "text", "text": json.dumps(
                    {"ok": True, "idle": True, "waited": round(time.time() - start, 3)})}]
        else:
            stable_since = None
        time.sleep(0.03)
    return [{"type": "text", "text": json.dumps(
        {"ok": False, "idle": False, "error": "timeout", "waited": round(time.time() - start, 3)})}]


# ---- MCP ------------------------------------------------------------------

TOOLS = [
    {
        "name": "stars_status",
        "description": "Report whether the harness is connected.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "stars_observe",
        "description": (
            "Return the whole guest window tree as JSON: every window with its "
            "class, title, control id, rectangle, visibility, enabled state, "
            "and for controls their kind and checked/selected state. Dialog "
            "windows carry \"dialog\": true. Use the returned \"hwnd\" and \"id\" "
            "values with the other tools."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "stars_click",
        "description": (
            "Press a control. Address it either by its own hwnd (preferred - the "
            "game's custom panes give their children no control id), or by "
            "dialog hwnd plus control id for a real dialog."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": "string", "description": "hwnd of the control itself, from stars_observe"},
                "dialog": {"type": "string", "description": "hwnd of the dialog, for id addressing"},
                "id": {"type": "integer", "description": "control id, for dialog addressing"},
            },
        },
    },
    {
        "name": "stars_map",
        "description": "Force a window to repaint and return what it drew: text with "
                       "coordinates and the primitives (stars are small filled shapes).",
        "inputSchema": {
            "type": "object",
            "properties": {"window": {"type": "string", "description": "hwnd of the window"}},
            "required": ["window"],
        },
    },
    {
        "name": "stars_move",
        "description": "Move a window to (x, y) without resizing, so it stops covering another.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string"},
                "x": {"type": "integer"},
                "y": {"type": "integer"},
            },
            "required": ["window", "x", "y"],
        },
    },
    {
        "name": "stars_stars",
        "description": "Repaint the map and return every named star as {name: [x, y]} "
                       "in map client coordinates, by pairing drawn labels with drawn dots.",
        "inputSchema": {
            "type": "object",
            "properties": {"window": {"type": "string", "description": "hwnd of the map (starsscan)"}},
            "required": ["window"],
        },
    },
    {
        "name": "stars_list",
        "description": "List a list box's items and current selection.",
        "inputSchema": {
            "type": "object",
            "properties": {"window": {"type": "string"}},
            "required": ["window"],
        },
    },
    {
        "name": "stars_list_select",
        "description": "Select a list box item by index, as if clicked.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string"},
                "index": {"type": "integer"},
            },
            "required": ["window", "index"],
        },
    },
    {
        "name": "stars_list_dblclick",
        "description": "Double-click the selected list item; with shift, a "
                       "queue adds ten at once (the emulated Shift is set).",
        "inputSchema": {
            "type": "object",
            "properties": {"window": {"type": "string"},
                           "shift": {"type": "boolean"}},
            "required": ["window"],
        },
    },
    {
        "name": "stars_combo",
        "description": "List a combo box's items and current selection.",
        "inputSchema": {
            "type": "object",
            "properties": {"window": {"type": "string", "description": "hwnd of the combo"}},
            "required": ["window"],
        },
    },
    {
        "name": "stars_combo_select",
        "description": "Select a combo box item by index, as if picked.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string"},
                "index": {"type": "integer"},
            },
            "required": ["window", "index"],
        },
    },
    {
        "name": "stars_click_at",
        "description": (
            "Click at client coordinates inside a window. This is for the game "
            "map and other surfaces that are not controls; prefer stars_click "
            "for anything with an hwnd."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string", "description": "hwnd, e.g. the starsscan map"},
                "x": {"type": "integer", "description": "client x"},
                "y": {"type": "integer", "description": "client y"},
                "flags": {"type": "integer", "description": "MK_ flags, e.g. 4 = Shift, 8 = Control"},
                "right": {"type": "boolean"},
                "double": {"type": "boolean"},
            },
            "required": ["window", "x", "y"],
        },
    },
    {
        "name": "stars_click_real",
        "description": (
            "Click at client coordinates with modifiers. Injected into the game's "
            "own message queue (never host Windows input); the emulated GetCursorPos "
            "and GetKeyState report the injected position and Shift, so this is the "
            "tool for the map."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string", "description": "hwnd, e.g. the starsscan map"},
                "x": {"type": "integer"},
                "y": {"type": "integer"},
                "shift": {"type": "boolean"},
                "control": {"type": "boolean"},
                "right": {"type": "boolean", "description": "right-click"},
                "double": {"type": "boolean", "description": "double-click"},
            },
            "required": ["window", "x", "y"],
        },
    },
    {
        "name": "stars_drag",
        "description": "Press the left mouse at (x1,y1), drag to (x2,y2), release, "
                       "all injected. For gauges and sliders drawn by the game.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string"},
                "x1": {"type": "integer"}, "y1": {"type": "integer"},
                "x2": {"type": "integer"}, "y2": {"type": "integer"},
            },
            "required": ["window", "x1", "y1", "x2", "y2"],
        },
    },
    {
        "name": "stars_drag_to",
        "description": "Press in one window and drag to a point in another, then "
                       "release, all injected. For dragging a waypoint from a list "
                       "onto the map.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "from": {"type": "string", "description": "hwnd to press in"},
                "x1": {"type": "integer"}, "y1": {"type": "integer"},
                "to": {"type": "string", "description": "hwnd holding the drop target"},
                "x2": {"type": "integer"}, "y2": {"type": "integer"},
            },
            "required": ["from", "x1", "y1", "to", "x2", "y2"],
        },
    },
    {
        "name": "stars_menu_items",
        "description": (
            "The items of the last popup menu the game opened. The host popup "
            "menu cannot be clicked by injected input, so the harness records "
            "the menu instead; right-click, then call this, then stars_menu_pick "
            "and right-click again to choose; the game then sees the pick."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "stars_menu_pick",
        "description": (
            "Choose the item at `index` (from stars_menu_items) the next time the "
            "popup menu opens; the game receives that item's id."),
        "inputSchema": {
            "type": "object",
            "properties": {"index": {"type": "integer"}},
            "required": ["index"],
        },
    },
    {
        "name": "stars_command",
        "description": "Send a menu command id to a window (WM_COMMAND).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": "string", "description": "target window, usually the frame"},
                "id": {"type": "integer", "description": "menu command id"},
            },
            "required": ["hwnd", "id"],
        },
    },
    {
        "name": "stars_set_text",
        "description": "Set the text of a dialog control.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "dialog": {"type": "string"},
                "id": {"type": "integer"},
                "text": {"type": "string"},
            },
            "required": ["dialog", "id", "text"],
        },
    },
    {
        "name": "stars_key",
        "description": "Post a key down/up to a window. vk is a virtual-key code.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": "string"},
                "vk": {"type": "integer"},
            },
            "required": ["hwnd", "vk"],
        },
    },
    {
        "name": "stars_type",
        "description": "Post text to a window as WM_CHAR messages.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "hwnd": {"type": "string"},
                "text": {"type": "string"},
            },
            "required": ["hwnd", "text"],
        },
    },
    {
        "name": "stars_screenshot",
        "description": (
            "Capture a window to a PNG image. Use this for the game map and the "
            "custom-drawn panes, which no control tree describes."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string", "description": "hwnd of the window, from stars_observe"},
            },
            "required": ["window"],
        },
    },
    {
        "name": "stars_wait_idle",
        "description": (
            "Block until the game has no queued messages and no window waiting to "
            "repaint, and has been that way briefly. Use after any action before "
            "observing, instead of sleeping."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "timeout": {"type": "number", "description": "seconds, default 15"},
                "stable": {"type": "number", "description": "seconds of quiet required, default 0.15"},
            },
        },
    },
    {
        "name": "stars_msgbox",
        "description": (
            "Return the last message box the game asked for. The harness "
            "answers message boxes itself (a host modal would otherwise wedge "
            "it) and records the text here."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "stars_text",
        "description": (
            "Return the text the game has drawn into a window since its last "
            "paint. This reads the custom-drawn panes (tutor, messages, planet "
            "summary) as text, with no need for a screenshot."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "window": {"type": "string", "description": "hwnd from stars_observe"},
            },
            "required": ["window"],
        },
    },
    {
        "name": "stars_journal",
        "description": (
            "Return high-level UI events (commands sent, dialogs opened and "
            "closed) with sequence numbers greater than `since`. Poll it to learn "
            "what an action did without diffing screenshots."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "since": {"type": "integer", "description": "last sequence number seen, 0 for all"},
            },
        },
    },
]


def call_tool(h, name, args):
    if name == "stars_status":
        return [{"type": "text", "text": h.request("STATUS")}]
    if name == "stars_observe":
        return [{"type": "text", "text": h.request("OBSERVE", timeout=30.0)}]
    if name == "stars_click":
        if args.get("hwnd"):
            return [{"type": "text", "text": h.request("CLICKW %s" % args["hwnd"])}]
        return [{"type": "text", "text": h.request("CLICK %s %d" % (args["dialog"], int(args["id"])))}]
    if name == "stars_move":
        return [{"type": "text", "text": h.request(
            "MOVE %s %d %d" % (args["window"], int(args["x"]), int(args["y"])))}]
    if name == "stars_stars":
        hwnd = args["window"]
        h.request("REPAINT %s" % hwnd)
        info = json.loads(h.request("MAP %s" % hwnd))
        dots = [(d[1] + d[3] // 2, d[2] + d[4] // 2)
                for d in info["draws"] if 0 < d[3] <= 6 and 0 < d[4] <= 6]
        out = {}
        for x, y, s in info["texts"]:
            if y >= 690 or not s:
                continue
            ex, ey = x + 3 * len(s), y - 6
            if dots:
                bx, by = min(dots, key=lambda p: (p[0] - ex) ** 2 + (p[1] - ey) ** 2)
                if (bx - ex) ** 2 + (by - ey) ** 2 <= 900:
                    ex, ey = bx, by
            out[s] = [ex, ey]
        return [{"type": "text", "text": json.dumps(out)}]
    if name == "stars_map":
        h.request("REPAINT %s" % args["window"])
        return [{"type": "text", "text": h.request("MAP %s" % args["window"])}]
    if name == "stars_list":
        return [{"type": "text", "text": h.request("LIST %s" % args["window"])}]
    if name == "stars_list_dblclick":
        return [{"type": "text", "text": h.request(
            "DBLCLK %s %d" % (args["window"], 1 if args.get("shift") else 0))}]
    if name == "stars_list_select":
        return [{"type": "text", "text": h.request(
            "LISTSEL %s %d" % (args["window"], int(args["index"])))}]
    if name == "stars_combo":
        return [{"type": "text", "text": h.request("COMBO %s" % args["window"])}]
    if name == "stars_combo_select":
        return [{"type": "text", "text": h.request(
            "COMBOSEL %s %d" % (args["window"], int(args["index"])))}]
    if name == "stars_click_at":
        flags = int(args.get("flags", 0)) | (16 if args.get("right") else 0) \
            | (32 if args.get("double") else 0)
        return [{"type": "text", "text": h.request(
            "CLICKAT %s %d %d %d" % (args["window"], int(args["x"]), int(args["y"]), flags))}]
    if name == "stars_click_real":
        flags = (4 if args.get("shift") else 0) | (8 if args.get("control") else 0) \
            | (16 if args.get("right") else 0) | (32 if args.get("double") else 0)
        return [{"type": "text", "text": h.request(
            "CLICKAT %s %d %d %d" % (args["window"], int(args["x"]), int(args["y"]), flags))}]
    if name == "stars_drag":
        return [{"type": "text", "text": h.request(
            "DRAG %s %d %d %d %d" % (args["window"], int(args["x1"]), int(args["y1"]),
                                     int(args["x2"]), int(args["y2"])))}]
    if name == "stars_drag_to":
        return [{"type": "text", "text": h.request(
            "DRAG2 %s %d %d %s %d %d" % (args["from"], int(args["x1"]), int(args["y1"]),
                                         args["to"], int(args["x2"]), int(args["y2"])))}]
    if name == "stars_menu_items":
        return [{"type": "text", "text": h.request("MENUITEMS")}]
    if name == "stars_menu_pick":
        return [{"type": "text", "text": h.request("PICK %d" % int(args["index"]))}]
    if name == "stars_command":
        return [{"type": "text", "text": h.request("COMMAND %s %d" % (args["hwnd"], int(args["id"])))}]
    if name == "stars_set_text":
        return [{"type": "text", "text": h.request(
            "SETTEXT %s %d %s" % (args["dialog"], int(args["id"]), args["text"]))}]
    if name == "stars_key":
        return [{"type": "text", "text": h.request("KEY %s %x" % (args["hwnd"], int(args["vk"])))}]
    if name == "stars_type":
        return [{"type": "text", "text": h.request("TYPE %s %s" % (args["hwnd"], args["text"]))}]
    if name == "stars_screenshot":
        return _screenshot(h, args)
    if name == "stars_wait_idle":
        return _wait_idle(h, args)
    if name == "stars_msgbox":
        return [{"type": "text", "text": h.request("MSGBOX")}]
    if name == "stars_text":
        return [{"type": "text", "text": h.request("TEXT %s" % args["window"], timeout=15.0)}]
    if name == "stars_journal":
        since = int(args.get("since", 0))
        return [{"type": "text", "text": h.request("JOURNAL %d" % since, timeout=15.0)}]
    raise ValueError("unknown tool " + name)


def handle(msg, h):
    mid = msg.get("id")
    method = msg.get("method")
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "starsvm-harness", "version": "0.1"}}}
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}}
    if method == "tools/call":
        name = msg["params"]["name"]
        args = msg["params"].get("arguments") or {}
        try:
            content = call_tool(h, name, args)
            err = False
        except Exception as e:  # noqa: BLE001 - the agent should see the reason
            content = [{"type": "text", "text": json.dumps({"ok": False, "error": str(e)})}]
            err = True
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "content": content, "isError": err}}
    return {"jsonrpc": "2.0", "id": mid,
            "error": {"code": -32601, "message": "method not found"}}


def serve_mcp(h):
    for raw in sys.stdin.buffer:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except ValueError:
            continue
        resp = handle(msg, h)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()


def run_cli(argv):
    attach = "--attach" in argv
    argv = [a for a in argv if a not in ("--attach", "--cli")]
    h = Harness(launch=not attach, log=os.path.join(ROOT, "harness.log"))
    try:
        if not argv:
            print(json.dumps(json.loads(h.request("OBSERVE", timeout=30.0)), indent=2))
            return 0
        cmd = argv[0].lower()
        if cmd == "observe":
            text = h.request("OBSERVE", timeout=30.0)
        elif cmd == "status":
            text = h.request("STATUS")
        elif cmd == "click":
            if len(argv) >= 3:
                text = h.request("CLICK %s %d" % (argv[1], int(argv[2])))
            else:
                text = h.request("CLICKW %s" % argv[1])
        elif cmd == "clickat":
            text = h.request("CLICKAT %s %d %d" % (argv[1], int(argv[2]), int(argv[3])))
        elif cmd == "command" or cmd == "menu":
            text = h.request("COMMAND %s %d" % (argv[1], int(argv[2])))
        elif cmd == "text":
            text = h.request("TEXT %s" % argv[1])
        elif cmd == "msgbox":
            text = h.request("MSGBOX")
        elif cmd == "settext":
            text = h.request("SETTEXT %s %d %s" % (argv[1], int(argv[2]), " ".join(argv[3:])))
        elif cmd == "key":
            text = h.request("KEY %s %x" % (argv[1], int(argv[2])))
        elif cmd == "type":
            text = h.request("TYPE %s %s" % (argv[1], " ".join(argv[2:])))
        else:
            sys.stderr.write("unknown command %s\n" % cmd)
            return 2
        try:
            print(json.dumps(json.loads(text), indent=2))
        except ValueError:
            print(text)
        return 0
    finally:
        h.close()


def main():
    if "--cli" in sys.argv:
        return run_cli(sys.argv[1:])
    h = Harness(launch=True, log=os.path.join(ROOT, "harness.log"))
    try:
        serve_mcp(h)
    finally:
        h.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())