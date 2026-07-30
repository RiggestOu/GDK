#!/bin/sh
# GKD mini WiFi 手动连接脚本 v2
# 用法：把此脚本 + wifi.conf 放到 SD 卡根目录（/media/roms/）
# 在设备上执行: sh /media/roms/wifi_connect.sh
# v2 修正：上次日志显示 "wlan0: No such device" → 本版先做硬件诊断，
#          自动加载 WiFi 驱动模块，并从 /sys/class/net 探测真实接口名。

LOG="/media/roms/wifi_log.txt"
CONF="/media/roms/wifi.conf"

echo "=== WiFi 连接脚本 v2 $(date) ===" > "$LOG"

# ---------- 0. 硬件诊断（无论后面成败，先把关键信息写进日志） ----------
echo "--- [诊断] 所有网络接口 (/sys/class/net) ---" >> "$LOG"
ls /sys/class/net >> "$LOG" 2>&1
echo "--- [诊断] ifconfig -a ---" >> "$LOG"
ifconfig -a >> "$LOG" 2>&1
echo "--- [诊断] 已加载内核模块 ---" >> "$LOG"
lsmod >> "$LOG" 2>&1
echo "--- [诊断] 可用内核模块 (/lib/modules) ---" >> "$LOG"
find /lib/modules -name '*.ko' 2>/dev/null >> "$LOG"
echo "--- [诊断] rfkill 状态 ---" >> "$LOG"
rfkill list >> "$LOG" 2>&1
for f in /sys/class/rfkill/rfkill*/state; do
    [ -f "$f" ] && echo "$f = $(cat $f)" >> "$LOG"
done
echo "--- [诊断结束] ---" >> "$LOG"

# ---------- 1. 配置检查 ----------
if [ ! -f "$CONF" ]; then
    echo "[ERROR] 找不到 $CONF，请先编辑 wifi.conf 填入 SSID 和密码" >> "$LOG"
    exit 1
fi
SSID=$(grep '^SSID=' "$CONF" | cut -d= -f2)
PSK=$(grep '^PSK=' "$CONF" | cut -d= -f2)
if [ -z "$SSID" ] || [ -z "$PSK" ]; then
    echo "[ERROR] wifi.conf 中 SSID 或 PSK 为空" >> "$LOG"
    exit 1
fi
echo "目标 SSID: $SSID" >> "$LOG"

# ---------- 2. 解除 rfkill 软屏蔽（若有） ----------
rfkill unblock wifi 2>/dev/null
rfkill unblock all 2>/dev/null
for f in /sys/class/rfkill/rfkill*/soft; do
    [ -f "$f" ] && echo 0 > "$f" 2>/dev/null
done

# ---------- 3. 探测无线接口；没有则尝试加载驱动 ----------
find_iface() {
    for n in /sys/class/net/*; do
        i=$(basename "$n")
        [ -d "$n/wireless" ] && { echo "$i"; return; }
        case "$i" in wlan*|mlan*|ra0|ath0) echo "$i"; return;; esac
    done
}
IFACE=$(find_iface)
if [ -z "$IFACE" ]; then
    echo "未发现无线接口，尝试加载 WiFi 驱动模块..." >> "$LOG"
    for m in $(find /lib/modules -name '*.ko' 2>/dev/null); do
        base=$(basename "$m" .ko)
        case "$base" in
            *8189*|*8723*|*8188*|*bcm*|*brcm*|*esp*|*ssv*|*atbm*|*aic*|*mt76*|*rtl*|*wlan*|*wifi*|*cfg80211*|*mac80211*)
                echo "  insmod $base" >> "$LOG"
                modprobe "$base" 2>>"$LOG" || insmod "$m" 2>>"$LOG"
                ;;
        esac
    done
    sleep 3
    IFACE=$(find_iface)
fi
if [ -z "$IFACE" ]; then
    echo "[ERROR] 仍未发现无线接口。请把本日志发回分析（重点看上方[诊断]段）" >> "$LOG"
    exit 1
fi
echo "无线接口: $IFACE" >> "$LOG"

# ---------- 4. 清残留、拉起接口 ----------
killall wpa_supplicant 2>/dev/null
killall udhcpc 2>/dev/null
sleep 1
ifconfig "$IFACE" up 2>>"$LOG"
sleep 2

# ---------- 5. 扫描（确认能看到目标 AP，结果写日志） ----------
echo "--- 扫描附近 AP ---" >> "$LOG"
iwlist "$IFACE" scan 2>/dev/null | grep -i 'essid\|signal' | head -20 >> "$LOG"

# ---------- 6. wpa_supplicant ----------
WPACONF=/tmp/wpa_gkd.conf
cat > "$WPACONF" << EOF
ctrl_interface=/var/run/wpa_supplicant
update_config=1
network={
    ssid="$SSID"
    psk="$PSK"
    key_mgmt=WPA-PSK
    scan_ssid=1
    priority=5
}
EOF
echo "wpa_supplicant.conf 已生成" >> "$LOG"
wpa_supplicant -B -i"$IFACE" -c"$WPACONF" 2>>"$LOG"
sleep 3

echo "开始关联..." >> "$LOG"
for i in $(seq 1 20); do
    STATUS=$(wpa_cli -i "$IFACE" status 2>/dev/null | grep wpa_state | cut -d= -f2)
    echo "[$i] wpa_state=$STATUS" >> "$LOG"
    [ "$STATUS" = "COMPLETED" ] && { echo "关联成功！" >> "$LOG"; break; }
    sleep 1
done

# ---------- 7. DHCP ----------
echo "请求 DHCP..." >> "$LOG"
udhcpc -i "$IFACE" -n -q -t 10 2>>"$LOG"
sleep 2

# ---------- 8. 结果 ----------
IP=$(ifconfig "$IFACE" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
GW=$(route -n 2>/dev/null | grep "$IFACE" | grep UG | awk '{print $2}')
echo "" >> "$LOG"
echo "=== 最终状态 ===" >> "$LOG"
echo "IP 地址: ${IP:-未获取}   网关: ${GW:-无}" >> "$LOG"
wpa_cli -i "$IFACE" status >> "$LOG" 2>/dev/null
if [ -n "$IP" ]; then
    echo "WiFi 连接成功！IP=$IP  （可用 telnet $IP 登录设备）" >> "$LOG"
    ping -c 2 -W 3 223.5.5.5 >>"$LOG" 2>&1 && echo "外网连通 OK" >> "$LOG" || echo "外网不通" >> "$LOG"
fi
echo "=== 完成 ===" >> "$LOG"
