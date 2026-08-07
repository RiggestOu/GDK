import socket, time, hashlib, sys

HOST="192.168.0.45"; PORT=23
LOCAL="E:/WorkBuddy/GDKmini/GDK/src/connect_wifi.sh"
DEVICE="/media/roms/apps/netinfo/connect_wifi.sh"
TMP="/tmp/cw_v6.tmp"

def connect(tries=45, wait=1.0):
    last=None
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(4)
            s.connect((HOST,PORT)); s.settimeout(10); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print("CONNECT_FAIL",last); return None

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

def send_file(s, path, target):
    data=open(path,"rb").read()
    s.sendall(("cat > %s\n"%target).encode())
    time.sleep(0.3)
    s.sendall(data)
    time.sleep(0.5)
    s.shutdown(socket.SHUT_WR)
    s.close()
    return len(data)

data=open(LOCAL,"rb").read()
print("本地文件 %d 字节, md5=%s"%(len(data), hashlib.md5(data).hexdigest()))

# 阶段1: 传文件到 tmp
s=connect()
if not s:
    print(">>> 设备不可达, 无法部署。"); sys.exit(1)
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except: pass
n=send_file(s, LOCAL, TMP)
print("阶段1 发送 %d 字节"%n)
time.sleep(1.5)

# 阶段2: md5 校验
s=connect()
time.sleep(0.5)
try: s.settimeout(2); s.recv(8192)
except: pass
remote_md5=run(s,"md5sum %s | awk '{print $1}'"%TMP).strip()
s.close()
local_md5=hashlib.md5(data).hexdigest()
print("阶段2 md5 本地=%s 设备=%s -> %s"%(local_md5, remote_md5, "一致" if local_md5==remote_md5 else "不一致!!!"))
if local_md5!=remote_md5:
    print(">>> md5 不一致, 中止部署(不覆盖旧文件)。"); sys.exit(1)

# 阶段3a: 先 sh -n 语法校验 tmp (通过才覆盖正式文件, 避免坏脚本上线)
s=connect()
time.sleep(0.5)
try: s.settimeout(2); s.recv(8192)
except: pass
res=run(s,"sh -n %s && echo SYNTAX_OK"%TMP)
s.close()
print("阶段3a 语法校验:\n"+res)
if "SYNTAX_OK" not in res:
    print(">>> 语法校验未通过, 不覆盖旧文件(保留当前可用版本)。"); sys.exit(1)

# 阶段3b: 覆盖 + chmod
s=connect()
time.sleep(0.5)
try: s.settimeout(2); s.recv(8192)
except: pass
res=run(s,"mv -f %s %s && chmod 755 %s && echo MVOK"%(TMP,DEVICE,DEVICE))
s.close()
print("阶段3b 安装:\n"+res)

# 阶段4: 杀掉旧双实例(用 [c] 技巧避免自杀)
s=connect()
time.sleep(0.5)
try: s.settimeout(2); s.recv(8192)
except: pass
res2=run(s,"pkill -f '[c]onnect_wifi.sh' 2>/dev/null; sleep 1; echo PKILLDONE")
s.close()
print("阶段4 杀旧实例: "+res2.strip())

print("\n>>> 部署完成。请退出 netinfo 重进(或重启设备), v6 将拉起单实例 telnet 服务。")
