import socket, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.45"
PORT = 23
TIMEOUT = 12

cmds = r'''
echo "@@@START@@@"
echo "HOME=$HOME"
echo "=== esoteric root ($HOME/.esoteric) ==="
ls -la "$HOME/.esoteric/" 2>&1
echo "=== sections/applications ==="
ls -la "$HOME/.esoteric/sections/applications/" 2>&1
echo "=== find esoteric binary/config ==="
find / -name 'esoteric*' -maxdepth 5 2>/dev/null | head
echo "=== apps on SD ==="
ls /media/roms/apps/ 2>&1
echo "=== EPUBReader desktop ==="
cat /media/roms/apps/EPUBReader/default.gcw0.desktop 2>&1
echo "=== EPUBReader launch.sh ==="
cat /media/roms/apps/EPUBReader/launch.sh 2>&1
echo "=== EPUBReader icon magic (first16 bytes) ==="
od -An -tx1 -N 16 /media/roms/apps/EPUBReader/epubreader_icon.png 2>&1
echo "=== SAMPLE: other apps desktop+icon ==="
for d in /media/roms/apps/*/; do
  echo "---- DIR: $d"
  ls "$d" 2>&1
  if [ -f "$d"default.gcw0.desktop ]; then cat "$d"default.gcw0.desktop; fi
  echo
done | head -80
echo "@@@ALLDONE@@@"
'''

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    try:
        s.connect((HOST, PORT))
    except Exception as e:
        print("CONNECT FAIL:", e); return
    time.sleep(1.5)
    try:
        s.sendall(cmds.encode("latin1"))
    except Exception as e:
        print("SEND FAIL:", e)
    # 读输出
    buf = b""
    end = time.time() + TIMEOUT
    while time.time() < end:
        try:
            s.settimeout(2)
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            if b"@@@ALLDONE@@@" in buf:
                break
            continue
        except Exception:
            break
        if b"@@@ALLDONE@@@" in buf:
            break
    s.close()
    text = buf.decode("latin1", "replace")
    # 截取标记之间
    a = text.find("@@@START@@@")
    b = text.find("@@@ALLDONE@@@")
    if a >= 0 and b >= 0:
        seg = text[a+len("@@@START@@@"):b]
    else:
        seg = text
    # 去掉命令行回显噪声：行首匹配已发送命令的粗略过滤
    print(seg)

if __name__ == "__main__":
    main()
