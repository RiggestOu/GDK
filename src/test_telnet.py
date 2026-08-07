import socket, time, sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.45"
PORT = 23
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(6)
try:
    s.connect((HOST, PORT))
    print("[OK] TCP 连接到 %s:%d 成功" % (HOST, PORT))
except Exception as e:
    print("[FAIL] 连接失败:", e)
    sys.exit(1)

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

drain("shell")

cmds = [
    "echo HOME=$HOME\n",
    "ls -la /usr/local/home/.esoteric/sections/applications/ 2>&1\n",
    "ls -la /h/local/home/.esoteric/ 2>&1\n",
    "find / -path '*esoteric*sections*' -maxdepth 12 2>/dev/null | head -40\n",
    "ls /media/roms/apps/ 2>&1\n",
]
for c in cmds:
    s.sendall(c.encode()); time.sleep(1.2); drain("cmd: " + c.strip())

print("\n########## 对比一个正常app ##########")
# 选第一个非 EPUBReader/netinfo 的 app 目录做对比
s.sendall("APPS=$(ls /media/roms/apps/); for a in $APPS; do if [ \"$a\" != EPUBReader ] && [ \"$a\" != netinfo ]; then echo \"--- $a ---\"; ls -la /media/roms/apps/$a/ 2>&1; cat /media/roms/apps/$a/*.desktop 2>&1; break; fi; done\n".encode())
time.sleep(2.0)
drain("对比app desktop")

print("\n########## launch.sh 自愈注册逻辑 ##########")
s.sendall("cat /media/roms/apps/EPUBReader/launch.sh 2>&1\n".encode())
time.sleep(1.5)
drain("launch.sh")

s.close()
print("[DONE]")
