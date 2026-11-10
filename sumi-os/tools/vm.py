#!/usr/bin/env python3
"""Drive a running Sumi OS QEMU instance through QMP.

usage: vm.py [-s SOCK] cmd [args] [cmd [args] ...]   (commands are chained with '+')
  key <qcode-combo>     e.g. key ret, key ctrl-s, key alt-f4
  type <text>           types ASCII text
  move <x> <y>          absolute pointer position (pixels, needs vmmouse) 
  rmove <dx> <dy>       relative move (PS/2)
  home                  relative move to top-left corner
  rto <x> <y>           home + relative move to x,y (PS/2 only path)
  click [l|r|m]         press+release
  down / up [l|r|m]
  dclick                double click
  wheel <n>             wheel steps (positive = up)
  shot <file.png>       screenshot
  sleep <sec>
  quit                  quit qemu
  raw <json>            raw QMP command
"""
import json, socket, sys, time, os

SOCK = os.environ.get("SUMI_QMP", "/tmp/sumi-qmp.sock")
W = int(os.environ.get("SUMI_W", "1024")); H = int(os.environ.get("SUMI_H", "768"))

class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path)
        self.f = self.s.makefile("rw")
        self._read()
        self.cmd("qmp_capabilities")
    def _read(self):
        while True:
            line = self.f.readline()
            if not line: raise EOFError
            d = json.loads(line)
            if "event" in d: continue
            return d
    def cmd(self, name, **args):
        m = {"execute": name}
        if args: m["arguments"] = args
        self.f.write(json.dumps(m) + "\n"); self.f.flush()
        return self._read()

KEYMAP = {' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
          ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', '\\': 'backslash', ',': 'comma', '.': 'dot', '/': 'slash'}
SHIFT = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
         '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', ':': 'semicolon', '"': 'apostrophe',
         '~': 'grave_accent', '|': 'backslash', '<': 'comma', '>': 'dot', '?': 'slash'}

def keys_for(ch):
    if ch in KEYMAP: return [KEYMAP[ch]]
    if ch in SHIFT: return ['shift', SHIFT[ch]]
    if ch.isdigit(): return [ch]
    if ch.islower(): return [ch]
    if ch.isupper(): return ['shift', ch.lower()]
    raise ValueError("cannot type %r" % ch)

def sendkeys(q, names, hold=40):
    q.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names], **{"hold-time": hold})
    time.sleep(hold / 1000.0 + 0.03)

def ev(q, events):
    r = q.cmd("input-send-event", events=events)
    if "error" in r: print(r, file=sys.stderr)

def rel(q, dx, dy):
    while dx or dy:
        sx = max(-60, min(60, dx)); sy = max(-60, min(60, dy))
        e = []
        if sx: e.append({"type": "rel", "data": {"axis": "x", "value": sx}})
        if sy: e.append({"type": "rel", "data": {"axis": "y", "value": sy}})
        ev(q, e); dx -= sx; dy -= sy
        time.sleep(0.012)

BTN = {'l': 'left', 'r': 'right', 'm': 'middle'}
def btn(q, b, down):
    ev(q, [{"type": "btn", "data": {"down": down, "button": BTN[b]}}]); time.sleep(0.05)

def main(argv):
    global SOCK
    if argv and argv[0] == '-s': SOCK = argv[1]; argv = argv[2:]
    q = QMP(SOCK)
    i = 0
    while i < len(argv):
        c = argv[i]; i += 1
        def arg():
            nonlocal i
            v = argv[i]; i += 1; return v
        def optbtn():
            nonlocal i
            if i < len(argv) and argv[i] in BTN:
                i += 1; return argv[i-1]
            return 'l'
        if c == '+': continue
        if c == 'key': sendkeys(q, arg().split('-'))
        elif c == 'type':
            for ch in arg(): sendkeys(q, keys_for(ch), 25)
        elif c == 'move':
            x = int(arg()); y = int(arg())
            ev(q, [{"type": "abs", "data": {"axis": "x", "value": (x * 0x7fff) // (W - 1)}},
                   {"type": "abs", "data": {"axis": "y", "value": (y * 0x7fff) // (H - 1)}}])
            time.sleep(0.08)
        elif c == 'rmove': rel(q, int(arg()), int(arg()))
        elif c == 'home': rel(q, -W - 200, -H - 200)
        elif c == 'rto':
            x = int(arg()); y = int(arg()); rel(q, -W - 200, -H - 200); time.sleep(0.1); rel(q, x, y); time.sleep(0.1)
        elif c == 'click':
            b = optbtn(); btn(q, b, True); btn(q, b, False)
        elif c == 'dclick':
            for _ in range(2): btn(q, 'l', True); btn(q, 'l', False)
        elif c == 'down': btn(q, optbtn(), True)
        elif c == 'up': btn(q, optbtn(), False)
        elif c == 'wheel':
            n = int(arg())
            for _ in range(abs(n)):
                b = 'wheel-up' if n > 0 else 'wheel-down'
                ev(q, [{"type": "btn", "data": {"down": True, "button": b}}])
                ev(q, [{"type": "btn", "data": {"down": False, "button": b}}]); time.sleep(0.03)
        elif c == 'shot':
            f = os.path.abspath(arg()); r = q.cmd("screendump", filename=f, format="png")
            if "error" in r: print(r, file=sys.stderr)
        elif c == 'sleep': time.sleep(float(arg()))
        elif c == 'quit':
            try: q.cmd("quit")
            except Exception: pass
            return
        elif c == 'raw': print(q.cmd(*[arg()]))
        else: raise SystemExit("unknown command " + c)

if __name__ == "__main__":
    main(sys.argv[1:])
