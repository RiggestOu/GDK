import socket, time

HOST="192.168.0.45"; PORT=23
def connect(tries=45, wait=1.0):
    last=None
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(4)
            s.connect((HOST,PORT)); s.settimeout(8); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print("CONNECT_FAIL",last); return None
def run(s, cmd, to=12.0):
    mark="__VMK_%d__"%int(time.time()*1000%100000000)
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
                     and "can't open __VMK" not in l)

s=connect()
if not s:
    print(">>> 设备当前 telnet 不可达(IP掉?/nc没在听)"); raise SystemExit
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass
print("=== [A] connect_wifi.sh 进程 ===")
print(run(s,"ps 2>/dev/null | grep -E 'connect_wifi|nc ' | grep -v grep"))
print("\n=== [B] v6 pidfile ===")
print(run(s,"cat /var/run/connect_wifi.pid 2>/dev/null; echo '(pidfile end)'"))
print("\n=== [C] 端口23 监听 ===")
print(run(s,"grep -qE ':0017 ' /proc/net/tcp && echo LISTEN || echo NOLISTEN"))
print("\n=== [D] netinfo 进程 ===")
print(run(s,"pgrep -f netinfo | tr '\\n' ' '; echo"))
print("\n=== [E] wifi.log 末尾 8 行 ===")
print(run(s,"tail -8 /media/roms/apps/netinfo/wifi.log"))
s.sendall(b"exit\n"); s.close()
