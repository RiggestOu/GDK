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
    sys.exit(2)

time.sleep(1.2)
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

try: s.settimeout(2); s.recv(8192)
except: pass

# 完整 wifi.log
s.sendall("echo '===== FULL wifi.log ====='; cat /media/roms/apps/netinfo/wifi.log\n".encode())
time.sleep(1.5)
drain("wifi.log 全文")

# 当前实时状态确认
s.sendall("echo '===== 当前状态 ====='; iwconfig wlan0 2>/dev/null | grep -E 'ESSID|Access Point|Link Quality'; route -n 2>/dev/null | grep '^0.0'; cat /media/roms/apps/netinfo/status.txt\n".encode())
time.sleep(1.5)
drain("实时状态")

s.close()
print("[DONE]")
