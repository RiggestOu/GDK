import socket, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.45"
PORT = 23
TIMEOUT = 12

cmds = r'''
echo "@@@START@@@"
echo "=== epubreader section content ==="
cat /usr/local/home/.esoteric/sections/applications/epubreader
echo ""
echo "=== netinfo section content ==="
cat /usr/local/home/.esoteric/sections/applications/netinfo
echo ""
echo "=== 10_terminal (system, working) ==="
cat /usr/local/home/.esoteric/sections/applications/10_terminal
echo ""
echo "=== 25_gmu (system, has icon?) ==="
cat /usr/local/home/.esoteric/sections/applications/25_gmu
echo ""
echo "=== grep icon= in all sections ==="
grep -r "icon=" /usr/local/home/.esoteric/sections/applications/ 2>&1
echo "=== epubreader icon file ==="
ls -la /media/roms/apps/EPUBReader/epubreader_icon.png 2>&1
od -An -tx1 -N 16 /media/roms/apps/EPUBReader/epubreader_icon.png 2>&1
echo "=== where do system icons live? find png in esoteric ==="
ls /usr/local/home/.esoteric/skins/ 2>&1 | head
find /usr/local/home/.esoteric -name '*.png' 2>/dev/null | head
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
