import socket, time, sys
targets = ["192.168.0.45", "192.168.0.226"]
N = 14
for ip in targets:
    print("===== %s :23 快速重试 %d 次 =====" % (ip, N))
    ok = False
    for i in range(N):
        s = socket.socket(); s.settimeout(1.2)
        try:
            s.connect((ip, 23))
            print("  [%02d] 连上了!" % i); ok = True
            s.close(); break
        except Exception as e:
            code = getattr(e, 'winerror', '')
            print("  [%02d] %s %s" % (i, code, e))
        finally:
            try: s.close()
            except: pass
        time.sleep(0.4)
    print("  => %s\n" % ("成功" if ok else "始终无监听"))
