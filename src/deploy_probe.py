#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""telnet(192.168.0.45:23, nc -e /bin/sh) 探查设备部署环境: 能力/空间/路径, 并验证 heredoc 可用."""
import socket, time, sys

HOST = "192.168.0.45"
PORT = 23

def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(4)
    s.connect((HOST, PORT))
    s.settimeout(8)
    return s

def run(s, cmd, to=8.0):
    mark = "__PMK_%d__" % int(time.time()*1000 % 100000000)
    s.sendall(("%s ; echo %s\n" % (cmd, mark)).encode())
    buf = b""
    deadline = time.time() + to
    while time.time() < deadline:
        try:
            d = s.recv(8192)
        except socket.timeout:
            break
        except Exception:
            break
        if not d:
            break
        buf += d
        if mark.encode() in buf:
            break
    txt = buf.decode("utf-8", "replace")
    out = []
    for ln in txt.splitlines():
        if mark in ln: continue
        if ln.strip().endswith("echo " + mark): continue
        if ln.strip() == cmd: continue
        if "can't open __PMK" in ln: continue
        out.append(ln)
    return "\n".join(out).strip("\n")

def main():
    print("[*] 连接 %s:%d ..." % (HOST, PORT))
    s = connect()
    time.sleep(1.0)
    try:
        s.settimeout(2); s.recv(8192)
    except Exception:
        pass
    print("[OK] 已连接\n")

    cmds = [
        "echo PROBE_OK",
        "busybox base64 -d /dev/null 2>&1 | head -1",   # 有 base64 appplet 则报用法, 无则 applet not found
        "command -v wget",
        "command -v nc",
        "command -v md5sum",
        "df -h /media/roms 2>/dev/null | tail -2",
        "ls -la /media/roms/apps/netinfo/ 2>&1",
        "echo HEDOC_TEST",
        "cat > /tmp/__htest <<'HX'\nabc123\nHX",
        "cat /tmp/__htest",
        "rm -f /tmp/__htest",
        "cat /proc/net/tcp | grep -i ':0017' | head -2",  # 找 PC(telnet客户端)对端IP
        "ifconfig wlan0 2>/dev/null | grep -i 'inet addr' | head -1",
    ]
    for c in cmds:
        print("### %s" % c)
        print(run(s, c))
        print()

    s.sendall(b"exit\n")
    s.close()

if __name__ == "__main__":
    main()
