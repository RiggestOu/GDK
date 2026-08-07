/* netinfo - GDK mini 网络信息 + WiFi 稳定连接 + telnet 启动器
 * 1. 后台启动 connect_wifi.sh（读 wifi.conf 的 ssid/psk，稳定连接 + 关省电 + 看门狗重连 + telnet）
 * 2. SDL 屏幕实时显示 SSID / 状态 / IP / MAC / TELNET 端口
 * 编译: 复用 build_netinfo.sh 工具链, -O0, 链接 -lSDL
 *
 * 2026-08-07 修复记录:
 *  - 闪退: 旧版「任意 SDL_KEYDOWN -> quit」致命（音量键/L2/R2 等系统键持续以 KEYDOWN 上报）。
 *  - START 退不出: GDK mini 的 START/SELECT 实际以键盘事件上报
 *      (START=SDLK_RETURN, SELECT=SDLK_ESCAPE)，而非 JOYBUTTONDOWN button=5。
 *      现改认 RETURN/ESCAPE 退出；并保留「任意手柄 JOYBUTTONDOWN 退出」（手柄键不会自动上报，安全）。
 *  - WiFi 权限: connect_wifi.sh 非 root 时自动 su/sudo 提权（配无线接口 + 监听<1024 端口都需 root）；
 *      并自动探测 WiFi 接口名（不再硬编码 wlan0）。
 *  - telnet 端口: busybox telnetd 不支持 -p 24，改用 utelnetd -p 24（若有）或 telnetd 默认 23；
 *      实际端口写入 status.txt，界面实时显示。
 *  - SSID 中文: 位图字体仅 ASCII，中文 SSID 显示提示串（连接仍用原始 SSID，不受影响）。
 *
 * 2026-08-07 增强记录 (v2): 新增「系统诊断 + 外网连通性」
 *  - 系统诊断: 电池电量% + 充放电状态、WiFi 信号强度(dBm + 等级)、CPU 负载、内存/交换使用、温度。
 *  - 外网连通性: 后台定频 ping 网关 + ping 公网 DNS(8.8.8.8) 取延迟；wget 取公网出口 IP。
 *      外网探测走后台子进程写 /tmp 缓存，主循环只读数，绝不阻塞 800ms 刷新。
 *  - 重要: 位图字体仅 ASCII！原界面中文标签(已连接/退出等)实际画不出(空白)，本版全部改为 ASCII 缩写。
 */
#include <SDL/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include "netinfo_font.h"

static int g_port = 23;
static const char* CONF = "/media/roms/apps/netinfo/wifi.conf";
static const char* STAF = "/media/roms/apps/netinfo/status.txt";
static const char* LOGF = "/media/roms/apps/netinfo/netinfo.log";
static int g_logfd = -1;

/* ---- 诊断日志 ---- */
static char* nowstr(char* b, int n) {
    time_t t = time(NULL); struct tm* tm = localtime(&t);
    snprintf(b, n, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    return b;
}
static void nlog(const char* fmt, ...) {
    if (g_logfd < 0) return;
    char ts[16]; nowstr(ts, sizeof ts);
    char buf[600]; int off = snprintf(buf, sizeof buf, "[%s] ", ts);
    va_list ap; va_start(ap, fmt);
    off += vsnprintf(buf + off, sizeof buf - off, fmt, ap);
    va_end(ap);
    if (off < 0) off = 0;
    if ((size_t)off > sizeof buf) off = sizeof buf;
    write(g_logfd, buf, off);
}
static int tiny_itoa(int v, char* out) {
    if (v == 0) { out[0] = '0'; return 1; }
    char tmp[12]; int k = 0;
    while (v) { tmp[k++] = '0' + (v % 10); v /= 10; }
    int i = 0; while (k) out[i++] = tmp[--k]; return i;
}
static void on_crash(int sig) {
    if (g_logfd >= 0) {
        const char* m1 = "[CRASH] signal "; write(g_logfd, m1, strlen(m1));
        char num[12]; int nl = tiny_itoa(sig, num); write(g_logfd, num, nl);
        const char* m2 = " (see netinfo.log)\n"; write(g_logfd, m2, strlen(m2));
    }
    _exit(2);
}

/* ---- 位图字体绘制 ---- */
static void draw_glyph(SDL_Surface* s, int x0, int y0, char c, Uint32 col) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= FONT_N) return;
    const uint16_t* g = &FONT_BITMAP[idx * FONT_H];
    for (int y = 0; y < FONT_H; y++) {
        uint16_t row = g[y];
        for (int x = 0; x < FONT_W; x++) {
            if (row & (1 << (FONT_W - 1 - x))) {
                SDL_Rect r = { x0 + x, y0 + y, 1, 1 };
                SDL_FillRect(s, &r, col);
            }
        }
    }
}
static void draw_text(SDL_Surface* s, int x, int y, const char* t, Uint32 col) {
    int cx = x;
    for (const char* p = t; *p; p++) {
        if (*p == '\n') { cx = x; y += FONT_H + 2; continue; }
        draw_glyph(s, cx, y, *p, col);
        cx += FONT_W + 1;
    }
}

/* 读网卡 IP/MAC（用 ifconfig 解析，避免 uClibc ioctl 结构差异） */
static int get_if(const char* name, char* ip, char* mac) {
    char cmd[64];
    snprintf(cmd, sizeof cmd, "ifconfig %s 2>/dev/null", name);
    FILE* fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[256];
    *ip = 0; *mac = 0;
    int got_ip = 0, got_mac = 0;
    while (fgets(line, sizeof line, fp)) {
        char* p = strstr(line, "inet addr:");
        if (p) { strcpy(ip, p + 10); ip[strcspn(ip, " \t\r\n")] = 0; got_ip = 1; }
        char* m = strstr(line, "HWaddr");
        if (m) { strcpy(mac, m + 7); mac[strcspn(mac, " \t\r\n")] = 0; got_mac = 1; }
    }
    pclose(fp);
    /* 返回语义: 0=同时有IP+MAC, 1=仅MAC(接口在但还没拿到IP), -1=皆无/读错 */
    if (got_ip && got_mac) return 0;
    if (got_mac) return 1;
    return -1;
}

/* 读 connect_wifi.sh 写入的实际 telnet 端口（默认 23） */
static int read_status_port(void) {
    FILE* f = fopen(STAF, "r");
    if (!f) return 0;
    char line[128]; int port = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncasecmp(line, "PORT=", 5) == 0) { port = atoi(line + 5); break; }
    }
    fclose(f);
    return port;
}

/* 实时检测 23 端口是否真在监听（直接读内核 /proc/net/tcp，不依赖 netstat 格式） */
static int port_listen_23(void) {
    const char* paths[] = { "/proc/net/tcp", "/proc/net/tcp6", NULL };
    for (int i = 0; paths[i]; i++) {
        FILE* fp = fopen(paths[i], "r");
        if (!fp) continue;
        char line[256];
        fgets(line, sizeof line, fp); /* 跳过表头 */
        while (fgets(line, sizeof line, fp)) {
            char local[64] = {0}, st[8] = {0};
            if (sscanf(line, "%*s %63s %*s %7s", local, st) >= 2) {
                if (strstr(local, ":0017") && strcmp(st, "0A") == 0) { fclose(fp); return 1; }
            }
        }
        fclose(fp);
    }
    return 0;
}

/* telnet 端口(23)实时状态: 0=无(未监听) 1=LISTEN(可连接) 2=会话中(ESTABLISHED,无LISTEN)
 * 直接读 /proc/net/tcp, 不依赖 status.txt 缓存, 保证界面颜色为实时真实状态 */
static int port23_state(void) {
    int listen = 0, estab = 0;
    const char* paths[] = { "/proc/net/tcp", "/proc/net/tcp6", NULL };
    for (int i = 0; paths[i]; i++) {
        FILE* fp = fopen(paths[i], "r");
        if (!fp) continue;
        char line[256];
        fgets(line, sizeof line, fp); /* 跳过表头 */
        while (fgets(line, sizeof line, fp)) {
            char local[64] = {0}, st[8] = {0};
            if (sscanf(line, "%*s %63s %*s %7s", local, st) >= 2) {
                if (strstr(local, ":0017")) {
                    if (strcmp(st, "0A") == 0) listen = 1;       /* LISTEN */
                    else if (strcmp(st, "01") == 0) estab = 1;   /* ESTABLISHED */
                }
            }
        }
        fclose(fp);
    }
    if (listen) return 1;
    if (estab) return 2;
    return 0;
}

/* 是否纯 ASCII（位图字体仅支持 ASCII） */
static int is_ascii_str(const char* s) {
    for (; *s; s++) if ((unsigned char)*s > 127) return 0;
    return 1;
}

/* ============ v2 新增: 系统诊断采集 ============ */

/* 电池: GKDmini 固件 capacity 节点放电时常恒=100(驱动 bug, 见 EPUB 阅读器 main.c)。
   改参考 EPUB 阅读器做法, 用 voltage_now 电压区间映射(V_EMPTY=3.40V / V_FULL=4.19V)做主,
   capacity 仅兜底; status 仍用于充放电符号。 */
static int read_battery(int* pct, char* st, int n) {
    *pct = -1; *st = 0;
    static const char *vpaths[] = {
        "/sys/class/power_supply/battery/voltage_now",
        "/sys/class/power_supply/BAT/voltage_now",
        "/sys/class/power_supply/bat/voltage_now",
        "/sys/class/power_supply/jz-battery/voltage_now",
        NULL
    };
    static const char *cpaths[] = {
        "/sys/class/power_supply/battery/capacity",
        "/sys/class/power_supply/BAT/capacity",
        "/sys/class/power_supply/bat/capacity",
        NULL
    };
    /* 单节锂电典型区间(微伏): 空 3.40V / 满 4.19V (与 EPUB 阅读器一致) */
    const long V_EMPTY = 3400000, V_FULL = 4190000;
    long vraw = -1;
    for (int i = 0; vpaths[i]; i++) {
        FILE *f = fopen(vpaths[i], "r");
        if (f) { long mv = -1; if (fscanf(f, "%ld", &mv) == 1) vraw = mv; fclose(f); if (vraw > 0) break; }
    }
    int craw = -1;
    for (int i = 0; cpaths[i]; i++) {
        FILE *f = fopen(cpaths[i], "r");
        if (f) { int v = -1; if (fscanf(f, "%d", &v) == 1) craw = v; fclose(f); if (craw >= 0) break; }
    }
    if (vraw > 1000) {                 /* 真实电压(微伏): 线性映射 */
        long p = (vraw - V_EMPTY) * 100L / (V_FULL - V_EMPTY);
        if (p < 0) p = 0; else if (p > 100) p = 100;
        *pct = (int)p;
    } else if (craw >= 0) {           /* 无电压节点才退化用 capacity */
        *pct = craw;
    }
    FILE* fs = fopen("/sys/class/power_supply/battery/status", "r");
    if (fs) { if (fgets(st, n, fs)) st[strcspn(st, "\r\n")] = 0; fclose(fs); }
    return (*pct >= 0);
}
/* 充放电状态 -> ASCII 标记: +=充电 -=放电 ==满电 ~=未充电 */
static char bat_sign(const char* s) {
    if (!s || !*s) return '?';
    if (strcasecmp(s, "Charging") == 0)     return '+';
    if (strcasecmp(s, "Discharging") == 0)  return '-';
    if (strcasecmp(s, "Full") == 0)         return '=';
    if (strcasecmp(s, "Not charging") == 0) return '~';
    return '?';
}

/* WiFi 信号: 优先 /sys/class/net/<if>/wireless/{level,link} (wext), 回退 /proc/net/wireless */
static int read_rssi(const char* iface, int* dBm, int* linkq) {
    *dBm = 0; *linkq = 0;
    char p[96];
    snprintf(p, sizeof p, "/sys/class/net/%s/wireless/level", iface);
    FILE* f = fopen(p, "r");
    if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1) *dBm = v; fclose(f); }
    snprintf(p, sizeof p, "/sys/class/net/%s/wireless/link", iface);
    f = fopen(p, "r");
    if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1) *linkq = v; fclose(f); }
    if (*dBm != 0 || *linkq != 0) return 1;
    /* 回退 /proc/net/wireless: 找含 iface 的行, 取 link/level 列 */
    f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, iface)) {
                int lk = 0, lv = 0, ns = 0;
                if (sscanf(line, "%*s %*s %d %d %d", &lk, &lv, &ns) >= 2) {
                    *linkq = lk; *dBm = lv; break;
                }
            }
        }
        fclose(f);
    }
    return (*dBm != 0 || *linkq != 0);
}
/* 信号等级 (仅对 dBm<0 有效): G=优 F=良 M=中 W=弱 ?=无 */
static const char* rssi_grade(int dBm) {
    if (dBm == 0) return "?";
    if (dBm >= 0) return "?"; /* 相对值无法分级 */
    if (dBm >= -50) return "G";
    if (dBm >= -60) return "F";
    if (dBm >= -70) return "M";
    return "W";
}

/* CPU 负载: /proc/loadavg 第一字段 (1分钟均值) */
static void read_loadavg(char* out, int n) {
    *out = 0;
    FILE* f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%15s", out) != 1) *out = 0; fclose(f); }
}

/* 内存+交换: /proc/meminfo */
static void read_mem(int* used_mb, int* total_mb, int* swap_used_mb) {
    *used_mb = 0; *total_mb = 0; *swap_used_mb = 0;
    int mt = 0, mf = 0, mb = 0, mc = 0, st = 0, sf = 0;
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char k[32]; int v = 0;
    while (fscanf(f, "%31s %d kB", k, &v) == 2) {
        if      (!strcmp(k, "MemTotal:"))  mt = v;
        else if (!strcmp(k, "MemFree:"))   mf = v;
        else if (!strcmp(k, "Buffers:"))   mb = v;
        else if (!strcmp(k, "Cached:"))    mc = v;
        else if (!strcmp(k, "SwapTotal:")) st = v;
        else if (!strcmp(k, "SwapFree:"))  sf = v;
    }
    fclose(f);
    *total_mb     = mt / 1024;
    *used_mb      = (mt - mf - mb - mc) / 1024;
    *swap_used_mb = (st - sf) / 1024;
    if (*used_mb < 0) *used_mb = 0;
    if (*swap_used_mb < 0) *swap_used_mb = 0;
}

/* 温度: 尽力读 thermal 节点, 读不到返回 0(界面显示 N/A) */
static int read_temp(int* c) {
    const char* paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        "/sys/devices/platform/cpu_thermal/hwmon/hwmon0/temp1_input",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE* f = fopen(paths[i], "r");
        if (f) {
            int t = 0;
            if (fscanf(f, "%d", &t) == 1) {
                fclose(f);
                *c = (t > 1000) ? t / 1000 : t; /* 多数节点给 millidegrees */
                return 1;
            }
            fclose(f);
        }
    }
    return 0;
}

/* 默认网关: /proc/net/route 中 Destination=00000000 的 Gateway (十六进制小端反转) */
static int read_gw(char* gw, int n) {
    *gw = 0;
    FILE* f = fopen("/proc/net/route", "r");
    if (!f) return 0;
    char line[256]; fgets(line, sizeof line, f); /* header */
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char iface[32], dest[16], gwhex[16];
        if (sscanf(line, "%31s %15s %15s", iface, dest, gwhex) >= 3) {
            if (strcmp(dest, "00000000") == 0) {
                int b[4];
                if (sscanf(gwhex, "%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3]) == 4) {
                    snprintf(gw, n, "%d.%d.%d.%d", b[3], b[2], b[1], b[0]);
                    found = 1; break;
                }
            }
        }
    }
    fclose(f);
    return found;
}

/* ============ v2 新增: 外网连通性探测 (后台, 不阻塞主循环) ============ */

/* 定频触发: 后台起 ping 网关 + ping 公网 + wget 公网IP, 结果写 /tmp 缓存。
 * 已有探测在跑则跳过(防并发叠加)。 */
static void kick_netprobe(const char* gw) {
    /* 防重入: 若已有 ping 在跑直接返回 */
    if (system("pgrep -f 'ping -c1' >/dev/null 2>&1") == 0) return;
    char cmd[640];
    if (gw && *gw) {
        snprintf(cmd, sizeof cmd,
            "sh -c 'ping -c1 -W1 %s 2>/dev/null | grep time= >/tmp/pgw 2>&1; "
            "ping -c1 -W2 8.8.8.8 2>/dev/null | grep time= >/tmp/ppub 2>&1; "
            "wget -qT5 -O- http://api.ipify.org 2>/dev/null >/tmp/pubip 2>&1' &",
            gw);
    } else {
        snprintf(cmd, sizeof cmd,
            "sh -c 'ping -c1 -W2 8.8.8.8 2>/dev/null | grep time= >/tmp/ppub 2>&1; "
            "wget -qT5 -O- http://api.ipify.org 2>/dev/null >/tmp/pubip 2>&1' &");
    }
    nlog("kick_netprobe gw=%s\n", gw ? gw : "(none)");
    system(cmd);
}

/* 解析 "time=1.23 ms" 取毫秒 */
static void parse_ping_ms(const char* file, char* out, int n) {
    *out = 0;
    FILE* f = fopen(file, "r");
    if (f) {
        char l[192];
        if (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            char* t = strstr(l, "time=");
            if (t) {
                double ms = 0;
                if (sscanf(t, "time=%lf", &ms) == 1) snprintf(out, n, "%.0fms", ms);
            }
        }
        fclose(f);
    }
    if (!*out) strcpy(out, "-"); /* - = 未测/超时/无应答 */
}

/* 读外网探测缓存 */
static void read_netprobe(char* gw_ms, int gn, char* pub_ms, int pn, char* pub_ip, int in) {
    gw_ms[0] = pub_ms[0] = pub_ip[0] = 0;
    parse_ping_ms("/tmp/pgw",   gw_ms,  gn);
    parse_ping_ms("/tmp/ppub",  pub_ms, pn);
    FILE* f = fopen("/tmp/pubip", "r");
    if (f) {
        char l[64];
        if (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            if (strchr(l, '.') || strchr(l, ':')) {
                strncpy(pub_ip, l, in - 1); pub_ip[in - 1] = 0;
            }
        }
        fclose(f);
    }
    if (!pub_ip[0]) strcpy(pub_ip, "N/A");
}

/* 若 wifi.conf 不存在，自动创建模板（英文，保证位图字体能显示提示） */
static void ensure_config(void) {
    if (access(CONF, F_OK) == 0) return;
    system("mkdir -p /media/roms/apps/netinfo");
    FILE* f = fopen(CONF, "w");
    if (!f) return;
    fputs("# GDK mini WiFi 配置 - 编辑下面 ssid= 和 psk= 后保存\n", f);
    fputs("# 注意：本屏字体仅支持英文，中文 SSID 会在屏幕上显示提示，但连接不受影响\n", f);
    fputs("ssid=YOUR_WIFI_SSID\n", f);
    fputs("psk=YOUR_WIFI_PASSWORD\n", f);
    fclose(f);
    nlog("ensure_config: 已创建模板 %s\n", CONF);
}

/* 读 wifi.conf 的 ssid 字段 */
static void read_ssid(char* out, int n) {
    *out = 0;
    FILE* f = fopen(CONF, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        if (strncasecmp(line, "ssid=", 5) == 0) {
            char* v = line + 5;
            while (*v == ' ' || *v == '\t') v++;
            v[strcspn(v, "\r\n")] = 0;
            strncpy(out, v, n - 1); out[n - 1] = 0;
            break;
        }
    }
    fclose(f);
}

/* 后台启动 connect_wifi.sh（优先固定部署路径，否则按 /proc/self/exe 推算同目录） */
static void start_wifi(void) {
    char fixed[300] = "/media/roms/apps/netinfo/connect_wifi.sh";
    char exe[300];
    if (access(fixed, F_OK) == 0) {
        strcpy(exe, fixed);
    } else {
        strcpy(exe, "./connect_wifi.sh");
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char* s = strrchr(exe, '/');
            if (s) strcpy(s + 1, "connect_wifi.sh");
            else   strcpy(exe, "connect_wifi.sh");
        }
    }
    char cmd[500];
    /* 关键: 不再把输出丢进 /dev/null, 否则脚本静默失败时无从排查。
       stdout+stderr 落盘到 SD 卡 wifi_stderr.txt, 拔卡即可看到真实报错。
       用 setsid 让 connect_wifi 脱离 netinfo 会话(自成 session), 这样退出/重进 netinfo
       都不会连带杀掉 telnet 子进程 —— telnet 在 netinfo 退出后仍常驻, 不再随 app 退出而断。 */
    snprintf(cmd, sizeof cmd,
             "setsid sh \"%s\" >/media/roms/apps/netinfo/wifi_stderr.txt 2>&1 </dev/null &", exe);
    nlog("start_wifi: %s (stderr-> wifi_stderr.txt)\n", exe);
    system(cmd);
}

int main(int argc, char** argv) {
    if (argc > 1) g_port = atoi(argv[1]);

    g_logfd = open(LOGF, O_WRONLY | O_CREAT | O_APPEND, 0644);
    signal(SIGSEGV, on_crash); signal(SIGILL, on_crash);
    signal(SIGABRT, on_crash); signal(SIGFPE, on_crash); signal(SIGBUS, on_crash);
    nlog("=== netinfo v2 start pid=%d default_port=%d ===\n", (int)getpid(), g_port);

    putenv("SDL_VIDEODRIVER=fbcon");
    putenv("SDL_FBDEV=/dev/fb0");
    putenv("SDL_NOMOUSE=1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
        nlog("SDL_Init FAIL: %s\n", SDL_GetError());
        return 1;
    }
    nlog("SDL_Init OK\n");
    SDL_Surface* screen = SDL_SetVideoMode(320, 240, 16, SDL_SWSURFACE);
    if (!screen) { nlog("SetVideoMode FAIL: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    nlog("video mode OK\n");

    start_wifi();   /* 后台稳定连接 WiFi + 启动 telnet */

    char ssid[64]; ensure_config(); read_ssid(ssid, sizeof ssid);

    /* SSID 显示串：位图字体仅 ASCII -> 中文显示 ASCII 提示；ASCII 过长截断到 18 */
    char ssid_disp[64];
    if (!is_ascii_str(ssid))      strcpy(ssid_disp, "[CN-SSID used]");
    else if (!*ssid)              strcpy(ssid_disp, "(no config)");
    else { strncpy(ssid_disp, ssid, sizeof ssid_disp - 1); ssid_disp[sizeof ssid_disp - 1] = 0; }
    if (strlen(ssid_disp) > 18) ssid_disp[18] = 0;

    Uint32 bg    = SDL_MapRGB(screen->format, 18, 28, 48);
    Uint32 white = SDL_MapRGB(screen->format, 255, 255, 255);
    Uint32 cyan  = SDL_MapRGB(screen->format, 120, 230, 255);
    Uint32 yel   = SDL_MapRGB(screen->format, 255, 230, 90);
    Uint32 grn   = SDL_MapRGB(screen->format, 120, 255, 140);
    Uint32 red   = SDL_MapRGB(screen->format, 255, 90, 90);
    char b1[80], b2[80];

    int quit = 0, loops = 0;
    while (!quit) {
        loops++;

        /* ---- 网络接口遍历: 优先有IP的, 否则第一个有MAC的 ---- */
        char ip[64] = "?", mac[64] = "?", iface[32] = "?";
        int connected = 0;
        DIR* nd = opendir("/sys/class/net");
        if (nd) {
            struct dirent* de;
            int mac_only = 0;
            while ((de = readdir(nd))) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                if (strcmp(de->d_name, "lo") == 0) continue;
                char tip[64] = "?", tmac[64] = "?";
                int r = get_if(de->d_name, tip, tmac);
                if (r == 0 && strcmp(tip, "0.0.0.0") != 0) {
                    strcpy(ip, tip); strcpy(mac, tmac); strcpy(iface, de->d_name);
                    connected = 1; break;
                }
                if ((r == 1 || r == 0) && !mac_only) {
                    strcpy(mac, tmac); strcpy(iface, de->d_name); mac_only = 1;
                }
            }
            closedir(nd);
        }

        /* ---- v2 诊断采集 (读文件, 非阻塞) ---- */
        int bat = -1; char bat_st[16] = "";
        read_battery(&bat, bat_st, sizeof bat_st);
        int rssi_dBm = 0, rssi_link = 0;
        read_rssi(strcmp(iface, "?") == 0 ? "wlan0" : iface, &rssi_dBm, &rssi_link);
        char load[16] = "?"; read_loadavg(load, sizeof load);
        int used_mb = 0, total_mb = 0, swap_used = 0;
        read_mem(&used_mb, &total_mb, &swap_used);
        int temp = 0; int have_temp = read_temp(&temp);
        char gw[16] = ""; read_gw(gw, sizeof gw);

        int port = read_status_port(); if (port == 0) port = 23;
        int tstate = port23_state();   /* 实时读 /proc/net/tcp */

        /* ---- 外网探测: 每 6 轮(约4.8s)后台触发一次 ---- */
        if (loops % 6 == 0) kick_netprobe(gw);
        char gw_ms[16] = "-", pub_ms[16] = "-", pub_ip[40] = "N/A";
        read_netprobe(gw_ms, sizeof gw_ms, pub_ms, sizeof pub_ms, pub_ip, sizeof pub_ip);

        /* ---- 拼装显示串 ---- */
        char bat_str[16];
        if (bat >= 0) snprintf(bat_str, sizeof bat_str, "%d%% %c", bat, bat_sign(bat_st));
        else           strcpy(bat_str, "N/A");

        char sig_str[16];
        if (rssi_dBm != 0) snprintf(sig_str, sizeof sig_str, "%d %s", rssi_dBm, rssi_grade(rssi_dBm));
        else                strcpy(sig_str, "N/A");

        char mem_str[16]; snprintf(mem_str, sizeof mem_str, "%d/%dM", used_mb, total_mb);
        char swap_str[8]; snprintf(swap_str, sizeof swap_str, "%dM", swap_used);
        char temp_str[8]; if (have_temp) snprintf(temp_str, sizeof temp_str, "%dC", temp); else strcpy(temp_str, "N/A");

        /* ---- 绘制 (全部 ASCII, 位图字体仅 ASCII) ---- */
        SDL_FillRect(screen, NULL, bg);
        draw_text(screen, 10, 4,   "GDK mini NET INFO", cyan);

        draw_text(screen, 10, 22,  "SSID:", yel); draw_text(screen, 75, 22, ssid_disp, white);
        draw_text(screen, 10, 40,  "IP:",   yel); draw_text(screen, 49, 40, ip, white);
        draw_text(screen, 10, 58,  "MAC:",  yel); draw_text(screen, 62, 58, mac, white);

        draw_text(screen, 10, 76,  "NIC:",  yel); draw_text(screen, 62, 76, iface, white);
        draw_text(screen, 175, 76, "Sig:",  yel); draw_text(screen, 227, 76, sig_str, white);

        draw_text(screen, 10, 94,  "Stat:", yel);
        draw_text(screen, 75, 94, connected ? "ONLINE" : "CONNECTING", connected ? grn : yel);
        draw_text(screen, 175, 94, "Batt:", yel); draw_text(screen, 240, 94, bat_str, white);

        draw_text(screen, 10, 112, "Mem:",  yel); draw_text(screen, 62, 112, mem_str, white);
        draw_text(screen, 175, 112,"CPU:",  yel); draw_text(screen, 227, 112, load, white);

        draw_text(screen, 10, 130, "Swap:", yel); draw_text(screen, 75, 130, swap_str, white);
        draw_text(screen, 175, 130,"Temp:", yel); draw_text(screen, 240, 130, temp_str, white);

        /* TELNET 实时状态行 */
        if (tstate == 1) {
            sprintf(b1, "TELNET: %s:%d", connected ? ip : "???", port);
            draw_text(screen, 10, 148, b1, cyan);       /* 监听中: 青色, 可连接 */
        } else if (tstate == 2) {
            sprintf(b1, "TELNET: %s:%d LINKED", connected ? ip : "???", port);
            draw_text(screen, 10, 148, b1, grn);        /* 会话中: 绿色, 已被占用 */
        } else {
            draw_text(screen, 10, 148, "TELNET: DOWN (see wifi.log)", red);
        }

        draw_text(screen, 10, 166, "Gate:", yel); draw_text(screen, 75, 166, gw_ms, white);
        draw_text(screen, 175, 166,"Net:",  yel); draw_text(screen, 227, 166, pub_ms, white);

        draw_text(screen, 10, 184, "PubIP:", yel); draw_text(screen, 88, 184, pub_ip, white);

        draw_text(screen, 10, 202, "START/SELECT exit", yel);
        SDL_Flip(screen);

        if (loops % 5 == 1) nlog("loop %d ip=%s mac=%s gw=%s telnet=%s bat=%s sig=%d load=%s\n",
                                 loops, ip, mac, gw[0]?gw:"-",
                                 tstate==1?"LISTEN":(tstate==2?"ESTAB":"DOWN"),
                                 bat_str, rssi_dBm, load);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { nlog("quit by SDL_QUIT\n"); quit = 1; }
            /* 任意手柄按钮 -> 退出（手柄键不会自动上报，安全） */
            else if (e.type == SDL_JOYBUTTONDOWN) {
                nlog("quit by JOYBUTTON(%d)\n", e.jbutton.button); quit = 1;
            }
            /* GDK mini 的 START/SELECT 以键盘事件上报：START=RETURN, SELECT=ESCAPE */
            else if (e.type == SDL_KEYDOWN) {
                int sym = e.key.keysym.sym;
                if (sym == SDLK_RETURN || sym == SDLK_ESCAPE) {
                    nlog("quit by key sym=%d (START/SELECT)\n", sym); quit = 1;
                }
                /* 其他键盘事件（音量键/L2/R2 等系统键）一律忽略，避免误退出 */
            }
        }
        SDL_Delay(800);
    }
    nlog("normal quit after %d loops\n", loops);
    SDL_Quit();
    return 0;
}
