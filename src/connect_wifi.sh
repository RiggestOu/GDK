#!/bin/sh
# connect_wifi.sh v7 - 修复 v6.1「端口去重自杀」反噬(5 处修复见下方注释)
# v6 关键修复:
#   1. 强单实例(pidfile+锁目录) -> 根除"多次进出 netinfo 累积双 connect_wifi 抢端口"
#   2. 用 kill_port_23 端口精确清理 替代 危险的 `killall nc`(多实例时互杀对方监听)
#   3. nc 0s 失败退避3s, 不再空转刷日志+占CPU
# v6.1 加固: 端口级去重(最可靠双保险) - 23 端口已有人监听即视为已有实例, 直接退出,
#   彻底堵死"锁/pidfile 竞态窗口里第二个实例抢到端口"的幽灵监管问题。
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

# ---- 端口级探测(v7 修正) ----
# v6.1 的致命 bug: 只要 23 端口有任何条目就 exit 0, 导致 WiFi 连接流程一步都不跑。
#   而孤儿 nc(上次实例已死、nc 仍占着 23 LISTEN)是常态 -> 新实例永远自杀 -> WiFi 永不重连。
# v7: 端口占用只作为"telnet 无需重启"的参考, 绝不终止脚本; 是否重复实例交给下面的
#   pidfile+锁目录判断(那个才可靠)。同时判据收紧为 local_address 且状态 0A(LISTEN)。
port23_now() {
    for f in /proc/net/tcp /proc/net/tcp6; do
        [ -r "$f" ] || continue
        awk 'NR>1 && $4=="0A" && $2 ~ /:0017$/ {found=1} END{exit !found}' "$f" 2>/dev/null && return 0
    done
    return 1
}
if port23_now; then
    log "23 端口当前有 LISTEN(可能是上次残留的孤儿 nc), 稍后由 start_telnet 精确清理接管; 本实例继续跑 WiFi 流程"
fi

# ---- 强单实例: pidfile + 锁目录, 根除"多个 netinfo 累积 connect_wifi 抢端口" ----
# v5 旧逻辑: pgrep 排除 PPID 导致 su 提权场景下旧实例被杀不掉 -> 双实例 ->
#   两个实例各自的 `killall nc` 互杀对方监听 -> 端口23被反复抢占 -> 用户 telnet 会话频断。
PIDF=/var/run/connect_wifi.pid
LOCKD=/var/run/connect_wifi.lock
# 原子锁: 拿不到锁且持有者存活 -> 直接退出(绝不起第二份)
if ! mkdir "$LOCKD" >/dev/null 2>&1; then
    if [ -f "$PIDF" ]; then
        OPID=$(cat "$PIDF" 2>/dev/null)
        if [ -n "$OPID" ]; then
            if kill -0 "$OPID" >/dev/null 2>&1; then
                log "已有实例 pid=$OPID 存活, 本实例退出(避免双实例抢端口)"
                exit 0
            fi
        fi
    fi
    rm -rf "$LOCKD" >/dev/null 2>&1
    mkdir "$LOCKD" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log "锁争用, 退出"
        exit 0
    fi
fi
# 杀掉真正存在的旧实例(连同它的 nc 子进程), 避免残留监听争用
if [ -f "$PIDF" ]; then
    OPID=$(cat "$PIDF" 2>/dev/null)
    if [ -n "$OPID" ]; then
        if kill -0 "$OPID" >/dev/null 2>&1; then
            log "清理旧实例 pid=$OPID (含其 nc 子进程)"
            pkill -P "$OPID" >/dev/null 2>&1
            kill "$OPID" >/dev/null 2>&1
            sleep 1
        fi
    fi
fi
echo $$ > "$PIDF"
cleanup() {
    rm -f "$PIDF" >/dev/null 2>&1
    rmdir "$LOCKD" >/dev/null 2>&1
}
trap cleanup EXIT

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

# ---- v7 新增: 接口是否 UP(内核 flags 里有 UP 位) ----
# 现象: rtl8188fu 这类 Realtek 树外驱动加载后接口是 DOWN 的(<BROADCAST,MULTICAST>,
#   既无 UP 也无 LOWER_UP), wpa_supplicant 不会自动 up 它 -> 扫描/关联全部失败。
#   旧脚本全程没有任何 ifconfig up, 这是 WiFi 连不上的根本原因之一。
iface_is_up() {
    [ -n "$1" ] || return 1
    # 首选: 读 /sys/class/net/<if>/flags 的 IFF_UP 位(bit0), 最可靠且不依赖 grep 扩展
    if [ -r "/sys/class/net/$1/flags" ]; then
        fl=$(cat "/sys/class/net/$1/flags" 2>/dev/null)
        if [ -n "$fl" ]; then
            up=$(( fl & 1 )) 2>/dev/null
            [ "$up" = "1" ] && return 0
            [ "$up" = "0" ] && return 1
        fi
    fi
    # 回退: 解析 ifconfig 的 flags 行(busybox grep 不保证支持 \b, 用 POSIX 字符类)
    ifconfig "$1" 2>/dev/null | grep -qE '(^|[[:space:]])UP([[:space:]]|,|$)'
}
iface_up() {
    [ -n "$1" ] || return 1
    iface_is_up "$1" && return 0
    log "  接口 $1 处于 DOWN, 执行 ifconfig $1 up"
    ifconfig "$1" up 2>>"$LOG"
    ip link set "$1" up 2>/dev/null
    i=0
    while [ $i -lt 8 ]; do
        iface_is_up "$1" && { log "  接口 $1 已 UP"; return 0; }
        sleep 1; i=$((i+1))
    done
    log "  !! 接口 $1 up 失败(8s 超时)"
    return 1
}

# ---- v7 新增: 无线接口是否真的关联上 AP ----
iface_assoc() {
    [ -n "$1" ] || return 1
    # 非无线接口(无 iwconfig 信息)直接视为"已连"(有线场景)
    iwconfig "$1" 2>/dev/null | grep -q 'no wireless extensions' && return 0
    iwconfig "$1" 2>/dev/null | grep -qiE 'Not-Associated|unassociated' && return 1
    iwconfig "$1" 2>/dev/null | grep -qiE 'Access Point: *([0-9A-Fa-f]{2}:){5}' && return 0
    return 1
}

# ---- v7 新增: 清掉未关联时残留的幽灵 IP ----
# 现象: 上次 udhcpc 配上 192.168.0.45 后接口 down/掉联, IP 仍挂在接口上。
#   旧 first_ip 只看 IP -> 误判"系统已连接" -> 直接进 sleep 60 死循环, 永不重连。
clear_ghost_ip() {
    [ -n "$1" ] || return 0
    gip=$(ifconfig "$1" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
    if [ -n "$gip" ] && [ "$gip" != "0.0.0.0" ]; then
        log "  清除未关联状态下的残留 IP $gip @ $1"
        ifconfig "$1" 0.0.0.0 2>/dev/null
    fi
}

# ---- 找任意非lo接口"真正可用"的IP (v7 加严) ----
# 旧版只要接口上挂着 IP 就算连上 -> 被残留 IP 欺骗。
# 新版三条同时满足才算: 接口 UP + 已关联 AP + 有非 0.0.0.0 的 IP。
first_ip() {
    for d in /sys/class/net/*; do
        [ -e "$d" ] || continue
        n=$(basename "$d"); [ "$n" = "lo" ] && continue
        ip=$(ifconfig "$n" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
        [ -n "$ip" ] && [ "$ip" != "0.0.0.0" ] || continue
        iface_is_up "$n" || continue
        iface_assoc "$n" || continue
        echo "$n:$ip"; return
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
# 精确清理: 只杀占用 23 端口 LISTEN 的进程(通过 /proc/net/tcp 反查 inode->pid)
# 替代危险的 `killall nc`(多实例时会互杀对方监听) 与无差别 pkill
kill_port_23() {
    for f in /proc/net/tcp /proc/net/tcp6; do
        [ -r "$f" ] || continue
        # v7 修正: $4 是 st 状态列, LISTEN=0A(旧代码写成 01=ESTABLISHED, 永远杀不掉监听者
        #   -> 孤儿 nc 一直占着 23 端口)。这里 0A 与 01 都清, 彻底释放端口。
        awk 'NR>1 && ($4=="0A" || $4=="01") && $2 ~ /:0017$/ {print $10}' "$f" 2>/dev/null
    done | while read ino; do
        [ -n "$ino" ] || continue
        for ff in /proc/[0-9]*/fd/*; do
            tgt=$(readlink "$ff" 2>/dev/null)
            if [ "$tgt" = "socket:[$ino]" ]; then
                pid=$(echo "$ff" | cut -d/ -f3)
                [ -n "$pid" ] && [ "$pid" != "$$" ] && kill "$pid" 2>/dev/null
            fi
        done
    done
}
# nc 循环保活: 断开后自动重启监听, 支持反复连接
# (解决 busybox nc -l 单连接即退、导致只能连一次的问题)
start_nc_loop() {
    log "  启动 nc 循环保活(支持反复连接, 端口精确清理非 killall nc)"
    kill_port_23; sleep 1
    # 方式A: 支持 -k (断开保持监听)
    nc -l -k -p 23 -e /bin/sh >/dev/null 2>&1 &
    local pid=$!; sleep 2
    if port23_listen; then log "  => nc -k 监听成功(pid=$pid)"; return 0; fi
    kill $pid 2>/dev/null
    # 方式B: 不支持 -k, 用 while 循环不断重启 nc; 每次 nc 返回=一次连接断开
    ( c=0
      while true; do
        kill_port_23; sleep 1          # 仅清占用23端口的残留, 不再无差别 killall nc
        t0=$(date +%s)
        nc -l -p 23 -e /bin/sh >/dev/null 2>&1
        t1=$(date +%s); dur=$((t1 - t0)); c=$((c + 1))
        if [ "$dur" -ge 2 ]; then
          log "  [telnet] 会话#$c 断开 @ $(date '+%m-%d %H:%M:%S') 持续${dur}s -> 重新监听(可重连)"
        else
          # 0s 退出多为端口被占(双实例时已根除); 退避3s避免空转刷日志+占CPU
          log "  [telnet] nc退出#$c @ $(date '+%H:%M:%S') 仅${dur}s(端口占用?) -> 退避3s重试"
        fi
        sleep 3
      done ) >/dev/null 2>&1 &
    sleep 2
    if port23_listen; then log "  => nc 循环监听成功(断开会记录时间到本log)"; return 0; fi
    # 方式C: nc 无 -e, 用 mkfifo 管道接 sh; 同样记录每次断开
    FIFO=/tmp/tnfifo; rm -f "$FIFO"; mkfifo "$FIFO" 2>/dev/null
    ( c=0
      while true; do
        kill_port_23; sleep 1
        t0=$(date +%s)
        sh </"$FIFO" | nc -l -p 23 >"$FIFO"
        t1=$(date +%s); dur=$((t1 - t0)); c=$((c + 1))
        log "  [telnet-fifo] 会话#$c 断开 @ $(date '+%m-%d %H:%M:%S') 持续${dur}s -> 重新监听"
        sleep 3
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
    log "启动 telnet 服务(端口23, 强单实例保活)"
    # telnetd 在本机不可用(devpts busy / 三次尝试均失败), 直接走最可靠的 nc 循环
    write_ok() { echo "PORT=$PORT" >> "$STATUS"; echo "TELNET=ok" >> "$STATUS"; }
    kill_port_23; sleep 1
    start_nc_loop && { write_ok; return 0; }
    log "ERROR: nc 循环未能在23端口监听"
    echo "PORT=0" >> "$STATUS"
    echo "TELNET=fail" >> "$STATUS"
    return 1
}

log "=== connect_wifi v7 启动(修复: 端口自杀/孤儿nc/接口未UP/幽灵IP/无默认路由) ==="
log "DIAG ifaces: $(ls /sys/class/net/ 2>/dev/null | tr '\n' ' ')"
log "DIAG wpa 进程: $(pgrep -f wpa_supplicant 2>/dev/null | tr '\n' ' ')"
for _n in $(ls /sys/class/net/ 2>/dev/null); do
    [ "$_n" = "lo" ] && continue
    log "DIAG $_n: UP=$(iface_is_up "$_n" && echo yes || echo no) 关联=$(iface_assoc "$_n" && echo yes || echo no) IP=$(ifconfig "$_n" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)"
done
log "DIAG lsusb: $(lsusb 2>/dev/null | tr '\n' ' ')"
log "DIAG lsmod: $(lsmod 2>/dev/null | tail -n +2 | awk '{print $1}' | tr '\n' ' ')"
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
# ---- v7 关键: 关联前必须先把接口 UP, 并清掉未关联时残留的幽灵 IP ----
log "接口预处理: up + 清残留IP"
iface_up "$IFACE"
if ! iface_assoc "$IFACE"; then
    clear_ghost_ip "$IFACE"
fi
log "  接口状态: UP=$(iface_is_up "$IFACE" && echo yes || echo no)  已关联=$(iface_assoc "$IFACE" && echo yes || echo no)"

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

# ---- v7: 等待真正关联上 AP 再 DHCP(未关联就 DHCP 只会白等 30 次) ----
log "等待关联 AP (最多 25s)..."
i=0
while [ $i -lt 25 ]; do
    if iface_assoc "$IFACE"; then
        log "  已关联: $(iwconfig "$IFACE" 2>/dev/null | grep -o 'Access Point:.*' | head -1)"
        break
    fi
    # 关联过程中接口可能被驱动重置回 DOWN, 每 5s 兜底 up 一次
    [ $((i % 5)) -eq 4 ] && iface_up "$IFACE"
    sleep 1; i=$((i+1))
done
if ! iface_assoc "$IFACE"; then
    log "  !! 25s 内未关联成功。诊断:"
    log "     iwconfig: $(iwconfig "$IFACE" 2>&1 | tr '\n' ' ' | cut -c1-300)"
    log "     扫描到的AP: $(iwlist "$IFACE" scan 2>/dev/null | grep -c ESSID) 个"
    log "     本机SSID[$SSID] 是否可见: $(iwlist "$IFACE" scan 2>/dev/null | grep -F "\"$SSID\"" >/dev/null 2>&1 && echo YES || echo NO)"
    log "     wpa进程: $(pgrep -f wpa_supplicant 2>/dev/null | tr '\n' ' ')"
fi

log "udhcpc -i $IFACE (获取IP, 最多重试30)"
udhcpc -i "$IFACE" -t 30 -n 2>>"$LOG"

# ---- v7: DHCP 后校验 IP 与默认路由(旧版 route -n 为空, 即使有IP也出不去) ----
GOTIP=$(ifconfig "$IFACE" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
log "DHCP 结果: IP=${GOTIP:-无}"
if [ -n "$GOTIP" ] && [ "$GOTIP" != "0.0.0.0" ]; then
    if ! route -n 2>/dev/null | grep -qE '^0\.0\.0\.0'; then
        GW=$(echo "$GOTIP" | awk -F. '{print $1"."$2"."$3".1"}')
        log "  无默认路由, 补一条: route add default gw $GW dev $IFACE"
        route add default gw "$GW" dev "$IFACE" 2>>"$LOG"
    fi
    log "  路由表: $(route -n 2>/dev/null | tail -n +3 | tr '\n' ' | ')"
    echo "IP=$GOTIP" >> "$STATUS"
else
    log "  !! 未取得 IP"
    echo "IP=" >> "$STATUS"
fi

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
    # v7: 光有 IP 不算连上(可能是掉联后残留的幽灵 IP)。接口 DOWN 或未关联同样视为断网。
    if ! iface_is_up "$IFACE" || ! iface_assoc "$IFACE"; then
        log "看门狗: 接口异常(UP=$(iface_is_up "$IFACE" && echo yes || echo no) 关联=$(iface_assoc "$IFACE" && echo yes || echo no)), 判定为断网"
        clear_ghost_ip "$IFACE"
        iface_up "$IFACE"
        ip=""
    fi
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
        iface_up "$IFACE"
        if ! pgrep -f "wpa_supplicant.*$IFACE" >/dev/null 2>&1; then
            killall wpa_supplicant 2>/dev/null; sleep 1
            wpa_supplicant -i "$IFACE" -c "$WPA" -B 2>>"$LOG"
        fi
        # v7: 等关联再 DHCP
        j=0
        while [ $j -lt 20 ]; do
            iface_assoc "$IFACE" && break
            [ $((j % 5)) -eq 4 ] && iface_up "$IFACE"
            sleep 1; j=$((j+1))
        done
        udhcpc -i "$IFACE" -t 30 -n 2>>"$LOG"
        NIP=$(ifconfig "$IFACE" 2>/dev/null | grep 'inet addr' | awk '{print $2}' | cut -d: -f2)
        if [ -n "$NIP" ] && [ "$NIP" != "0.0.0.0" ]; then
            route -n 2>/dev/null | grep -qE '^0\.0\.0\.0' || \
                route add default gw "$(echo "$NIP" | awk -F. '{print $1"."$2"."$3".1"}')" dev "$IFACE" 2>/dev/null
            log "看门狗: 重连成功 IP=$NIP"
        fi
        # v7: 断线重连后 telnet 监听常已随接口失效, 确保重新监听
        port23_now || { log "看门狗: 23端口无监听, 重启 telnet"; start_telnet; }
    fi
    iw dev "$IFACE" set power_save off 2>/dev/null
    iwconfig "$IFACE" power off 2>/dev/null
done
