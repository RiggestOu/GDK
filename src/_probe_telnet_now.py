import socket, time, sys

HOST = "192.168.0.45"
PORT = 23

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(6)
try:
    s.connect((HOST, PORT))
    print("[OK] TCP 连接到 %s:%d 成功" % (HOST, PORT))
except Exception as e:
    print("[FAIL] 连接失败:", e)
    print(">>> 设备当前不可达：可能 (1) SD 卡仍在电脑上、设备未启动；(2) 设备未连上 WiFi 故无 192.168.0.45；(3) 本机与设备不在同一网段。")
    sys.exit(2)

time.sleep(1.5)
s.settimeout(3.0)

def drain(label):
    buf = b""
    s.settimeout(2.0)
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            buf += d
    except Exception:
        pass
    txt = buf.decode("utf-8", "replace")
    print("=== %s (%d bytes) ===" % (label, len(buf)))
    print(txt if txt.strip() else "(无输出)")
    return txt

drain("shell banner")

cmds = [
    "uname -a\n",
    "echo '--- wlan0 ifconfig ---'; ifconfig wlan0 2>/dev/null; echo '--- wlan0 iwconfig ---'; iwconfig wlan0 2>/dev/null\n",
    "echo '--- route ---'; route -n 2>/dev/null; echo '--- ip route ---'; ip route 2>/dev/null\n",
    "echo '--- lsusb wifi ---'; lsusb 2>/dev/null | grep -i 0bda; echo '--- lsmod rtl ---'; lsmod 2>/dev/null | grep rtl\n",
    "echo '--- procs ---'; ps 2>/dev/null | grep -E 'nc |connect_wifi|netinfo' | grep -v grep\n",
    "echo '--- wifi.log tail ---'; tail -n 25 /media/roms/apps/netinfo/wifi.log 2>/dev/null\n",
    "echo '--- status.txt ---'; cat /media/roms/apps/netinfo/status.txt 2>/dev/null\n",
]
for c in cmds:
    s.sendall(c.encode())
    time.sleep(1.3)
    drain("cmd: " + c.strip()[:40])

s.close()
print("[DONE]")
