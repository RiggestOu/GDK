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
    print(">>> 设备不可达, 无法重启。"); sys.exit(1)
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass

print("=== 重启前: netinfo 进程 ===")
print(run(s,"ps 2>/dev/null | grep -E '[n]etinfo' | grep -v grep | head -3; echo '--- connect_wifi 实例数 ---'; ps 2>/dev/null | grep '[c]onnect_wifi.sh' | grep -v grep | wc -l | tr -d ' '"))

# 杀旧 netinfo 并后台重拉(connect_wifi 已脱离 netinfo, 不受影响, telnet 不断)
cmd=("pkill -x netinfo 2>/dev/null; sleep 1; "
     "nohup /media/roms/apps/netinfo/netinfo >/dev/null 2>&1 & ")
s.sendall((cmd+"echo NETINFO_RESTART_OK\n").encode())
time.sleep(3.0)
# 收一下 ack
try:
    s.settimeout(5); 
    buf=b""
    end=time.time()+5
    while time.time()<end:
        try: d=s.recv(8192)
        except: break
        if not d: break
        buf+=d
        if b"NETINFO_RESTART_OK" in buf: break
    print("重启指令回显: "+buf.decode("utf-8","replace").splitlines()[-1] if buf else "(无回显)")
except Exception as e:
    print("重启指令回显读取: ",e)
s.close()

# 验证
time.sleep(3.0)
s=connect()
if not s:
    print(">>> 重启后重连失败! 请用菜单重新进入 netinfo。telnet 仍由 connect_wifi 提供, 不受影响。")
    sys.exit(1)
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass
print("\n=== 重启后验证 ===")
print(run(s,"echo '--- netinfo 进程 ---'; ps 2>/dev/null | grep '[n]etinfo' | grep -v grep | head -3"))
print(run(s,"echo '--- connect_wifi 实例数(应=1) ---'; ps 2>/dev/null | grep '[c]onnect_wifi.sh' | grep -v grep | wc -l | tr -d ' '"))
print(run(s,"echo '--- 端口23 LISTEN inode(应唯一) ---'; awk 'NR>1 && $4==\"01\" && $2 ~ /:0017$/ {print \"inode=\"$10}' /proc/net/tcp /proc/net/tcp6 2>/dev/null | sort -u"))
print(run(s,"echo '--- on-disk netinfo md5 ---'; md5sum /media/roms/apps/netinfo/netinfo 2>/dev/null"))
print(run(s,"echo '--- netinfo.log 最新 ---'; tail -3 /media/roms/apps/netinfo/netinfo.log 2>/dev/null"))
s.close()
print("\n>>> 重启+验证完成。")
