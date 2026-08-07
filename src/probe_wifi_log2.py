import socket, time

HOST = "192.168.0.45"; PORT = 23

def connect(host, port=23, tries=45, wait=1.0):
    last=None
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET, socket.SOCK_STREAM); s.settimeout(4)
            s.connect((host,port)); s.settimeout(8); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print("CONNECT_FAIL", last); return None

def run(s, cmd, to=20.0):
    mark="__XMK_%d__"%int(time.time()*1000%100000000)
    try: s.sendall(("%s ; echo %s\n"%(cmd,mark)).encode())
    except Exception as e: return "[send fail]%s"%e
    buf=b""; dl=time.time()+to
    while time.time()<dl:
        try: d=s.recv(8192)
        except Exception: break
        if not d: break
        buf+=d
        if mark.encode() in buf: break
    return "\n".join(l for l in buf.decode("utf-8","replace").splitlines()
                     if mark not in l and l.strip()!="echo "+mark and l.strip()!=cmd
                     and "can't open __XMK" not in l)

s=connect(HOST,PORT)
if not s:
    print("DEVICE_UNREACHABLE"); raise SystemExit
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except Exception: pass
print("=== CONNECTED ===")
print("\n--- [A] connect_wifi.sh 完整内容 (行数) ---")
print(run(s, "wc -l /media/roms/apps/netinfo/connect_wifi.sh"))
print("\n--- [B] connect_wifi.sh 全文 ---")
print(run(s, "cat -n /media/roms/apps/netinfo/connect_wifi.sh", to=25))
print("\n--- [C] netinfo.log 末尾 40 行 ---")
print(run(s, "tail -40 /media/roms/apps/netinfo/netinfo.log"))
print("\n--- [D] 端口23 当前监听进程 ---")
print(run(s, "for f in /proc/net/tcp /proc/net/tcp6; do awk 'NR>1 && $4==\"01\" && $2 ~ /:0017$/ {print \"LISTEN inode=\"$10}' \"$f\"; done; echo '--pid by inode--'; for ff in /proc/[0-9]*/fd/*; do ls -l \"$ff\" 2>/dev/null | grep -o 'socket:\\[[0-9]*\\]' | while read sk; do grep -l \"$sk\" /dev/null >/dev/null; done; done; echo done"))
s.sendall(b"exit\n"); s.close()
