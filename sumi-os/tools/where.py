#!/usr/bin/env python3
"""Debug helper: sample the guest EIP a few times and resolve it against the kernel symbol table."""
import json, socket, sys, time, re, bisect, os
sock = os.environ.get("SUMI_QMP", "/tmp/sumi-qmp.sock")
B = os.environ.get("BUILD", "build")
syms = []
for l in open(B + "/gen/syms.txt"):
    p = l.split()
    if len(p) == 3 and p[1] in "tT": syms.append((int(p[0], 16), p[2]))
nm2 = os.popen("nm -n %s/sumi.elf" % B).read().split("\n")
syms = sorted((int(p.split()[0], 16), p.split()[2]) for p in nm2 if len(p.split()) == 3 and p.split()[1] in "tT")
addrs = [a for a, _ in syms]
s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
f.readline(); f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
for i in range(int(sys.argv[1]) if len(sys.argv) > 1 else 8):
    f.write(json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": "info registers"}}) + "\n"); f.flush()
    while True:
        d = json.loads(f.readline())
        if "return" in d: break
    m = re.search(r"EIP=([0-9a-f]+)", d["return"]); eip = int(m.group(1), 16)
    efl = re.search(r"EFL=([0-9a-f]+)", d["return"]).group(1)
    k = bisect.bisect_right(addrs, eip) - 1
    print("EIP=%08x %s+0x%x EFL=%s" % (eip, syms[k][1] if k >= 0 else "?", eip - syms[k][0] if k >= 0 else 0, efl))
    time.sleep(0.05)
