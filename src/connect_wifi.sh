#!/bin/sh
# connect_wifi.sh v5 - 完全非破坏性（绝不破坏系统自带连接）
# 核心原则:
#   1. 系统本就能连 wlan0(WPA/WPA2) -> 我们被动等待系统自己连, 绝不抢接口
#   2. 仅当系统确实没连、且无任何 wpa 在管接口、用户又填了配置时, 才温和辅助连接
#   3. 任何情况下都只起 telnet 服务(不影响 WiFi), 实际端口写入 status.txt
# 关键修复(对比 v4): 去掉"启动即检测→未连就强连"的竞态; 改成先等待系统连接;
#   不再反复 insmod/重启 wpa; 检测到系统已有 wpa_supplicant 在管理时绝不二次接管。
set +e

CONF="/media/roms/apps/netinfo/wifi.conf"
WPA="/tmp/wpa_supplicant.conf"
LOG="/media/roms/apps/netinfo/wifi.log"
STATUS="/media/roms/apps/netinfo/status.txt"
DIR="/media/roms/apps/netinfo"

mkdir -p "$DIR" /var/run/wpa_supplicant 2>/dev/null

# ---- 日志可写性自检 + /tmp 兜底 ----
# 教训: 上一版用 `rm -f "$LOG"` 想清掉 PC 侧损坏的 FAT 项, 结果 SD 上该项删不掉也建不了,
#       导致所有 log 写入静默失败, wifi.log 一字节没有, 完全无法排查。
#       现改为: 只尝试截断, 写不进就自动降级到 /tmp, 保证任何情况下都有日志。
: >> "$LOG" 2>/dev/null
if ! echo "probe" >> "$LOG" 2>/dev/null; then
    LOG="/tmp/wifi.log"          # SD 不可写 -> 降级
    LOG_FALLBACK=1
    : > "$LOG" 2>/dev/null
else
    : > "$LOG" 2>/dev/null       # 可写: 清空本次会话
    LOG_FALLBACK=0
fi
log() { echo "[$(date +%H:%M:%S)] $*" >> "$LOG" 2>/dev/null; }

log "==== connect_wifi.sh 启动 @ $(date '+%Y-%m-%d %H:%M:%S') ===="
[ "$LOG_FALLBACK" = "1" ] && log "!! SD 卡日志不可写, 已降级到 /tmp/wifi.log (稍后会尝试回拷 SD)"

self="$0"
case "$self" in /*) ;; *) self="$(pwd)/$self" ;; esac
log "self=$self  pid=$$  ppid=$PPID  uid=$(id -u)"

# ---- 单实例: 杀掉旧的看门狗, 避免多次进入 netinfo 累积进程 ----
# 注意: 必须排除自己($$)和父进程($PPID)。提权时父进程是 `su root -c "sh 本脚本"`,
#       其命令行同样含 connect_wifi.sh, 误杀父进程会把自己一起带走(静默死亡)。
for p in $(pgrep -f "[c]onnect_wifi.sh" 2>/dev/null); do
    [ "$p" = "$$" ] && continue
    [ "$p" = "$PPID" ] && continue
    kill "$p" 2>/dev/null
done

# ---- 提权 ----
if [ "$(id -u)" != "0" ]; then
    log "提权: uid=$(id -u)"
    if command -v sudo >/dev/null 2>&1; then exec sudo sh "$self"; fi
    exec su root -c "sh '$self'"
fi
log "权限: uid=$(id -u)"

# ---- 探测第一个非lo无线接口 ----
detect_iface() {
    IFACE=""
    for d in /sys/class/net/*; do
        [ -e "$d" ] || continue
        n=$(basename "$d")
        case "$n" in lo|eth*|usb*|ppp*|bnep*|tun*) continue;; esac
        IFACE="$n"; break
    done
    [ -z "$IFACE" ] && IFACE=wlan0
}

# ---- 找任意非lo接口已获取的IP ----
first_ip() {
    for d in /sys/class/net/*; do
        n=$(basename "$d"); [ "$n" = "lo" ] && continue
        ip=$(ifconfig "$n" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
        [ -n "$ip" ] && [ "$ip" != "0.0.0.0" ] && { echo "$n:$ip"; return; }
    done
    echo ""
}

# ---- telnet 服务（端口 23; 后台化启动 + 启动后精确验证端口是否真监听） ----
port23_listen() {
    grep -qE ':0017 ' /proc/net/tcp 2>/dev/null && return 0
    grep -qE ':0017 ' /proc/net/tcp6 2>/dev/null && return 0
    netstat -tln 2>/dev/null | grep -qE ':23[[:space:]]' && return 0
    return 1
}
# nc 循环保活: 断开后自动重启监听, 支持反复连接
# (解决 busybox nc -l 单连接即退、导致只能连一次的问题)
start_nc_loop() {
    log "  启动 nc 循环保活(支持反复连接)"
    killall nc 2>/dev/null; sleep 1
    # 方式A: 支持 -k (断开保持监听)
    nc -l -k -p 23 -e /bin/sh >/dev/null 2>&1 &
    local pid=$!; sleep 2
    if port23_listen; then log "  => nc -k 监听成功(pid=$pid)"; return 0; fi
    kill $pid 2>/dev/null
    # 方式B: 不支持 -k, 用 while 循环不断重启 nc; 每次 nc 返回=一次连接断开, 记录时间戳与时长
    ( c=0
      while true; do
        t0=$(date +%s)
        nc -l -p 23 -e /bin/sh >/dev/null 2>&1
        t1=$(date +%s); dur=$((t1 - t0)); c=$((c + 1))
        if [ "$dur" -ge 2 ]; then
          log "  [telnet] 会话#$c 断开 @ $(date '+%m-%d %H:%M:%S') 持续${dur}s -> 1s后重新监听(可重连)"
        else
          log "  [telnet] nc退出#$c @ $(date '+%H:%M:%S') 仅${dur}s(空闲/端口占用?) -> 重试"
        fi
        sleep 1
      done ) >/dev/null 2>&1 &
    sleep 2
    if port23_listen; then log "  => nc 循环监听成功(断开会记录时间到本log)"; return 0; fi
    # 方式C: nc 无 -e, 用 mkfifo 管道接 sh; 同样记录每次断开
    FIFO=/tmp/tnfifo; rm -f "$FIFO"; mkfifo "$FIFO" 2>/dev/null
    ( c=0
      while true; do
        t0=$(date +%s)
        sh </"$FIFO" | nc -l -p 23 >"$FIFO"
        t1=$(date +%s); dur=$((t1 - t0)); c=$((c + 1))
        log "  [telnet-fifo] 会话#$c 断开 @ $(date '+%m-%d %H:%M:%S') 持续${dur}s -> 重新监听"
        sleep 1
      done ) >/dev/null 2>&1 &
    sleep 2
    if port23_listen; then log "  => nc+fifo 循环监听成功(断开会记录时间到本log)"; return 0; fi
    return 1
}
# ---- dump esoteric 图标注册相关文件到 SD 卡(绕开 WiFi 不稳, 便于拔卡排查) ----
dump_esoteric() {
    DUMP="/media/roms/apps/netinfo/esoteric_dump.txt"
    H="${HOME:-/usr/local/home}"
    log ">>> 开始 dump esoteric 配置 -> $DUMP (HOME=$H)"
    : > "$DUMP" 2>/dev/null || { log "DUMP 不可写: $DUMP"; return 1; }
    {
      echo "=== \$HOME=$H ==="
      echo "=== esoteric 目录结构 ==="
      ls -la "$H/.esoteric/" 2>&1
      echo "=== ls sections/applications ==="
      ls -la "$H/.esoteric/sections/applications/" 2>&1
      for f in epubreader netinfo 10_terminal 25_gmu 40_o2xiv; do
        echo "=== [$f] ==="
        cat "$H/.esoteric/sections/applications/$f" 2>&1
      done
      echo "=== grep icon= all sections ==="
      grep -r "icon=" "$H/.esoteric/sections/applications/" 2>&1
      echo "=== cache/images 结构(能显图标的第三方app都在此) ==="
      ls -la "$H/.esoteric/cache/images/" 2>&1
      find "$H/.esoteric/cache/images/" -maxdepth 2 -type f 2>&1 | head -40
      echo "=== netinfo 缓存图标是否已生成 ==="
      ls -la "$H/.esoteric/cache/images/netinfo/" 2>&1
      echo "=== esoteric log.txt (可能含图标加载报错) ==="
      tail -60 "$H/.esoteric/log.txt" 2>&1
      echo "=== 当前皮肤 icons 目录 ==="
      ls -la "$H/.esoteric/skins/"*/icons/ 2>&1 | head -30
      echo "=== EPUBReader 文件夹 ==="
      ls -la /media/roms/apps/EPUBReader/ 2>&1
      echo "=== epubreader icon file ==="
      ls -la /media/roms/apps/EPUBReader/epubreader_icon.png 2>&1
      echo "=== icon png header ==="
      od -An -tx1 -N 8 /media/roms/apps/EPUBReader/epubreader_icon.png 2>&1
      echo "=== esoteric.conf ==="
      cat "$H/.esoteric/esoteric.conf" 2>&1
      echo "=== 系统所有 .desktop 里 Icon 字段样例 ==="
      grep -rh "Icon=" /media/roms/apps/*/default.gcw0.desktop 2>/dev/null | head -20
    } > "$DUMP" 2>&1
    sync
    SZ=$(wc -c < "$DUMP" 2>/dev/null)
    log "已 dump esoteric 图标信息 -> $DUMP (${SZ} 字节)"
}

# ---- 完整系统信息 dump 到 SD 卡(绕开 WiFi/telnet 不稳, 拔卡即可拿全部信息) ----
dump_sysinfo() {
    SYS="/media/roms/apps/netinfo/sysinfo_dump.txt"
    log ">>> 开始 dump 系统信息 -> $SYS"
    : > "$SYS" 2>/dev/null || { log "SYSINFO 不可写: $SYS"; return 1; }
    {
      echo "########## GDK mini 系统信息 dump @ $(date '+%Y-%m-%d %H:%M:%S') ##########"
      echo
      echo "===== [1] 内核 / 系统 ====="
      echo "--- uname -a ---";        uname -a 2>&1
      echo "--- /proc/version ---";   cat /proc/version 2>&1
      echo "--- /etc/os-release ---"; cat /etc/os-release 2>&1
      echo "--- /etc/issue ---";      cat /etc/issue 2>&1
      echo "--- 开机时长 uptime ---"; uptime 2>&1; cat /proc/uptime 2>&1
      echo
      echo "===== [2] CPU ====="
      echo "--- /proc/cpuinfo ---";   cat /proc/cpuinfo 2>&1
      echo "--- 当前频率 ---";        cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null
      echo "--- 可用频率 ---";        cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies 2>/dev/null
      echo "--- 调频策略 ---";        cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
      echo
      echo "===== [3] 内存 ====="
      echo "--- free ---";            free 2>&1
      echo "--- /proc/meminfo ---";   cat /proc/meminfo 2>&1
      echo
      echo "===== [4] 存储 / 挂载 ====="
      echo "--- df -h ---";           df -h 2>&1
      echo "--- mount ---";           mount 2>&1
      echo "--- /proc/mounts ---";    cat /proc/mounts 2>&1
      echo "--- /proc/partitions ---";cat /proc/partitions 2>&1
      echo
      echo "===== [5] 网络 ====="
      echo "--- ifconfig -a ---";     ifconfig -a 2>&1
      echo "--- ip addr ---";         ip addr 2>/dev/null
      echo "--- route -n ---";        route -n 2>&1
      echo "--- /sys/class/net ---";  ls -l /sys/class/net/ 2>&1
      echo "--- iwconfig ---";        iwconfig 2>&1
      echo "--- resolv.conf ---";     cat /etc/resolv.conf 2>&1
      echo "--- wpa 进程 ---";        pgrep -af wpa_supplicant 2>&1
      echo "--- /proc/net/tcp(监听端口) ---"; cat /proc/net/tcp 2>&1 | head -40
      echo "--- lsusb ---";           lsusb 2>&1
      echo
      echo "===== [6] 内核模块 ====="
      echo "--- lsmod ---";           lsmod 2>&1
      echo "--- wifi 驱动目录 ---";   find /lib/modules -iname '*.ko' 2>/dev/null | grep -iE 'wifi|wireless|rtl|8188|8192|8189|8723|cfg80211|mac80211' 2>&1
      echo
      echo "===== [7] 共享库 (SDL 等) ====="
      echo "--- /usr/lib SDL/相关 ---"; ls -l /usr/lib/ 2>/dev/null | grep -iE 'sdl|png|jpeg|freetype|webp|z\.so|ttf|font' 2>&1
      echo "--- ldconfig -p (关键库) ---"; ldconfig -p 2>/dev/null | grep -iE 'sdl|png|jpeg|freetype|webp' 2>&1
      echo
      echo "===== [8] 字体 ====="
      echo "--- /usr/share/fonts ---"; ls -lR /usr/share/fonts/ 2>/dev/null | head -40
      echo
      echo "===== [9] 进程 / 环境 ====="
      echo "--- ps ---";              ps 2>&1 | head -60
      echo "--- env ---";             env 2>&1
      echo "--- id ---";              id 2>&1
      echo "--- busybox applets ---"; busybox --list 2>/dev/null | tr '\n' ' '
      echo
      echo "===== [10] 关键目录结构 ====="
      echo "--- /media/roms ---";     ls -la /media/roms/ 2>&1
      echo "--- /media/roms/apps ---";ls -la /media/roms/apps/ 2>&1
      echo "--- /usr/local/home ---"; ls -la /usr/local/home/ 2>&1
      echo "--- HOME=${HOME:-/usr/local/home} ---"; ls -la "${HOME:-/usr/local/home}" 2>&1
      echo "--- /usr/bin (前80) ---"; ls /usr/bin/ 2>&1 | head -80
      echo
      echo "########## dump 结束 ##########"
    } >> "$SYS" 2>&1
    sync
    SZ=$(wc -c < "$SYS" 2>/dev/null)
    log "已 dump 系统信息 -> $SYS (${SZ} 字节)"
}
start_telnet() {
    PORT=23
    if port23_listen; then
        log "23 端口已有人监听, 复用(不重启)"
        echo "PORT=$PORT" >> "$STATUS"
        echo "TELNET=already" >> "$STATUS"
        return 0
    fi
    log "启动 telnet 服务(后台化+精确验证)..."
    log "DIAG telnetd: $(command -v telnetd 2>&1)"
    log "DIAG nc: $(command -v nc 2>&1) / busybox nc: $(busybox --list 2>/dev/null | grep -ix nc | tr '\n' ' ')"
    log "DIAG /dev/pts: $(ls /dev/pts 2>&1 | head -2)"

    # busybox telnetd 常需 /dev/pts 已挂载(已挂则报 busy, 忽略)
    mount -t devpts devpts /dev/pts 2>>"$LOG"; sleep 1

    # 核心修复: 每种方式都"后台启动 + sleep 2 + 检测端口", 绝不前台阻塞
    try_telnetd() {
        log "  尝试 telnetd: $*"
        killall telnetd 2>/dev/null; sleep 1
        telnetd "$@" >/dev/null 2>&1 &
        local pid=$!; sleep 2
        if port23_listen; then log "  => telnetd 监听成功(pid=$pid)"; return 0; fi
        kill $pid 2>/dev/null; return 1
    }
    write_ok() { echo "PORT=$PORT" >> "$STATUS"; echo "TELNET=ok" >> "$STATUS"; }

    # 1) telnetd 系列(若本机 busybox telnetd 能 standalone 起来最理想)
    try_telnetd -l /bin/sh && { write_ok; return 0; }
    try_telnetd -l /bin/login && { write_ok; return 0; }
    try_telnetd && { write_ok; return 0; }
    # 2) nc 循环保活(最可靠; 断开后自动重启监听, 支持反复连接)
    start_nc_loop && { write_ok; return 0; }

    log "ERROR: 所有方式均未能在23端口监听"
    echo "PORT=0" >> "$STATUS"
    echo "TELNET=fail" >> "$STATUS"
    return 1
}

log "=== connect_wifi v5 启动(非破坏性) ==="
log "DIAG ifaces: $(ls /sys/class/net/ 2>/dev/null | tr '\n' ' ')"
log "DIAG wpa 进程: $(pgrep -af wpa_supplicant 2>/dev/null | tr '\n' ' ')"
# 无条件先 dump 一次(不依赖任何分支, 确保一定产出 esoteric_dump.txt)
dump_esoteric
# 无条件 dump 完整系统信息(核心: 不依赖 WiFi/telnet, 拔卡即可拿到全部系统信息)
dump_sysinfo

# 若日志降级到了 /tmp, 立刻回拷一份到 SD 卡, 保证拔卡能看到
sync_log_to_sd() {
    if [ "$LOG_FALLBACK" = "1" ] && [ -f "$LOG" ]; then
        cp "$LOG" /media/roms/apps/netinfo/wifi_tmp.log 2>/dev/null && sync
    fi
}
sync_log_to_sd

# ===== 阶段1: 被动等待系统自带连接(最多18秒), 期间绝不碰WiFi =====
log "阶段1: 等待系统自带连接(最多18s)..."
SYS=""
i=0
while [ $i -lt 18 ]; do
    SYS=$(first_ip)
    [ -n "$SYS" ] && break
    sleep 1; i=$((i+1))
done
if [ -n "$SYS" ]; then
    if=$(echo "$SYS" | cut -d: -f1); ip=$(echo "$SYS" | cut -d: -f2)
    log "系统已连接: $SYS —— 复用, 仅起telnet, 不碰WiFi"
    {
      echo "SSID=$(grep -i '^ssid=' "$CONF" 2>/dev/null | head -1 | cut -d= -f2-)"
      echo "IFACE=$if"
      echo "PERM=root"
    } > "$STATUS"
    start_telnet
    dump_esoteric
    log "v5 完成(系统模式), 进入静默监控"
    while true; do sleep 60; done
    exit 0
fi

# ===== 阶段2: 系统没连上. 检查是否有 wpa_supplicant 在跑(系统可能在慢连) =====
if pgrep -f wpa_supplicant >/dev/null 2>&1; then
    log "阶段2: 检测到 wpa_supplicant 在运行(系统管理中), 继续等待30s..."
    i=0
    while [ $i -lt 30 ]; do
        SYS=$(first_ip); [ -n "$SYS" ] && break
        sleep 1; i=$((i+1))
    done
    if [ -n "$SYS" ]; then
        if=$(echo "$SYS" | cut -d: -f1); ip=$(echo "$SYS" | cut -d: -f2)
        log "系统最终连上: $SYS"
        { echo "SSID=$(grep -i '^ssid=' "$CONF" 2>/dev/null | head -1 | cut -d= -f2-)"; echo "IFACE=$if"; echo "PERM=root"; } > "$STATUS"
        start_telnet
        dump_esoteric
        log "v5 完成(系统模式2), 进入静默监控"
        while true; do sleep 60; done
        exit 0
    fi
    log "系统wpa在跑但仍未连上(可能密码/信号问题), 不强行接管"
fi

# ===== 阶段3: 系统没连且无wpa: 用用户配置温和尝试(不强制驱动, udhcpc重试) =====
SSID=$(grep -i '^ssid=' "$CONF" 2>/dev/null | head -1 | cut -d= -f2- | sed 's/^[ \t]*//;s/[ \t]*$//')
PSK=$(grep -i '^psk='  "$CONF" 2>/dev/null | head -1 | cut -d= -f2- | sed 's/^[ \t]*//;s/[ \t]*$//')
if [ -z "$SSID" ] || [ "$SSID" = "YOUR_WIFI_SSID" ]; then
    log "无有效配置, 仅起telnet便于本地调试"
    { echo "SSID="; echo "IFACE="; echo "PERM=root"; } > "$STATUS"
    start_telnet
    dump_esoteric
    exit 1
fi
log "阶段3: 用 wifi.conf 连接 SSID=[$SSID] PSK_LEN=${#PSK}"
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
detect_iface
log "使用接口: $IFACE"
# 仅当接口确实不存在时才一次性加载驱动(已加载的ko反复insmod会把接口搞掉)
if [ ! -d "/sys/class/net/$IFACE" ]; then
    log "接口缺失, 一次性加载驱动"
    for ko in $(find /lib/modules /usr/lib/modules -type f -name '*.ko' 2>/dev/null | grep -iE '8188|8192|rtl|rtw|8189|8723|mt7601|mt76|ath|brcm'); do
        insmod "$ko" >>"$LOG" 2>&1 && log "  insmod OK: $ko" || log "  insmod 跳过(可能已加载): $ko"
    done
    sleep 3; detect_iface
    log "加载后接口: $(ls /sys/class/net/ 2>/dev/null | tr '\n' ' ')"
fi
{
  echo "SSID=$SSID"
  echo "IFACE=$IFACE"
  echo "PERM=root"
} > "$STATUS"
# 若系统已有 wpa 在管本接口, 绝不二次接管
if pgrep -f "wpa_supplicant.*$IFACE" >/dev/null 2>&1; then
    log "wpa 已在管 $IFACE, 复用"
else
    killall wpa_supplicant 2>/dev/null; sleep 1
    wpa_supplicant -i "$IFACE" -c "$WPA" -B 2>>"$LOG"
    log "wpa_supplicant started (未指定 -D, 自动探测驱动)"
fi
iw dev "$IFACE" set power_save off 2>/dev/null
iwconfig "$IFACE" power off 2>/dev/null
log "udhcpc -i $IFACE (等待关联+获取IP, 最多重试30)"
udhcpc -i "$IFACE" -t 30 -n 2>>"$LOG"
start_telnet
dump_esoteric
# telnet 起来后再 dump 一次系统信息(此时 /proc/net/tcp 含最终监听状态)
dump_sysinfo
sync_log_to_sd
log "=== 初始化完成, 进入看门狗 (telnet状态见上; 系统信息见 sysinfo_dump.txt) ==="

# ===== 看门狗: 无IP时按"系统优先"原则处理, 绝不盲目重启wpa =====
while true; do
    sleep 30
    ip=$(ifconfig "$IFACE" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
    if [ -z "$ip" ] || [ "$ip" = "0.0.0.0" ]; then
        # 系统 wpa 在跑 -> 等它, 不抢
        if pgrep -f wpa_supplicant >/dev/null 2>&1; then
            log "看门狗: 无IP但wpa在跑(系统管理), 等待"; continue
        fi
        # 接口消失 -> 仅此情况才加载驱动
        if [ ! -d "/sys/class/net/$IFACE" ]; then
            log "看门狗: 接口消失, 加载驱动"
            for ko in $(find /lib/modules /usr/lib/modules -type f -name '*.ko' 2>/dev/null | grep -iE '8188|8192|rtl|rtw|8189|8723|mt7601|mt76|ath|brcm'); do
                insmod "$ko" >>"$LOG" 2>&1
            done
            sleep 3; detect_iface
        fi
        # 仍是我们的接口且无人管 -> 温和重连一次
        if ! pgrep -f "wpa_supplicant.*$IFACE" >/dev/null 2>&1; then
            killall wpa_supplicant 2>/dev/null; sleep 1
            wpa_supplicant -i "$IFACE" -c "$WPA" -B 2>>"$LOG"
        fi
        udhcpc -i "$IFACE" -t 30 -n 2>>"$LOG"
    fi
    iw dev "$IFACE" set power_save off 2>/dev/null
    iwconfig "$IFACE" power off 2>/dev/null
done
