import socket, time

HOST="192.168.0.45"; PORT=23
def connect(timeout=3, tries=60, wait=2):
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(timeout)
            s.connect((HOST,PORT)); s.settimeout(8); return s
        except Exception:
            time.sleep(wait)
    return None
def run(s, cmd, to=12.0):
    mark="__AMK_%d__"%int(time.time()*1000%100000000)
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
                     and "can't open __AMK" not in l)

s=connect()
if not s:
    print(">>> 持续连不上(可能WiFi暂断或端口无服务)。请在设备上: 退出 netinfo -> 重新进入 GDK NET INFO, v6 会自动拉起 telnet。")
    raise SystemExit
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass

print(">>> 已连接。检查 v6 telnet 服务状态...")
st=run(s,"echo CNT=$(pgrep -f '[c]onnect_wifi.sh' | wc -l | tr -d ' '); echo PIDF=$(cat /var/run/connect_wifi.pid 2>/dev/null)")
print("    状态: "+" / ".join(x for x in st.splitlines() if x.strip()))
cnt=0
for ln in st.splitlines():
    if ln.startswith("CNT="):
        try: cnt=int(ln.split("=")[1])
        except: cnt=0
if cnt==0:
    print(">>> 无 telnet 服务在跑, 立即后台拉起 v6 ...")
    try: s.sendall(b"nohup sh /media/roms/apps/netinfo/connect_wifi.sh >/dev/null 2>&1 &\n")
    except: pass
    time.sleep(3)
    try: s.close()
    except: pass
    s2=connect()
    if s2:
        time.sleep(1.0)
        try: s2.settimeout(2); s2.recv(8192)
        except: pass
        v=run(s2,"grep -qE ':0017 ' /proc/net/tcp && echo LISTEN || echo NOLISTEN; echo PROC=$(pgrep -f '[c]onnect_wifi.sh' | wc -l | tr -d ' ')")
        print(">>> 恢复后: "+" / ".join(x for x in v.splitlines() if x.strip()))
        s2.sendall(b"exit\n"); s2.close()
        print(">>> telnet 已由 v6 恢复, 可重新连接。")
    else:
        print(">>> 拉起后重连失败, 请设备端退出并重新进入 netinfo。")
else:
    print(">>> v6 已在运行(进程数=%d)。若仍连不上多为WiFi瞬时抖动, 稍候重试即可。"%cnt)
    try: s.sendall(b"exit\n"); s.close()
    except: pass
