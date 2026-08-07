#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""清理设备上残留的 find 进程(上次采集卡住遗留), 释放 CPU。"""
import socket, time

HOST, PORT = "192.168.0.45", 23

def run(s, cmd, t=6.0):
    mark = "__ENDMK_%d__" % int(time.time() * 1000 % 100000000)
    s.sendall(("%s ; echo %s\n" % (cmd, mark)).encode())
    buf = b""
    s.settimeout(t)
    dl = time.time() + t
    while time.time() < dl:
        try:
            d = s.recv(8192)
        except Exception:
            break
        if not d:
            break
        buf += d
        if mark.encode() in buf:
            break
    return "\n".join(l for l in buf.decode("utf-8", "replace").splitlines()
                     if mark not in l and "can't open __ENDMK" not in l).strip()

def main():
    s = None
    for i in range(200):
        try:
            s = socket.socket(); s.settimeout(1.5); s.connect((HOST, PORT)); break
        except Exception:
            try: s.close()
            except Exception: pass
            s = None
            time.sleep(0.3)
    if s is None:
        print("[FAIL] 连不上"); return
    print("[OK] 已连接")
    s.settimeout(6)
    time.sleep(0.5)
    try: s.recv(8192)
    except Exception: pass

    print("--- 清理前 find 进程 ---")
    print(run(s, "ps | grep '[f]ind /' "))
    run(s, "killall find 2>/dev/null; pkill -f 'find /' 2>/dev/null")
    time.sleep(1)
    print("--- 清理后 ---")
    left = run(s, "ps | grep '[f]ind /'")
    print(left if left else "(已无 find 进程)")
    print("--- 负载 ---")
    print(run(s, "cat /proc/loadavg"))
    try: s.sendall(b"exit\n")
    except Exception: pass
    s.close()

main()
