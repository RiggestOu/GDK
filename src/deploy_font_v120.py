# -*- coding: utf-8 -*-
"""通过 telnet (192.168.0.45:23, nc -e /bin/sh 裸 shell) 部署:
  1) 备份并覆盖设备系统字体 /usr/share/fonts/SourceHanSans-Regular-04.ttf <- system_fixed.ttf
  2) 删除设备上旧 loose /media/roms/apps/EPUBReader/system.ttf (避免 app 读到错字 loose 字体)
  3) 部署新 EPUBReader.opk (v1.2.0, 已移除内置字体, app 回退系统字体)
  4) 同步 loose epubreader 二进制 (双保险)
  传输法: cat > 文件 + 裸二进制流 + shutdown(SHUT_WR) 触发 FIN 让 cat 收尾; 重连 md5 校验后 mv。
  (设备 busybox 无 base64, heredoc 在裸 shell 失效, 此法最稳)"""
import socket, time, hashlib, os, sys

HOST, PORT = "192.168.0.45", 23
LOCAL_FONT = "E:/WorkBuddy/GDKmini/GDK/system_fixed.ttf"
LOCAL_OPK  = "E:/WorkBuddy/GDKmini/GDK/src/EPUBReader.opk"
LOCAL_BIN  = "E:/WorkBuddy/GDKmini/GDK/src/epubreader"

DEV_FONT       = "/usr/share/fonts/SourceHanSans-Regular-04.ttf"
DEV_FONT_BAK   = "/usr/share/fonts/SourceHanSans-Regular-04.ttf.bak"
DEV_LOOSE_FONT = "/media/roms/apps/EPUBReader/system.ttf"
DEV_OPK        = "/media/roms/apps/EPUBReader/EPUBReader.opk"
DEV_BIN        = "/media/roms/apps/EPUBReader/epubreader"


def connect(retry=60):
    last = None
    for _ in range(retry):
        try:
            s = socket.socket(); s.settimeout(4); s.connect((HOST, PORT)); s.settimeout(10)
            return s
        except Exception as e:
            last = e; time.sleep(1)
    print("CONNECT_FAIL:", last); sys.exit(1)


def run(s, cmd, to=25.0):
    mark = "__DMK_%d__" % int(time.time() * 1000 % 100000000)
    try:
        s.sendall(("%s ; echo %s\n" % (cmd, mark)).encode())
    except Exception as e:
        return "[sendfail]%s" % e
    buf = b""; dl = time.time() + to
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
                     if mark not in l and l.strip() != "echo " + mark and l.strip() != cmd
                     and "can't open __DMK" not in l)


def drain(s, to=12.0):
    s.settimeout(to); buf = b""
    try:
        while True:
            d = s.recv(8192)
            if not d:
                break
            buf += d
    except Exception:
        pass
    return buf


def push(local, dev_tmp):
    data = open(local, "rb").read()
    s = connect()
    s.sendall(("cat > %s\n" % dev_tmp).encode())
    s.sendall(data)
    s.shutdown(socket.SHUT_WR)
    drain(s)
    s.close()
    return len(data)


def md5_local(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


def md5_remote(path):
    s = connect()
    out = run(s, "md5sum %s 2>/dev/null | awk '{print $1}'" % path, to=30)
    s.close()
    for line in out.splitlines():
        line = line.strip()
        if line:
            return line.split()[0]
    return None


def deploy_file(local, dev_tmp, dev_final, label):
    sz = os.path.getsize(local)
    print(">>> [%s] 传输 %d B ..." % (label, sz))
    push(local, dev_tmp)
    rmd5 = md5_remote(dev_tmp)
    lmd5 = md5_local(local)
    print("    tmp md5: local=%s remote=%s" % (lmd5, rmd5))
    if rmd5 != lmd5:
        print("    !!! MD5 不匹配，终止部署"); sys.exit(1)
    s = connect()
    run(s, "mv -f %s %s ; chmod 755 %s ; echo DONE" % (dev_tmp, dev_final, dev_final), to=15)
    s.close()
    print("    [OK] 已落盘 %s" % dev_final)


print("== 阶段0: 环境探测 ==")
s = connect()
print(run(s, "echo PROBE_OK; id -un; df -h /usr/share/fonts | tail -1; ls -la /usr/share/fonts/SourceHanSans-Regular-04.ttf", to=15))
s.close()

print("== 阶段1: 备份原系统字体 -> .bak ==")
s = connect()
print(run(s, "cp -f %s %s && echo BAK_OK || echo BAK_FAIL" % (DEV_FONT, DEV_FONT_BAK), to=90))
s.close()

print("== 阶段2: 覆盖系统字体 ==")
deploy_file(LOCAL_FONT, DEV_FONT + ".tmp", DEV_FONT, "系统字体 SourceHanSans-Regular-04.ttf")

print("== 阶段3: 删除设备上旧 loose system.ttf ==")
s = connect()
print(run(s, "rm -f %s && echo RM_OK || echo RM_FAIL" % DEV_LOOSE_FONT, to=15))
s.close()

print("== 阶段4: 部署新 EPUBReader.opk (v1.2.0, 已移除内置字体) ==")
deploy_file(LOCAL_OPK, DEV_OPK + ".tmp", DEV_OPK, "EPUBReader.opk")

print("== 阶段5: 同步 loose epubreader 二进制 ==")
deploy_file(LOCAL_BIN, DEV_BIN + ".tmp", DEV_BIN, "epubreader binary")

print("== 部署完成 ==")
s = connect()
print(run(s, "echo FINAL; ls -la /usr/share/fonts/SourceHanSans-Regular-04.ttf; ls /media/roms/apps/EPUBReader/", to=15))
s.close()
