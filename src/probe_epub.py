#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import socket, time, sys

HOST="192.168.0.45"; PORT=23
def connect(retry=30,wait=1.0):
    last=None
    for i in range(retry):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(4); s.connect((HOST,PORT)); s.settimeout(8); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print("[FAIL] 重连失败:",last); return None
def run(s,cmd,to=12.0):
    mark="__PMK_%d__"%int(time.time()*1000%100000000)
    s.sendall(("%s ; echo %s\n"%(cmd,mark)).encode())
    buf=b""; dl=time.time()+to
    while time.time()<dl:
        try: d=s.recv(8192)
        except Exception: break
        if not d: break
        buf+=d
        if mark.encode() in buf: break
    txt=buf.decode("utf-8","replace")
    out=[]
    for ln in txt.splitlines():
        if mark in ln: continue
        if ln.strip().endswith("echo "+mark): continue
        if ln.strip()==cmd: continue
        if "can't open __PMK" in ln: continue
        out.append(ln)
    return "\n".join(out).strip("\n")

s=connect()
if s is None: sys.exit(1)
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except Exception: pass
print("=== /media/roms/apps/epubreader 目录 ===")
print(run(s,"ls -la /media/roms/apps/epubreader/"))
print("\n=== 当前二进制 md5 ===")
print(run(s,"md5sum /media/roms/apps/epubreader/* 2>/dev/null"))
print("\n=== epubreader 是否运行 ===")
print(run(s,"ps | grep epubreader | grep -v grep"))
s.sendall(b"exit\n"); s.close()
