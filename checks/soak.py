#!/usr/bin/env python3
"""30-minute soak (main agent's tool): terminal save, Files, Ink, Clock open/close every ~25 s."""
import sys,time,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from qmp import QMP
q=QMP(sys.argv[3]); D=sys.argv[1].rstrip("/")+"/"; MIN=float(sys.argv[2])  # args: screenshot dir, minutes, QMP socket
T=lambda s:(q.type(s,delay=.12),time.sleep(.2))
q.key("esc"); time.sleep(.5)
end=time.time()+MIN*60; n=0
while time.time()<end:
    n+=1
    q.click(288,728); time.sleep(1.5)
    T("date"); q.key("ret"); time.sleep(.4)
    T("echo soak-%d > soak.txt"%n); q.key("ret"); time.sleep(.4)
    T("cat soak.txt"); q.key("ret"); time.sleep(.8)
    if n%10==1: q.shot(D+"soak-%03d.png"%n)
    q.key("alt","f4"); time.sleep(1)
    for x in (416,608,480):
        q.click(x,728); time.sleep(1.5); q.key("alt","f4"); time.sleep(1)
    if n%10==1: q.shot(D+"soak-%03db.png"%n)
    time.sleep(10)
q.click(288,728); time.sleep(1.5); T("uptime"); q.key("ret"); time.sleep(.5); T("cat soak.txt"); q.key("ret"); time.sleep(1)
q.shot(D+"soak-end.png"); print("iterations",n)
