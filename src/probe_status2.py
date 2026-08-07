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

print("=== [A] 进程树 (pid/ppid/etime/stat) ===")
print(run(s,"ps -o pid,ppid,etime,stat,args 2>/dev/null | grep -E 'connect_wifi|nc ' | grep -v grep"))
print("\n=== [B] 端口23 inode 37066 的持有者 ===")
print(run(s,"for ff in /proc/[0-9]*/fd/*; do tgt=$(readlink $ff 2>/dev/null); case $tgt in socket:[37066]) echo \"$ff -> $tgt (pid=$(echo $ff|cut -d/ -f3))\";; esac; done"))
print("\n=== [C] 两实例各自 fd 中的 socket ===")
print(run(s,"for p in 15432 15578; do echo \"--- pid $p ---\"; ls -la /proc/$p/fd 2>/dev/null | grep -i socket || echo '无socket或无该进程'; done"))
print("\n=== [D] 完整 wifi.log ===")
print(run(s,"cat /media/roms/apps/netinfo/wifi.log 2>/dev/null"))
s.close()
print("\n>>> 探针完成。")
