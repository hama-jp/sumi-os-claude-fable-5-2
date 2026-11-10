#!/usr/bin/env python3
"""Soak test for Sumi OS: drives a running, freshly booted VM like an impatient user for N minutes.

usage: soak.py MINUTES [seed]     (VM started with tools/start.sh; env SUMI_QMP, SUMI_SERIAL)

It logs in, then loops over scripted "user stories" (edit+save+close, terminal commands, file
manager browsing, window dragging) mixed with random monkey input (clicks, drags, keys), and
watches the serial log for kernel panics, application faults and a missing heartbeat (freeze).
Exit status 0 = the system survived; the summary lists every fault seen.
"""
import os, random, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vm

SERIAL = os.environ.get("SUMI_SERIAL", "/tmp/sumi-serial.log")
DOCK = {'terminal': 288, 'editor': 352, 'files': 416, 'clock': 480, 'monitor': 544, 'ink': 608, 'gomoku': 672, 'about': 736}
WORDS = "ink paper brush stone mountain river cloud pine crane moon bamboo snow temple bridge lantern".split()

class Soak:
    def __init__(self, minutes, seed):
        self.q = vm.QMP(vm.SOCK)
        self.rnd = random.Random(seed)
        self.end = time.time() + minutes * 60
        self.saved = 0; self.stories = 0; self.log_pos = 0; self.faults = []; self.last_beat = time.time()
    # --- input helpers ---
    def key(self, combo): vm.sendkeys(self.q, combo.split('-'))
    def type(self, text):
        for ch in text: vm.sendkeys(self.q, vm.keys_for(ch), 15)
    def move(self, x, y):
        x = max(0, min(1023, x)); y = max(0, min(767, y))
        vm.ev(self.q, [{"type": "abs", "data": {"axis": "x", "value": (x * 0x7fff) // 1023}},
                       {"type": "abs", "data": {"axis": "y", "value": (y * 0x7fff) // 767}}])
        time.sleep(0.04)
    def click(self, x, y, b='l'):
        self.move(x, y); vm.btn(self.q, b, True); vm.btn(self.q, b, False)
    def drag(self, x0, y0, x1, y1):
        self.move(x0, y0); vm.btn(self.q, 'l', True)
        for i in range(1, 6): self.move(x0 + (x1 - x0) * i // 5, y0 + (y1 - y0) * i // 5)
        vm.btn(self.q, 'l', False)
    def launch(self, app):
        self.click(DOCK[app], 728); time.sleep(1.2)
    def close(self): self.key('alt-f4'); time.sleep(0.4)
    def tidy(self, n):
        # close whatever is open; 'd' answers "Save changes?" with Discard (dialog buttons react to their first letter)
        for _ in range(n): self.key('alt-f4'); time.sleep(0.3); self.key('d'); time.sleep(0.2)
    def sentence(self): return ' '.join(self.rnd.choice(WORDS) for _ in range(self.rnd.randint(3, 8)))
    # --- health ---
    def check(self):
        try: data = open(SERIAL, 'rb').read()
        except OSError: return
        new = data[self.log_pos:].decode('latin-1'); self.log_pos = len(data)
        for line in new.splitlines():
            if 'heartbeat' in line: self.last_beat = time.time()
            if 'KERNEL PANIC' in line: raise SystemExit('FAIL: kernel panic\n' + new)
            if 'fault in thread' in line: self.faults.append(line.strip()); print('  APP FAULT:', line.strip())
        if time.time() - self.last_beat > 100: raise SystemExit('FAIL: no heartbeat for 100 s - the system is frozen')
    # --- stories ---
    def story_editor(self):
        self.launch('editor')
        for _ in range(self.rnd.randint(2, 6)): self.type(self.sentence()); self.key('ret')
        self.key('ctrl-s'); time.sleep(0.8)
        self.saved += 1
        self.type('soak%d.txt' % (self.saved % 12)); self.key('ret'); time.sleep(0.8)
        self.key('ret'); time.sleep(0.5)            # confirms "Replace?" when the name existed (else a newline in the text)
        self.type(self.sentence()); self.key('ctrl-s'); time.sleep(0.5)
        self.close(); time.sleep(0.3)
    def story_terminal(self):
        self.launch('terminal')
        cmds = ['ls -l', 'cat welcome.txt', 'df', 'free', 'ps', 'date', 'uptime', 'sysinfo', 'wc haiku.txt', 'grep ink welcome.txt',
                'echo %s > note%d.txt' % (self.sentence(), self.rnd.randint(0, 9)), 'cat soak%d.txt' % self.rnd.randint(0, 11),
                'mkdir d%d' % self.rnd.randint(0, 5), 'rm -r d%d' % self.rnd.randint(0, 5), 'cp haiku.txt copy%d.txt' % self.rnd.randint(0, 5),
                'rm copy%d.txt' % self.rnd.randint(0, 5), 'ls /etc', 'history', 'help', 'stat welcome.txt', 'hexdump haiku.txt', 'clear']
        for _ in range(self.rnd.randint(3, 8)): self.type(self.rnd.choice(cmds)); self.key('ret'); time.sleep(0.25)
        if self.rnd.random() < 0.7: self.type('exit'); self.key('ret')
        else: self.close()
    def story_files(self):
        self.launch('files')
        for _ in range(self.rnd.randint(3, 8)):
            self.key(self.rnd.choice(['down', 'down', 'up', 'ret', 'backspace', 'pgdn', 'home', 'f5'])); time.sleep(0.3)
        self.key('esc'); self.tidy(3)
    def story_ink(self):
        self.launch('ink')
        for _ in range(self.rnd.randint(2, 6)):
            self.key(self.rnd.choice('12345678'))
            x, y = self.rnd.randint(260, 600), self.rnd.randint(200, 430)
            self.move(x, y); vm.btn(self.q, 'l', True)
            for _ in range(self.rnd.randint(3, 10)):
                x = max(200, min(760, x + self.rnd.randint(-60, 60))); y = max(120, min(520, y + self.rnd.randint(-40, 40))); self.move(x, y)
            vm.btn(self.q, 'l', False)
            if self.rnd.random() < 0.3: self.key('ctrl-z')
        if self.rnd.random() < 0.6:
            self.key('ctrl-s'); time.sleep(0.8); self.type('soak%d' % self.rnd.randint(0, 3)); self.key('ret'); time.sleep(0.8); self.key('ret'); time.sleep(1.5)
        self.tidy(2)
    def story_gomoku(self):
        self.launch('gomoku')
        for _ in range(self.rnd.randint(3, 12)):
            self.click(self.rnd.randint(180, 520), self.rnd.randint(190, 540)); time.sleep(0.9)
            if self.rnd.random() < 0.15: self.key(self.rnd.choice(['u', 'n']))
        self.tidy(2)
    def story_monitor(self):
        self.launch('monitor')
        for _ in range(self.rnd.randint(2, 6)): self.key(self.rnd.choice(['down', 'up', 'home', 'end'])); time.sleep(0.5)
        time.sleep(2); self.tidy(2)
    def story_net(self):
        self.launch('terminal')
        for c in self.rnd.sample(['ifconfig', 'ping 10.0.2.2 2', 'nslookup example.com', 'wget http://10.0.2.2:1/', 'ping 10.0.2.99 1'], 2):
            self.type(c); self.key('ret'); time.sleep(4)
        self.type('exit'); self.key('ret'); self.tidy(1)
    def story_windows(self):
        apps = self.rnd.sample(list(DOCK), self.rnd.randint(2, 4))
        for a in apps: self.launch(a)
        for _ in range(self.rnd.randint(2, 6)):
            x, y = self.rnd.randint(120, 700), self.rnd.randint(60, 300)
            self.drag(x, y, x + self.rnd.randint(-200, 300), y + self.rnd.randint(-40, 300))
            if self.rnd.random() < 0.4: self.key('alt-tab')
        self.tidy(len(apps) + 1)
    def story_monkey(self):
        for _ in range(self.rnd.randint(20, 60)):
            r = self.rnd.random()
            x, y = self.rnd.randint(0, 1023), self.rnd.randint(34, 767)     # never the panel: its menu can log out
            if r < 0.35: self.click(x, y, self.rnd.choice('lllr'))
            elif r < 0.5: self.drag(x, y, self.rnd.randint(0, 1023), self.rnd.randint(34, 767))
            elif r < 0.6: self.move(x, y); vm.btn(self.q, 'l', True); vm.btn(self.q, 'l', False); vm.btn(self.q, 'l', True); vm.btn(self.q, 'l', False)
            elif r < 0.85: self.type(self.rnd.choice(WORDS) + self.rnd.choice([' ', '\n', '']))
            elif r < 0.95: self.key(self.rnd.choice(['ret', 'esc', 'tab', 'up', 'down', 'left', 'right', 'pgup', 'pgdn', 'home', 'end', 'delete',
                                                      'backspace', 'ctrl-s', 'ctrl-z', 'ctrl-a', 'ctrl-c', 'ctrl-v', 'ctrl-x', 'f2', 'f5', 'alt-tab', 'ctrl-n']))
            else: self.close()
        self.key('esc'); self.tidy(12)
    def run(self):
        self.type('guest'); self.key('tab'); self.type('guest'); self.key('ret'); time.sleep(3)
        stories = [self.story_editor, self.story_editor, self.story_terminal, self.story_terminal, self.story_files, self.story_windows, self.story_monkey,
                   self.story_ink, self.story_gomoku, self.story_monitor]
        if os.environ.get('SUMI_SOAK_NET'): stories.append(self.story_net)
        t0 = time.time()
        while time.time() < self.end:
            s = self.rnd.choice(stories)
            s(); self.stories += 1
            self.check()
            if self.stories % 10 == 0:
                print('  %4d s: %d stories, %d saves, %d app faults' % (time.time() - t0, self.stories, self.saved, len(self.faults))); sys.stdout.flush()
        time.sleep(35); self.check()
        if time.time() - self.last_beat > 70: raise SystemExit('FAIL: heartbeat stopped at the end')
        print('PASS: survived %d s, %d stories, %d editor saves, %d application faults' % (time.time() - t0, self.stories, self.saved, len(self.faults)))
        for f in self.faults: print('   ', f)

if __name__ == '__main__':
    Soak(float(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 1).run()
