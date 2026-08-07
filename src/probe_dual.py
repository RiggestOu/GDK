import socket, time
HOST="192.168.0.45"; PORT=23
def connect(tries=45, wait=1.0):
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(4)
            s.connect((HOST,PORT)); s.settimeout(8); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print("CONNECT_FAIL",last); return None
def run(s, cmd, to=12.0):
    mark="__DMK_%d__"%int(time.time()*1000%100000000)
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
                     and "can't open __DMK" not in l)
s=connect()
if not s:
    print("UNREACHABLE"); raise SystemExit
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass
print("=== [A] 两个 connect_wifi.sh 进程详情 ===")
print(run(s,"ps -o pid,ppid,etime,args 2>/dev/null | grep connect_wifi | grep -v grep"))
print("\n=== [B] 端口23 LISTEN socket (inode) ===")
print(run(s,"grep -E ':0017 ' /proc/net/tcp 2>/dev/null; echo '---tcp6---'; grep -E ':0017 ' /proc/net/tcp6 2>/dev/null; echo done"))
print("\n=== [C] 锁/pid 文件 ===")
print(run(s,"ls -la /var/run/connect_wifi.* 2>/dev/null; echo '--- pid内容 ---'; cat /var/run/connect_wifi.pid 2>/dev/null; echo"))
print("\n=== [D] wifi.log 末尾 18 行 (看 v6 启动/双实例) ===")
print(run(s,"tail -18 /media/roms/apps/netinfo/wifi.log"))
print("\n=== [E] netinfo 进程 ===")
print(run(s,"pgrep -af netinfo | grep -v grep"))
s.sendall(b"exit\n"); s.close()
