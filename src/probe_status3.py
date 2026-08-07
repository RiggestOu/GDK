import socket, time, sys

HOST="192.168.0.45"; PORT=23

def connect(tries=30, wait=1.0):
    last=None
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(4)
            s.connect((HOST,PORT)); s.settimeout(10); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print(">>> CONNECT_FAIL:",last); return None

def run(s, cmd, to=15.0):
    mark="__RK_%d__"%int(time.time()*1000%100000000)
    try: s.sendall(("%s ; echo %s\n"%(cmd,mark)).encode())
    except Exception as e: return "[send fail]%s"%e
    buf=b""; dl=time.time()+to
    while time.time()<dl:
        try: d=s.recv(8192)
        except: break
        if not d: break
        buf+=d
        if mark.encode() in buf: break
    return "\n".join(l for l in buf.decode("utf-8","replace").splitlines()
                     if mark not in l and l.strip()!="echo "+mark and l.strip()!=cmd
                     and "can't open __RK" not in l)

s=connect()
if not s:
    print(">>> 设备不可达。"); sys.exit(1)
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass

print("=== [A] 进程快照 (busybox ps) ===")
print(run(s,"ps 2>/dev/null | grep -E 'connect_wifi|nc ' | grep -v grep"))
print("\n=== [B] 当前端口23 inode + 持有者 ===")
print(run(s,"INO=$(awk 'NR>1 && $4==\"01\" && $2 ~ /:0017$/ {print $10; exit}' /proc/net/tcp /proc/net/tcp6 2>/dev/null); echo \"端口23 inode=$INO\"; if [ -n \"$INO\" ]; then for ff in /proc/[0-9]*/fd/*; do t=$(readlink $ff 2>/dev/null); if [ \"$t\" = \"socket:[$INO]\" ]; then echo \"持有者: $ff -> pid=$(echo $ff|cut -d/ -f3)\"; fi; done; fi"))
print("\n=== [C] wifi_stderr.txt (netinfo 捕获的 connect_wifi 输出) ===")
print(run(s,"tail -30 /media/roms/apps/netinfo/wifi_stderr.txt 2>/dev/null || echo NO_STDERR_LOG"))
print("\n=== [D] 当前时间与 uptime ===")
print(run(s,"date; uptime 2>/dev/null"))
s.close()
print("\n>>> 探针完成。")
