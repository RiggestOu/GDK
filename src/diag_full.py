import socket, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.45"
PORT = 23
TIMEOUT = 14

cmds = r'''
echo "@@@START@@@"
echo "=== epubreader section ==="
cat /usr/local/home/.esoteric/sections/applications/epubreader
echo ""
echo "=== netinfo section ==="
cat /usr/local/home/.esoteric/sections/applications/netinfo
echo ""
echo "=== 10_terminal (system, working) ==="
cat /usr/local/home/.esoteric/sections/applications/10_terminal
echo ""
echo "=== 25_gmu (system, working, has icon) ==="
cat /usr/local/home/.esoteric/sections/applications/25_gmu
echo ""
echo "=== grep icon= all sections ==="
grep -r "icon=" /usr/local/home/.esoteric/sections/applications/ 2>&1
echo "=== epubreader icon magic ==="
od -An -tx1 -N 16 /media/roms/apps/EPUBReader/epubreader_icon.png 2>&1
echo "=== esoteric.conf (menu root paths?) ==="
cat /usr/local/home/.esoteric/esoteric.conf 2>&1
echo "=== any system icon png under sections? ==="
find /usr/local/home/.esoteric/sections -name '*.png' 2>/dev/null | head
echo "=== full listing of sections/applications ==="
ls -la /usr/local/home/.esoteric/sections/applications/
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
    a = text.find("@@@START@@@")
    b = text.find("@@@ALLDONE@@@")
    seg = text[a+len("@@@START@@@"):b] if (a>=0 and b>=0) else text
    print(seg)

if __name__ == "__main__":
    main()
