#!/usr/bin/env python3
"""Screen recorder for the independent OS check (main agent's tool). Shares one QMP connection with the input driver:
a thread takes PPM screendumps at a fixed rate while the scenario sends keys and mouse events.
usage: from rec import Rec; r = Rec(sock, "/abs/out.mp4"); q = r.q; r.start(); ...; r.stop()"""
import os, shutil, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp import QMP


class Rec:
    def __init__(self, sock, out, fps=12, tmp=None):
        self.q = QMP(sock); self.out = out; self.fps = fps
        self.dir = tempfile.mkdtemp(prefix="rec-", dir=tmp); self.stamps = []; self.on = False
        lock = threading.Lock(); raw = self.q.cmd
        def cmd(name, **a):
            with lock: return raw(name, **a)
        self.q.cmd = cmd

    def _loop(self):
        i = 0
        while self.on:
            t = time.time(); p = f"{self.dir}/{i:06d}.ppm"
            self.q.cmd("screendump", filename=p)
            self.stamps.append((p, t)); i += 1
            time.sleep(max(0, 1 / self.fps - (time.time() - t)))

    def start(self):
        self.on = True; self.t = threading.Thread(target=self._loop, daemon=True); self.t.start(); time.sleep(.3)

    def stop(self):
        time.sleep(.5); self.on = False; self.t.join(); time.sleep(.3)
        lst = f"{self.dir}/list.txt"
        with open(lst, "w") as f:
            for (p, t), (_, t2) in zip(self.stamps, self.stamps[1:] + [(None, self.stamps[-1][1] + 1 / self.fps)]):
                if os.path.getsize(p) > 0: f.write(f"file '{p}'\nduration {t2 - t:.4f}\n")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst, "-vf", "scale=1024:768:flags=lanczos,fps=30,format=yuv420p",
                        "-c:v", "libx264", "-crf", "16", self.out], check=True)
        n = len(self.stamps); dur = self.stamps[-1][1] - self.stamps[0][1]
        shutil.rmtree(self.dir); return n, dur
