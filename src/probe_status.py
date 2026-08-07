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
    print(">>> 设备不可达, 无法读取状态。"); sys.exit(1)
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass

print("=== [1] wifi.log 末尾 40 行 ===")
print(run(s,"tail -40 /media/roms/apps/netinfo/wifi.log 2>/dev/null || echo NO_WIFI_LOG"))
print("\n=== [2] connect_wifi / nc 进程 ===")
print(run(s,"ps 2>/dev/null | grep -E 'connect_wifi|nc ' | grep -v grep"))
print("\n=== [3] 端口 23 监听情况 (/proc/net/tcp) ===")
print(run(s,"grep -E ':0017 ' /proc/net/tcp /proc/net/tcp6 2>/dev/null | grep -i ' 01 ' || echo '端口23无LISTEN'"))
print("\n=== [4] on-disk 脚本版本头 + md5 ===")
print(run(s,"head -5 /media/roms/apps/netinfo/connect_wifi.sh 2>/dev/null; echo '---'; md5sum /media/roms/apps/netinfo/connect_wifi.sh 2>/dev/null"))
print("\n=== [5] pidfile / lock ===")
print(run(s,"ls -la /var/run/connect_wifi.* 2>/dev/null; echo '--- pid内容 ---'; cat /var/run/connect_wifi.pid 2>/dev/null; echo"))
s.close()
print("\n>>> 探针完成。")
