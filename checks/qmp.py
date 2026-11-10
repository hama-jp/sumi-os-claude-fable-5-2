#!/usr/bin/env python3
"""Minimal QMP client for the independent OS check (written by the main agent; not the model's tool).
usage as module: q = QMP(sock); q.shot(path); q.type("text"); q.key("ret"); q.move(x,y); q.click(); q.drag(x1,y1,x2,y2)
Mouse uses absolute coordinates (needs -device usb-tablet or virtio-tablet) and falls back to relative moves if `rel=True`."""
import json, socket, time

KEYMAP = {" ": "spc", "\n": "ret", ".": "dot", ",": "comma", "-": "minus", "/": "slash", "=": "equal", ";": "semicolon", "'": "apostrophe",
          "[": "bracket_left", "]": "bracket_right", "\\": "backslash", "`": "grave_accent"}
SHIFT = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
         ":": "semicolon", '"': "apostrophe", "<": "comma", ">": "dot", "?": "slash", "|": "backslash", "~": "grave_accent", "{": "bracket_left", "}": "bracket_right"}

class QMP:
    def __init__(self, path, w=1024, h=768):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); self.f = self.s.makefile("rw"); self.w, self.h = w, h
        self.f.readline(); self.cmd("qmp_capabilities")
    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args} if args else {"execute": name}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def shot(self, path):
        r = self.cmd("screendump", filename=path, format="png"); time.sleep(.3); return r
    def key(self, *keys, hold=80):
        return self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys], **{"hold-time": hold})
    def type(self, text, delay=.06):
        for ch in text:
            if ch in SHIFT: self.key("shift", SHIFT[ch])
            elif ch.isupper(): self.key("shift", ch.lower())
            else: self.key(KEYMAP.get(ch, ch))
            time.sleep(delay)
    def _ev(self, events): return self.cmd("input-send-event", events=events)
    def move(self, x, y):
        return self._ev([{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (self.w - 1))}}, {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (self.h - 1))}}])
    def rel(self, dx, dy):
        return self._ev([{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
    def btn(self, down, button="left"): return self._ev([{"type": "btn", "data": {"down": down, "button": button}}])
    def click(self, x=None, y=None):
        if x is not None: self.move(x, y); time.sleep(.15)
        self.btn(True); time.sleep(.08); self.btn(False); time.sleep(.2)
    def drag(self, x1, y1, x2, y2, steps=20):
        self.move(x1, y1); time.sleep(.15); self.btn(True); time.sleep(.1)
        for i in range(1, steps + 1):
            self.move(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps); time.sleep(.03)
        self.btn(False); time.sleep(.2)
