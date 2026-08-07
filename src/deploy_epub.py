#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
通过 telnet(192.168.0.45:23, nc -e /bin/sh 裸 shell) 把新版 EPUB 阅读器部署到设备。
设备 section 实际启动方式: exec=/usr/bin/opkrun params=-m default.gcw0.desktop "EPUBReader.opk"
→ 必须更新 OPK（菜单路径）。同时同步 loose epubreader 二进制（launch.sh 直启路径），两条路都覆盖。
传输: 直接把二进制流喂给 `cat > 目标.tmp`, 关闭写端(FIN)让 cat 收尾; 再重连做 md5 校验并原子 mv。
不依赖 base64(设备无) / heredoc(裸shell不支持) / wget(需PC IP+防火墙)。
"""
import socket, time, hashlib, sys

HOST = "192.168.0.45"
PORT = 23
SRC_OPK = r"E:\WorkBuddy\GDKmini\GDK\src\EPUBReader.opk"
SRC_BIN = r"E:\WorkBuddy\GDKmini\GDK\src\epubreader"
BASE    = "/media/roms/apps/EPUBReader"
TMP_OPK = BASE + "/EPUBReader.opk.tmp"
DST_OPK = BASE + "/EPUBReader.opk"
TMP_BIN = BASE + "/epubreader.tmp"
DST_BIN = BASE + "/epubreader"

def connect(retry=40, wait=1.0):
    last = None
    for i in range(retry):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(4)
            s.connect((HOST, PORT))
            s.settimeout(30)
            return s
        except Exception as e:
            last = e
            time.sleep(wait)
    print("[FAIL] 重连 %d 次仍失败: %s" % (retry, last))
    return None

def run(s, cmd, to=30.0):
    mark = "__EMK_%d__" % int(time.time()*1000 % 100000000)
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
        if "can't open __EMK" in ln: continue
        out.append(ln)
    return "\n".join(out).strip("\n")

def drain(s):
    try: s.settimeout(2); s.recv(8192)
    except Exception: pass

def send_file(s, path, target):
    data = open(path, "rb").read()
    s.sendall(("cat > %s\n" % target).encode())   # shell 执行 cat, cat 接管后续 stdin
    time.sleep(0.3)
    s.sendall(data)                                # 裸 TCP, nc 不解析 0xFF/0x00, cat 逐字节写入
    time.sleep(1.0)
    s.shutdown(socket.SHUT_WR)                     # FIN -> cat 收 EOF -> 文件写完成
    s.close()
    return len(data)

def deploy_one(name, SRC, TMP, DST):
    local = open(SRC, "rb").read()
    local_md5 = hashlib.md5(local).hexdigest()
    sz = len(local)
    print("[*] 本地 %s: %d 字节, md5=%s" % (name, sz, local_md5))
    print("[*] 传输到 %s ..." % TMP)
    s = connect()
    if s is None: return False
    time.sleep(1.0); drain(s)
    n = send_file(s, SRC, TMP)
    print("[OK] 已发送 %d 字节 (TCP 可靠传输)" % n)
    # 大文件(OPK)写 SD 卡较慢, 等多一会儿让 nc 重启监听
    wait = 10.0 if sz > 1000000 else 2.0
    print("[*] 等待 %g 秒让 nc 重启监听..." % wait)
    time.sleep(wait)
    s2 = connect()
    if s2 is None:
        print("[WARN] 无法重连, 文件已落到 %s 但未校验" % TMP); return False
    time.sleep(0.8); drain(s2)
    dev_md5 = run(s2, "md5sum %s" % TMP).replace("\n", " ").strip()
    dhash = dev_md5.split()[0] if dev_md5 else ""
    print("[设备] md5sum tmp: %s" % dev_md5)
    if dhash == local_md5:
        print("[OK] md5 一致, 原子移动并设可执行...")
        res = run(s2, "mv -f %s %s && chmod 755 %s && ls -l %s" % (TMP, DST, DST, DST))
        print(res)
        ok = True
    else:
        print("[FAIL] md5 不一致! 删除临时文件, 保留旧版。")
        run(s2, "rm -f %s" % TMP)
        print("  本地: %s\n  设备: %s" % (local_md5, dhash))
        ok = False
    try: s2.sendall(b"exit\n"); s2.close()
    except Exception: pass
    return ok

def main():
    ok_opk = deploy_one("EPUBReader.opk", SRC_OPK, TMP_OPK, DST_OPK)
    ok_bin = deploy_one("epubreader(loose)", SRC_BIN, TMP_BIN, DST_BIN)
    print("\n==== 部署结果 ====")
    print("OPK :", "已更新" if ok_opk else "未更新(见上)")
    print("BIN :", "已更新" if ok_bin else "未更新(见上)")
    if ok_opk:
        print("\n[DEPLOY DONE] 请退出并【重启 EPUB 阅读器】(当前若正在运行, 退出重进才能加载新 OPK)。")
    else:
        print("\n[DEPLOY INCOMPLETE] OPK 未更新, 请检查 telnet 并重试。")

if __name__ == "__main__":
    main()
