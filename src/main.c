#include "render.h"
#include "epub.h"
#include "zip.h"
#include "util.h"
#include "layout.h"
#include "imgdec.h"   /* img_decode 原图按需解码 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>   /* access()：自定义快捷键导出/导入检测文件 */
#include <fcntl.h>    /* evdev 直读：open O_NONBLOCK */
#include <sys/ioctl.h>   /* evdev 直读：ioctl EVIOCGNAME */
#include <linux/input.h> /* evdev 直读：struct input_event / EVIOCGNAME */

/* ================= 配置与配色表 ================= */
typedef struct { int font_index; int fg_index; int bright_pct; } cfg_t;

static const int   font_sizes[4] = {12, 14, 18, 22};
static const char *font_labels[4] = {"小", "中", "大", "特大"};
static const int   fg_colors[5][3] = {{235,235,235},{255,225,120},{150,240,150},{150,220,255},{255,160,200}};
static const char *fg_labels[5] = {"白", "黄", "绿", "青", "粉"};
static const int   brights[5] = {30, 50, 70, 90, 100};
static const char *bright_labels[5] = {"30%", "50%", "70%", "90%", "100%"};

/* ================= 动作 ================= */
enum Action { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_SELECT, A_BACK, A_MENU, A_BOOKMARK, A_PIC, A_QUIT_FORCE, A_VOL, A_START, A_TOC, A_BINDCAP };

/* GDK mini 音量键（2026-07-29 真机 log 实测修正）：
   音量键以孤立 SDL_KEYDOWN 上报：sym=270(SDLK_KP_PLUS)=音量+，sym=269(SDLK_KP_MINUS)=音量-。
   旧值 78/74 是错误假设（实机从未出现），导致音量功能全失效。
   注意：sym=280/281(PAGEUP/PAGEDOWN) 是 L2/R2 的键盘侧伴随事件，不是音量键！ */
#define KEY_VOLUME_UP    270   /* SDLK_KP_PLUS */
#define KEY_VOLUME_DOWN  269   /* SDLK_KP_MINUS */
#define KEY_107         107
#define KEY_MENU        102
/* 音量键调亮度步进（由 event_to_action 写入，主循环读取）：正=增、负=减 */
static int g_bright_delta = 0;

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    return strcasecmp(s + ls - lf, suf) == 0;
}

/* GDK mini 物理键→事件映射（2026-07-29 KeyTest 权威定案）：
   手柄键 A/B/X/Y/L1/R1/START/SELECT → SDL_JOYBUTTON(b1/b0/b2/b3/b6/b7/b5/b4)
   L2/R2 → 键盘 SDL_KEYDOWN sym=280/281（无 JOYBUTTON）
   音量+/− → 键盘 sym=270/269   圆3/圆4 → 键盘 sym=279/278
   圆1/圆2 = 与 A/B 完全相同的硬件复制键（事件不可区分） */
static enum Action key_to_action(int key) {
    switch (key) {
        case SDLK_UP:    case SDLK_w: return A_UP;
        case SDLK_DOWN:  case SDLK_s: return A_DOWN;
        case SDLK_LEFT:  case SDLK_a: return A_LEFT;
        case SDLK_RIGHT: case SDLK_d: return A_RIGHT;
        case SDLK_LCTRL:                          return A_SELECT;   /* A 键 */
        case SDLK_RETURN:                         return A_NONE;     /* START 按下不触发；息屏在「抬起且无组合」时触发（见 event_to_action 键盘抬起分支），避免与 L1+START 组合冲突/双触发 */
        case SDLK_LALT:  case SDLK_q:             return A_BACK;     /* B 键 */
        case SDLK_ESCAPE:                         return A_BACK;     /* SELECT */
        case SDLK_LSHIFT: case SDLK_m:            return A_MENU;     /* X 键 */
        case SDLK_SPACE:  case SDLK_k:            return A_BOOKMARK; /* Y 键 */
        case SDLK_TAB:      case SDLK_PAGEUP:     return A_NONE;     /* L1/L2 单按无操作（专用于图片功能，避免与 L1+方向 组合冲突） */
        case SDLK_BACKSPACE:case SDLK_PAGEDOWN:   return A_NONE;     /* R1/R2 单按无操作 */
        default: return A_NONE;
    }
}

/* 调试：把收到的 SDL 事件打到 stderr（.dge 已重定向到 run.log），便于无串口头诊断按键映射 */
static void dbg_event(const SDL_Event *ev) {
    /* sc=scancode：L1/START/SELECT 在实机上 sym=0，只能靠 scancode 区分（2026-07-29 log 实证） */
    if (ev->type == SDL_KEYDOWN)
        fprintf(stderr, "[dbg] KEYDOWN sym=%d sc=%d\n", (int)ev->key.keysym.sym, (int)ev->key.keysym.scancode);
    else if (ev->type == SDL_KEYUP)
        fprintf(stderr, "[dbg] KEYUP sym=%d sc=%d\n", (int)ev->key.keysym.sym, (int)ev->key.keysym.scancode);
    else if (ev->type == SDL_JOYBUTTONDOWN)
        fprintf(stderr, "[dbg] JOYBUTTON button=%d\n", ev->jbutton.button);
    else if (ev->type == SDL_JOYBUTTONUP)
        fprintf(stderr, "[dbg] JOYBUTTONUP button=%d\n", ev->jbutton.button);
    else if (ev->type == SDL_JOYHATMOTION)
        fprintf(stderr, "[dbg] JOYHAT value=%d\n", ev->jhat.value);
    else
        fprintf(stderr, "[dbg] event type=%d\n", ev->type);
}

/* ================= 输入模型（2026-07-29 KeyTest 定案，v1.4 起） =================
   实机 KeyTest 实测：全部 20 个物理键 SDL 均能正确上报——
     A/B/X/Y/L1/R1/START/SELECT → SDL_JOYBUTTON(b1/b0/b2/b3/b6/b7/b5/b4)
     L2/R2 → 纯键盘 SDL_KEYDOWN sym=280/281（无 JOYBUTTON）
     音量+/− → 纯键盘 sym=270/269
     圆3/圆4 → 纯键盘 sym=279/278
   故无需 evdev 直读；evdev 合成事件反而会与 JOYBUTTON 形成双触发（一次按压翻两页/双息屏），
   整段 evdev_init/evdev_poll 已于 v1.4 删除。所有键统一走 SDL 事件 + 跨源合并掩码 g_mod。 */

static int wait_event_timeout(SDL_Event *ev, Uint32 timeout_ms) {
    Uint32 start = SDL_GetTicks();
    for (;;) {
        if (SDL_PollEvent(ev)) {
            /* 肩键/START/SELECT 的 SDL 键盘事件是 sym=0 sc=0 的无信息空事件，
               真实信息在 JOYBUTTON 里，这类空事件直接丢弃避免干扰。 */
            int drop = (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) &&
                       ev->key.keysym.sym == 0 && ev->key.keysym.scancode == 0;
            if (drop) continue;
            dbg_event(ev);
            return 1;
        }
        if ((SDL_GetTicks() - start) >= timeout_ms) return 0;
        SDL_Delay(10);
    }
}

/* 2026-07-29 KeyTest 实测 JOYBTN 索引（SDL_JOYBUTTON button 值）。
   设备上报顺序与标准 GCW0 不同，且 A/B 互换（物理 A=1、物理 B=0）。 */
#define BTN_B 0        /* 物理 B 键 / 圆2（硬件复制不可区分）：返回/退出 */
#define BTN_A 1        /* 物理 A 键 / 圆1（硬件复制不可区分）：确认/下一页 */
#define BTN_X 2        /* X 键：书签 */
#define BTN_Y 3        /* Y 键：菜单 */
#define BTN_SELECT 4   /* SELECT 键：返回/退出 */
#define BTN_START 5    /* START 键：息屏 */
#define BTN_L1 6       /* L1 肩键：下一页 / 图片缩放组合 */
#define BTN_R1 7       /* R1 肩键：下一页 / 图片缩放组合 */
/* 注意：L2/R2 是纯键盘事件(sym=280/281)，没有对应 JOYBTN，不定义 BTN_L2/BTN_R2 */

/* 记录当前按下的手柄键（位掩码），用于"A/B/X/Y 单按"判定 */
static int g_jbtn = 0;

/* 统一修饰键状态：键盘来源与手柄来源任一置位均生效。
   设备实机：A/B/L/R/START 以 SDL_JOYBUTTON 上报，音量键以 SDL_KEYDOWN(sym 78/74) 上报；
   旧代码键盘(g_mod)/手柄(g_jbtn)两套状态各自独立，导致跨源组合键
   （例如 L1手柄 + 音量键盘、L1手柄 + START手柄）全部失效，且单独 START 落到 default->A_NONE。
   故合并为单一 g_mod，组合键判定跨源生效。 */
#define M_L1     (1 << 0)
#define M_L2     (1 << 1)
#define M_R1     (1 << 2)
#define M_R2     (1 << 3)
#define M_START  (1 << 4)
#define MODMASK  (M_L1 | M_L2 | M_R1 | M_R2 | M_START)   /* 全部可作组合修饰的位 */
/* ⚠️ 实机双事件（2026-07-29 log 实证）：按一次 L2/R2，设备会同时上报
   JOYBUTTON(button=6/7) 和 KEYDOWN(sym=280/281) 两个事件。若两源共用一个掩码，
   两次"抬起"会各触发一次翻页（一按翻两页）。故拆成键盘/手柄两个掩码，
   读取时合并（g_mod 宏），抬起翻页仅在"该源确实持有该位 && 合并后归零"时触发一次。 */
static int g_mod_kb = 0, g_mod_joy = 0;
#define g_mod (g_mod_kb | g_mod_joy)
/* 本轮肩键按住期间是否产生过组合动作；若没有，则肩键单按抬起=翻页 */
static int g_shoulder_combo = 0;

/* ================= 自定义快捷键 =================
 * 逻辑动作 → 物理键 绑定表，可被用户修改并保存。
 * 默认绑定与现有操作一致；用户在「自定义快捷键」界面可改，程序自动检测冲突。 */
#define KIND_ACTION 0   /* 普通动作绑定 */
#define KIND_BRIGHT 1   /* 亮度步进绑定：g_bind_mod=需按住的修饰位，g_bind_step=步进值 */
typedef enum { BIND_NEXT, BIND_PREV, BIND_OPEN, BIND_BACK, BIND_MENU, BIND_BM, BIND_SUSPEND,
               BIND_PIC, BIND_QUITAPP,
               BIND_BRIGHT_BIG, BIND_BRIGHT_MED, BIND_BRIGHT_SMALL,
               BIND_PIC_UP, BIND_PIC_DOWN, BIND_PIC_LEFT, BIND_PIC_RIGHT,
               BIND_TOC, NUM_BIND } bind_t;
#define BIND_SLOTS 4   /* 每个功能最多 4 套绑定（用户要求：如 下一页=L1/R1 都行） */
static int g_bind_src[NUM_BIND][BIND_SLOTS];      /* 0=键盘sym 1=手柄button 2=纯修饰键组合(code=修饰位掩码) */
static int g_bind_code[NUM_BIND][BIND_SLOTS];
static int g_bind_mod[NUM_BIND][BIND_SLOTS];     /* 组合键修饰位掩码(M_L1|M_L2|M_R1|M_R2|M_START)，0=单键 */
static int g_bind_kind[NUM_BIND];     /* KIND_ACTION / KIND_BRIGHT（每功能一个，全槽共用） */
static int g_bind_step[NUM_BIND];     /* 每功能一个：亮度=步进值；图片缩放=缩放步进(符号区分 ±%) */
static char g_bind_path[256] = {0};   /* 导入时记录的默认打开路径 */
static char g_default_dir[256] = "/media/sdcard/Ebook";  /* 默认打开目录（永久固化） */
static const char *bind_names[NUM_BIND] = {"下一页","上一页","打开/确认","返回/退出","菜单","书签","息屏","图片缩放","退出App",
                                           "亮度+50%","亮度+20%","亮度+5%","放大1%","缩小1%","缩小10%","放大10%","目录"};
static int g_last_src = 0, g_last_code = 0;  /* 最近一次物理键（捕获绑定用） */
static int g_last_mod = 0;                    /* 最近一次物理键按下瞬间的修饰键状态（组合键捕获用） */
static int g_capture = 0;                     /* 捕获绑定态：修饰键组合按下也要冒泡到主循环 */
/* 设置某功能的第 s 套绑定（s 越界忽略） */
static void bind_set(int i, int s, int src, int code, int mod) {
    if (s < 0 || s >= BIND_SLOTS) return;
    g_bind_src[i][s] = src; g_bind_code[i][s] = code; g_bind_mod[i][s] = mod & MODMASK;
}
static void bind_defaults(void) {
    for (int i = 0; i < NUM_BIND; i++)
        for (int s = 0; s < BIND_SLOTS; s++) { g_bind_src[i][s]=0; g_bind_code[i][s]=0; g_bind_mod[i][s]=0; g_bind_kind[i]=KIND_ACTION; g_bind_step[i]=0; }
    /* 单键翻页：下一页 L1/R1；上一页 L2/R2（每功能两套） */
    bind_set(BIND_NEXT, 0, 1, BTN_L1, 0);
    bind_set(BIND_NEXT, 1, 1, BTN_R1, 0);
    bind_set(BIND_PREV, 0, 0, SDLK_PAGEUP, 0);    /* L2=键盘280 */
    bind_set(BIND_PREV, 1, 0, SDLK_PAGEDOWN, 0);  /* R2=键盘281 */
    bind_set(BIND_OPEN, 0, 1, BTN_A, 0);
    bind_set(BIND_BACK, 0, 1, BTN_B, 0);
    bind_set(BIND_MENU, 0, 1, BTN_Y, 0);
    bind_set(BIND_BM,   0, 1, BTN_X, 0);
    bind_set(BIND_SUSPEND, 0, 1, BTN_START, 0);
    /* 进入/退出图片缩放模式：L1+L2 或 R1+R2（两套） */
    bind_set(BIND_PIC, 0, 2, M_L1 | M_L2, 0);
    bind_set(BIND_PIC, 1, 2, M_R1 | M_R2, 0);
    /* 退出App：L1+START 或 R1+START（两套；remap_mod 另有固定别名兜底） */
    bind_set(BIND_QUITAPP, 0, 2, M_L1 | M_START, 0);
    bind_set(BIND_QUITAPP, 1, 2, M_R1 | M_START, 0);
    /* 亮度步进：按住修饰键+音量键；L1=±50% / L2=±20% / 无修饰=±5% */
    bind_set(BIND_BRIGHT_BIG,   0, 2, 0, M_L1); g_bind_kind[BIND_BRIGHT_BIG]   = KIND_BRIGHT; g_bind_step[BIND_BRIGHT_BIG]   = 50;
    bind_set(BIND_BRIGHT_MED,   0, 2, 0, M_L2); g_bind_kind[BIND_BRIGHT_MED]   = KIND_BRIGHT; g_bind_step[BIND_BRIGHT_MED]   = 20;
    bind_set(BIND_BRIGHT_SMALL, 0, 2, 0, 0);    g_bind_kind[BIND_BRIGHT_SMALL] = KIND_BRIGHT; g_bind_step[BIND_BRIGHT_SMALL] = 5;
    /* 图片缩放四档（每档两套：L1+方向键 / R1+动作键）。肩键由图片查看器叠加判定，绑定只记“键”本身 */
    bind_set(BIND_PIC_RIGHT, 0, 0, SDLK_RIGHT, M_L1); bind_set(BIND_PIC_RIGHT, 1, 1, BTN_A, M_R1); g_bind_step[BIND_PIC_RIGHT]= 10; /* 放大10%: L1+→ / R1+A */
    bind_set(BIND_PIC_LEFT,  0, 0, SDLK_LEFT,  M_L1); bind_set(BIND_PIC_LEFT,  1, 1, BTN_Y, M_R1); g_bind_step[BIND_PIC_LEFT] = -10; /* 缩小10%: L1+← / R1+Y */
    bind_set(BIND_PIC_UP,    0, 0, SDLK_UP,    M_L1); bind_set(BIND_PIC_UP,    1, 1, BTN_X, M_R1); g_bind_step[BIND_PIC_UP]   =  1; /* 放大1%:  L1+↑ / R1+X */
    bind_set(BIND_PIC_DOWN,  0, 0, SDLK_DOWN,  M_L1); bind_set(BIND_PIC_DOWN,  1, 1, BTN_B, M_R1); g_bind_step[BIND_PIC_DOWN] = -1; /* 缩小1%:  L1+↓ / R1+B */
    /* 目录：默认不绑定（用菜单「目录」进入） */
}
/* 绑定项 → 逻辑动作 */
static enum Action bind_action(int i) {
    switch (i) {
        case BIND_NEXT:    return A_DOWN;
        case BIND_PREV:    return A_UP;
        case BIND_OPEN:    return A_SELECT;
        case BIND_BACK:    return A_BACK;
        case BIND_MENU:    return A_MENU;
        case BIND_BM:      return A_BOOKMARK;
        case BIND_SUSPEND: return A_START;
        case BIND_PIC:     return A_PIC;
        case BIND_QUITAPP: return A_QUIT_FORCE;
        case BIND_BRIGHT_BIG:
        case BIND_BRIGHT_MED:
        case BIND_BRIGHT_SMALL: return A_VOL;
        case BIND_PIC_UP:    return A_UP;
        case BIND_PIC_DOWN:  return A_DOWN;
        case BIND_PIC_LEFT:  return A_LEFT;
        case BIND_PIC_RIGHT: return A_RIGHT;
        case BIND_TOC:      return A_TOC;
    }
    return A_NONE;
}
/* 把物理键 (src:0=键盘sym / 1=手柄button, code, 当前修饰位 mod) 映射到逻辑动作；未绑定返回 A_NONE。
   mod 必须与绑定的 g_bind_mod 完全相等（单键绑定要求 mod=0，组合绑定要求修饰键正按住）。 */
static enum Action remap_phys(int src, int code, int mod) {
    for (int i = 0; i < NUM_BIND; i++) {
        if (g_bind_kind[i] == KIND_BRIGHT) continue;   /* 亮度绑定不在此匹配（音量键专用通道） */
        for (int s = 0; s < BIND_SLOTS; s++)
            if (g_bind_src[i][s] == src && g_bind_code[i][s] == code && g_bind_mod[i][s] == mod)
                return bind_action(i);
    }
    return A_NONE;
}
/* 纯修饰键组合（src=2）：当前按住的修饰位掩码恰好等于某绑定的 code 即触发。
   未被用户占用的掩码保留固定别名：R1+R2=图片缩放、L2+START=退出App（与出厂习惯一致）。 */
static enum Action remap_mod(int mods) {
    if (!mods || (mods & (mods - 1)) == 0) return A_NONE;   /* 至少两个修饰键才算组合 */
    for (int i = 0; i < NUM_BIND; i++) {
        if (g_bind_kind[i] == KIND_BRIGHT) continue;   /* 亮度绑定不在此匹配 */
        for (int s = 0; s < BIND_SLOTS; s++)
            if (g_bind_src[i][s] == 2 && g_bind_code[i][s] == mods) return bind_action(i);
    }
    if (mods == (M_L1 | M_START)) return A_QUIT_FORCE;  /* 固定别名：L1+START 永远退出App（不可被改绑关闭） */
    if (mods == (M_R1 | M_R2))    return A_PIC;         /* 固定别名（未占用时） */
    if (mods == (M_L2 | M_START)) return A_QUIT_FORCE;  /* 固定别名（未占用时） */
    return A_NONE;
}
/* 可写配置目录：$HOME/.epubreader（OPK 以只读方式挂载，配置/快捷键不能写在 ./） */
static const char *cfg_dir(void) {
    static char dir[256];
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/media/home";
    snprintf(dir, sizeof dir, "%s/.epubreader", home);
    mkdir(dir, 0755);   /* 已存在则忽略错误 */
    return dir;
}
static const char *cfg_path(const char *name) {
    static char buf[512];
    snprintf(buf, sizeof buf, "%s/%s", cfg_dir(), name);
    return buf;
}
static void save_keymap(void) {
    FILE *f = fopen(cfg_path("epub_reader_keys.cfg"), "w");
    if (!f) return;
    /* 格式 v4：i:s=src:code:mod:kind:step（每功能最多 BIND_SLOTS 套绑定）。load 兼容旧 v1/v2/v3 */
    for (int i = 0; i < NUM_BIND; i++)
        for (int s = 0; s < BIND_SLOTS; s++)
            if (g_bind_src[i][s] || g_bind_code[i][s] || g_bind_mod[i][s])
                fprintf(f, "%d:%d=%d:%d:%d:%d:%d\n", i, s, g_bind_src[i][s], g_bind_code[i][s], g_bind_mod[i][s], g_bind_kind[i], g_bind_step[i]);
    if (g_bind_path[0]) fprintf(f, "path=%s\n", g_bind_path);
    fclose(f);
}
static void load_keymap(void) {
    bind_defaults();
    FILE *f = fopen(cfg_path("epub_reader_keys.cfg"), "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        int i, s = 0, src, code, mod, kind = 0, step = 0;
        if (sscanf(line, "%d:%d=%d:%d:%d:%d:%d", &i, &s, &src, &code, &mod, &kind, &step) == 7 && i >= 0 && i < NUM_BIND && s >= 0 && s < BIND_SLOTS) {
            g_bind_src[i][s] = src; g_bind_code[i][s] = code; g_bind_mod[i][s] = mod & MODMASK;
            g_bind_kind[i] = kind; if (kind == KIND_BRIGHT) g_bind_step[i] = step;
        } else if (sscanf(line, "%d=%d:%d:%d:%d:%d", &i, &src, &code, &mod, &kind, &step) == 6 && i >= 0 && i < NUM_BIND) {
            g_bind_src[i][0] = src; g_bind_code[i][0] = code; g_bind_mod[i][0] = mod & MODMASK;  /* 旧 v3 格式（无 slot，装入第 0 套） */
            g_bind_kind[i] = kind; if (kind == KIND_BRIGHT) g_bind_step[i] = step;
        } else if (sscanf(line, "%d=%d:%d:%d", &i, &src, &code, &mod) == 4 && i >= 0 && i < NUM_BIND) {
            g_bind_src[i][0] = src; g_bind_code[i][0] = code; g_bind_mod[i][0] = mod & MODMASK;
        } else if (sscanf(line, "%d=%d:%d", &i, &src, &code) == 3 && i >= 0 && i < NUM_BIND) {
            g_bind_src[i][0] = src; g_bind_code[i][0] = code; g_bind_mod[i][0] = 0;
        } else if (strncmp(line, "path=", 5) == 0) {
            strncpy(g_bind_path, line + 5, sizeof(g_bind_path) - 1);
            g_bind_path[strcspn(g_bind_path, "\n")] = 0;
        }
    }
    fclose(f);
    /* 亮度类绑定：保证 kind/step 有效（旧格式无该字段时按默认恢复） */
    g_bind_kind[BIND_BRIGHT_BIG]   = KIND_BRIGHT; if (!g_bind_step[BIND_BRIGHT_BIG])   g_bind_step[BIND_BRIGHT_BIG]   = 50;
    g_bind_kind[BIND_BRIGHT_MED]   = KIND_BRIGHT; if (!g_bind_step[BIND_BRIGHT_MED])   g_bind_step[BIND_BRIGHT_MED]   = 20;
    g_bind_kind[BIND_BRIGHT_SMALL] = KIND_BRIGHT; if (!g_bind_step[BIND_BRIGHT_SMALL]) g_bind_step[BIND_BRIGHT_SMALL] = 5;
}
static const char *key_label(int src, int code) {
    static char buf[16];
    if (src == 1) {  /* 手柄 button（实测 JOYBTN 索引） */
        const char *jb[] = {"B","A","X","Y","SELECT","START","L1","R1"};
        if (code >= 0 && code <= 7) { snprintf(buf, sizeof buf, "%s", jb[code]); return buf; }
        snprintf(buf, sizeof buf, "BTN%d", code); return buf;
    }
    /* 键盘 sym（L2/R2=280/281、音量270/269、圆3/圆4=279/278） */
    switch (code) {
        case SDLK_PAGEUP:   return "L2";
        case SDLK_PAGEDOWN: return "R2";
        case 279:           return "圆3";
        case 278:           return "圆4";
        case SDLK_UP: return "↑";
        case SDLK_DOWN: return "↓";
        case SDLK_LEFT: return "←";
        case SDLK_RIGHT: return "→";
        default: snprintf(buf, sizeof buf, "K%d", code); return buf;
    }
}
/* 单槽位标签（src=2 纯组合如 "L1+L2"；亮度如 "L1+音量"；单键如 "L1+A"）。返回静态缓冲，调用方须立即拷贝 */
static const char *bind_slot_label(int i, int s) {
    static char buf[48];
    buf[0] = 0;
    if (g_bind_kind[i] == KIND_BRIGHT) {   /* 亮度：按住修饰键 + 音量键 */
        int m = g_bind_mod[i][s];
        if (m & M_L1)    strcat(buf, "L1+");
        if (m & M_L2)    strcat(buf, "L2+");
        if (m & M_R1)    strcat(buf, "R1+");
        if (m & M_R2)    strcat(buf, "R2+");
        if (m & M_START) strcat(buf, "START+");
        if (buf[0] == 0) strcat(buf, "(无)+");  /* 无修饰 = 音量键本身 */
        strcat(buf, "音量");
        return buf;
    }
    if (g_bind_src[i][s] == 2) {   /* 纯修饰键组合：code=修饰位掩码 */
        int m = g_bind_code[i][s];
        if (m & M_L1)    strcat(buf, "L1+");
        if (m & M_L2)    strcat(buf, "L2+");
        if (m & M_R1)    strcat(buf, "R1+");
        if (m & M_R2)    strcat(buf, "R2+");
        if (m & M_START) strcat(buf, "START+");
        size_t L = strlen(buf); if (L) buf[L-1] = 0;   /* 去掉末尾的 + */
        return buf;
    }
    if (g_bind_src[i][s] == 0 && g_bind_code[i][s] == 0) { strcpy(buf, "（菜单）"); return buf; }  /* 未绑定 */
    int mod = g_bind_mod[i][s];
    if (mod & M_L1)    strcat(buf, "L1+");
    if (mod & M_L2)    strcat(buf, "L2+");
    if (mod & M_R1)    strcat(buf, "R1+");
    if (mod & M_R2)    strcat(buf, "R2+");
    if (mod & M_START) strcat(buf, "START+");
    strncat(buf, key_label(g_bind_src[i][s], g_bind_code[i][s]), sizeof(buf) - strlen(buf) - 1);
    return buf;
}
/* 绑定项完整标签（多套用 " / " 拼接，如 "L1 / R1"、"L1+L2 / R1+R2"） */
static const char *bind_label(int i) {
    static char buf[96];
    buf[0] = 0;
    for (int s = 0; s < BIND_SLOTS; s++) {
        if (!g_bind_src[i][s] && !g_bind_code[i][s] && !g_bind_mod[i][s]) continue;
        if (buf[0]) strncat(buf, " / ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, bind_slot_label(i, s), sizeof(buf) - strlen(buf) - 1);
    }
    if (buf[0] == 0) strcpy(buf, "（菜单）");
    return buf;
}
/* 绑定项标签：高亮当前正在编辑的槽位（用 [ ] 括起），其余槽用 " / " 拼接 */
static const char *bind_label_active(int i, int active) {
    static char buf[96];
    buf[0] = 0;
    for (int s = 0; s < BIND_SLOTS; s++) {
        if (!g_bind_src[i][s] && !g_bind_code[i][s] && !g_bind_mod[i][s]) continue;
        char tmp[64];
        const char *sl = bind_slot_label(i, s);
        if (s == active) snprintf(tmp, sizeof tmp, "[%s]", sl);
        else             snprintf(tmp, sizeof tmp, "%s", sl);
        if (buf[0]) strncat(buf, " / ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
    }
    if (buf[0] == 0) strcpy(buf, "（菜单）");
    return buf;
}
/* 导出到 epub_reader_1..9，第一个不存在的编号；满 9 个则覆盖第 9 个（均写在可写目录） */
static void export_keymap(void) {
    char path[512]; int n = 1;
    for (; n <= 9; n++) { snprintf(path, sizeof path, "%s/epub_reader_%d", cfg_dir(), n); if (access(path, 0) != 0) break; }
    if (n > 9) { n = 9; snprintf(path, sizeof path, "%s/epub_reader_9", cfg_dir()); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < NUM_BIND; i++)
        for (int s = 0; s < BIND_SLOTS; s++)
            if (g_bind_src[i][s] || g_bind_code[i][s] || g_bind_mod[i][s])
                fprintf(f, "%d:%d=%d:%d:%d:%d:%d\n", i, s, g_bind_src[i][s], g_bind_code[i][s], g_bind_mod[i][s], g_bind_kind[i], g_bind_step[i]);
    fprintf(f, "path=%s\n", g_bind_path[0] ? g_bind_path : g_default_dir);
    fclose(f);
}

/* App 级退出标志：阅读中按 L1/L2+START 也要直接退出整个 App（不是退回浏览器） */
static int g_quit_app = 0;

/* 把 SDL 事件转成 Action。2026-07-29 KeyTest 权威定案：
   - A/B/X/Y/L1/R1/START/SELECT -> SDL_JOYBUTTON(b1/b0/b2/b3/b6/b7/b5/b4)，并伴随无信息 KEYDOWN sym=0（wait 已丢弃）
   - L2/R2 -> 纯键盘 KEYDOWN(sym 280/281)，无 JOYBUTTON
   - 音量键 -> 纯键盘(sym 270=+, 269=-)
   - 圆3/圆4 -> 纯键盘(sym 279/278)
   肩键(手柄 L1/R1)与 L2/R2(键盘)来自不同源，修饰键状态合并 g_mod 才能正确判定组合键。 */
static enum Action event_to_action(const SDL_Event *ev) {
    if (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) {
        int sym  = (int)ev->key.keysym.sym;
        int down = (ev->type == SDL_KEYDOWN);
        if (down) { g_last_src = 0; g_last_code = sym; g_last_mod = g_mod & MODMASK; }  /* 记录最近物理键+修饰态（此刻尚未含自身位），供组合键捕获 */
        /* 键盘来源的肩键/START 并入统一状态 g_mod */
        /* 键盘来源只有：L2/R2(sym 280/281)、音量(270/269)、圆3/圆4(279/278)。
           L1/R1/START/SELECT 都走 JOYBUTTON，不会以键盘事件出现（sym=0 空事件已在 wait 丢弃）。 */
        int km = 0;
        if (sym == SDLK_PAGEUP)        km = M_L2;   /* 280：纯键盘 L2 */
        else if (sym == SDLK_PAGEDOWN) km = M_R2;   /* 281：纯键盘 R2 */
        if (km) {
            if (down) {
                if (!(g_mod & km)) g_shoulder_combo = 0;
                g_mod_kb |= km;
            } else {
                int held = g_mod_kb & km;   /* 该位确为键盘源持有才算一次有效抬起 */
                g_mod_kb &= ~km;
                if (held && !g_mod && !g_shoulder_combo) {
                    enum Action a = remap_phys(0, sym, 0);   /* L2/R2 单按 → 走绑定表（可改绑） */
                    if (a != A_NONE) return a;
                }
                return A_NONE;
            }
        }
        if (!down) return A_NONE;
        /* 捕获绑定态：任何按下（含修饰键/音量键）都冒泡到主循环做绑定，不触发原功能 */
        if (g_capture) { g_shoulder_combo = 1; return A_BINDCAP; }
        /* 音量键：调亮度（GDK mini 实测 音量+=270 KP_PLUS / 音量-=269 KP_MINUS）。
           组合：任意肩键(L1/L2/R1/R2) 每按 +-50%（2026-07-29 用户定案），纯按 +-5%。
           g_mod 含手柄肩键位 -> 跨源组合生效。 */
        if (sym == KEY_VOLUME_UP || sym == KEY_VOLUME_DOWN) {
            int dir  = (sym == KEY_VOLUME_UP) ? +1 : -1;
            int step = 5;
            int m = g_mod & MODMASK;
            for (int i = 0; i < NUM_BIND; i++)
                for (int s = 0; s < BIND_SLOTS; s++)
                    if (g_bind_kind[i] == KIND_BRIGHT && g_bind_mod[i][s] == m) { step = g_bind_step[i]; break; }
            g_bright_delta = dir * step;
            if (m) g_shoulder_combo = 1;
            return A_VOL;
        }
        /* 自定义快捷键重映射（支持组合键，2026-07-29）：
           主键不能是修饰键自身（km!=0 时跳过，L2/R2 抬起翻页走硬逻辑）；
           匹配要求当前修饰态与绑定 mod 完全相等（单键绑定要求无修饰键按住）。 */
        if (!km) {
            enum Action ra = remap_phys(0, sym, g_mod & MODMASK);
            if (ra != A_NONE) {
                if (g_mod & MODMASK) g_shoulder_combo = 1;  /* 组合命中：修饰键抬起不再翻页 */
                return ra;
            }
        }
        /* 纯修饰键组合（跨源、可自定义 2026-07-29）：按住修饰位精确匹配绑定表
           （默认 L1+L2=图片缩放、L1+START=退出App；R1+R2 / L2+START 为固定别名） */
        {
            enum Action ma = remap_mod(g_mod & MODMASK);
            if (ma != A_NONE) { g_shoulder_combo = 1; return ma; }
        }
        /* 普通键：若正按住肩键则标记组合（避免抬起误翻页） */
        enum Action ka = key_to_action(sym);
        if (ka != A_NONE && (g_mod & (M_L1 | M_L2 | M_R1 | M_R2))) g_shoulder_combo = 1;
        return ka;
    }
    if (ev->type == SDL_JOYBUTTONDOWN || ev->type == SDL_JOYBUTTONUP) {
        int b = ev->jbutton.button;
        int down = (ev->type == SDL_JOYBUTTONDOWN);
        if (down) { g_last_src = 1; g_last_code = b; g_last_mod = g_mod & MODMASK; }  /* 记录最近物理键+修饰态（此刻尚未含自身位），供组合键捕获 */
        /* 手柄来源的肩键/START 并入统一状态 g_mod（L2/R2 是键盘源，不在此） */
        int jm = 0;
        if (b == BTN_L1)         jm = M_L1;
        else if (b == BTN_R1)    jm = M_R1;
        else if (b == BTN_START) jm = M_START;
        if (jm) {
            if (down) {
                if (!(g_mod & jm)) g_shoulder_combo = 0;
                g_mod_joy |= jm;
            } else {
                int held = g_mod_joy & jm;  /* 该位确为手柄源持有才算一次有效抬起 */
                g_mod_joy &= ~jm;
                g_jbtn &= ~(1 << b);
                if (held && !g_mod && !g_shoulder_combo) {
                    enum Action a = remap_phys(1, b, 0);   /* L1/R1 单按 → 走绑定表（可改绑） */
                    if (a != A_NONE) return a;
                }
                return A_NONE;
            }
        }
        if (!down) { g_jbtn &= ~(1 << b); return A_NONE; }
        if (!((g_jbtn >> b) & 1)) g_shoulder_combo = 0; /* 新一次按下 */
        g_jbtn |= (1 << b);
        /* 捕获绑定态：任何按下（含修饰键）都冒泡到主循环做绑定，不触发原功能 */
        if (g_capture) { g_shoulder_combo = 1; return A_BINDCAP; }
        /* 铁定兜底：L1+START / L2+START 立即退出App；R1+R2 立即图片缩放。
           放在捕获态之后（保证「自定义快捷键」改绑时仍能被捕获），但早于任何其它逻辑，
           确保无论处于哪个界面、无论此前状态如何，组合键都绝不会被吞掉。 */
        {
            int md = g_mod & MODMASK;
            if (md == (M_L1 | M_START) || md == (M_L2 | M_START)) {
                fprintf(stderr, "[dbg] FORCEQUIT combo (joy mods=0x%x)\n", md); fflush(stderr);
                return A_QUIT_FORCE;
            }
            if (md == (M_R1 | M_R2)) { fprintf(stderr, "[dbg] PIC combo (joy)\n"); fflush(stderr); return A_PIC; }
        }
        /* 自定义快捷键重映射（支持组合键，2026-07-29）：
           主键不能是修饰键自身（jm!=0 跳过，L1/R1 抬起翻页/START 息屏走硬逻辑）；
           除修饰键与自身外不允许有其它手柄键按住；修饰态须与绑定 mod 完全相等。 */
        if (!jm) {
            int joymodbits = (1 << BTN_L1) | (1 << BTN_R1) | (1 << BTN_START);
            if ((g_jbtn & ~joymodbits & ~(1 << b)) == 0) {
                enum Action ra = remap_phys(1, b, g_mod & MODMASK);
                if (ra != A_NONE) {
                    if (g_mod & MODMASK) g_shoulder_combo = 1;  /* 组合命中：修饰键抬起不再翻页 */
                    return ra;
                }
            }
        }
        /* 纯修饰键组合（跨源、可自定义 2026-07-29）：按住修饰位精确匹配绑定表
           （默认 L1+L2=图片缩放、L1+START=退出App；R1+R2 / L2+START 为固定别名） */
        {
            enum Action ma = remap_mod(g_mod & MODMASK);
            if (ma != A_NONE) { g_shoulder_combo = 1; return ma; }
        }
        /* 仅当「按下的是非肩键/非START 且 当前有肩键按住」才算组合（避免肩键自按置 combo 致抬页失效） */
        if (!jm && (g_mod & (M_L1 | M_L2 | M_R1 | M_R2))) g_shoulder_combo = 1;
        /* 单键映射（实测 JOYBTN：A=1 B=0 X=2 Y=3 SELECT=4 START=5 L1=6 R1=7；圆1/圆2 同 A/B 不可区分） */
        switch (b) {
            case BTN_A:      return A_SELECT;   /* 物理 A 键 = 确认/下一页 */
            case BTN_B:      return A_BACK;     /* 物理 B 键 = 返回/退出 */
            case BTN_X:      return A_BOOKMARK; /* X 键 = 书签 */
            case BTN_Y:      return A_MENU;     /* Y 键 = 菜单 */
            case BTN_SELECT: return A_BACK;     /* SELECT = 返回 */
            case BTN_START:  return A_START;    /* 单独 START = 息屏 */
            case BTN_L1: case BTN_R1: return A_NONE;  /* 单肩无操作，专用于图片功能 */
            default: return A_NONE;
        }
    }
    if (ev->type == SDL_JOYHATMOTION) {
        if (g_mod & (M_L1 | M_L2 | M_R1 | M_R2)) g_shoulder_combo = 1; /* 肩键+方向=组合（选图），跨源判定 */
        if (ev->jhat.value & SDL_HAT_UP)    return A_UP;
        if (ev->jhat.value & SDL_HAT_DOWN)  return A_DOWN;
        if (ev->jhat.value & SDL_HAT_LEFT)  return A_LEFT;
        if (ev->jhat.value & SDL_HAT_RIGHT) return A_RIGHT;
    }
    return A_NONE;
}

/* ================= 运行时环境（SDL fbcon） ================= */
static void setup_runtime_env(void) {
    if (!getenv("SDL_VIDEODRIVER")) setenv("SDL_VIDEODRIVER", "fbcon", 1);
    if (!getenv("SDL_FBDEV"))       setenv("SDL_FBDEV", "/dev/fb0", 1);
    if (!getenv("SDL_NOMOUSE"))     setenv("SDL_NOMOUSE", "1", 1);
}

/* ================= 崩溃诊断（SIGILL/SIGSEGV 精确定位） =================
 * 崩溃时用 async-safe 的 write() 把信号名、出错PC地址、指令字、
 * 以及 /proc/self/maps 全部倒进 stderr（.dge 已重定向到 run.log）。
 * 有了 PC 地址 + maps 就能算出崩在哪个 .so 的哪个偏移。 */
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

static void wr_str(const char *s) { size_t n=0; while (s[n]) n++; write(2, s, n); }
static void wr_hex(unsigned long v) {
    char buf[11]; buf[0]='0'; buf[1]='x';
    for (int i=0;i<8;i++) { int d=(int)((v>>((7-i)*4))&0xf); buf[2+i]=(char)(d<10 ? '0'+d : 'a'+d-10); }
    buf[10]=0; wr_str(buf);
}
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    /* 防递归：先把所有信号恢复默认，处理器内再崩就直接内核处置 */
    signal(SIGILL, SIG_DFL); signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL); signal(SIGFPE, SIG_DFL);
    wr_str("\n[CRASH] signal=");
    wr_str(sig==SIGILL?"SIGILL":sig==SIGSEGV?"SIGSEGV":sig==SIGBUS?"SIGBUS":sig==SIGFPE?"SIGFPE":"???");
    wr_str(" fault_addr="); wr_hex((unsigned long)si->si_addr);
    /* SIGILL/SIGFPE 的 si_addr 即出错指令 PC，读出指令字 */
    if (sig==SIGILL || sig==SIGFPE) {
        unsigned long pc = (unsigned long)si->si_addr & ~3ul;
        if (pc >= 0x10000) { wr_str(" insn="); wr_hex(*(volatile unsigned long*)pc); }
    }
    wr_str("\n[CRASH] /proc/self/maps:\n");
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0) {
        char buf[512]; int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) write(2, buf, n);
        close(fd);
    }
    wr_str("[CRASH] end\n");
    _exit(97); /* 独特退出码，与内核默认的 132/139 区分 */
}
static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    int r1 = sigaction(SIGILL,  &sa, NULL);
    int r2 = sigaction(SIGSEGV, &sa, NULL);
    int r3 = sigaction(SIGBUS,  &sa, NULL);
    int r4 = sigaction(SIGFPE,  &sa, NULL);
    fprintf(stderr, "[diag] sigaction 结果: ILL=%d SEGV=%d BUS=%d FPE=%d\n", r1,r2,r3,r4);
    fflush(stderr);
}

#include <sys/wait.h>
/* 黑匣子自检：fork 子进程故意触发 SIGSEGV，验证处理器真的能接管(应 exit 97) */
static void selftest_crash_handler(void) {
    pid_t pid = fork();
    if (pid == 0) {                    /* 子进程：装处理器 → 故意崩 */
        install_crash_handler();
        *(volatile int *)0x11 = 42;    /* SIGSEGV */
        _exit(1);                      /* 不应到达 */
    }
    if (pid > 0) {
        int st = 0; waitpid(pid, &st, 0);
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
        fprintf(stderr, "[diag] 黑匣子自检: 子进程退出=%d (%s)\n",
                code, code == 97 ? "处理器有效" : "处理器无效!信号未被接管");
        fflush(stderr);
    }
}

/* ================= 电量 / 时间 / 息屏 ================= */
static int read_battery_pct(void) {
    static const char *paths[] = {
        "/sys/class/power_supply/battery/capacity",
        "/sys/class/power_supply/BAT/capacity",
        "/sys/class/power_supply/bat/capacity",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (f) { int v = -1; if (fscanf(f, "%d", &v) == 1) { fclose(f); return v; } fclose(f); }
    }
    return -1;
}
static void get_clock_str(char *buf, int n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) snprintf(buf, n, "%02d:%02d", tm->tm_hour, tm->tm_min);
    else snprintf(buf, n, "--:--");
}
/* 尝试真正关闭背光（写 sysfs blank；失败则仅黑屏代替，忽略错误） */
static void screen_blank(int on) {
    static const char *paths[] = {
        "/sys/class/graphics/fb0/blank",
        "/sys/class/backlight/backlight/blank",
        "/sys/class/backlight/lcd-backlight/blank",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "w");
        if (f) { fputc(on ? '1' : '0', f); fclose(f); }
    }
}
/* 息屏：保存进度后熄屏；除 START 外任意键无效，按 START 恢复画面 */
static void app_suspend(reader_ui_t *ui) {
    screen_blank(1);
    ui_clear(ui);
    ui_text_rgb(ui, ui->margin, SCREEN_H / 2 - 6, "已息屏", 120, 120, 120);
    ui_text_rgb(ui, ui->margin, SCREEN_H / 2 + 8, "按 START 恢复", 120, 120, 120);
    ui_flip(ui);
    SDL_Event ev;
    while (1) {
        if (wait_event_timeout(&ev, 400)) {
            int st = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_RETURN) st = 1;
            else if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_START) st = 1;
            if (st) break;   /* 其它键一律忽略 */
        }
    }
    screen_blank(0);
}

/* ================= 配置读写 ================= */
static void load_config(cfg_t *c) {
    c->font_index = 1; c->fg_index = 0; c->bright_pct = 100; /* 默认 中/白/100% */
    FILE *f = fopen(cfg_path("config.cfg"), "r");
    if (f) {
        int v; char line[128];
        while (fgets(line, sizeof(line), f)) {
            if      (sscanf(line, "font_index=%d", &v) == 1) c->font_index = v;
            else if (sscanf(line, "fg_index=%d", &v) == 1)   c->fg_index = v;
            else if (sscanf(line, "bright=%d", &v) == 1)      c->bright_pct = v;
            else if (sscanf(line, "bright_index=%d", &v) == 1) c->bright_pct = brights[v]; /* 兼容旧档 */
            else if (strncmp(line, "default_dir=", 11) == 0) {
                strncpy(g_default_dir, line + 11, sizeof(g_default_dir) - 1);
                g_default_dir[sizeof(g_default_dir) - 1] = 0;
                g_default_dir[strcspn(g_default_dir, "\r\n")] = 0;
            }
        }
        fclose(f);
    }
    if (c->font_index < 0 || c->font_index > 3) c->font_index = 1;
    if (c->fg_index   < 0 || c->fg_index   > 4) c->fg_index = 0;
    if (c->bright_pct < 0 || c->bright_pct > 100) c->bright_pct = 100;
}
static void save_config(const cfg_t *c) {
    FILE *f = fopen(cfg_path("config.cfg"), "w");
    if (f) {
        fprintf(f, "font_index=%d\nfg_index=%d\nbright=%d\ndefault_dir=%s\n", c->font_index, c->fg_index, c->bright_pct, g_default_dir);
        fclose(f);
    }
}
static void apply_config(reader_ui_t *ui, const cfg_t *c) {
    fprintf(stderr,"[diag] apply_config 入口 font_index=%d\n", c->font_index); fflush(stderr);
    ui_set_font_size(ui, font_sizes[c->font_index]);
    fprintf(stderr,"[diag] apply_config: set_font_size 返回\n"); fflush(stderr);
    ui_set_fg(ui, fg_colors[c->fg_index][0], fg_colors[c->fg_index][1], fg_colors[c->fg_index][2]);
    ui_set_brightness(ui, c->bright_pct);
}

/* ================= 进度 / 书签 ================= */
static char *sidecar_path(const char *book, const char *ext) {
    size_t n = strlen(book);
    char *p = malloc(n + strlen(ext) + 1);
    strcpy(p, book);
    if (ends_with(p, ".epub")) strcpy(p + n - 5, ext);
    else strcat(p, ext);
    return p;
}
static void load_progress(const char *book, int *sp, int *pg) {
    *sp = 0; *pg = 0;
    char *pp = sidecar_path(book, ".progress");
    FILE *f = fopen(pp, "r");
    if (f) { fscanf(f, "%d %d", sp, pg); fclose(f); }
    free(pp);
}
static void save_progress(const char *book, int sp, int pg) {
    char *pp = sidecar_path(book, ".progress");
    FILE *f = fopen(pp, "w");
    if (f) { fprintf(f, "%d %d\n", sp, pg); fclose(f); }
    free(pp);
}
/* 最近阅读的书（供启动续读）：记录完整路径 */
static char *load_lastbook(void) {
    char *p = malloc(512);
    if (!p) return NULL;
    FILE *f = fopen(cfg_path("lastbook"), "r");
    if (!f) { free(p); return NULL; }
    if (!fgets(p, 512, f)) { fclose(f); free(p); return NULL; }
    fclose(f);
    p[strcspn(p, "\r\n")] = 0;
    while (p[0] == '/' && p[1] == '/') memmove(p, p + 1, strlen(p));  /* 规范化历史双斜杠 // → / */
    if (p[0] == 0) return NULL;
    struct stat st; if (stat(p, &st) != 0 || !ends_with(p, ".epub")) return NULL;
    return p;   /* 调用者负责 free */
}
static void save_lastbook(const char *path) {
    while (path[0] == '/' && path[1] == '/') path++;   /* 规范化：去掉多余前导斜杠，避免 //media 损坏续读路径 */
    FILE *f = fopen(cfg_path("lastbook"), "w");
    if (f) { fprintf(f, "%s\n", path); fclose(f); }
}
/* 多书签：.bookmark 文件每行一条 "spine页 page页 正文摘录"（2026-07-29 起摘录=正文 10 个字） */
#define MAX_BM 32
#define BM_TEXT_SZ 64   /* 10 个中文字 = 30 字节 UTF-8，留足余量 */
typedef struct { int sp, pg; char text[BM_TEXT_SZ]; } bm_t;
static int load_bookmarks(const char *book, bm_t *bms) {
    int n = 0;
    char *pp = sidecar_path(book, ".bookmark");
    FILE *f = fopen(pp, "r");
    if (f) {
        char line[192];
        while (n < MAX_BM && fgets(line, sizeof(line), f)) {
            int sp, pg, off = 0;
            if (sscanf(line, "%d %d%n", &sp, &pg, &off) >= 2 && sp >= 0) {
                bms[n].sp = sp; bms[n].pg = pg;
                const char *t = line + off;
                while (*t == ' ') t++;
                size_t tl = strcspn(t, "\r\n");
                if (tl >= sizeof(bms[n].text)) tl = sizeof(bms[n].text) - 1;
                memcpy(bms[n].text, t, tl); bms[n].text[tl] = 0;
                if (!bms[n].text[0])  /* 旧格式无摘录 → 退化显示章页 */
                    snprintf(bms[n].text, sizeof(bms[n].text), "第%d章 第%d页", sp + 1, pg + 1);
                n++;
            }
        }
        fclose(f);
    }
    free(pp);
    return n;
}
static void save_bookmarks(const char *book, const bm_t *bms, int n) {
    char *pp = sidecar_path(book, ".bookmark");
    FILE *f = fopen(pp, "w");
    if (f) {
        for (int i = 0; i < n; i++) fprintf(f, "%d %d %s\n", bms[i].sp, bms[i].pg, bms[i].text);
        fclose(f);
    }
    free(pp);
}
static int bm_find(const bm_t *bms, int n, int sp, int pg) {
    for (int i = 0; i < n; i++) if (bms[i].sp == sp && bms[i].pg == pg) return i;
    return -1;
}
/* 从排版结果 L 的 start_line 行起，提取正文前 10 个字（UTF-8 码点）作为书签摘录。
   跳过图片行/空行与空白（含全角空格缩进），不足 10 字则尽量取。 */
/* 取前 n 个 unichar（含空白）的个数，用于列光标 clamp */
static int utf8_count(const char *s) {
    if (!s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p) { int len = 1; if (*p >= 0xF0) len = 4; else if (*p >= 0xE0) len = 3; else if (*p >= 0xC0) len = 2; p += len; n++; }
    return n;
}

/* 书签摘录：从 (start_line, start_col) 起，取最多 23 个非空白中文字；
   若后续仍有内容则追加 "..."。start_col 为行内 unichar 列索引（0=行首，含空白） */
static void bm_extract(layout_t *L, int start_line, int start_col, char *out, size_t outsz) {
    size_t o = 0; int chars = 0; out[0] = 0;
    if (!L || outsz < 8) return;
    int more = 0;                                  /* 23 字之后是否还有非空白字符 */
    for (int i = (start_line < 0 ? 0 : start_line); i < L->n_lines; i++) {
        const char *t = L->lines[i].text;
        if (!t) continue;                          /* 图片行/空行跳过 */
        const unsigned char *p = (const unsigned char *)t;
        int ci = 0;
        while (*p) {
            int len = 1;
            if (*p >= 0xF0) len = 4; else if (*p >= 0xE0) len = 3; else if (*p >= 0xC0) len = 2;
            if (i == start_line && ci < start_col) { p += len; ci++; continue; }  /* 跳过起始列之前 */
            int is_space = (*p == ' ' || *p == '\t' || (p[0]==0xE3 && p[1]==0x80 && p[2]==0x80));
            if (is_space) { p += len; ci++; continue; }
            if (chars < 23) {
                if (o + (size_t)len + 1 > outsz) { out[o] = 0; return; }
                for (int k = 0; k < len && p[k]; k++) out[o++] = (char)p[k];
                p += len; ci++; chars++;
            } else {
                more = 1;
                p += len; ci++;
            }
        }
        if (chars >= 23 && more) break;
    }
    out[o] = 0;
    if (more) {                                    /* 追加省略号 */
        const char *e = "...";
        for (int k = 0; e[k] && o + 1 < outsz; k++) out[o++] = e[k];
        out[o] = 0;
    }
    if (!out[0]) snprintf(out, outsz, "(无正文)");
}

/* ================= 浏览器辅助 ================= */
typedef struct { nav_ent_t *e; int n, cap; } nav_list_t;
static void nav_add(nav_list_t *l, const char *name, int is_dir) {
    if (l->n >= l->cap) { l->cap = l->cap ? l->cap*2 : 16; l->e = realloc(l->e, l->cap*sizeof(nav_ent_t)); }
    l->e[l->n].name = strdup(name);
    l->e[l->n].is_dir = is_dir;
    l->n++;
}
static void nav_free(nav_list_t *l) {
    for (int i=0;i<l->n;i++) free((void*)l->e[i].name);
    free(l->e); l->e=NULL; l->n=l->cap=0;
}
static char *parent_dir(const char *d) {
    size_t n = strlen(d);
    while (n>1 && d[n-1]=='/') n--;
    const char *slash = strrchr(d, '/');
    if (!slash || slash==d) return strdup("/");
    char *res = malloc(slash-d+1);
    memcpy(res, d, slash-d); res[slash-d]=0;
    return res;
}
static void build_list(const char *dir, nav_list_t *l) {
    if (strcmp(dir,"/")!=0) nav_add(l,"..",1);
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *ent; char buf[4096];
    while ((ent=readdir(dp))) {
        if (!strcmp(ent->d_name,".")||!strcmp(ent->d_name,"..")) continue;
        snprintf(buf,sizeof(buf),"%s/%s",dir,ent->d_name);
        struct stat st;
        if (stat(buf,&st)!=0) continue;
        if (S_ISDIR(st.st_mode)) nav_add(l,ent->d_name,1);
        else if (ends_with(ent->d_name,".epub")) nav_add(l,ent->d_name,0);
    }
    closedir(dp);
}

/* ================= 阅读状态 ================= */
typedef struct {
    epub_t *ep;
    int spine_idx;
    int page;
    layout_t *lay;      /* 当前章排版结果（像素级分页，含图片） */
    char *title;
} reading_t;

static void free_lay(reading_t *r) {
    if (r->lay) { layout_free(r->lay); r->lay = NULL; }
}
/* 章节排版结果缓存：跨章翻页命中缓存避免重新排版（首排仍慢，之后衔接与章节内翻页同速）。
   g_lay_cache 为文件级全局，每次进入 read_book 时先释放旧书残留再使用。 */
#define LAY_CACHE_N 4
typedef struct { int spine; layout_t *lay; } lay_cache_t;
static lay_cache_t g_lay_cache[LAY_CACHE_N];
/* 当前正在显示的 layout（r->lay 的别名）。缓存淘汰时必须跳过它，
   否则快速 ←/→ 连续跨章时 prefetch 会把正在显示的 layout 释放掉
   → 野指针 → 崩溃（v1.2 实测：快速按左右键崩溃的根因）。 */
static layout_t *g_lay_inuse = NULL;
static int g_lay_evict = 0;   /* 轮转淘汰指针，避免总淘汰 slot 0 造成 ping-pong */

/* 取某章排版结果：命中缓存直接返回，否则排版并缓存（缓存满则轮转淘汰，跳过在用项） */
static layout_t *get_layout(reader_ui_t *ui, epub_t *ep, int idx) {
    for (int i = 0; i < LAY_CACHE_N; i++)
        if (g_lay_cache[i].lay && g_lay_cache[i].spine == idx) return g_lay_cache[i].lay;
    char *html = epub_read_html(ep, ep->spine[idx]);
    if (!html) html = strdup("<p>(空章节)</p>");
    layout_t *L = layout_chapter(ui, ep, html, ep->spine[idx]);
    free(html);
    int slot = -1;
    for (int i = 0; i < LAY_CACHE_N; i++) if (!g_lay_cache[i].lay) { slot = i; break; }
    if (slot < 0) {
        /* 轮转找一个「不是正在显示」的槽位淘汰 */
        for (int k = 0; k < LAY_CACHE_N; k++) {
            int i = (g_lay_evict + k) % LAY_CACHE_N;
            if (g_lay_cache[i].lay != g_lay_inuse) { slot = i; break; }
        }
        if (slot < 0) slot = 0;              /* 理论不可达（N>1） */
        if (g_lay_cache[slot].lay && g_lay_cache[slot].lay != g_lay_inuse)
            layout_free(g_lay_cache[slot].lay);
        g_lay_evict = (slot + 1) % LAY_CACHE_N;
    }
    g_lay_cache[slot].spine = idx;
    g_lay_cache[slot].lay = L;
    return L;
}

/* 清空并释放排版缓存（换书 / 改字号等需重排时调用）。
   调用后 r->lay 一律悬空，调用方必须立即重新 open_chapter。 */
static void invalidate_layout_cache(void) {
    for (int i = 0; i < LAY_CACHE_N; i++)
        if (g_lay_cache[i].lay) { layout_free(g_lay_cache[i].lay); g_lay_cache[i].lay = NULL; }
    g_lay_inuse = NULL;
}

static void open_chapter(reader_ui_t *ui, reading_t *r, int idx) {
    if (idx<0 || idx>=r->ep->n_spine) return;
    r->spine_idx = idx;
    r->lay = get_layout(ui, r->ep, idx);
    g_lay_inuse = r->lay;   /* 标记在用，缓存淘汰时跳过 */
    r->page = 0;
    if (r->page >= r->lay->n_pages) r->page = r->lay->n_pages - 1;
    if (r->page < 0) r->page = 0;
}

/* 在边界页（首页/末页）预取相邻章，使 ←/→ 跨章翻页即时。
   开销只在用户停留于边界页时触发一次，不阻塞翻页操作本身。 */
static void prefetch_neighbors(reader_ui_t *ui, reading_t *r) {
    if (!r->lay) return;
    int tp = r->lay->n_pages;
    if (r->page >= tp - 1 && r->spine_idx + 1 < r->ep->n_spine)
        get_layout(ui, r->ep, r->spine_idx + 1);  /* 末页：预取下一章 */
    if (r->page <= 0 && r->spine_idx > 0)
        get_layout(ui, r->ep, r->spine_idx - 1);  /* 首页：预取上一章 */
}

/* ---------- 目录树辅助 ----------
   toc[] 带 level（1 起）。expanded[] 控制每个父节点是否展开。
   可见节点 = 所有祖先均展开的节点。 */
static int toc_has_children(epub_t *ep, int i) {
    return (i + 1 < ep->n_toc && ep->toc[i + 1].level > ep->toc[i].level);
}
static int toc_parent(epub_t *ep, int i) {
    int lv = ep->toc[i].level;
    for (int j = i - 1; j >= 0; j--)
        if (ep->toc[j].level < lv) return j;
    return -1;
}
static int toc_visible(epub_t *ep, const unsigned char *expanded, int i) {
    int p = toc_parent(ep, i);
    while (p >= 0) {
        if (!expanded[p]) return 0;
        p = toc_parent(ep, p);
    }
    return 1;
}
/* 构建可见列表：返回可见项数量，vis[k]=toc 索引 */
static int toc_build_visible(epub_t *ep, const unsigned char *expanded, int *vis, int max) {
    int n = 0;
    for (int i = 0; i < ep->n_toc && n < max; i++)
        if (toc_visible(ep, expanded, i)) vis[n++] = i;
    return n;
}
/* 折叠 i 的整棵子树 */
static void toc_collapse_subtree(epub_t *ep, unsigned char *expanded, int i) {
    int lv = ep->toc[i].level;
    expanded[i] = 0;
    for (int j = i + 1; j < ep->n_toc && ep->toc[j].level > lv; j++) expanded[j] = 0;
}

typedef enum { ST_READ, ST_TOC, ST_MENU, ST_COLOR, ST_FONTSZ, ST_BRIGHT, ST_MODE, ST_BMLIST, ST_PICVIEW, ST_KEYMAP, ST_IMPORT } rstate;

/* ================= 图片缩放/平移查看器 ================= */
#define PICV_X 0
#define PICV_Y TITLE_H
#define PICV_W SCREEN_W
#define PICV_H (SCREEN_H - STATUS_H - TITLE_H)

/* 进入查看器：按屏宽适配起始缩放，居中显示 */
static void enter_picview(reader_ui_t *ui, SDL_Surface **disp, int *zoom, int *ox, int *oy, SDL_Surface *full) {
    int iw = full->w, ih = full->h;
    int z = PICV_W * 100 / iw;
    if (z < 20) z = 20; if (z > 400) z = 400;
    while (iw * z / 100 > 2048 && z > 20) z--;
    int DW = iw * z / 100, DH = ih * z / 100;
    if (*disp) SDL_FreeSurface(*disp);
    *disp = SDL_CreateRGBSurface(SDL_SWSURFACE, DW, DH,
                                 ui->screen->format->BitsPerPixel,
                                 ui->screen->format->Rmask, ui->screen->format->Gmask,
                                 ui->screen->format->Bmask, ui->screen->format->Amask);
    if (*disp) SDL_SoftStretch(full, NULL, *disp, NULL);
    *zoom = z;
    *ox = (PICV_W - DW) / 2; if (*ox < 0) *ox = 0;
    *oy = (PICV_H - DH) / 2; if (*oy < 0) *oy = 0;
}
/* 按新缩放系数重建显示 surface（仅 zoom 变化时调用，平移不重建）。
   2026-07-29 用户需求：缩放以【屏幕中心】为锚点——缩放前后，屏幕中心对准的
   图像点保持不动。换算：图上点 = (视口中心 - 偏移) / 旧zoom，新偏移 = 视口中心 - 图上点*新zoom */
static void zoom_picview(reader_ui_t *ui, SDL_Surface **disp, int *zoom, int *ox, int *oy, SDL_Surface *full, int newz) {
    if (*zoom > 0) {
        *ox = PICV_W / 2 - (int)((long)(PICV_W / 2 - *ox) * newz / *zoom);
        *oy = PICV_H / 2 - (int)((long)(PICV_H / 2 - *oy) * newz / *zoom);
    }
    int iw = full->w, ih = full->h;
    int DW = iw * newz / 100, DH = ih * newz / 100;
    if (DW < 1) DW = 1; if (DH < 1) DH = 1;
    if (*disp) SDL_FreeSurface(*disp);
    *disp = SDL_CreateRGBSurface(SDL_SWSURFACE, DW, DH,
                                 ui->screen->format->BitsPerPixel,
                                 ui->screen->format->Rmask, ui->screen->format->Gmask,
                                 ui->screen->format->Bmask, ui->screen->format->Amask);
    if (*disp) SDL_SoftStretch(full, NULL, *disp, NULL);
    *zoom = newz;
}
/* 限制偏移：图比视口大则偏移范围 [VW-DW, 0]，否则图居中范围 [0, VW-DW] */
static void clamp_picview(int *ox, int *oy, SDL_Surface *disp) {
    if (!disp) return;
    int DW = disp->w, DH = disp->h;
    if (DW <= PICV_W) { if (*ox < 0) *ox = 0; if (*ox > PICV_W - DW) *ox = PICV_W - DW; }
    else { if (*ox > 0) *ox = 0; if (*ox < PICV_W - DW) *ox = PICV_W - DW; }
    if (DH <= PICV_H) { if (*oy < 0) *oy = 0; if (*oy > PICV_H - DH) *oy = PICV_H - DH; }
    else { if (*oy > 0) *oy = 0; if (*oy < PICV_H - DH) *oy = PICV_H - DH; }
}
static void ui_draw_picview(reader_ui_t *ui, SDL_Surface *disp, int ox, int oy, int zoom, int idx, int total) {
    ui_clear(ui);
    ui_rect(ui, 0, 0, SCREEN_W, SCREEN_H, 18, 18, 22);
    if (disp) {
        SDL_Rect dst = { (Sint16)(PICV_X + ox), (Sint16)(PICV_Y + oy), disp->w, disp->h };
        SDL_BlitSurface(disp, NULL, ui->screen, &dst);
    } else {
        ui_text_rgb(ui, 20, SCREEN_H / 2, "图片无法解码", 220, 80, 80);
    }
    ui_rect(ui, 0, 0, SCREEN_W, TITLE_H, 255, 210, 60);
    char t[48]; snprintf(t, sizeof(t), "图片 %d/%d  缩放 %d%%", idx, total, zoom);
    ui_text_rgb(ui, ui->margin, 3, t, 0, 0, 0);
    ui_draw_status(ui, "L1+L2 退出", "L1+方向 缩放");
    ui_draw_hud(ui);
    ui_flip(ui);
}

static void read_book(reader_ui_t *ui, epub_t *ep, const char *book, cfg_t *cfg) {
    reading_t r; memset(&r,0,sizeof(r)); r.ep=ep;
    fprintf(stderr, "[dbg] read_book enter spine=%d\n", ep->n_spine);
    invalidate_layout_cache();  /* 释放上一本书遗留的排版缓存 */
    g_jbtn = 0; g_mod_kb = 0; g_mod_joy = 0; /* 清空按键状态，避免跨文件残留触发组合键 */
    char *nm = strrchr(book,'/'); nm = nm?nm+1:(char*)book;
    r.title = strdup(nm);

    int sp=0, pg=0; load_progress(book,&sp,&pg);
    if (sp>=ep->n_spine) sp=0;
    open_chapter(ui,&r,sp);
    r.page = pg;
    if (r.lay && r.page >= r.lay->n_pages) r.page = r.lay->n_pages - 1;
    if (r.page < 0) r.page = 0;

    /* 多书签 */
    bm_t bms[MAX_BM]; int n_bm = load_bookmarks(book, bms);

    /* 目录树状态：expanded[i]=父节点是否展开（默认全折叠只看一级） */
    unsigned char *expanded = calloc((size_t)(ep->n_toc > 0 ? ep->n_toc : 1), 1);
    int *vis = malloc(sizeof(int) * (size_t)(ep->n_toc > 0 ? ep->n_toc : 1));
    int toc_sel = 0; /* 可见列表中的选中下标 */

    rstate st = ST_READ;
    int menu_sel=0, sub_sel=0, bm_sel=0;
    int km_sel=0, km_rebind=0, km_slot=0; char km_msg[64]={0}; int km_n=NUM_BIND+2;
    int imp_sel=0, imp_n=0, imp_list[9];
    int quit=0;

    /* 书签光标（2026-07-30 增强）：SELECT 出光标，十字键上下换整行、左右行内移字，
       精确定位书签起点；X 从(行,列)起取正文 23 字+...加书签。-1=无光标 */
    int bm_cursor = -1;   /* 光标所在行索引 */
    int bm_col = 0;       /* 光标所在行内 unichar 列索引（0=行首，含空白） */
    /* 图片缩放/平移查看器状态 */
    int pic_focus = 0;            /* 当前页焦点图索引（本页图片列表内） */
    SDL_Surface *pic_full = NULL;  /* 当前缩放查看的原图 */
    SDL_Surface *pic_disp = NULL;  /* 按 zoom 缩放后的显示 surface */
    int pic_zoom = 100;
    int pic_ox = 0, pic_oy = 0;    /* 图在视口内的偏移（屏幕像素，正值=图向右/下移） */
    while (!quit) {
        int total_pages = r.lay ? r.lay->n_pages : 1;
        int at_bm = bm_find(bms, n_bm, r.spine_idx, r.page) >= 0;
        /* 本页图片列表（仅当前页） */
        int np = 0, page_pics[32];
        if (r.lay) for (int k = 0; k < r.lay->n_pics && np < 32; k++)
            if (r.lay->pics[k].page == r.page) page_pics[np++] = k;
        if (pic_focus >= np) pic_focus = 0;
        /* 刷新状态栏数据：电量 / 时间（24h） */
        {
            char clk[8]; get_clock_str(clk, sizeof(clk));
            ui_set_hud(read_battery_pct(), clk);
        }
        if (st == ST_READ) {
            int pct = total_pages>0 ? (r.page+1)*100/total_pages : 0;
            int focus_line = (np>0) ? r.lay->pics[page_pics[pic_focus]].line : -1;
            int focus_col  = -1;
            char flbl[32]; if (np>0) snprintf(flbl, sizeof(flbl), "图 %d/%d", pic_focus+1, np); else flbl[0]=0;
            if (bm_cursor >= 0) { focus_line = bm_cursor; focus_col = bm_col; snprintf(flbl, sizeof(flbl), "书签起点 X确认"); }
            ui_draw_reader_layout(ui, r.lay, r.page, r.title, pct, at_bm, focus_line,
                                  focus_col, (np>0 || bm_cursor>=0) ? flbl : NULL);
        } else if (st == ST_PICVIEW) {
            ui_draw_picview(ui, pic_disp, pic_ox, pic_oy, pic_zoom, pic_focus+1, np);
        } else if (st == ST_TOC) {
            int nv = toc_build_visible(ep, expanded, vis, ep->n_toc);
            if (toc_sel >= nv) toc_sel = nv > 0 ? nv - 1 : 0;
            static char lbl[256][160]; /* 组合缩进+标记+标签 */
            menu_item_t items[256];
            int n = nv < 256 ? nv : 256;
            for (int k = 0; k < n; k++) {
                int i = vis[k];
                int lv = ep->toc[i].level;
                char *dst = lbl[k]; int o = 0;
                for (int d = 1; d < lv && o < 12; d++) { dst[o++]=' '; dst[o++]=' '; }
                if (toc_has_children(ep, i)) dst[o++] = expanded[i] ? '-' : '+';
                else dst[o++] = ' ';
                dst[o++] = ' ';
                snprintf(dst + o, sizeof(lbl[0]) - (size_t)o, "%s", ep->toc[i].label);
                items[k] = (menu_item_t){dst, "", 1};
            }
            ui_draw_menu(ui,"目录 [A展开/跳转 →入 ←出]",items,n,toc_sel);
        } else if (st == ST_BMLIST) {
            static char blbl[MAX_BM][96];
            menu_item_t items[MAX_BM + 1];
            if (n_bm == 0) {
                items[0] = (menu_item_t){"(无书签, 阅读中按 X 添加)","",0};
                ui_draw_menu(ui,"书签",items,1,0);
            } else {
                for (int i=0;i<n_bm;i++) {
                    /* 2026-07-29 用户定案：显示正文摘录（10 字），不再写第几章第几页 */
                    snprintf(blbl[i], sizeof(blbl[0]), "%d. %s", i+1, bms[i].text);
                    items[i]=(menu_item_t){blbl[i],"",1};
                }
                if (bm_sel >= n_bm) bm_sel = n_bm - 1;
                ui_draw_menu(ui,"书签 [A跳转 X删除]",items,n_bm,bm_sel);
            }
        } else if (st == ST_MENU) {
            char bmv[16]; snprintf(bmv,sizeof(bmv),"%d",n_bm);
            static char brightv[16]; snprintf(brightv,sizeof brightv,"%d%%",cfg->bright_pct);
            menu_item_t items[8]; int mn=0;
            items[mn++]=(menu_item_t){"目录","",1};
            items[mn++]=(menu_item_t){"书签",bmv,1};
            items[mn++]=(menu_item_t){"正文颜色",fg_labels[cfg->fg_index],1};
            items[mn++]=(menu_item_t){"字号",font_labels[cfg->font_index],1};
            items[mn++]=(menu_item_t){"亮度",brightv,1};
            items[mn++]=(menu_item_t){"退出App","自动书签",1};
            items[mn++]=(menu_item_t){"自定义快捷键","",1};
            items[mn++]=(menu_item_t){"返回书架","",1};
            ui_draw_menu(ui,"菜单",items,mn,menu_sel);
        } else if (st == ST_COLOR) {
            menu_item_t items[5];
            for (int i=0;i<5;i++) items[i]=(menu_item_t){fg_labels[i], i==cfg->fg_index?"●":"",1};
            ui_draw_menu(ui,"正文颜色 (仅正文, 标题不变)",items,5,sub_sel);
        } else if (st == ST_FONTSZ) {
            menu_item_t items[4];
            for (int i=0;i<4;i++) items[i]=(menu_item_t){font_labels[i], i==cfg->font_index?"●":"",1};
            ui_draw_menu(ui,"字号",items,4,sub_sel);
        } else if (st == ST_BRIGHT) {
            static char bv[16]; snprintf(bv,sizeof bv,"当前 %d%%", cfg->bright_pct);
            menu_item_t items[5];
            for (int i=0;i<5;i++) items[i]=(menu_item_t){bright_labels[i], (cfg->bright_pct/20)==i?"●":"",1};
            items[0] = (menu_item_t){"↑/↓ 调亮度", bv, 1};
            ui_draw_menu(ui,"亮度 (↑+5% ↓-5%)",items,5,sub_sel);
        } else if (st == ST_MODE) {
            menu_item_t items[1] = {{"阅读模式: 敬请期待","",0}};
            ui_draw_menu(ui,"阅读模式",items,1,0);
        } else if (st == ST_KEYMAP) {
            static char kbuf[NUM_BIND+2][64];
            menu_item_t kitems[NUM_BIND+2];
            for (int i=0;i<NUM_BIND;i++) {
                const char *lbl = (i == km_sel && km_sel < NUM_BIND)
                    ? bind_label_active(i, km_slot) : bind_label(i);
                snprintf(kbuf[i],sizeof kbuf[i],"%s: %s", bind_names[i], lbl);
                kitems[i]=(menu_item_t){kbuf[i],"",1};
            }
            kitems[NUM_BIND]   = (menu_item_t){"导出配置","",1};
            kitems[NUM_BIND+1] = (menu_item_t){"导入配置","",1};
            char ttl[96]; snprintf(ttl,sizeof ttl,"自定义[←→选槽 A重绑 SEL复位] 槽%d/%d %s", km_slot+1, BIND_SLOTS, km_msg);
            ui_draw_menu(ui, ttl, kitems, km_n, km_sel);
        } else if (st == ST_IMPORT) {
            static char ibuf[9][40];
            menu_item_t iitems[9];
            int in = imp_n>0 ? imp_n : 1;
            if (imp_n == 0) iitems[0] = (menu_item_t){"(无可用配置)","",0};
            else for (int i=0;i<imp_n;i++){ snprintf(ibuf[i],sizeof ibuf[i],"epub_reader_%d", imp_list[i]); iitems[i]=(menu_item_t){ibuf[i],"",1}; }
            ui_draw_menu(ui,"导入配置 [A载入 B返回]", iitems, in, imp_sel);
        }

        SDL_Event ev;
        if (!wait_event_timeout(&ev,300)) continue;
        enum Action act = event_to_action(&ev);
        if (act == A_NONE) continue;

        /* 自定义快捷键：捕获绑定态优先于一切（即使按 START/音量/组合键 也只作绑定，不触发其原功能）。
           必须放在 A_QUIT_FORCE 之前，否则改绑「退出App」时按当前组合会直接退出。 */
        if (st == ST_KEYMAP && km_rebind) {
            if (act != A_BINDCAP) {   /* 十字键等不产生捕获事件的输入：忽略并继续等待 */
                snprintf(km_msg, sizeof km_msg, "该键不可绑定，请重按");
                continue;
            }
            int src = g_last_src, code = g_last_code, mod = g_last_mod & MODMASK;
            /* 主键本身是修饰键 → 转成纯修饰键组合绑定 (src=2, code=按下瞬间全部修饰位) */
            int self = 0;
            if (src == 0) { if (code == SDLK_PAGEUP) self = M_L2; else if (code == SDLK_PAGEDOWN) self = M_R2; }
            else if (src == 1) { if (code == BTN_L1) self = M_L1; else if (code == BTN_R1) self = M_R1; else if (code == BTN_START) self = M_START; }
            if (km_sel < NUM_BIND && g_bind_kind[km_sel] == KIND_BRIGHT) {
                /* 亮度绑定：仅“按住修饰键 + 音量键”触发，故只能绑定到修饰键或修饰键组合（单键即可，无需另一键） */
                if (!self) {
                    snprintf(km_msg, sizeof km_msg, "亮度需绑定修饰键");
                    km_rebind = 0; g_capture = 0;
                    continue;
                }
                int mods = (mod | self) & MODMASK;
                src = 2; code = 0; mod = mods;   /* 亮度通道按 g_bind_mod 匹配，code 无意义 */
            } else if (self) {
                int mods = (mod | self) & MODMASK;
                if ((mods & (mods - 1)) == 0) {   /* 目前只按了一个修饰键：等组合的另一键 */
                    snprintf(km_msg, sizeof km_msg, "请再按组合的另一键...");
                    continue;                      /* 保持捕获态 */
                }
                src = 2; code = mods; mod = 0;
            }
            km_rebind = 0; g_capture = 0;
            if (km_sel >= NUM_BIND) snprintf(km_msg, sizeof km_msg, "请在按键行上按A");
            else if (src != 0 && src != 1 && src != 2) snprintf(km_msg, sizeof km_msg, "未捕获按键");
            else {
                int conflict = -1;
                for (int i = 0; i < NUM_BIND; i++)
                    for (int s = 0; s < BIND_SLOTS; s++)
                        if (i != km_sel && g_bind_src[i][s] == src && g_bind_code[i][s] == code && g_bind_mod[i][s] == mod) conflict = i;
                if (conflict >= 0) snprintf(km_msg, sizeof km_msg, "冲突:%s", bind_names[conflict]);
                else {
                    /* 写入当前选中的槽位 km_slot；其余槽位保留出厂默认/其它绑定，不破坏多套设置 */
                    g_bind_src[km_sel][km_slot] = src; g_bind_code[km_sel][km_slot] = code; g_bind_mod[km_sel][km_slot] = mod;
                    snprintf(km_msg, sizeof km_msg, "已绑定槽%d: %s", km_slot+1, bind_label(km_sel));
                    save_keymap();
                }
            }
            continue;
        }
        if (act == A_BINDCAP) continue;   /* 非捕获态收到冒泡事件：忽略（防御） */

        /* 强制退出（默认 L1+START / L2+START，可自定义）：任意界面生效，直接退出整个 App，自动加书签 */
        if (act == A_QUIT_FORCE) {
            if (bm_find(bms, n_bm, r.spine_idx, r.page) < 0 && n_bm < MAX_BM) {
                bms[n_bm].sp = r.spine_idx; bms[n_bm].pg = r.page;
                    bm_extract(r.lay, r.lay ? r.lay->page_start[r.page] : 0, 0,
                               bms[n_bm].text, sizeof(bms[n_bm].text));
                n_bm++;
                save_bookmarks(book, bms, n_bm);
            }
            save_progress(book, r.spine_idx, r.page);
            g_quit_app = 1;
            quit = 1;
            continue;
        }
        /* START：任意界面自动保存进度并息屏；息屏后仅 START 可恢复 */
        if (act == A_START) {
            save_progress(book, r.spine_idx, r.page);
            app_suspend(ui);
            continue;
        }
        /* 音量键：调亮度（步进由 event_to_action 写入 g_bright_delta） */
        if (act == A_VOL) {
            cfg->bright_pct += g_bright_delta;
            if (cfg->bright_pct > 100) cfg->bright_pct = 100;
            if (cfg->bright_pct < 0)   cfg->bright_pct = 0;
            ui_set_brightness(ui, cfg->bright_pct);
            save_config(cfg);
            continue;
        }

        /* ---- 阅读页：书签光标模式（优先于其它阅读页操作） ---- */
        if (st == ST_READ && bm_cursor >= 0 && r.lay) {
            int pstart = r.lay->page_start[r.page];
            int pend   = (r.page + 1 < r.lay->n_pages) ? r.lay->page_start[r.page + 1] : r.lay->n_lines;
            if (act == A_UP || act == A_MENU) {            /* ↑/Y = 上一行 */
                if (bm_cursor > pstart) {
                    bm_cursor--;
                    int lc = utf8_count(r.lay->lines[bm_cursor].text);
                    if (bm_col > lc - 1) bm_col = (lc > 0 ? lc - 1 : 0);
                }
                continue;
            }
            if (act == A_DOWN || act == A_SELECT) {        /* ↓/A = 下一行 */
                if (bm_cursor < pend - 1) {
                    bm_cursor++;
                    int lc = utf8_count(r.lay->lines[bm_cursor].text);
                    if (bm_col > lc - 1) bm_col = (lc > 0 ? lc - 1 : 0);
                }
                continue;
            }
            if (act == A_LEFT)  { if (bm_col > 0) bm_col--; continue; }            /* ← = 左移一字 */
            if (act == A_RIGHT) {                                                          /* → = 右移一字 */
                int lc = utf8_count(r.lay->lines[bm_cursor].text);
                if (bm_col < lc - 1) bm_col++;
                continue;
            }
            if (act == A_BOOKMARK) {                                       /* X = 从(行,列)起摘录 23 字+... 加书签 */
                int at = bm_find(bms, n_bm, r.spine_idx, r.page);
                if (at >= 0) {
                    bm_extract(r.lay, bm_cursor, bm_col, bms[at].text, sizeof(bms[at].text));  /* 已有则更新摘录 */
                } else if (n_bm < MAX_BM) {
                    bms[n_bm].sp = r.spine_idx; bms[n_bm].pg = r.page;
                    bm_extract(r.lay, bm_cursor, bm_col, bms[n_bm].text, sizeof(bms[n_bm].text));
                    n_bm++;
                }
                save_bookmarks(book, bms, n_bm);
                bm_cursor = -1; bm_col = 0;
                continue;
            }
            if (act == A_BACK) { bm_cursor = -1; bm_col = 0; continue; }    /* B/SELECT = 取消光标 */
            continue;                                                       /* 光标模式屏蔽其余动作 */
        }

        /* ---- 阅读页：图片焦点选择 / 进缩放 ---- */
        if (st == ST_READ) {
            if (act == A_PIC) {
                if (np > 0) {
                    int idx = page_pics[pic_focus];
                    pic_full = r.lay->pics[idx].full;
                    if (!pic_full && r.ep && r.lay->pics[idx].href) {
                        /* 原图延迟解码：开章时不再解码，进缩放时才解，避免加载缓慢 */
                        size_t fsz = 0;
                        unsigned char *fbuf = epub_read_file_rel(r.ep, r.ep->spine[r.spine_idx], r.lay->pics[idx].href, &fsz);
                        if (fbuf) { pic_full = img_decode(fbuf, fsz); free(fbuf); if (pic_full) r.lay->pics[idx].full = pic_full; }
                    }
                    if (pic_full) {
                        enter_picview(ui, &pic_disp, &pic_zoom, &pic_ox, &pic_oy, pic_full);
                        st = ST_PICVIEW;
                    }
                }
                continue;
            }
            int shoulder = (g_mod & (M_L1 | M_R1));
            if (shoulder && np > 0) {
                int dir = 0;
                if (act == bind_action(BIND_PIC_UP) || act == bind_action(BIND_PIC_RIGHT)) dir = +1;
                else if (act == bind_action(BIND_PIC_DOWN) || act == bind_action(BIND_PIC_LEFT)) dir = -1;
                if (dir != 0) {
                    pic_focus = (pic_focus + dir + np) % np;
                    continue;
                }
            }
        }

        if (st == ST_READ) {
            switch (act) {
                case A_UP:
                    if (r.page > 0) {
                        r.page--;
                    } else if (r.spine_idx > 0) {
                        open_chapter(ui, &r, r.spine_idx - 1);
                        r.page = (r.lay ? r.lay->n_pages - 1 : 0);
                    }
                    save_progress(book, r.spine_idx, r.page);
                    break;
                case A_DOWN:
                    if (r.page < total_pages - 1) {
                        r.page++;
                    } else if (r.spine_idx < ep->n_spine - 1) {
                        open_chapter(ui, &r, r.spine_idx + 1);
                        r.page = 0;
                    }
                    save_progress(book, r.spine_idx, r.page);
                    break;
                case A_SELECT:
                    if (r.page < total_pages - 1) {
                        r.page++;
                    } else if (r.spine_idx < ep->n_spine - 1) {
                        open_chapter(ui, &r, r.spine_idx + 1);
                        r.page = 0;
                    }
                    save_progress(book, r.spine_idx, r.page);
                    break; /* A=下一页 */
                case A_LEFT:  if (r.spine_idx>0){r.page=0; open_chapter(ui,&r,r.spine_idx-1); r.page=0; save_progress(book,r.spine_idx,r.page);} break;
                case A_RIGHT: if (r.spine_idx<ep->n_spine-1){r.page=0; open_chapter(ui,&r,r.spine_idx+1); r.page=0; save_progress(book,r.spine_idx,r.page);} break;
                case A_BACK:
                    /* SELECT（物理键区分）= 出书签光标，落在当前页第一行；B = 退出阅读 */
                    if (g_last_src == 1 && g_last_code == BTN_SELECT && r.lay) {
                        bm_cursor = r.lay->page_start[r.page]; bm_col = 0;
                    } else quit=1;
                    break;
                case A_MENU:  st=ST_MENU; menu_sel=0; break;
                case A_TOC:   st=ST_TOC; toc_sel=0; save_progress(book, r.spine_idx, r.page); break;
                case A_BOOKMARK: {
                    int at = bm_find(bms, n_bm, r.spine_idx, r.page);
                    if (at >= 0) { /* 已有 → 删除（切换语义） */
                        for (int i=at;i<n_bm-1;i++) bms[i]=bms[i+1];
                        n_bm--;
                    } else if (n_bm < MAX_BM) {
                        /* 未动光标直接按 X：默认从当前页第一行起摘录正文 10 字 */
                        bms[n_bm].sp=r.spine_idx; bms[n_bm].pg=r.page;
                        bm_extract(r.lay, r.lay ? r.lay->page_start[r.page] : 0, 0,
                                   bms[n_bm].text, sizeof(bms[n_bm].text));
                        n_bm++;
                    }
                    save_bookmarks(book, bms, n_bm);
                    break;
                }
                default: break;
            }
            if (st == ST_READ) prefetch_neighbors(ui, &r);  /* 边界页预取相邻章，跨章翻页即时 */
        } else if (st == ST_PICVIEW) {
            if (act == A_PIC) {                 /* 退出缩放，回阅读页 */
                st = ST_READ;
                if (pic_disp) { SDL_FreeSurface(pic_disp); pic_disp = NULL; }
                continue;
            }
            int shoulder = (g_mod & (M_L1 | M_R1));
            if (shoulder) {
                /* 缩放：方向键（L1+方向）或动作键（R1+动作键）均可；步长取各自绑定值
                   →/A = 放大10%   ←/Y = 缩小10%   ↑/X = 放大1%   ↓/B = 缩小1% */
                int delta = 0;
                if      (act == bind_action(BIND_PIC_RIGHT) || act == A_SELECT)   delta =  g_bind_step[BIND_PIC_RIGHT];
                else if (act == bind_action(BIND_PIC_LEFT)  || act == A_MENU)    delta =  g_bind_step[BIND_PIC_LEFT];
                else if (act == bind_action(BIND_PIC_UP)    || act == A_BOOKMARK) delta =  g_bind_step[BIND_PIC_UP];
                else if (act == bind_action(BIND_PIC_DOWN)  || act == A_BACK)     delta =  g_bind_step[BIND_PIC_DOWN];
                if (delta != 0) {
                    int nz = pic_zoom + delta;
                    if (nz < 20) nz = 20; if (nz > 400) nz = 400;
                    if (nz != pic_zoom) zoom_picview(ui, &pic_disp, &pic_zoom, &pic_ox, &pic_oy, pic_full, nz);
                }
            } else {
                int step = 24;
                /* 镜头语义：↑=镜头上→图下；↓=镜头下→图上；←=镜头左→图右；→=镜头右→图左 */
                if (act == bind_action(BIND_PIC_UP))        pic_oy += step;  /* 图下 */
                else if (act == bind_action(BIND_PIC_DOWN))  pic_oy -= step;  /* 图上 */
                else if (act == bind_action(BIND_PIC_LEFT))  pic_ox += step;  /* 图右 */
                else if (act == bind_action(BIND_PIC_RIGHT)) pic_ox -= step;  /* 图左 */
            }
            clamp_picview(&pic_ox, &pic_oy, pic_disp);
            continue;
        } else if (st == ST_TOC) {
            int nv = toc_build_visible(ep, expanded, vis, ep->n_toc);
            if (nv == 0) { st = ST_MENU; continue; }
            if (toc_sel >= nv) toc_sel = nv - 1;
            int cur = vis[toc_sel];
            switch (act) {
                case A_UP:   if (toc_sel>0) toc_sel--; break;
                case A_DOWN: if (toc_sel<nv-1) toc_sel++; break;
                case A_SELECT: /* A：父节点=展开/折叠；叶子=跳转 */
                    if (toc_has_children(ep, cur)) {
                        if (expanded[cur]) toc_collapse_subtree(ep, expanded, cur);
                        else expanded[cur] = 1;
                    } else {
                        int idx = epub_find_spine(ep, ep->toc[cur].href);
                        if (idx>=0){ r.page=0; open_chapter(ui,&r,idx); save_progress(book,r.spine_idx,r.page); }
                        st=ST_READ;
                    }
                    break;
                case A_MENU: { /* Y：无论父子直接跳转 */
                    int idx = epub_find_spine(ep, ep->toc[cur].href);
                    if (idx>=0){ r.page=0; open_chapter(ui,&r,idx); save_progress(book,r.spine_idx,r.page); }
                    st=ST_READ; break;
                }
                case A_RIGHT: /* →：展开并进入下一级（选中第一个子项） */
                    if (toc_has_children(ep, cur)) {
                        expanded[cur] = 1;
                        int nv2 = toc_build_visible(ep, expanded, vis, ep->n_toc);
                        for (int k=0;k<nv2;k++) if (vis[k]==cur+1) { toc_sel=k; break; }
                    }
                    break;
                case A_LEFT: { /* ←：折叠当前子树；已折叠/叶子则回到上一级并折叠之 */
                    if (toc_has_children(ep, cur) && expanded[cur]) {
                        toc_collapse_subtree(ep, expanded, cur);
                    } else {
                        int par = toc_parent(ep, cur);
                        if (par >= 0) {
                            toc_collapse_subtree(ep, expanded, par);
                            int nv2 = toc_build_visible(ep, expanded, vis, ep->n_toc);
                            for (int k=0;k<nv2;k++) if (vis[k]==par) { toc_sel=k; break; }
                        }
                    }
                    break;
                }
                case A_BACK: st=ST_MENU; break;
                default: break;
            }
        } else if (st == ST_BMLIST) {
            switch (act) {
                case A_UP:   if (bm_sel>0) bm_sel--; break;
                case A_DOWN: if (bm_sel<n_bm-1) bm_sel++; break;
                case A_SELECT: case A_MENU:
                    if (n_bm > 0 && bm_sel < n_bm) {
                        int tsp=bms[bm_sel].sp, tpg=bms[bm_sel].pg;
                        if (tsp>=0 && tsp<ep->n_spine) {
                            r.page = 0; open_chapter(ui,&r,tsp);
                            r.page = tpg;
                            if (r.lay && r.page>=r.lay->n_pages) r.page=r.lay->n_pages-1;
                            if (r.page<0) r.page=0;
                            save_progress(book,r.spine_idx,r.page);
                        }
                        st=ST_READ;
                    }
                    break;
                case A_BOOKMARK: /* X：删除选中书签 */
                    if (n_bm > 0 && bm_sel < n_bm) {
                        for (int i=bm_sel;i<n_bm-1;i++) bms[i]=bms[i+1];
                        n_bm--;
                        save_bookmarks(book, bms, n_bm);
                        if (bm_sel >= n_bm && bm_sel > 0) bm_sel--;
                    }
                    break;
                case A_BACK: st=ST_MENU; break;
                default: break;
            }
        } else if (st == ST_MENU) {
            int n = 8;
            switch (act) {
                case A_UP:   if (menu_sel>0) menu_sel--; break;
                case A_DOWN: if (menu_sel<n-1) menu_sel++; break;
                case A_SELECT: case A_MENU:
                    if      (menu_sel==0) { st=ST_TOC; toc_sel=0; }
                    else if (menu_sel==1) { st=ST_BMLIST; bm_sel=0; }
                    else if (menu_sel==2) { st=ST_COLOR;  sub_sel=cfg->fg_index; }
                    else if (menu_sel==3) { st=ST_FONTSZ; sub_sel=cfg->font_index; }
                    else if (menu_sel==4) { st=ST_BRIGHT; }
                    else if (menu_sel==5) { /* 退出App（整个程序），自动书签 */
                        if (bm_find(bms, n_bm, r.spine_idx, r.page) < 0 && n_bm < MAX_BM) {
                            bms[n_bm].sp=r.spine_idx; bms[n_bm].pg=r.page;
                            bm_extract(r.lay, r.lay ? r.lay->page_start[r.page] : 0, 0,
                                       bms[n_bm].text, sizeof(bms[n_bm].text));
                            n_bm++;
                            save_bookmarks(book, bms, n_bm);
                        }
                        save_progress(book, r.spine_idx, r.page);
                        g_quit_app=1; quit=1;
                    }
                    else if (menu_sel==6) { st=ST_KEYMAP; km_sel=0; }
                    else if (menu_sel==7) { quit=1; }   /* 返回书架：退出阅读回到文件列表 */
                    break;
                case A_BACK: st=ST_READ; break;
                default: break;
            }
        } else if (st == ST_COLOR) {
            switch (act) {
                case A_UP:   if (sub_sel>0) sub_sel--; break;
                case A_DOWN: if (sub_sel<4) sub_sel++; break;
                case A_SELECT: case A_MENU:
                    cfg->fg_index=sub_sel; ui_set_fg(ui,fg_colors[sub_sel][0],fg_colors[sub_sel][1],fg_colors[sub_sel][2]); save_config(cfg); st=ST_MENU; break;
                case A_BACK: st=ST_MENU; break;
                default: break;
            }
        } else if (st == ST_FONTSZ) {
            switch (act) {
                case A_UP:   if (sub_sel>0) sub_sel--; break;
                case A_DOWN: if (sub_sel<3) sub_sel++; break;
                case A_SELECT: case A_MENU:
                    cfg->font_index=sub_sel; ui_set_font_size(ui,font_sizes[sub_sel]); save_config(cfg);
                    invalidate_layout_cache();  /* 字号变了，旧缓存布局失效，必须重排 */
                    r.page=0; open_chapter(ui,&r,r.spine_idx); save_progress(book,r.spine_idx,r.page);
                    st=ST_MENU; break;
                case A_BACK: st=ST_MENU; break;
                default: break;
            }
        } else if (st == ST_BRIGHT) {
            switch (act) {
                case A_UP:   cfg->bright_pct += 5; if (cfg->bright_pct>100) cfg->bright_pct=100; ui_set_brightness(ui,cfg->bright_pct); save_config(cfg); break;
                case A_DOWN: cfg->bright_pct -= 5; if (cfg->bright_pct<0) cfg->bright_pct=0; ui_set_brightness(ui,cfg->bright_pct); save_config(cfg); break;
                case A_SELECT: case A_MENU: st=ST_MENU; break;
                case A_BACK: st=ST_MENU; break;
                default: break;
            }
        } else if (st == ST_KEYMAP) {
            switch (act) {
                case A_UP:   if (km_sel>0) { km_sel--; km_slot=0; } break;
                case A_DOWN: if (km_sel<km_n-1) { km_sel++; km_slot=0; } break;
                case A_LEFT:  if (km_sel < NUM_BIND) { km_slot = (km_slot + BIND_SLOTS - 1) % BIND_SLOTS; } break;
                case A_RIGHT: if (km_sel < NUM_BIND) { km_slot = (km_slot + 1) % BIND_SLOTS; } break;
                case A_SELECT:
                    if (km_sel == NUM_BIND) { export_keymap(); snprintf(km_msg,sizeof km_msg,"已导出配置"); }
                    else if (km_sel == NUM_BIND + 1) {
                        imp_n = 0;
                        for (int k=1;k<=9;k++){ char p[512]; snprintf(p,sizeof p,"%s/epub_reader_%d", cfg_dir(), k); if (access(p,0)==0) imp_list[imp_n++]=k; }
                        if (imp_n==0) snprintf(km_msg,sizeof km_msg,"无可用配置");
                        else { imp_sel=0; st=ST_IMPORT; }
                    } else { km_rebind = 1; g_capture = 1; snprintf(km_msg, sizeof km_msg, "重绑槽%d/%d: 按键或组合(如L1+L2/L1+A)...", km_slot+1, BIND_SLOTS); }
                    break;
                case A_MENU:  st=ST_MENU; break;   /* Y 也可退出 */
                case A_BACK:  /* 2026-07-29 用户定案：SELECT=恢复默认、B=退出（按物理键区分，二者同映射 A_BACK） */
                    if (g_last_src == 1 && g_last_code == BTN_SELECT) {
                        bind_defaults(); save_keymap(); snprintf(km_msg,sizeof km_msg,"已恢复默认");
                    } else {
                        st = ST_MENU;   /* B 退出 */
                    }
                    break;
                default: break;
            }
        } else if (st == ST_IMPORT) {
            switch (act) {
                case A_UP:   if (imp_sel>0) imp_sel--; break;
                case A_DOWN: if (imp_sel<imp_n-1) imp_sel++; break;
                case A_SELECT: { /* 载入选中的配置 */
                    if (imp_n > 0) {
                        char p[512]; snprintf(p,sizeof p,"%s/epub_reader_%d", cfg_dir(), imp_list[imp_sel]);
                        FILE *f = fopen(p,"r");
                        if (f) {
                            char line[160];
                            while (fgets(line,sizeof line,f)) {
                                int i,s=0,src,code,mod,kind=0,step=0;
                                if (sscanf(line,"%d:%d=%d:%d:%d:%d:%d",&i,&s,&src,&code,&mod,&kind,&step)==7 && i>=0 && i<NUM_BIND && s>=0 && s<BIND_SLOTS) { g_bind_src[i][s]=src; g_bind_code[i][s]=code; g_bind_mod[i][s]=mod&MODMASK; g_bind_kind[i]=kind; if(kind==KIND_BRIGHT) g_bind_step[i]=step; }
                                else if (sscanf(line,"%d=%d:%d:%d:%d:%d",&i,&src,&code,&mod,&kind,&step)==6 && i>=0 && i<NUM_BIND) { g_bind_src[i][0]=src; g_bind_code[i][0]=code; g_bind_mod[i][0]=mod&MODMASK; g_bind_kind[i]=kind; if(kind==KIND_BRIGHT) g_bind_step[i]=step; }
                                else if (sscanf(line,"%d=%d:%d:%d",&i,&src,&code,&mod)==4 && i>=0 && i<NUM_BIND) { g_bind_src[i][0]=src; g_bind_code[i][0]=code; g_bind_mod[i][0]=mod&MODMASK; }
                                else if (sscanf(line,"%d=%d:%d",&i,&src,&code)==3 && i>=0 && i<NUM_BIND) { g_bind_src[i][0]=src; g_bind_code[i][0]=code; g_bind_mod[i][0]=0; }
                                else if (strncmp(line,"path=",5)==0) { strncpy(g_bind_path,line+5,sizeof g_bind_path-1); g_bind_path[strcspn(g_bind_path,"\n")]=0; }
                            }
                            g_bind_kind[BIND_BRIGHT_BIG]=KIND_BRIGHT; if(!g_bind_step[BIND_BRIGHT_BIG]) g_bind_step[BIND_BRIGHT_BIG]=50;
                            g_bind_kind[BIND_BRIGHT_MED]=KIND_BRIGHT; if(!g_bind_step[BIND_BRIGHT_MED]) g_bind_step[BIND_BRIGHT_MED]=20;
                            g_bind_kind[BIND_BRIGHT_SMALL]=KIND_BRIGHT; if(!g_bind_step[BIND_BRIGHT_SMALL]) g_bind_step[BIND_BRIGHT_SMALL]=5;
                            fclose(f); save_keymap(); snprintf(km_msg,sizeof km_msg,"已导入 epub_reader_%d", imp_list[imp_sel]);
                        }
                    }
                    st=ST_KEYMAP; break;
                }
                case A_BACK: case A_MENU: st=ST_KEYMAP; break;
                default: break;
            }
        } else if (st == ST_MODE) {
            if (act==A_BACK || act==A_MENU) st=ST_MENU;
        }
    }
    invalidate_layout_cache();  /* 含当前章 layout；r->lay 即缓存项之一，统一释放避免 double free */
    r.lay = NULL;
    free(r.title);
    if (pic_disp) SDL_FreeSurface(pic_disp);
    free(expanded);
    free(vis);
}

/* ================= 打开 EPUB（带错误屏显 + 诊断） ================= */
static void wait_back(reader_ui_t *ui) {
    SDL_Event ev;
    Uint32 start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 8000) {
        if (wait_event_timeout(&ev, 200)) {
            enum Action a = event_to_action(&ev);
            if (a == A_START) { app_suspend(ui); continue; }
            if (a == A_BACK || a == A_MENU) return;
        }
    }
}
static void open_epub(reader_ui_t *ui, const char *path, cfg_t *cfg) {
    fprintf(stderr, "[dbg] open_epub %s\n", path);
    zip_t *z = zip_open(path);
    if (!z) {
        ui_draw_error(ui, "打开失败", "无法读取文件\n可能不是合法 EPUB/zip\n或文件已损坏");
        wait_back(ui);
        return;
    }
    epub_t *ep = epub_open(z);
    fprintf(stderr, "[dbg] epub_open ret=%p spine=%d\n", (void*)ep, ep?ep->n_spine:0);
    if (ep) {
        if (ep->n_spine == 0) fprintf(stderr, "[dbg] warning: spine empty\n");
        save_lastbook(path);   /* 记录最近阅读，供下次启动续读 */
        read_book(ui, ep, path, cfg);
        fprintf(stderr, "[dbg] read_book returned\n");
        epub_close(ep);
    } else {
        ui_draw_error(ui, "解析失败", "EPUB 内部结构无法解析\n(缺失 OPF / spine 为空等)\n详见 run.log");
        wait_back(ui);
    }
    zip_close(z);
}

/* ================= 浏览器主循环 ================= */
static void run_browser(reader_ui_t *ui, cfg_t *cfg) {
    g_jbtn = 0; g_mod_kb = 0; g_mod_joy = 0;
    /* 默认打开目录：永久固化 g_default_dir（/media/sdcard/Ebook）；不存在则回退链，最后兜底 "/" */
    const char *candidate_roots[] = {g_default_dir,"/media/sdcard/Ebook","/media/roms/Ebook","/media/sdcard","/media/roms","/mnt/sd","/media",".",NULL};
    char *cwd = NULL;
    for (int i=0; candidate_roots[i]; i++) {
        struct stat st;
        if (stat(candidate_roots[i],&st)==0 && S_ISDIR(st.st_mode)) { cwd=strdup(candidate_roots[i]); break; }
    }
    if (!cwd) cwd = strdup("/");

    int quit=0;
    int diag_first = 1;
    while (!quit) {
        nav_list_t list={0};
        if (diag_first) { fprintf(stderr,"[diag] build_list(%s)... ", cwd); fflush(stderr); }
        build_list(cwd,&list);
        if (diag_first) { fprintf(stderr,"OK n=%d\n", list.n); fflush(stderr); diag_first = 0; }
        if (list.n == 0) {
            int ew=1;
            while (ew) {
                ui_draw_error(ui,"空目录","该目录没有可显示内容\nB 返回上级");
                SDL_Event ev;
                if (!wait_event_timeout(&ev,300)) continue;
                enum Action a = event_to_action(&ev);
                if (a==A_QUIT_FORCE) { quit=1; ew=0; break; }
                if (a==A_START) { app_suspend(ui); continue; }
                if (a==A_VOL) {
                    cfg->bright_pct += g_bright_delta;
                    if (cfg->bright_pct>100) cfg->bright_pct=100;
                    if (cfg->bright_pct<0) cfg->bright_pct=0;
                    ui_set_brightness(ui,cfg->bright_pct); save_config(cfg);
                    continue;
                }
                if (a==A_BACK || a==A_MENU) {
                    char *p=parent_dir(cwd);
                    if (!strcmp(p,cwd)) { quit=1; free(p); } else { free(cwd); cwd=p; }
                    ew=0;
                }
            }
            nav_free(&list);
            continue;
        }
        int sel=0;
        while (1) {
            int visible = (SCREEN_H - TITLE_H - STATUS_H - 6) / ui->line_h;
            if (visible<1) visible=1;
            int first = sel - visible/2; if (first<0) first=0;
            if (first+visible > list.n) first = list.n - visible; if (first<0) first=0;
            { char clk[8]; get_clock_str(clk, sizeof(clk)); ui_set_hud(read_battery_pct(), clk); }
            ui_draw_browser(ui, cwd, list.e, list.n, sel, first, first>0, (first+visible)<list.n);
            SDL_Event ev;
            if (!wait_event_timeout(&ev,300)) continue;
            enum Action act = event_to_action(&ev);
            if (act==A_NONE) continue;
            if (act==A_QUIT_FORCE) { quit=1; break; }
            if (act==A_START) { app_suspend(ui); continue; }
            if (act==A_VOL) {
                cfg->bright_pct += g_bright_delta;
                if (cfg->bright_pct>100) cfg->bright_pct=100;
                if (cfg->bright_pct<0) cfg->bright_pct=0;
                ui_set_brightness(ui,cfg->bright_pct); save_config(cfg);
                continue;
            }
            if (act==A_UP) { if (sel>0) sel--; }
            else if (act==A_DOWN) { if (sel<list.n-1) sel++; }
            else if (act==A_BACK) {
                char *p=parent_dir(cwd);
                if (!strcmp(p,cwd)) { quit=1; free(p); } else { free(cwd); cwd=p; strncpy(g_bind_path,cwd,sizeof g_bind_path-1); g_bind_path[sizeof g_bind_path-1]=0; }
                break;
            }
            else if (act==A_SELECT) {
                nav_ent_t *e=&list.e[sel];
                if (e->is_dir) {
                    if (!strcmp(e->name,"..")) { char *p=parent_dir(cwd); free(cwd); cwd=p; }
                    else { char *nw=malloc(strlen(cwd)+strlen(e->name)+2); sprintf(nw,"%s/%s",cwd,e->name); free(cwd); cwd=nw; }
                    strncpy(g_bind_path,cwd,sizeof g_bind_path-1); g_bind_path[sizeof g_bind_path-1]=0;
                    break;
                } else {
                    char *path=malloc(strlen(cwd)+strlen(e->name)+2);
                    sprintf(path,"%s/%s",cwd,e->name);
                    open_epub(ui,path,cfg);
                    free(path);
                    if (g_quit_app) { quit=1; break; } /* 阅读中 L1/L2+START → 直接退 App */
                }
            }
        }
        nav_free(&list);
        if (quit) break;
    }
    free(cwd);
}

/* ================= 入口 ================= */
int main(int argc, char **argv) {
    /* 日志重定向到 SD 卡（替代旧 launch.sh 行为，便于 PC 端排查；
       folder 部署改为直接 Exec=epubreader 后仍需此处保证日志落盘） */
    {
        FILE *lf = fopen("/media/roms/apps/EPUBReader/EPUBReader.log", "a");
        if (!lf) lf = fopen("/usr/local/home/.epubreader/run.log", "a");
        if (lf) {
            int fd = fileno(lf);
            dup2(fd, 1); dup2(fd, 2);
            fclose(lf);  /* fd 1/2 仍指向同一文件，关闭原描述符 */
        }
        fprintf(stderr, "=== EPUBReader 启动 %s ===\n", __DATE__ " " __TIME__);
    }
    /* 菜单注册自愈（2026-07-30 最终定案，源码 menu.cpp 实证）：
       esoteric 实际读取的 sections 目录 = $HOME/.esoteric/sections（$HOME=/usr/local/home，
       由 frontend_start 从 /etc/passwd 取得）。之前误写到 /usr/share/n/sections（出厂种子，
       运行时不读）是图标一直不显的真因。此处双保险：即使经 OPK(opkrun) 启动（不走 launch.sh），
       也把规范链接写入活目录，并清理任何指向 EPUBReader 的旧坏条目。 */
    {
        char epath[512];
        const char *home = getenv("HOME");
        if (!home || !*home) home = "/usr/local/home";
        snprintf(epath, sizeof(epath), "%s/.esoteric/sections/applications/epubreader", home);
        fprintf(stderr, "[self-heal] 活目录 sections 基址=%s/.esoteric/sections\n", home);
        /* 清理：扫描活目录所有链接，删除任何指向 EPUBReader 的残留（旧无图标/重复条目） */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "for f in \"%s/.esoteric/sections\"/*/* /usr/local/home/.esoteric/sections/*/*; do "
            "[ -f \"$f\" ] || continue; "
            "if grep -qi epubreader \"$f\" 2>/dev/null || grep -q apps/EPUBReader \"$f\" 2>/dev/null; then "
            "rm -f \"$f\"; fi; done; sync", home);
        system(cmd);
        /* 写入规范链接文件（与 sdl-palmsk 同格式：icon= 绝对路径 PNG） */
        FILE *lf = fopen(epath, "w");
        if (lf) {
            fputs("title=EPUB\xE9\x98\x85\xE8\xAF\xBB\xE5\x99\xA8\n"
                  "description=EPUB 3.0 reader v1.1.1\n"
                  "icon=/media/roms/apps/EPUBReader/epubreader_icon.png\n"
                  "exec=/usr/bin/opkrun\n"
                  "params=-m default.gcw0.desktop \"/media/roms/apps/EPUBReader/EPUBReader.opk\"\n"
                  "consoleapp=false\n"
                  "selectorbrowser=false\n", lf);
            fclose(lf);
            fprintf(stderr, "[self-heal] 已写入 esoteric 链接 %s\n", epath);
            system("sync");
        } else {
            fprintf(stderr, "[self-heal] esoteric 链接写入失败（sections 目录不存在或只读）: %s\n", epath);
        }
    }
    install_crash_handler();
    fprintf(stderr, "[diag] main() 启动, 崩溃处理器已装, build=" __DATE__ " " __TIME__ "\n"); fflush(stderr);
    selftest_crash_handler();
    setup_runtime_env();
    const char *font=NULL, *direct_file=NULL;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--font") && i+1<argc) font=argv[++i];
        else if (argv[i][0]=='-' && argv[i][1]=='-') continue;
        else { struct stat st; if (stat(argv[i],&st)==0 && ends_with(argv[i],".epub")) direct_file=argv[i]; }
    }
    reader_ui_t *ui = ui_init(font);
    if (!ui) { fprintf(stderr,"SDL/TTF 初始化失败 (缺少字体或显示设备)\n"); return 1; }
    fprintf(stderr,"[diag] ui_init 完成\n"); fflush(stderr);
    cfg_t cfg; load_config(&cfg);
    load_keymap();   /* 加载用户自定义快捷键（若有） */
    fprintf(stderr,"[diag] load_config 完成\n"); fflush(stderr);
    apply_config(ui,&cfg);
    fprintf(stderr,"[diag] apply_config 完成\n"); fflush(stderr);

    if (direct_file) {
        zip_t *z = zip_open(direct_file);
        if (!z) { fprintf(stderr,"无法打开 %s\n",direct_file); ui_quit(ui); return 1; }
        epub_t *ep = epub_open(z);
        if (!ep) { fprintf(stderr,"EPUB 解析失败: %s\n",direct_file); zip_close(z); ui_quit(ui); return 1; }
        read_book(ui, ep, direct_file, &cfg);
        epub_close(ep); zip_close(z);
        ui_quit(ui); return 0;
    }

    /* 续读：若上次退出时有阅读记录，直接进入该书的进度（其内部 load_progress 恢复章节/页） */
    {
        char *lb = load_lastbook();
        if (lb) {
            open_epub(ui, lb, &cfg);   /* open_epub 内含 save_lastbook 与错误屏 */
            free(lb);
        }
        if (g_quit_app) { ui_quit(ui); return 0; }   /* 续读中直接退出App → 不再进文件列表 */
    }

    fprintf(stderr,"[diag] 进入 run_browser\n"); fflush(stderr);
    run_browser(ui, &cfg);
    ui_quit(ui);
    return 0;
}
