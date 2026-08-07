import socket, time, hashlib, sys

HOST="192.168.0.45"; PORT=23
LOCAL="E:/WorkBuddy/GDKmini/GDK/src/connect_wifi.sh"
DEVICE="/media/roms/apps/netinfo/connect_wifi.sh"
TMP="/tmp/cw_v61.tmp"

def connect(tries=45, wait=1.0):
    last=None
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(4)
            s.connect((HOST,PORT)); s.settimeout(10); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print("CONNECT_FAIL",last); return None

def run(s, cmd, to=20.0):
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

def drain(s):
    try: s.settimeout(2); s.recv(8192)
    except: pass

def send_file(s, path, target):
    data=open(path,"rb").read()
    s.sendall(("cat > %s\n"%target).encode())
    time.sleep(0.3)
    s.sendall(data)
    time.sleep(0.5)
    s.shutdown(socket.SHUT_WR); s.close()
    return len(data)

data=open(LOCAL,"rb").read()
print("本地文件 %d 字节, md5=%s"%(len(data), hashlib.md5(data).hexdigest()))

# ---- 阶段1: 传文件到 tmp ----
s=connect()
if not s:
    print(">>> 设备不可达, 中止。"); sys.exit(1)
time.sleep(1.0); drain(s)
n=send_file(s, LOCAL, TMP)
print("阶段1 发送 %d 字节"%n)
time.sleep(1.5)

# ---- 阶段2: md5 ----
s=connect(); time.sleep(0.5); drain(s)
remote_md5=run(s,"md5sum %s | awk '{print $1}'"%TMP).strip()
s.close()
local_md5=hashlib.md5(data).hexdigest()
print("阶段2 md5 本地=%s 设备=%s -> %s"%(local_md5, remote_md5, "一致" if local_md5==remote_md5 else "不一致!!!"))
if local_md5!=remote_md5:
    print(">>> md5 不一致, 中止, 不覆盖旧文件。"); sys.exit(1)

# ---- 阶段3a: sh -n 语法校验 ----
s=connect(); time.sleep(0.5); drain(s)
res=run(s,"sh -n %s && echo SYNTAX_OK"%TMP)
s.close()
print("阶段3a 语法校验:\n"+res)
if "SYNTAX_OK" not in res:
    print(">>> 语法校验未通过, 不覆盖旧文件。"); sys.exit(1)

# ---- 阶段3b: 覆盖 + chmod ----
s=connect(); time.sleep(0.5); drain(s)
res=run(s,"mv -f %s %s && chmod 755 %s && echo MVOK"%(TMP,DEVICE,DEVICE))
s.close()
print("阶段3b 安装:\n"+res)

# ---- 阶段4: 后台脱钩重启(杀旧监管+nc, 清锁, 拉唯一新实例) ----
# 用 ( ... ) & 脱钩: 即使本 telnet 会话因 nc 被杀而消亡, 子 shell 仍由 init 收养继续执行
cmd=("( pkill -9 -f '[c]onnect_wifi.sh' 2>/dev/null;"
      " pkill -x nc 2>/dev/null;"
      " sleep 2;"
      " rm -f /var/run/connect_wifi.pid;"
      " rmdir /var/run/connect_wifi.lock 2>/dev/null;"
      " nohup sh /media/roms/apps/netinfo/connect_wifi.sh >/media/roms/apps/netinfo/wifi_stderr.txt 2>&1 &"
      " ) & echo RESTART_OK")
s=connect(); time.sleep(0.5); drain(s)
r=run(s,cmd,to=8.0)
s.close()
print("阶段4 重启指令回显: "+r.strip())
if "RESTART_OK" not in r:
    print(">>> 未收到 RESTART_OK, 重启指令可能未送达, 但文件已部署。稍后请退出 netinfo 重进以激活。")
else:
    print(">>> 已发出重启, 等待新实例拉起(约6s)...")

# ---- 阶段5: 重连验证 ----
time.sleep(6.0)
s=connect(tries=30)
if not s:
    print(">>> 验证: 重连失败! 服务可能未起来, 请退出 netinfo 重进或检查 wifi_stderr.txt。")
    sys.exit(1)
time.sleep(1.0); drain(s)
print("\n=== 验证 [A] connect_wifi 进程数(应为1) ===")
print(run(s,"N=$(ps 2>/dev/null | grep '[c]onnect_wifi.sh' | grep -v grep | wc -l | tr -d ' '); echo \"实例数=$N\"; ps 2>/dev/null | grep '[c]onnect_wifi.sh' | grep -v grep"))
print("\n=== 验证 [B] 端口23 LISTEN(应仅1个inode) ===")
print(run(s,"awk 'NR>1 && $4==\"01\" && $2 ~ /:0017$/ {print \"inode=\"$10}' /proc/net/tcp /proc/net/tcp6 2>/dev/null | sort -u"))
print("\n=== 验证 [C] wifi.log 最新启动行(应含 v6.1) ===")
print(run(s,"grep 'connect_wifi v6' /media/roms/apps/netinfo/wifi.log 2>/dev/null | tail -3; echo '--- 最近5行 ---'; tail -5 /media/roms/apps/netinfo/wifi.log 2>/dev/null"))
print("\n=== 验证 [D] on-disk md5 应与本地一致 ===")
print(run(s,"md5sum %s 2>/dev/null"%DEVICE))
s.close()
print("\n>>> 部署+重启+验证流程结束。")
