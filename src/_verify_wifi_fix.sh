#!/bin/sh
# 验证 connect_wifi.sh v7 新增判据的逻辑正确性（用设备实测数据做样本）

echo "=== 1. /sys/class/net/*/flags 十六进制位运算 (IFF_UP=bit0) ==="
for fl in 0x1003 0x1002 0x11043 0x1000; do
    up=$(( fl & 1 ))
    if [ "$up" = "1" ]; then st=UP; else st=DOWN; fi
    echo "  flags=$fl -> IFF_UP=$up ($st)"
done

echo
echo "=== 2. iface_assoc 关联判据 ==="
assoc_test() {
    txt="$1"
    echo "$txt" | grep -q 'no wireless extensions' && { echo "  => 非无线接口(视为已连) [return 0]"; return 0; }
    echo "$txt" | grep -qiE 'Not-Associated|unassociated' && { echo "  => 未关联 [return 1]"; return 1; }
    echo "$txt" | grep -qiE 'Access Point: *([0-9A-Fa-f]{2}:){5}' && { echo "  => 已关联 [return 0]"; return 0; }
    echo "  => 未知, 判未关联 [return 1]"; return 1
}

echo "[A] 设备当前实测输出(未关联):"
assoc_test 'wlan0     unassociated  Nickname:"<WIFI@REALTEK>"
          Mode:Managed  Frequency=2.437 GHz  Access Point: Not-Associated
          Link Quality:0  Signal level:0  Noise level:0'
echo "    期望: 未关联"

echo "[B] 关联成功时的输出:"
assoc_test 'wlan0     IEEE 802.11bgn  ESSID:"ptlordvoldemortou"
          Mode:Managed  Frequency:2.437 GHz  Access Point: 3C:CD:5D:A1:B2:C3
          Link Quality=52/70  Signal level=-58 dBm'
echo "    期望: 已关联"

echo "[C] 有线接口:"
assoc_test 'eth0      no wireless extensions.'
echo "    期望: 非无线(视为已连)"

echo
echo "=== 3. iface_is_up 回退正则(解析 ifconfig flags 行) ==="
check_up() {
    if echo "$1" | grep -qE '(^|[[:space:]])UP([[:space:]]|,|$)'; then
        echo "  匹配UP=YES  <- [$1]"
    else
        echo "  匹配UP=NO   <- [$1]"
    fi
}
check_up "          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1"
check_up "          BROADCAST MULTICAST  MTU:1500  Metric:1"
echo "    期望: 第一行 YES(已UP), 第二行 NO(设备当前状态,DOWN)"

echo
echo "=== 4. /proc/net/tcp 23端口 LISTEN 判据 ==="
TCP1=/tmp/_tcp_listen
TCP2=/tmp/_tcp_timewait
{
  echo '  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode'
  echo '   2: 00000000:0017 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 2311 1 8227cde0 100 0 0 10 0'
} > "$TCP1"
{
  echo '  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode'
  echo '   2: 0100007F:C350 0100007F:0017 06 00000000:00000000 00:00000000 00000000     0        0 0 1 0 100 0 0 10 0'
} > "$TCP2"

echo "[A] 真实 LISTEN 场景:"
if awk 'NR>1 && $4=="0A" && $2 ~ /:0017$/ {found=1} END{exit !found}' "$TCP1"; then
    echo "  新版 port23_now 检出 LISTEN: YES (正确)"
else
    echo "  新版 port23_now 检出 LISTEN: NO  (错误!)"
fi
awk 'NR>1 && ($4=="0A" || $4=="01") && $2 ~ /:0017$/ {print "  新版 kill_port_23 命中 inode=" $10 " (正确, 能杀掉孤儿nc)"}' "$TCP1"
awk 'NR>1 && $4=="01" && $2 ~ /:0017$/ {print "  旧版 kill_port_23 命中 inode=" $10}' "$TCP1" | grep -q . \
    && echo "  旧版也命中" || echo "  旧版 kill_port_23 命中: 无 (这就是孤儿nc杀不掉的bug)"

echo "[B] 仅有 TIME_WAIT 出站连接(远端端口23), 无监听:"
if awk 'NR>1 && $4=="0A" && $2 ~ /:0017$/ {found=1} END{exit !found}' "$TCP2"; then
    echo "  新版判定有监听: YES (误报!)"
else
    echo "  新版判定有监听: NO  (正确)"
fi
if grep -qE ':0017 ' "$TCP2"; then
    echo "  旧版判定有监听: YES (误报! 旧版会因此 exit 0 导致WiFi不跑)"
else
    echo "  旧版判定有监听: NO"
fi

rm -f "$TCP1" "$TCP2"
echo
echo "=== 验证结束 ==="
