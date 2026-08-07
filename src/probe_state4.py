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
print("=== [A] connect_wifi / nc 进程 + ppid ===")
print(run(s,"for p in $(pgrep -f '[c]onnect_wifi.sh'); do echo \"pid=$p ppid=$(awk '/^PPid:/{print $2}' /proc/$p/status 2>/dev/null) etime=$(cat /proc/$p/stat 2>/dev/null | awk '{print $22}') \"; done"))
print(run(s,"echo '--- nc ---'; pgrep -x nc | while read n; do echo \"nc pid=$n ppid=$(awk '/^PPid:/{print $2}' /proc/$n/status 2>/dev/null)\"; done"))
print("\n=== [B] 端口23 当前 inode + 持有者 ===")
print(run(s,"INO=$(awk 'NR>1 && $4==\"01\" && $2 ~ /:0017$/ {print $10; exit}' /proc/net/tcp /proc/net/tcp6 2>/dev/null); echo \"inode=$INO\"; if [ -n \"$INO\" ]; then for ff in /proc/[0-9]*/fd/*; do t=$(readlink $ff 2>/dev/null); if [ \"$t\" = \"socket:[$INO]\" ]; then echo \"holder pid=$(echo $ff|cut -d/ -f3)\"; fi; done; fi"))
print("\n=== [C] connect_wifi 进程数 ===")
print(run(s,"ps 2>/dev/null | grep '[c]onnect_wifi.sh' | grep -v grep | wc -l | tr -d ' '"))
print("\n=== [D] wifi.log 启动行计数(v6/v6.1) ===")
print(run(s,"grep -c 'connect_wifi v6' /media/roms/apps/netinfo/wifi.log 2>/dev/null; grep 'connect_wifi v6' /media/roms/apps/netinfo/wifi.log 2>/dev/null | tail -3"))
s.close()
print("\n>>> 探针完成。")
