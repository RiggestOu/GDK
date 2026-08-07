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
print("=== [A] 电压节点(微伏) ===")
print(run(s,"for p in /sys/class/power_supply/battery/voltage_now /sys/class/power_supply/BAT/voltage_now /sys/class/power_supply/bat/voltage_now /sys/class/power_supply/jz-battery/voltage_now; do echo -n \"$p = \"; cat $p 2>/dev/null || echo MISSING; done"))
print("\n=== [B] capacity 节点(不准的那个) ===")
print(run(s,"for p in /sys/class/power_supply/battery/capacity /sys/class/power_supply/BAT/capacity; do echo -n \"$p = \"; cat $p 2>/dev/null || echo MISSING; done"))
print("\n=== [C] status + usb/online ===")
print(run(s,"echo -n 'status='; cat /sys/class/power_supply/battery/status 2>/dev/null; echo -n 'usb_online='; cat /sys/class/power_supply/usb/online 2>/dev/null; echo -n 'ac_online='; cat /sys/class/power_supply/ac/online 2>/dev/null; echo"))
s.close()
print("\n>>> 探针完成。")
