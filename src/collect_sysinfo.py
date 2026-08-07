#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
通过 telnet(端口23, nc -e /bin/sh 裸 shell) 完整采集 GDK mini 系统信息。
用唯一 MARKER 分段，逐条命令可靠读取，输出存本地文件。
用法: python collect_sysinfo.py [IP]
"""
import socket, time, sys, io

# 支持传多个 IP 轮询; 默认抢连 45 和 226 (历史 WiFi 抖动的两个地址)
ARGS = [a for a in sys.argv[1:] if a]
HOSTS = ARGS if ARGS else ["192.168.0.45", "192.168.0.226"]
PORT = 23
OUT  = r"E:\WorkBuddy\GDKmini\GDK\src\sysinfo_dump.txt"
WAIT_WINDOW = 150   # 秒: 在此窗口内持续抢连, 抓住任一 WiFi 稳定+监听就绪的瞬间

def wait_connect():
    """在 WAIT_WINDOW 秒内轮询所有 HOST, 连上即返回 (sock, host)"""
    deadline = time.time() + WAIT_WINDOW
    tries = 0
    while time.time() < deadline:
        for h in HOSTS:
            tries += 1
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1.5)
            try:
                s.connect((h, PORT))
                print("[OK] 第%d次尝试连上 %s:%d" % (tries, h, PORT))
                s.settimeout(8)
                return s, h
            except Exception as e:
                if tries % 10 == 0:
                    print("  ...已试 %d 次 (最近: %s %s), 继续抢连..." % (tries, h, e))
                try: s.close()
                except Exception: pass
            time.sleep(0.3)
    return None, None

def run(s, cmd, per_timeout=8.0):
    """发送一条命令, 用 marker 读到命令结束, 返回输出文本"""
    # 注意: marker 绝不能含 < > | & 等 shell 元字符, 否则会被当成重定向/管道
    mark = "__ENDMK_%d__" % int(time.time()*1000 % 100000000)
    full = "%s ; echo %s\n" % (cmd, mark)
    s.sendall(full.encode())
    buf = b""
    s.settimeout(per_timeout)
    deadline = time.time() + per_timeout
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
    # 去掉回显的命令行与 marker 行
    lines = txt.splitlines()
    out = []
    for ln in lines:
        if mark in ln:
            continue
        if ln.strip().endswith("echo " + mark):
            continue
        if ln.strip() == cmd:
            continue
        if "can't open __ENDMK" in ln or "can't open END_" in ln:
            continue
        out.append(ln)
    return "\n".join(out).strip("\n")

SECTIONS = [
    ("内核 / 系统标识", [
        "uname -a",
        "cat /proc/version",
        "cat /etc/os-release 2>/dev/null",
        "cat /etc/issue 2>/dev/null",
        "cat /etc/*release 2>/dev/null",
        "cat /etc/opendingux-version 2>/dev/null",
        "hostname",
        "uptime",
        "date",
    ]),
    ("CPU / 硬件", [
        "cat /proc/cpuinfo",
        "cat /proc/loadavg",
    ]),
    ("内存", [
        "cat /proc/meminfo | head -20",
        "free 2>/dev/null || free -m 2>/dev/null",
    ]),
    ("磁盘 / 挂载", [
        "df -h 2>/dev/null || df 2>/dev/null",
        "mount",
        "cat /proc/mounts",
    ]),
    ("网络", [
        "ifconfig -a 2>/dev/null",
        "ip addr 2>/dev/null",
        "cat /proc/net/dev",
        "route -n 2>/dev/null || cat /proc/net/route",
        "cat /etc/resolv.conf 2>/dev/null",
        "for n in /sys/class/net/*; do echo \"--- $n ---\"; cat $n/address 2>/dev/null; cat $n/operstate 2>/dev/null; done",
    ]),
    ("libc / 工具链运行时", [
        "ls -la /lib/libc* 2>/dev/null",
        "ls -la /lib/ld* 2>/dev/null",
        "ls -la /lib 2>/dev/null | head -60",
        "busybox 2>&1 | head -5",
        "busybox 2>&1 | tail -20",
    ]),
    ("SDL / 图形库", [
        "ls -la /usr/lib/libSDL* 2>/dev/null",
        "ls -la /usr/lib/libSDL_* 2>/dev/null",
        "ls -la /usr/lib/libpng* /usr/lib/libjpeg* /usr/lib/libz* /usr/lib/libfreetype* 2>/dev/null",
        "ls -la /usr/lib/libwebp* 2>/dev/null",
        "sdl-config --version --libs --cflags 2>/dev/null",
        "ls /usr/lib/ | head -100",
    ]),
    ("字体", [
        "ls -la /usr/share/fonts/ 2>/dev/null",
        "find /usr/share/fonts -iname '*.ttf' -o -iname '*.otf' 2>/dev/null",
        "ls -la /usr/share/fonts/dejavu/ 2>/dev/null",
    ]),
    ("内核模块 (WiFi 驱动等)", [
        "uname -r",
        "ls -la /lib/modules/ 2>/dev/null",
        "find /lib/modules -iname '*.ko' 2>/dev/null | head -80",
        "lsmod 2>/dev/null",
        "lsusb 2>/dev/null",
    ]),
    ("目录结构 / 菜单系统", [
        "echo HOME=$HOME",
        "ls -la /media/roms/ 2>/dev/null",
        "ls -la /media/roms/apps/ 2>/dev/null",
        "ls -la /usr/local/home/ 2>/dev/null",
        "ls -la /usr/local/home/.esoteric/ 2>/dev/null",
        "ls -la /usr/local/home/.esoteric/sections/ 2>/dev/null",
        "ls -la /usr/local/home/.esoteric/sections/applications/ 2>/dev/null",
        "ls -la /usr/local/home/.esoteric/cache/images/ 2>/dev/null",
        "ls -la /usr/share/n/ 2>/dev/null",
    ]),
    ("已安装 opk / 应用", [
        "ls -la /media/roms/apps/*/ 2>/dev/null",
        "find /media/roms/apps -maxdepth 2 -iname '*.opk' 2>/dev/null",
        "cat /media/roms/apps/EPUBReader/default.gcw0.desktop 2>/dev/null",
    ]),
    ("电源 / 背光 / 系统节点", [
        "ls /sys/class/power_supply/ 2>/dev/null",
        "cat /sys/class/power_supply/*/capacity 2>/dev/null",
        "cat /sys/class/power_supply/*/status 2>/dev/null",
        "ls /sys/class/backlight/ 2>/dev/null",
        "cat /sys/class/backlight/*/max_brightness 2>/dev/null",
        "cat /sys/class/backlight/*/brightness 2>/dev/null",
        "ls /sys/devices/system/cpu/cpu0/cpufreq/ 2>/dev/null",
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null",
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null",
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies 2>/dev/null",
    ]),
    ("输入设备 / 按键", [
        "cat /proc/bus/input/devices 2>/dev/null",
        "ls -la /dev/input/ 2>/dev/null",
        "ls /dev/ 2>/dev/null",
    ]),
    ("显示 / framebuffer", [
        "ls -la /dev/fb* 2>/dev/null",
        "cat /sys/class/graphics/fb0/virtual_size 2>/dev/null",
        "cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null",
        "cat /sys/class/graphics/fb0/modes 2>/dev/null",
    ]),
    ("启动 / 服务 / 环境", [
        "cat /etc/inittab 2>/dev/null",
        "ls /etc/init.d/ 2>/dev/null",
        "echo PATH=$PATH",
        "cat /proc/cmdline 2>/dev/null",
        "ps 2>/dev/null | head -50",
    ]),
    ("esoteric 菜单配置", [
        "cat /usr/local/home/.esoteric/esoteric.conf 2>/dev/null",
        "ls /usr/local/home/.esoteric/ 2>/dev/null",
        "ls -R /usr/local/home/.esoteric/sections/ 2>/dev/null",
        "cat /usr/local/home/.esoteric/sections/applications/epubreader 2>/dev/null",
        "cat /usr/local/home/.esoteric/sections/applications/netinfo 2>/dev/null",
        "ls -la /usr/local/home/.esoteric/cache/images/ 2>/dev/null",
    ]),
]

def main():
    print("[*] 持续抢连 %s :%d (窗口 %ds)..." % (",".join(HOSTS), PORT, WAIT_WINDOW))
    s, host = wait_connect()
    if s is None:
        print("[FAIL] 窗口内始终连不上 (WiFi 抖动/nc 未监听)")
        sys.exit(1)
    print("[OK] 已连接 %s, 开始采集..." % host)
    HOST = host
    time.sleep(1.0)
    # 清空初始 banner
    try:
        s.settimeout(2.0)
        s.recv(8192)
    except Exception:
        pass

    parts = []
    parts.append("GDK mini 系统信息采集")
    parts.append("采集时间: " + time.strftime("%Y-%m-%d %H:%M:%S"))
    parts.append("设备: %s:%d" % (HOST, PORT))
    parts.append("=" * 60)

    for title, cmds in SECTIONS:
        print("  [采集] " + title)
        parts.append("\n\n" + "#" * 60)
        parts.append("# " + title)
        parts.append("#" * 60)
        for c in cmds:
            parts.append("\n$ " + c)
            try:
                out = run(s, c)
            except Exception as e:
                out = "(采集异常: %s)" % e
            parts.append(out if out.strip() else "(无输出)")

    try:
        s.sendall(b"exit\n")
    except Exception:
        pass
    s.close()

    text = "\n".join(parts)
    with io.open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("\n[DONE] 已保存到 %s (%d 字节)" % (OUT, len(text.encode("utf-8"))))

if __name__ == "__main__":
    main()
