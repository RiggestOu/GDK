import socket, time

HOST="192.168.0.45"; PORT=23
LOCAL="E:/WorkBuddy/GDKmini/GDK/src/connect_wifi.sh"
DEVICE="/media/roms/apps/netinfo/connect_wifi.sh"

def connect(host, port=23, tries=45, wait=1.0):
    last=None
    for i in range(tries):
        try:
            s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(4)
            s.connect((host,port)); s.settimeout(8); return s
        except Exception as e:
            last=e; time.sleep(wait)
    print("CONNECT_FAIL",last); return None

s=connect(HOST,PORT)
if not s:
    print("UNREACHABLE"); raise SystemExit
time.sleep(1.0)
try: s.settimeout(2); s.recv(8192)
except Exception: pass

mark="__DLMK_%d__"%int(time.time()*1000%100000000)
s.sendall(("cat %s ; echo %s\n"%(DEVICE,mark)).encode())
buf=b""; dl=time.time()+30
while time.time()<dl:
    try: d=s.recv(16384)
    except Exception: break
    if not d: break
    buf+=d
    if mark.encode() in buf: break

text=buf.decode("utf-8","replace")
lines=text.splitlines()
# 去掉命令回显行 + marker 之后
out=[]
seen_marker=False
for l in lines:
    if mark in l:
        seen_marker=True
        break
    out.append(l)
# 去掉首行命令回显
if out and out[0].strip().startswith("cat "):
    out=out[1:]
content="\n".join(out).rstrip()+"\n"
open(LOCAL,"w",encoding="utf-8").write(content)
print("DOWNLOADED bytes=%d lines=%d"%(len(content), content.count("\n")))
s.sendall(b"exit\n"); s.close()
