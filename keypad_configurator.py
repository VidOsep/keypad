#!/usr/bin/env python3
"""
Keypad configurator - tkinter replacement for configurator.html.

Same serial protocol and the same key codes as keypad.ino, so it is a drop-in
alternative to the browser configurator: no local server, no WebSerial, no
browser. Standard library only, except pyserial for the device link.

    pip3 install pyserial
    python3 keypad_configurator.py

macOS: if the window looks dated, the interpreter is linked against Tk 8.5.
Use the python.org build or `brew install python-tk`; check with
`python3 -m tkinter`, which prints the Tk version.
"""

import math
import platform
import queue
import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import messagebox

try:
    import serial
    import serial.tools.list_ports as list_ports
    HAVE_SERIAL = True
except ImportError:
    HAVE_SERIAL = False

IS_MAC = platform.system() == "Darwin"


# ---------------------------------------------------------------------------
# Firmware constants - must match keypad_config.h
# ---------------------------------------------------------------------------

KEYS = 28
LAYERS = 4
LAYER_BASE = 0x88          # code 0x88+n means "switch to layer n"
BAUD = 115200

EEPROM_SIZE = 1024
CAL_BYTES = 174            # JSCalRecord: header, 6 floats, 36-float LUT, crc
CFG_ADDR = 256
CFG_BYTES = 238            # KPConfig + crc, 2 bytes per key

# Arduino Keyboard.h codes. Printable characters are their own ASCII value.
NAMED = {
    0x80: "Ctrl L", 0x81: "Shift L", 0x82: "Alt L", 0x83: "Cmd L",
    0x84: "Ctrl R", 0x85: "Shift R", 0x86: "Alt R", 0x87: "Cmd R",
    0xB0: "Enter", 0xB1: "Esc", 0xB2: "Bksp", 0xB3: "Tab", 0xC1: "Caps",
    0xD1: "Ins", 0xD2: "Home", 0xD3: "PgUp", 0xD4: "Del", 0xD5: "End",
    0xD6: "PgDn", 0xD7: "→", 0xD8: "←", 0xD9: "↓",
    0xDA: "↑", 0x20: "Space",
}
for _i in range(1, 13):
    NAMED[0xC1 + _i] = f"F{_i}"

MOD_SYM = ["⌃", "⇧", "⌥", "⌘"]
MOD_NAME = ["Ctrl", "Shift", "Alt", "Cmd"]

KEYSYM = {
    "space": 0x20, "Return": 0xB0, "Escape": 0xB1, "BackSpace": 0xB2,
    "Tab": 0xB3, "Caps_Lock": 0xC1, "Insert": 0xD1, "Home": 0xD2,
    "Prior": 0xD3, "Delete": 0xD4, "End": 0xD5, "Next": 0xD6,
    "Right": 0xD7, "Left": 0xD8, "Down": 0xD9, "Up": 0xDA,
    "Shift_L": 0x81, "Control_L": 0x80, "Alt_L": 0x82, "Meta_L": 0x83,
    "Super_L": 0x83, "Shift_R": 0x85, "Control_R": 0x84, "Alt_R": 0x86,
    "Meta_R": 0x87, "Super_R": 0x87,
    "minus": 45, "equal": 61, "bracketleft": 91, "bracketright": 93,
    "backslash": 92, "semicolon": 59, "apostrophe": 39, "grave": 96,
    "comma": 44, "period": 46, "slash": 47,
}
for _i in range(1, 13):
    KEYSYM[f"F{_i}"] = 0xC1 + _i

QUICK = [("none", 0), ("Space", 0x20), ("Enter", 0xB0), ("Esc", 0xB1),
         ("Tab", 0xB3), ("Bksp", 0xB2), ("Shift", 0x81), ("Ctrl", 0x80),
         ("Alt", 0x82), ("Cmd", 0x83), ("↑", 0xDA), ("↓", 0xD9),
         ("←", 0xD8), ("→", 0xD7)]

MODES = ["gamepad", "WASD", "arrows"]

SETTINGS = [("d", "dead zone", 0, 40), ("o", "walk start", 10, 95),
            ("f", "walk end", 5, 90), ("a", "direction on", 10, 80),
            ("b", "direction off", 5, 75)]


def cap_of(code, mods=0):
    if code == 0:
        return ""
    if LAYER_BASE <= code < LAYER_BASE + LAYERS:
        return f"L{code - LAYER_BASE}"
    if code in NAMED:
        base = NAMED[code]
    elif 0x20 < code < 0x7F:
        base = chr(code).upper()
    else:
        base = f"0x{code:02X}"
    return "".join(MOD_SYM[b] for b in range(4) if mods & (1 << b)) + base


# ---------------------------------------------------------------------------
# Physical layout
# ---------------------------------------------------------------------------

PITCH = 72
HIT_R = 32
HALF = 29                  # half side of the square keys
CIRCLE_R = 29
HEX_R = 33

_X0, _Y0 = 150, 74
_COL = [_X0 + c * PITCH for c in range(4)]
_ROW = [_Y0 + r * PITCH for r in range(5)]
_THUMB_Y = _ROW[4] + PITCH + 38
_HEX_Y = _THUMB_Y + PITCH

KEY_LAYOUT = {}
for _r, _row in enumerate(([24, 25, 26, 27], [18, 19, 20, 21], [12, 13, 14, 15],
                           [6, 7, 8, 9], [0, 1, 2, 3])):
    for _c, _idx in enumerate(_row):
        KEY_LAYOUT[_idx] = (_COL[_c], _ROW[_r],
                            "square" if _row[0] in (12, 6) else "rsquare")
KEY_LAYOUT[23] = (_COL[0] - PITCH, _ROW[2], "rsquare")
KEY_LAYOUT[17] = (_COL[3] + PITCH, _ROW[2], "rsquare")
KEY_LAYOUT[4] = (_COL[0], _THUMB_Y, "circle")
KEY_LAYOUT[10] = (_COL[1], _THUMB_Y, "square")
KEY_LAYOUT[16] = (_COL[2], _THUMB_Y, "rsquare")
KEY_LAYOUT[22] = (_COL[3], _THUMB_Y, "rsquare")
KEY_LAYOUT[5] = (_COL[0], _HEX_Y, "hexagon")
KEY_LAYOUT[11] = (_COL[1], _HEX_Y, "hexagon")

CHORD_KEYS = {5, 11}
CAPTIONS = [(_COL[0] - PITCH - HIT_R, _ROW[0] - 46, "fingers"),
            (_COL[0] - HALF, _THUMB_Y - HIT_R - 24, "thumb")]

CANVAS_W = _COL[3] + PITCH + HIT_R + 20
CANVAS_H = _HEX_Y + HEX_R + 18


def regular_polygon(cx, cy, r, sides, rotation=0.0):
    pts = []
    for i in range(sides):
        a = math.radians(rotation - 90 + i * 360.0 / sides)
        pts += [cx + r * math.cos(a), cy + r * math.sin(a)]
    return pts


def round_rect(x0, y0, x1, y1, r):
    """Point list for a rounded rectangle, drawn with smooth=True."""
    return [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r,
            x1, y1, x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r,
            x0, y0 + r, x0, y0]


# ---------------------------------------------------------------------------
# Colour
# ---------------------------------------------------------------------------

LIGHT = dict(bg="#f2f3f5", surface="#ffffff", sunken="#f7f8fa",
             border="#e0e3e8", line="#eceef1", shadow="#e6e8ec",
             text="#16181d", dim="#767d87", faint="#a6adb6",
             accent="#3b62f6", accent_soft="#e3eafe", accent_text="#ffffff",
             warn="#e0a200", warn_soft="#fdf3dd", danger="#d64545",
             ok="#2e9e5b", trail="#c8ccd3", rest="#d64545")

DARK = dict(bg="#16181c", surface="#20232a", sunken="#1a1d22",
            border="#2e323b", line="#282c34", shadow="#101216",
            text="#e9ebf0", dim="#98a0ad", faint="#6b7280",
            accent="#6b8afd", accent_soft="#252d48", accent_text="#0f1115",
            warn="#e3b341", warn_soft="#2c2718", danger="#f0736b",
            ok="#4cc27d", trail="#3a3f48", rest="#f0736b")


def system_is_dark(root):
    """macOS reports its appearance through the semantic system colours."""
    try:
        r, g, b = root.winfo_rgb("systemWindowBackgroundColor")
        return (r + g + b) / 3 < 32768
    except tk.TclError:
        return False


# ---------------------------------------------------------------------------
# Small canvas-drawn controls, so the look is the same on every platform
# ---------------------------------------------------------------------------

class Chip(tk.Canvas):
    """A compact pill button."""

    def __init__(self, master, text, command, pal, width=None, tone="plain"):
        self.pal, self.tone, self.text_ = pal, tone, text
        f = (pal["family"], pal["size"] - 1)
        w = width or (tkfont.Font(font=f).measure(text) + 22)
        super().__init__(master, width=w, height=26, highlightthickness=0,
                         bg=pal["surface"], cursor="hand2")
        self.command, self.hover, self.w = command, False, w
        self.font = f
        self.bind("<Button-1>", lambda e: command())
        self.bind("<Enter>", lambda e: self._set_hover(True))
        self.bind("<Leave>", lambda e: self._set_hover(False))
        self.draw()

    def _set_hover(self, v):
        self.hover = v
        self.draw()

    def set_text(self, text):
        self.text_ = text
        self.w = tkfont.Font(font=self.font).measure(text) + 22
        self.configure(width=self.w)
        self.draw()

    def draw(self):
        p = self.pal
        self.delete("all")
        fill = p["accent_soft"] if (self.tone == "accent" or self.hover) else p["sunken"]
        edge = p["accent"] if self.tone == "accent" else p["border"]
        self.create_polygon(round_rect(1, 1, self.w - 1, 25, 8), fill=fill,
                            outline=edge, width=1, smooth=True)
        self.create_text(self.w / 2, 13, text=self.text_, font=self.font,
                         fill=p["accent"] if self.tone == "accent" else p["text"])


class ChipRow(tk.Canvas):
    """A grid of pills drawn on a single canvas.

    Every Tk widget is a real window, and on macOS each one costs something to
    map, so twenty-odd little button widgets make a panel slow to show. One
    canvas with hit testing behaves the same and is one window.
    """

    def __init__(self, master, labels, command, pal, cols=4, cw=64, ch=26, gap=4):
        self.pal, self.labels, self.command = pal, labels, command
        self.cols, self.cw, self.ch, self.gap = cols, cw, ch, gap
        self.active, self.hover = set(), None
        rows = (len(labels) + cols - 1) // cols
        w = cols * (cw + gap) - gap
        h = rows * (ch + gap) - gap
        super().__init__(master, width=w, height=h, highlightthickness=0,
                         bg=pal["surface"], cursor="hand2")
        self.bind("<Button-1>", self._click)
        self.bind("<Motion>", self._motion)
        self.bind("<Leave>", lambda e: self._set_hover(None))
        self.draw()

    def _box(self, i):
        r, c = divmod(i, self.cols)
        x0 = c * (self.cw + self.gap)
        y0 = r * (self.ch + self.gap)
        return x0, y0, x0 + self.cw, y0 + self.ch

    def _hit(self, x, y):
        for i in range(len(self.labels)):
            x0, y0, x1, y1 = self._box(i)
            if x0 <= x <= x1 and y0 <= y <= y1:
                return i
        return None

    def _click(self, ev):
        i = self._hit(ev.x, ev.y)
        if i is not None:
            self.command(i)

    def _motion(self, ev):
        self._set_hover(self._hit(ev.x, ev.y))

    def _set_hover(self, i):
        if i != self.hover:
            self.hover = i
            self.draw()

    def set_active(self, indices):
        indices = set(indices)
        if indices != self.active:
            self.active = indices
            self.draw()

    def draw(self):
        p = self.pal
        self.delete("all")
        for i, label in enumerate(self.labels):
            x0, y0, x1, y1 = self._box(i)
            on = i in self.active
            fill = p["accent_soft"] if (on or i == self.hover) else p["sunken"]
            edge = p["accent"] if on else p["border"]
            self.create_polygon(round_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 8),
                                fill=fill, outline=edge, width=1, smooth=True)
            self.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=label,
                             font=(p["family"], p["size"] - 1),
                             fill=p["accent"] if on else p["text"])


class Segmented(tk.Canvas):
    """iOS-style segmented control - used for the layer selector."""

    def __init__(self, master, labels, command, pal, seg_w=44):
        self.pal, self.labels, self.command = pal, labels, command
        self.value, self.seg_w = 0, seg_w
        w = seg_w * len(labels) + 6
        super().__init__(master, width=w, height=30, highlightthickness=0,
                         bg=pal["surface"], cursor="hand2")
        self.w = w
        self.bind("<Button-1>", self._click)
        self.draw()

    def _click(self, ev):
        n = int((ev.x - 3) // self.seg_w)
        if 0 <= n < len(self.labels):
            self.set(n)
            self.command(n)

    def set(self, n):
        self.value = n
        self.draw()

    def draw(self):
        p = self.pal
        self.delete("all")
        self.create_polygon(round_rect(1, 1, self.w - 1, 29, 8),
                            fill=p["sunken"], outline=p["border"], smooth=True)
        for i, label in enumerate(self.labels):
            x0 = 3 + i * self.seg_w
            if i == self.value:
                self.create_polygon(round_rect(x0 + 1, 4, x0 + self.seg_w - 1, 26, 7),
                                    fill=p["accent"], outline="", smooth=True)
            self.create_text(x0 + self.seg_w / 2, 15, text=label,
                             font=(p["family"], p["size"] - 1,
                                   "bold" if i == self.value else "normal"),
                             fill=p["accent_text"] if i == self.value else p["dim"])


class Slider(tk.Canvas):
    """Flat slider with a round knob."""

    def __init__(self, master, lo, hi, command, pal, width=190):
        self.pal, self.lo, self.hi, self.command = pal, lo, hi, command
        self.value, self.w = lo, width
        super().__init__(master, width=width, height=24, highlightthickness=0,
                         bg=pal["surface"], cursor="hand2")
        self.bind("<Button-1>", self._drag)
        self.bind("<B1-Motion>", self._drag)
        self.draw()

    def _drag(self, ev):
        frac = min(1.0, max(0.0, (ev.x - 10) / (self.w - 20)))
        v = int(round(self.lo + frac * (self.hi - self.lo)))
        if v != self.value:
            self.value = v
            self.draw()
            self.command(v)

    def set(self, v):
        self.value = max(self.lo, min(self.hi, int(v)))
        self.draw()

    def draw(self):
        p = self.pal
        self.delete("all")
        frac = (self.value - self.lo) / float(self.hi - self.lo or 1)
        x = 10 + frac * (self.w - 20)
        self.create_line(10, 12, self.w - 10, 12, fill=p["border"], width=4,
                         capstyle="round")
        self.create_line(10, 12, x, 12, fill=p["accent"], width=4, capstyle="round")
        self.create_oval(x - 7, 5, x + 7, 19, fill=p["surface"],
                         outline=p["accent"], width=2)


class Toggle(tk.Canvas):
    """Switch, for the boolean settings."""

    def __init__(self, master, command, pal, value=False):
        self.pal, self.command, self.value = pal, command, value
        super().__init__(master, width=40, height=24, highlightthickness=0,
                         bg=pal["surface"], cursor="hand2")
        self.bind("<Button-1>", self._click)
        self.draw()

    def _click(self, _ev):
        self.value = not self.value
        self.draw()
        self.command(1 if self.value else 0)

    def set(self, v):
        self.value = bool(v)
        self.draw()

    def draw(self):
        p = self.pal
        self.delete("all")
        self.create_polygon(round_rect(2, 4, 38, 20, 8),
                            fill=p["accent"] if self.value else p["border"],
                            outline="", smooth=True)
        x = 29 if self.value else 11
        self.create_oval(x - 7, 5, x + 7, 19, fill=p["surface"], outline="")


# ---------------------------------------------------------------------------
# Serial link
# ---------------------------------------------------------------------------

class Link:
    def __init__(self):
        self.port = None
        self.q = queue.Queue()
        self._stop = threading.Event()

    @property
    def connected(self):
        return self.port is not None and self.port.is_open

    def open(self, device):
        self.close()
        self.port = serial.Serial(device, BAUD, timeout=0.2)
        self._stop.clear()
        threading.Thread(target=self._reader, daemon=True).start()

    def close(self):
        self._stop.set()
        if self.port:
            try:
                self.port.close()
            except Exception:
                pass
        self.port = None

    def send(self, line):
        if self.connected:
            try:
                self.port.write((line + "\n").encode("ascii"))
            except Exception as exc:
                self.q.put(("error", str(exc)))

    def _reader(self):
        buf = b""
        while not self._stop.is_set() and self.port:
            try:
                chunk = self.port.read(256)
            except Exception as exc:
                self.q.put(("error", str(exc)))
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self.q.put(("line", raw.decode("ascii", "replace").strip()))


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

class App:
    def __init__(self, root):
        self.root = root
        self.dark = system_is_dark(root)
        self.pal = dict(DARK if self.dark else LIGHT)

        base = tkfont.nametofont("TkDefaultFont")
        self.pal["family"] = base.actual("family")
        self.pal["size"] = max(10, min(13, base.actual("size") or 12))

        self.keymap = [[0] * KEYS for _ in range(LAYERS)]
        self.mods = [[0] * KEYS for _ in range(LAYERS)]
        self.cfg = {"m": 0, "x": 1, "d": 6, "o": 50, "f": 32, "a": 40,
                    "b": 26, "p": 0}
        self.layer = 0
        self.sel = None
        self.mask = 0
        self.live = {"rx": 512, "ry": 512, "ox": 512, "oy": 512,
                     "mode": 0, "layer": 0, "st": 0}
        self.trail, self.rest = [], []
        self.cal = None
        self.link = Link()
        self.capture_on = False

        root.configure(bg=self.pal["bg"])
        self._build()
        self.root.after(40, self._pump)
        self.build_keys()
        self.build_views()
        self.sync_settings()
        self.draw_views()

    # -- chrome ----------------------------------------------------------
    def card(self, master, **kw):
        return tk.Frame(master, bg=self.pal["surface"], highlightthickness=1,
                        highlightbackground=self.pal["border"],
                        highlightcolor=self.pal["border"], **kw)

    def heading(self, master, text):
        return tk.Label(master, text=text.upper(), bg=self.pal["surface"],
                        fg=self.pal["faint"],
                        font=(self.pal["family"], self.pal["size"] - 2, "bold"))

    def label(self, master, text, tone="text", bold=False, **kw):
        return tk.Label(master, text=text, bg=self.pal["surface"],
                        fg=self.pal[tone], anchor="w", justify="left",
                        font=(self.pal["family"], self.pal["size"],
                              "bold" if bold else "normal"), **kw)

    def _build(self):
        p = self.pal
        outer = tk.Frame(self.root, bg=p["bg"])
        outer.pack(fill="both", expand=True, padx=14, pady=12)

        # --- header -----------------------------------------------------
        head = tk.Frame(outer, bg=p["bg"])
        head.pack(fill="x", pady=(0, 12))
        tk.Label(head, text="Keypad", bg=p["bg"], fg=p["text"],
                 font=(p["family"], p["size"] + 5, "bold")).pack(side="left")
        self.dot = tk.Canvas(head, width=10, height=10, highlightthickness=0, bg=p["bg"])
        self.dot.pack(side="left", padx=(14, 5), pady=(3, 0))
        self.status = tk.Label(head, text="not connected", bg=p["bg"], fg=p["dim"],
                               font=(p["family"], p["size"] - 1))
        self.status.pack(side="left")

        right = tk.Frame(head, bg=p["bg"])
        right.pack(side="right")
        Chip(right, "◑", self.toggle_theme, p, width=34).pack(side="right", padx=(6, 0))
        self.connect_chip = Chip(right, "Connect", self.toggle_connect, p, tone="accent")
        self.connect_chip.pack(side="right", padx=4)
        self.port_chip = Chip(right, "no port", self.pick_port, p, width=186)
        self.port_chip.configure(bg=p["bg"])
        self.port_chip.pack(side="right")
        self.scan_ports()

        tabs = tk.Frame(outer, bg=p["bg"])
        tabs.pack(fill="x")
        self.tab_seg = Segmented(tabs, ["Keys", "Joystick"], self.show_tab, p, seg_w=92)
        self.tab_seg.configure(bg=p["bg"])
        self.tab_seg.pack(side="left", pady=(0, 10))

        body = tk.Frame(outer, bg=p["bg"])
        body.pack(fill="both", expand=True)
        body.rowconfigure(0, weight=1)
        body.columnconfigure(0, weight=1)
        # Both tabs live in the same cell and are swapped with tkraise, so
        # switching never re-lays-out or re-maps their contents.
        self.tabs = [tk.Frame(body, bg=p["bg"]), tk.Frame(body, bg=p["bg"])]
        for frame in self.tabs:
            frame.grid(row=0, column=0, sticky="nsew")
        self._build_keys(self.tabs[0])
        self._build_joy(self.tabs[1])
        self.tabs[0].tkraise()

        foot = tk.Frame(outer, bg=p["bg"])
        foot.pack(fill="x", pady=(10, 0))
        self.budget = tk.Label(foot, bg=p["bg"], fg=p["faint"], anchor="w",
                               font=(p["family"], p["size"] - 2))
        self.budget.pack(side="left")
        self.budget.configure(
            text=f"EEPROM  ·  calibration {CAL_BYTES} B at 0  ·  config "
                 f"{CFG_BYTES} B at {CFG_ADDR}  ·  "
                 f"{EEPROM_SIZE - CFG_ADDR - CFG_BYTES} B free")
        for text, cmd, tone in (("Write to EEPROM", self.write_eeprom, "accent"),
                                ("Defaults", self.load_defaults, "plain"),
                                ("Reload", lambda: self.send("G"), "plain")):
            Chip(foot, text, cmd, p, tone=tone).pack(side="right", padx=(6, 0))

    def show_tab(self, n):
        self.tabs[n].tkraise()
        if n == 1:
            self.draw_views()

    def pick_port(self):
        if not HAVE_SERIAL:
            messagebox.showerror("pyserial missing", "pip3 install pyserial")
            return
        ports = [q.device for q in list_ports.comports()]
        menu = tk.Menu(self.root, tearoff=0)
        if ports:
            for q in ports:
                menu.add_command(label=q, command=lambda v=q: self.port_chip.set_text(v))
        else:
            menu.add_command(label="no serial ports found", state="disabled")
        menu.add_separator()
        menu.add_command(label="Rescan", command=self.scan_ports)
        menu.tk_popup(self.port_chip.winfo_rootx(),
                      self.port_chip.winfo_rooty() + self.port_chip.winfo_height())

    def _build_keys(self, parent):
        p = self.pal
        left = self.card(parent)
        left.pack(side="left", fill="both", expand=True)
        bar = tk.Frame(left, bg=p["surface"])
        bar.pack(fill="x", padx=16, pady=(14, 0))
        self.heading(bar, "layer").pack(side="left", pady=(6, 0))
        self.layer_seg = Segmented(bar, [str(n) for n in range(LAYERS)],
                                   self.on_layer, p)
        self.layer_seg.pack(side="left", padx=10)
        self.hint = self.label(bar, "click a key, then assign it on the right",
                               tone="faint")
        self.hint.pack(side="left", padx=6, pady=(4, 0))

        self.canvas = tk.Canvas(left, bg=p["surface"], highlightthickness=0,
                                width=CANVAS_W, height=CANVAS_H)
        self.canvas.pack(padx=10, pady=6)
        self.canvas.bind("<Button-1>", self.on_click)

        side = tk.Frame(parent, bg=p["bg"], width=316)
        side.pack(side="left", fill="y", padx=(12, 0))
        side.pack_propagate(False)

        insp = self.card(side)
        insp.pack(fill="x")
        pad = tk.Frame(insp, bg=p["surface"])
        pad.pack(fill="x", padx=16, pady=14)
        self.heading(pad, "selected key").pack(anchor="w")
        self.sel_lbl = self.label(pad, "none", bold=True)
        self.sel_lbl.configure(font=(p["family"], p["size"] + 3, "bold"))
        self.sel_lbl.pack(anchor="w", pady=(4, 10))

        self.cap_box = tk.Canvas(pad, height=52, highlightthickness=0,
                                 bg=p["surface"], cursor="hand2")
        self.cap_box.pack(fill="x")
        self.cap_box.bind("<Button-1>", self.focus_capture)
        self.cap_box.bind("<KeyPress>", self.on_capture)
        self.cap_box.bind("<FocusOut>", lambda e: self.draw_capture(False))
        self.cap_box.bind("<Configure>", lambda e: self.draw_capture(self.capture_on))

        self.mod_row = ChipRow(pad, MOD_NAME, self.toggle_mod, p, cols=4, cw=66)
        self.mod_row.pack(anchor="w", pady=(10, 2))

        self.heading(pad, "quick").pack(anchor="w", pady=(12, 5))
        ChipRow(pad, [q[0] for q in QUICK],
                lambda i: self.set_code(QUICK[i][1]), p, cols=4, cw=66).pack(anchor="w")

        self.heading(pad, "switch to layer").pack(anchor="w", pady=(12, 5))
        ChipRow(pad, [f"L{n}" for n in range(LAYERS)],
                lambda i: self.set_code(LAYER_BASE + i), p, cols=4, cw=66).pack(anchor="w")

        note = self.card(side)
        note.pack(fill="x", pady=(10, 0))
        npad = tk.Frame(note, bg=p["surface"])
        npad.pack(fill="x", padx=16, pady=14)
        self.heading(npad, "function chord").pack(anchor="w")
        self.label(npad, "Keys 5 + 11 together: short = next joystick mode, "
                         "2 s = calibration, 5 s = bootloader. Each still works "
                         "as a normal key on its own.",
                   tone="dim", wraplength=268).pack(anchor="w", pady=(5, 0))
        dev = self.card(side)
        dev.pack(fill="x", pady=(10, 0))
        dpad = tk.Frame(dev, bg=p["surface"])
        dpad.pack(fill="x", padx=16, pady=14)
        self.heading(dpad, "device").pack(anchor="w", pady=(0, 8))

        row = tk.Frame(dpad, bg=p["surface"])
        row.pack(fill="x")
        self.supp = Toggle(row, self.on_suppress, p, value=True)
        self.supp.pack(side="left")
        self.label(row, "  mute keypad while editing", tone="dim").pack(side="left")

        row2 = tk.Frame(dpad, bg=p["surface"])
        row2.pack(fill="x", pady=(8, 0))
        self.persist = Toggle(row2, lambda v: self.set_field("p", v), p)
        self.persist.pack(side="left")
        self.label(row2, "  remember layer and mode", tone="dim").pack(side="left")
        self.label(dpad, "Off by default, so a layer switch costs no EEPROM "
                         "write cycle. Write to EEPROM still saves the current "
                         "state either way.",
                   tone="faint", wraplength=268).pack(anchor="w", pady=(7, 0))

    def _build_joy(self, parent):
        p = self.pal
        left = self.card(parent)
        left.pack(side="left", fill="y")
        pad = tk.Frame(left, bg=p["surface"])
        pad.pack(fill="both", padx=18, pady=16)

        self.heading(pad, "mode").pack(anchor="w")
        self.mode_seg = Segmented(pad, MODES, lambda n: self.set_field("m", n),
                                  p, seg_w=78)
        self.mode_seg.pack(anchor="w", pady=(6, 14))

        row = tk.Frame(pad, bg=p["surface"])
        row.pack(fill="x", pady=(0, 12))
        self.mirror = Toggle(row, lambda v: self.set_field("x", v), p)
        self.mirror.pack(side="left")
        self.label(row, "  swap left / right", tone="dim").pack(side="left")

        self.sliders = {}
        for field, text, lo, hi in SETTINGS:
            fr = tk.Frame(pad, bg=p["surface"])
            fr.pack(fill="x", pady=2)
            self.label(fr, text, tone="dim", width=13).pack(side="left")
            out = self.label(fr, "", bold=True, width=3)
            out.pack(side="right")
            sl = Slider(fr, lo, hi, lambda v, f=field: self.on_slider(f, v), p)
            sl.pack(side="right", padx=8)
            self.sliders[field] = (sl, out)

        tk.Frame(pad, bg=p["line"], height=1).pack(fill="x", pady=16)
        cb = tk.Frame(pad, bg=p["surface"])
        cb.pack(fill="x")
        Chip(cb, "Start calibration", lambda: self.send("C"), p, tone="accent").pack(side="left")
        Chip(cb, "Reload", lambda: self.send("L"), p).pack(side="left", padx=6)
        Chip(cb, "Clear trail", self.clear_trail, p).pack(side="left")
        self.cal_lbl = self.label(pad, "", tone="dim", wraplength=400)
        self.cal_lbl.pack(anchor="w", pady=(12, 0))
        self.label(pad, "Steps are advanced with key 'b' (index 5) on the keypad. "
                        "Grey is the raw trail; red marks where the stick came to "
                        "rest after release — that spread is what the dead "
                        "zone has to cover.", tone="faint", wraplength=400).pack(
            anchor="w", pady=(8, 0))

        right = self.card(parent)
        right.pack(side="left", fill="y", padx=(12, 0))
        rp = tk.Frame(right, bg=p["surface"])
        rp.pack(padx=16, pady=16)
        self.craw = tk.Canvas(rp, width=252, height=252, highlightthickness=0,
                              bg=p["sunken"])
        self.craw.grid(row=0, column=0, padx=5)
        self.heading(rp, "raw + resting spread").grid(row=1, column=0, pady=(8, 0))
        self.ccal = tk.Canvas(rp, width=252, height=252, highlightthickness=0,
                              bg=p["sunken"])
        self.ccal.grid(row=0, column=1, padx=5)
        self.heading(rp, "after calibration").grid(row=1, column=1, pady=(8, 0))

    # -- drawing ---------------------------------------------------------
    # The canvases are built once and then only reconfigured. Rebuilding them
    # every frame cost several milliseconds a go, which is invisible on X11 and
    # very much visible on a Mac.
    def build_keys(self):
        p, c = self.pal, self.canvas
        c.delete("all")
        self.key_items, self.key_state = {}, {}
        for x, y, text in CAPTIONS:
            c.create_text(x, y, text=text.upper(), anchor="w", fill=p["faint"],
                          font=(p["family"], p["size"] - 3, "bold"))
        for idx in sorted(KEY_LAYOUT):
            x, y, shape = KEY_LAYOUT[idx]
            shadow = self._shape(c, x, y + 2, shape, p["shadow"], "", 1)
            body = self._shape(c, x, y, shape, p["surface"], p["border"], 1)
            label = c.create_text(x, y - 4, text="", fill=p["text"],
                                  font=(p["family"], p["size"], "bold"))
            number = c.create_text(x, y + 15, text=str(idx), fill=p["faint"],
                                   font=(p["family"], p["size"] - 4))
            self.key_items[idx] = (shadow, body, label, number)
        self.draw_keys()

    def draw_keys(self):
        """Reconfigure only the keys whose appearance actually changed."""
        p, c = self.pal, self.canvas
        for idx, (_shadow, body, label, number) in self.key_items.items():
            down = bool(self.mask & (1 << idx))
            sel = idx == self.sel
            if down:
                fill, edge, ink, wide = p["accent"], p["accent"], p["accent_text"], 1
            elif sel:
                fill, edge, ink, wide = p["accent_soft"], p["accent"], p["text"], 2
            elif idx in CHORD_KEYS:
                fill, edge, ink, wide = p["warn_soft"], p["warn"], p["text"], 1
            else:
                fill, edge, ink, wide = p["surface"], p["border"], p["text"], 1
            code = self.keymap[self.layer][idx]
            text = cap_of(code, self.mods[self.layer][idx])
            size = p["size"] - (0 if len(text) <= 3 else 3)
            state = (fill, edge, wide, ink, text, size)
            if self.key_state.get(idx) == state:
                continue
            self.key_state[idx] = state
            c.itemconfig(body, fill=fill, outline=edge, width=wide)
            c.itemconfig(label, text=text, fill=ink,
                         font=(p["family"], size, "bold"))
            c.itemconfig(number, fill=p["accent_text"] if down else p["faint"])
        self.sync_selection()

    def _shape(self, c, x, y, shape, fill, edge, width):
        """Draw one key body and return its canvas id."""
        if shape == "circle":
            return c.create_oval(x - CIRCLE_R, y - CIRCLE_R, x + CIRCLE_R, y + CIRCLE_R,
                                 fill=fill, outline=edge, width=width)
        if shape == "rsquare":
            return c.create_polygon(round_rect(x - HALF, y - HALF, x + HALF, y + HALF, 13),
                                    fill=fill, outline=edge, width=width, smooth=True)
        if shape == "hexagon":
            return c.create_polygon(regular_polygon(x, y, HEX_R, 6, 30), fill=fill,
                                    outline=edge, width=width)
        return c.create_polygon(round_rect(x - HALF, y - HALF, x + HALF, y + HALF, 3),
                                fill=fill, outline=edge, width=width, smooth=True)

    def draw_capture(self, focused):
        p, c = self.pal, self.cap_box
        c.delete("all")
        w = max(c.winfo_width(), 100)
        c.create_polygon(round_rect(2, 2, w - 2, 50, 9),
                         fill=p["accent_soft"] if focused else p["sunken"],
                         outline=p["accent"] if focused else p["border"],
                         width=2 if focused else 1, smooth=True)
        c.create_text(w / 2, 26,
                      text="press any key now" if focused else "click here to record a key",
                      fill=p["accent"] if focused else p["dim"],
                      font=(p["family"], p["size"] - 1,
                            "bold" if focused else "normal"))

    def sync_selection(self):
        p = self.pal
        if self.sel is None:
            self.sel_lbl.configure(text="none", fg=p["faint"])
            self.mod_row.set_active(())
            return
        code = self.keymap[self.layer][self.sel]
        text = cap_of(code, self.mods[self.layer][self.sel])
        self.sel_lbl.configure(text=f"{self.sel}   {text or '—'}", fg=p["text"])
        m = self.mods[self.layer][self.sel]
        self.mod_row.set_active(b for b in range(4) if m & (1 << b))

    def sync_settings(self):
        self.mode_seg.set(min(self.cfg["m"], 2))
        self.mirror.set(self.cfg["x"])
        self.persist.set(self.cfg["p"])
        for field, _t, _lo, _hi in SETTINGS:
            sl, out = self.sliders[field]
            sl.set(self.cfg[field])
            out.configure(text=str(self.cfg[field]))

    TRAIL_N = 380
    REST_N = 60

    def build_views(self):
        """Create every dot once; later frames only move them."""
        p = self.pal
        self.trail_ids, self.rest_ids = [], []
        self.trail_pos = self.rest_pos = 0
        for c in (self.craw, self.ccal):
            c.delete("all")
            for g in range(1, 4):
                c.create_line(g * 63, 0, g * 63, 252, fill=p["line"])
                c.create_line(0, g * 63, 252, g * 63, fill=p["line"])
        self.ccal.create_oval(16, 16, 236, 236, outline=p["border"])
        self.dz_item = self.ccal.create_oval(0, 0, 0, 0, outline=p["rest"], dash=(3, 3))
        for _ in range(self.TRAIL_N):
            self.trail_ids.append(self.craw.create_oval(0, 0, 0, 0, fill=p["trail"],
                                                        outline="", state="hidden"))
        for _ in range(self.REST_N):
            self.rest_ids.append(self.craw.create_oval(0, 0, 0, 0, fill=p["rest"],
                                                       outline="", state="hidden"))
        self.centre_h = self.craw.create_line(0, 0, 0, 0, fill=p["accent"], state="hidden")
        self.centre_v = self.craw.create_line(0, 0, 0, 0, fill=p["accent"], state="hidden")
        self.dot_raw = self.craw.create_oval(0, 0, 0, 0, fill=p["accent"],
                                             outline=p["surface"], width=2)
        self.dot_cal = self.ccal.create_oval(0, 0, 0, 0, fill=p["accent"],
                                             outline=p["surface"], width=2)
        self.draw_views()

    def add_sample(self, rx, ry, resting):
        """One telemetry sample: move one recycled dot, nothing is created."""
        if not self.views_live():
            return
        s = 252 / 1024.0
        x, y = rx * s, 252 - ry * s
        item = self.trail_ids[self.trail_pos]
        self.craw.coords(item, x - 1, y - 1, x + 1, y + 1)
        self.craw.itemconfig(item, state="normal")
        self.trail_pos = (self.trail_pos + 1) % self.TRAIL_N
        if resting:
            item = self.rest_ids[self.rest_pos]
            self.craw.coords(item, x - 2.5, y - 2.5, x + 2.5, y + 2.5)
            self.craw.itemconfig(item, state="normal")
            self.rest_pos = (self.rest_pos + 1) % self.REST_N

    def views_live(self):
        return self.tab_seg.value == 1

    def draw_views(self):
        """Move the live dots and the dead-zone ring; everything else persists."""
        if not self.views_live():
            return
        s = 252 / 1024.0
        x, y = self.live["rx"] * s, 252 - self.live["ry"] * s
        self.craw.coords(self.dot_raw, x - 5, y - 5, x + 5, y + 5)
        x, y = self.live["ox"] * s, 252 - self.live["oy"] * s
        self.ccal.coords(self.dot_cal, x - 5, y - 5, x + 5, y + 5)
        dz = self.cfg["d"] / 100.0 * 110
        self.ccal.coords(self.dz_item, 126 - dz, 126 - dz, 126 + dz, 126 + dz)
        self.ccal.itemconfig(self.dz_item, state="normal" if dz > 0 else "hidden")
        if self.cal:
            cx, cy = self.cal[0] * s, 252 - self.cal[1] * s
            self.craw.coords(self.centre_h, cx - 7, cy, cx + 7, cy)
            self.craw.coords(self.centre_v, cx, cy - 7, cx, cy + 7)
            self.craw.itemconfig(self.centre_h, state="normal")
            self.craw.itemconfig(self.centre_v, state="normal")

    def set_status(self, text, tone="dim"):
        self.status.configure(text=text, fg=self.pal[tone])
        self.dot.delete("all")
        self.dot.create_oval(1, 1, 9, 9, fill=self.pal[tone], outline="")

    # -- interaction -----------------------------------------------------
    def toggle_theme(self):
        self.dark = not self.dark
        pal = dict(DARK if self.dark else LIGHT)
        pal["family"], pal["size"] = self.pal["family"], self.pal["size"]
        self.pal = pal
        for w in self.root.winfo_children():
            w.destroy()
        tab, port = self.tab_seg.value, self.port_chip.text_
        keep = (self.keymap, self.mods, self.cfg, self.layer, self.sel,
                self.mask, self.live, self.trail, self.rest, self.cal, self.link)
        self.root.configure(bg=pal["bg"])
        self._build()
        (self.keymap, self.mods, self.cfg, self.layer, self.sel, self.mask,
         self.live, self.trail, self.rest, self.cal, self.link) = keep
        self.layer_seg.set(self.layer)
        self.tab_seg.set(tab)
        self.show_tab(tab)
        self.port_chip.set_text(port)
        self.build_keys()
        self.sync_settings()
        self.build_views()
        self.draw_capture(False)
        self.set_status("connected" if self.link.connected else "not connected",
                        "ok" if self.link.connected else "dim")

    def on_click(self, ev):
        best, best_d = None, 1e9
        for idx, (x, y, _s) in KEY_LAYOUT.items():
            d = math.hypot(ev.x - x, ev.y - y)
            if d < HIT_R and d < best_d:
                best, best_d = idx, d
        self.sel = best
        self.draw_keys()

    def on_layer(self, n):
        self.layer = n
        self.send(f"Y {n}")
        self.draw_keys()

    def focus_capture(self, _ev=None):
        self.cap_box.focus_set()
        self.capture_on = True
        self.draw_capture(True)

    def on_capture(self, ev):
        if self.sel is None:
            return "break"
        code = KEYSYM.get(ev.keysym)
        if code is None and len(ev.keysym) == 1:
            code = ord(ev.keysym.lower())
        if code is None:
            return "break"
        if 0x80 <= code <= 0x87:
            self.set_code(code, 0)
            return "break"
        mods = 0
        if ev.state & 0x0004:
            mods |= 1 << 0
        if ev.state & 0x0001:
            mods |= 1 << 1
        if ev.state & 0x0008 or ev.state & 0x0080:
            mods |= 1 << 2
        self.set_code(code, mods)
        return "break"

    def set_code(self, code, mods=None):
        if self.sel is None:
            self.hint.configure(text="pick a key on the keypad first",
                                fg=self.pal["danger"])
            return
        self.hint.configure(text="click a key, then assign it on the right",
                            fg=self.pal["faint"])
        self.keymap[self.layer][self.sel] = code
        if mods is not None:
            self.mods[self.layer][self.sel] = mods
        if code == 0 or LAYER_BASE <= code < LAYER_BASE + LAYERS:
            self.mods[self.layer][self.sel] = 0
        self.push_key()

    def toggle_mod(self, bit):
        if self.sel is None:
            return
        self.mods[self.layer][self.sel] ^= (1 << bit)
        self.push_key()

    def push_key(self):
        self.send(f"K {self.layer} {self.sel} {self.keymap[self.layer][self.sel]} "
                  f"{self.mods[self.layer][self.sel]}")
        self.draw_keys()

    def on_slider(self, field, v):
        self.cfg[field] = v
        self.sliders[field][1].configure(text=str(v))
        self.send(f"P {field} {v}")
        if field == "d":
            self.draw_views()

    def set_field(self, field, value):
        self.cfg[field] = int(value)
        self.send(f"P {field} {int(value)}")

    def on_suppress(self, v):
        self.send("X1" if v else "X0")

    def clear_trail(self):
        self.trail.clear()
        self.rest.clear()
        for item in self.trail_ids + self.rest_ids:
            self.craw.itemconfig(item, state="hidden")
        self.trail_pos = self.rest_pos = 0
        self.draw_views()

    def write_eeprom(self):
        self.send("W")
        self.set_status("written to EEPROM", "ok")

    def load_defaults(self):
        if messagebox.askyesno("Load defaults",
                               "Replace the configuration in the device's RAM "
                               "with the defaults? Nothing reaches EEPROM until "
                               "you press Write to EEPROM."):
            self.send("D")
            self.send("G")

    # -- device ----------------------------------------------------------
    def scan_ports(self):
        if not HAVE_SERIAL:
            self.port_chip.set_text("pyserial not installed")
            return
        ports = [q.device for q in list_ports.comports()]
        # the Pro Micro shows up as /dev/cu.usbmodem* on macOS
        pick = next((q for q in ports if "usbmodem" in q or "ACM" in q), None)
        self.port_chip.set_text(pick or (ports[0] if ports else "no serial ports"))

    def toggle_connect(self):
        if not HAVE_SERIAL:
            messagebox.showerror("pyserial missing", "pip3 install pyserial")
            return
        if self.link.connected:
            self.send("T0")
            self.send("X0")
            self.link.close()
            self.connect_chip.text_ = "Connect"
            self.connect_chip.draw()
            self.set_status("not connected", "dim")
            return
        try:
            self.link.open(self.port_chip.text_)
        except Exception as exc:
            messagebox.showerror("Cannot open port", str(exc))
            return
        self.connect_chip.text_ = "Disconnect"
        self.connect_chip.draw()
        self.set_status("connecting…", "warn")
        self.root.after(1200, self._hello)      # the Pro Micro resets on open

    def _hello(self):
        for cmd in ("?", "G", "L", "T1"):
            self.send(cmd)
        self.on_suppress(self.supp.value)

    def send(self, line):
        self.link.send(line)

    def _pump(self):
        keys = views = settings = False
        try:
            while True:
                kind, payload = self.link.q.get_nowait()
                if kind == "error":
                    self.set_status(payload, "danger")
                    continue
                r = self._handle(payload)
                keys |= r == "keys"
                settings |= r == "settings"
                views |= r in ("views", "settings")
        except queue.Empty:
            pass
        if settings:
            self.sync_settings()
        if keys:
            self.draw_keys()
        if views:
            self.draw_views()
        self.root.after(40, self._pump)

    def _handle(self, line):
        if not line:
            return None
        head, _, rest = line.partition(" ")

        if head == "t":
            f = rest.split()
            if len(f) < 8:
                return None
            rx, ry, ox, oy = int(f[0]), int(f[1]), int(f[2]), int(f[3])
            self.live.update(rx=rx, ry=ry, ox=ox, oy=oy, mode=int(f[4]),
                             layer=int(f[5]), st=int(f[6]))
            mask = int(f[7], 16)
            self.trail.append((rx, ry))
            if len(self.trail) > 24:            # only the rest detector needs these
                del self.trail[:12]
            resting = False
            if len(self.trail) > 6:
                seg = self.trail[-6:]
                spread = max(max(abs(a[0] - b[0]), abs(a[1] - b[1]))
                             for a in seg for b in seg)
                near = math.hypot(rx - 512, ry - 512) < 160
                if spread <= 2 and near and (not self.rest or math.hypot(
                        rx - self.rest[-1][0], ry - self.rest[-1][1]) > 1.5):
                    self.rest.append((rx, ry))
                    resting = True
                    if len(self.rest) > 120:
                        del self.rest[:40]
            self.add_sample(rx, ry, resting)
            changed = mask != self.mask
            self.mask = mask
            return "keys" if changed else "views"

        if head == "KP":
            self.set_status(f"connected · config v{rest.split()[0]}", "ok")
            return None

        if head == "CFG":
            f = [int(v) for v in rest.split()]
            if len(f) >= 7:   # 8th value (persist) only on firmware v3+
                for key, v in zip(("m", "x", "d", "o", "f", "a", "b", "p"), f):
                    self.cfg[key] = v
            return "settings"

        if head in ("KM", "MD"):
            f = rest.split()
            if len(f) < 2:
                return None
            layer, hexs = int(f[0]), f[1]
            vals = [int(hexs[i:i + 2], 16) for i in range(0, len(hexs), 2)][:KEYS]
            target = self.keymap if head == "KM" else self.mods
            if 0 <= layer < LAYERS:
                target[layer][:len(vals)] = vals
            return "keys"

        if head == "CAL":
            f = [float(v) for v in rest.split()]
            if len(f) >= 6:
                self.cal = f
                self.cal_lbl.configure(
                    text=f"centre {f[0]:.1f}, {f[1]:.1f}   ·   forward "
                         f"{math.degrees(f[2]):.1f}°   ·   ellipse "
                         f"{f[3]:.0f} × {f[4]:.0f} at {math.degrees(f[5]):.1f}°")
            return "views"

        if head == "LUT":
            self.lut = [float(v) for v in rest.split()]
            return "views"

        return None


def main():
    root = tk.Tk()
    root.title("Keypad configurator")
    root.geometry("1180x880")
    root.minsize(1060, 720)
    app = App(root)
    root.after(60, lambda: app.draw_capture(False))
    root.mainloop()


if __name__ == "__main__":
    main()
