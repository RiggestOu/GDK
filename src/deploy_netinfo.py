#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
通过 telnet(192.168.0.45:23, nc -e /bin/sh 裸 shell) 把本地 netinfo 二进制部署到设备。
传输方式: 直接把二进制流喂给 `cat > 目标文件`, 关闭写端(FIN)让 cat 收尾; 再重连做 md5 校验并 mv。
不依赖 base64(设备无) / heredoc(裸shell不支持) / wget(需PC IP+防火墙放行)。
只覆盖 netinfo, 保护 connect_wifi.sh / wifi.conf / 其它文件。
"""
import socket, time, hashlib, sys

HOST = "192.168.0.45"
PORT = 23
SRC  = r"E:\WorkBuddy\GDKmini\GDK\src\netinfo"
TMP  = "/media/roms/apps/netinfo/netinfo.tmp"
DST  = "/media/roms/apps/netinfo/netinfo"

def connect(retry=30, wait=1.0):
    last = None
    for i in range(retry):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(4)
            s.connect((HOST, PORT))
            s.settimeout(8)
            return s
        except Exception as e:
            last = e
            time.sleep(wait)
    print("[FAIL] 重连 %d 次仍失败: %s" % (retry, last))
    return None

def run(s, cmd, to=12.0):
    mark = "__DMK_%d__" % int(time.time()*1000 % 100000000)
    s.sendall(("%s ; echo %s\n" % (cmd, mark)).encode())
    buf = b""
    dl = time.time() + to
    while time.time() < dl:
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
        if "can't open __DMK" in ln: continue
        out.append(ln)
    return "\n".join(out).strip("\n")

def send_file(s, path, target):
    """通过已建立的 shell 会话把二进制原样写入 target: `cat > target` + 二进制流 + FIN"""
    data = open(path, "rb").read()
    s.sendall(("cat > %s\n" % target).encode())   # shell 执行 cat, cat 接管后续 stdin
    time.sleep(0.3)
    s.sendall(data)                                # 裸 TCP, nc 不解析 0xFF/0x00, cat 逐字节写入
    time.sleep(0.5)
    s.shutdown(socket.SHUT_WR)                     # FIN -> cat 收 EOF -> 文件写完成
    s.close()
    return len(data)

def main():
    local = open(SRC, "rb").read()
    local_md5 = hashlib.md5(local).hexdigest()
    print("[*] 本地 netinfo: %d 字节, md5=%s" % (len(local), local_md5))

    # 阶段 A: 把二进制流写到临时文件
    print("[*] 阶段A: 连接并传输二进制到 %s ..." % TMP)
    s = connect()
    if s is None:
        sys.exit(1)
    time.sleep(1.0)
    try:
        s.settimeout(2); s.recv(8192)   # drain 初始
    except Exception:
        pass
    n = send_file(s, SRC, TMP)
    print("[OK] 已发送 %d 字节 (TCP 可靠传输)" % n)
    time.sleep(1.5)   # 等 nc 退出后 connect_wifi.sh 循环重起监听

    # 阶段 B: 重连校验 + 移动
    print("[*] 阶段B: 重连做 md5 校验...")
    s2 = connect()
    if s2 is None:
        print("[WARN] 无法重连, 文件已落到 %s 但未校验/未替换正式文件" % TMP)
        print("       请设备重启或确认 telnet 恢复后, 再让我跑一次校验移动。")
        sys.exit(2)
    time.sleep(0.8)
    try:
        s2.settimeout(2); s2.recv(8192)
    except Exception:
        pass
    dev_md5 = run(s2, "md5sum %s" % TMP).replace("\n", " ").strip()
    print("[设备] md5sum tmp: %s" % dev_md5)
    dhash = dev_md5.split()[0] if dev_md5 else ""
    if dhash == local_md5:
        print("[OK] md5 一致, 移动并设可执行...")
        res = run(s2, "mv -f %s %s && chmod 755 %s && ls -l %s" % (TMP, DST, DST, DST))
        print(res)
        print("\n[DEPLOY DONE] netinfo 已更新为 %d 字节" % len(local))
    else:
        print("[FAIL] md5 不一致! 删除临时文件, 保留旧版。")
        run(s2, "rm -f %s" % TMP)
        print("  本地: %s\n  设备: %s" % (local_md5, dhash))
    s2.sendall(b"exit\n")
    s2.close()

if __name__ == "__main__":
    main()
