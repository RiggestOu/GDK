import socket, time

HOST = "192.168.0.45"
PORT = 23
N = 10

def one_round(i):
    # connect：失败最多重试3次，应对 nc 重新监听的间隙
    s = None
    ct = None
    last_err = None
    for attempt in range(4):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(6)
            t0 = time.time()
            s.connect((HOST, PORT))
            ct = time.time() - t0
            break
        except Exception as e:
            last_err = e
            time.sleep(0.4)
    if s is None:
        return (i, False, None, "connect fail: %s" % last_err)

    # drain banner
    s.settimeout(1.5)
    try:
        s.recv(4096)
    except Exception:
        pass

    # 发送唯一标记，确认 shell 真正响应
    mark = "STRESS_%d" % i
    s.sendall(("echo %s\n" % mark).encode())
    got = b""
    s.settimeout(2.0)
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            got += d
            if mark.encode() in got:
                break
    except Exception:
        pass
    ok = mark.encode() in got

    try:
        s.shutdown(socket.SHUT_WR)
    except Exception:
        pass
    s.close()
    return (i, ok, ct, "" if ok else "resp missing")

results = []
for i in range(1, N + 1):
    r = one_round(i)
    results.append(r)
    print("round %2d: %s  conn=%.3fs  %s" % (r[0], "OK" if r[1] else "FAIL", r[2] if r[2] else 0, r[3]))
    time.sleep(0.8)  # 模拟真实断开后重连间隔

okc = sum(1 for r in results if r[1])
conns = [r[2] for r in results if r[2] is not None]
print("\n=== 汇总 ===")
print("成功率: %d/%d" % (okc, N))
if conns:
    print("连接耗时: min=%.3fs max=%.3fs avg=%.3fs" % (min(conns), max(conns), sum(conns)/len(conns)))
print("[DONE]")
