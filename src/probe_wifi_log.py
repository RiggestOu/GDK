import socket, time

HOSTS = ["192.168.0.45", "192.168.0.226"]
PORT = 23

def connect(host, port=23, tries=45, wait=1.0):
    last = None
    for i in range(tries):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(4)
            s.connect((host, port))
            s.settimeout(8)
            return s
        except Exception as e:
            last = e
            time.sleep(wait)
    print("CONNECT_FAIL %s : %s" % (host, last))
    return None

def run(s, cmd, to=12.0):
    mark = "__WMK_%d__" % int(time.time() * 1000 % 100000000)
    try:
        s.sendall(("%s ; echo %s\n" % (cmd, mark)).encode())
    except Exception as e:
        return "[send fail]%s" % e
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
    out = "\n".join(l for l in buf.decode("utf-8", "replace").splitlines()
                    if mark not in l and l.strip() != "echo " + mark and l.strip() != cmd
                    and "can't open __WMK" not in l)
    return out

for HOST in HOSTS:
    print("=== try %s:%d ===" % (HOST, PORT))
    s = connect(HOST, PORT)
    if not s:
        continue
    time.sleep(1.0)
    try:
        s.settimeout(2); s.recv(8192)
    except Exception:
        pass
    print("=== CONNECTED %s ===" % HOST)
    print("\n--- [1] 找 wifi 日志文件 ---")
    print(run(s, "ls -la /media/roms/ 2>/dev/null | grep -iE 'wifi|log'; echo '--netinfo dir--'; ls -la /media/roms/apps/netinfo/ 2>/dev/null | grep -iE 'wifi|log|conf'"))
    print("\n--- [2] wifi.log / wifi_log.txt 内容 ---")
    print(run(s, "for f in /media/roms/wifi.log /media/roms/wifi_log.txt /media/roms/apps/netinfo/wifi.log /media/roms/apps/netinfo/wifi_log.txt; do echo \"### $f\"; cat \"$f\" 2>/dev/null; done"))
    print("\n--- [3] wlan0 / 无线 / 路由状态 ---")
    print(run(s, "ifconfig wlan0 2>/dev/null; echo '--wireless--'; cat /proc/net/wireless 2>/dev/null; echo '--default route--'; cat /proc/net/route 2>/dev/null | awk 'NR==1||$2==\"00000000\"'"))
    print("\n--- [4] nc / wpa / udhcpc / connect_wifi 进程 ---")
    print(run(s, "ps 2>/dev/null | grep -E 'nc |wpa|udhcpc|connect_wifi|netinfo' "))
    print("\n--- [5] connect_wifi.sh 内容 ---")
    print(run(s, "cat /media/roms/apps/netinfo/connect_wifi.sh 2>/dev/null | head -70"))
    print("\n--- [6] dmesg 末尾 (看掉线/驱动) ---")
    print(run(s, "dmesg 2>/dev/null | tail -25"))
    s.sendall(b"exit\n")
    s.close()
    break
else:
    print("ALL_HOSTS_UNREACHABLE")
