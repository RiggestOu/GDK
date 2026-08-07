#!/bin/sh
# connect_wifi.sh - GDK mini 稳定 WiFi 连接 + 看门狗 + telnet(端口24)
# 由 netinfo 后台调用；读取 /media/roms/apps/netinfo/wifi.conf 的 ssid/psk
set +e

CONF="/media/roms/apps/netinfo/wifi.conf"
WPA="/tmp/wpa_supplicant.conf"
LOG="/media/roms/apps/netinfo/wifi.log"
PORT=24

log() { echo "[$(date +%H:%M:%S)] $*" >> "$LOG"; }

mkdir -p "$(dirname "$CONF")" "$(dirname "$LOG")" /var/run/wpa_supplicant

SSID=$(grep -i '^ssid=' "$CONF" 2>/dev/null | head -1 | cut -d= -f2- | sed 's/^[ \t]*//;s/[ \t]*$//')
PSK=$(grep -i '^psk='  "$CONF" 2>/dev/null | head -1 | cut -d= -f2- | sed 's/^[ \t]*//;s/[ \t]*$//')

log "=== connect_wifi start ==="
log "SSID=[$SSID] PSK_LEN=${#PSK}"

if [ -z "$SSID" ]; then
    log "ERROR: wifi.conf 缺少 ssid，无法连接"
    # 仍启动 telnet 便于本地/USB 调试
    killall telnetd 2>/dev/null
    telnetd -p $PORT -l /bin/sh 2>>"$LOG" &
    exit 1
fi

# 生成 wpa_supplicant 配置
cat > "$WPA" <<EOF
ctrl_interface=/var/run/wpa_supplicant
update_config=1
network={
    ssid="$SSID"
    psk="$PSK"
    key_mgmt=WPA-PSK
    scan_ssid=1
}
EOF
log "wrote $WPA"

# 先停掉系统可能已有的网络管理，避免冲突
killall wpa_supplicant 2>/dev/null
killall udhcpc 2>/dev/null
ifconfig wlan0 down 2>/dev/null
sleep 1
ifconfig wlan0 up 2>/dev/null

# 关键：关闭省电（避免连上后很快掉线）
iw dev wlan0 set power_save off 2>/dev/null
iwconfig wlan0 power off 2>/dev/null

# 启动 wpa_supplicant（nl80211 优先，回退 wext）
wpa_supplicant -i wlan0 -c "$WPA" -B -D nl80211,wext 2>>"$LOG"
log "wpa_supplicant started, wait auth..."
sleep 4

# 再次关闭省电（保险）
iw dev wlan0 set power_save off 2>/dev/null
iwconfig wlan0 power off 2>/dev/null

# 获取 IP
udhcpc -i wlan0 -n -q 2>>"$LOG" || udhcpc -i wlan0 -b 2>>"$LOG"
sleep 2

# 启动 telnet（端口 24），供 PC 直连
killall telnetd 2>/dev/null
telnetd -p $PORT -l /bin/sh 2>>"$LOG" &
log "telnet started on port $PORT"

# ===== 看门狗：每 15 秒检查，掉线自动重连 =====
while true; do
    sleep 15
    IP=$(ifconfig wlan0 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
    if [ -z "$IP" ]; then
        log "WATCHDOG: 无 IP，重连..."
        killall wpa_supplicant 2>/dev/null
        ifconfig wlan0 down 2>/dev/null
        sleep 1
        ifconfig wlan0 up 2>/dev/null
        iw dev wlan0 set power_save off 2>/dev/null
        wpa_supplicant -i wlan0 -c "$WPA" -B -D nl80211,wext 2>>"$LOG"
        sleep 4
        udhcpc -i wlan0 -b 2>>"$LOG"
    fi
    # 持续关闭省电（防止驱动重置）
    iw dev wlan0 set power_save off 2>/dev/null
    iwconfig wlan0 power off 2>/dev/null
done
