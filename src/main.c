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

/* ================= 配置与配色表 ================= */
typedef struct { int font_index; int fg_index; int bright_index; } cfg_t;

static const int   font_sizes[4] = {12, 14, 18, 22};
static const char *font_labels[4] = {"小", "中", "大", "特大"};
static const int   fg_colors[5][3] = {{235,235,235},{255,225,120},{150,240,150},{150,220,255},{255,160,200}};
static const char *fg_labels[5] = {"白", "黄", "绿", "青", "粉"};
static const int   brights[5] = {30, 50, 70, 90, 100};
static const char *bright_labels[5] = {"30%", "50%", "70%", "90%", "100%"};

/* ================= 动作 ================= */
enum Action { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_SELECT, A_BACK, A_MENU, A_BOOKMARK, A_PIC, A_QUIT_FORCE };

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    return strcasecmp(s + ls - lf, suf) == 0;
}

/* GDK mini 物理键→键盘事件映射（run.log 实测 + OpenDingux 标准）：
   A=LCTRL(306) B=LALT(308) X=LSHIFT(304) Y=SPACE(32)
   L1=TAB(9) R1=BACKSPACE(8) START=RETURN(13) SELECT=ESC(27) 方向=光标键 */
static enum Action key_to_action(int key) {
    switch (key) {
        case SDLK_UP:    case SDLK_w: return A_UP;
        case SDLK_DOWN:  case SDLK_s: return A_DOWN;
        case SDLK_LEFT:  case SDLK_a: return A_LEFT;
        case SDLK_RIGHT: case SDLK_d: return A_RIGHT;
        case SDLK_LCTRL:                          return A_SELECT;   /* A 键 */
        case SDLK_RETURN:                         return A_SELECT;   /* START */
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
    if (ev->type == SDL_KEYDOWN)
        fprintf(stderr, "[dbg] KEYDOWN sym=%d\n", (int)ev->key.keysym.sym);
    else if (ev->type == SDL_JOYBUTTONDOWN)
        fprintf(stderr, "[dbg] JOYBUTTON button=%d\n", ev->jbutton.button);
    else if (ev->type == SDL_JOYHATMOTION)
        fprintf(stderr, "[dbg] JOYHAT value=%d\n", ev->jhat.value);
    else
        fprintf(stderr, "[dbg] event type=%d\n", ev->type);
}

static int wait_event_timeout(SDL_Event *ev, Uint32 timeout_ms) {
    Uint32 start = SDL_GetTicks();
    for (;;) {
        if (SDL_PollEvent(ev)) { dbg_event(ev); return 1; }
        if ((SDL_GetTicks() - start) >= timeout_ms) return 0;
        SDL_Delay(10);
    }
}

/* OpenDingux/GCW Zero 系手柄按键索引（不同设备顺序可能略有差异；
   若 L1/R1/L2/R2/START 不对应，查看设备 run.log 的 [dbg] JOYBUTTON button=N 调整此处） */
#define BTN_A 0
#define BTN_B 1
#define BTN_X 2
#define BTN_Y 3
#define BTN_L1 4
#define BTN_R1 5
#define BTN_L2 6
#define BTN_R2 7
#define BTN_START 8
#define BTN_SELECT 9

/* 记录当前按下的手柄键（位掩码），用于组合键判定 */
static int g_btn_down = 0;

/* 键盘事件版组合键状态（GDK mini 实机把手柄键发成键盘事件）：
   L1=TAB  L2=PAGEUP  R1=BACKSPACE  R2=PAGEDOWN  START=RETURN */
#define KMOD_L1    (1 << 0)
#define KMOD_L2    (1 << 1)
#define KMOD_START (1 << 2)
#define KMOD_R1    (1 << 3)
#define KMOD_R2    (1 << 4)
static int g_key_down = 0;
/* 本轮肩键按住期间是否产生过组合动作；若没有，则肩键单按抬起=翻页 */
static int g_shoulder_combo = 0;

/* App 级退出标志：阅读中按 L1/L2+START 也要直接退出整个 App（不是退回浏览器） */
static int g_quit_app = 0;

/* 把 SDL 事件转成 Action：joystick 按钮 0/1/2/3 = A/B/X/Y，HAT=方向；键盘 keymap 兼容 */
static enum Action event_to_action(const SDL_Event *ev) {
    if (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) {
        int sym  = (int)ev->key.keysym.sym;
        int down = (ev->type == SDL_KEYDOWN);
        int bit  = 0;
        if (sym == SDLK_TAB)         bit = KMOD_L1;    /* L1 */
        else if (sym == SDLK_PAGEUP) bit = KMOD_L2;    /* L2（有的固件 L2=PageUp） */
        else if (sym == SDLK_RETURN) bit = KMOD_START; /* START */
        else if (sym == SDLK_BACKSPACE) bit = KMOD_R1; /* R1 */
        else if (sym == SDLK_PAGEDOWN)  bit = KMOD_R2; /* R2 */
        if (bit) {
            if (down) {
                if (!(g_key_down & bit)) g_shoulder_combo = 0; /* 新一次肩键按下，清空组合标志 */
                g_key_down |= bit;
            } else {
                g_key_down &= ~bit;
                /* 肩键抬起：本轮没产生过组合 → 单按 = 翻页 */
                if (!g_key_down && !g_shoulder_combo) {
                    if (bit == KMOD_L1 || bit == KMOD_R1) return A_DOWN;  /* L1/R1 = 下一页 */
                    if (bit == KMOD_L2 || bit == KMOD_R2) return A_UP;    /* L2/R2 = 上一页 */
                }
                return A_NONE;
            }
        }
        if (!down) return A_NONE;
        /* 组合键：L1+START / L2+START = 强制退出（自动书签） */
        if (sym == SDLK_RETURN && (g_key_down & (KMOD_L1 | KMOD_L2))) { g_shoulder_combo = 1; return A_QUIT_FORCE; }
        if ((sym == SDLK_TAB || sym == SDLK_PAGEUP) && (g_key_down & KMOD_START)) { g_shoulder_combo = 1; return A_QUIT_FORCE; }
        /* 图片层：L1+L2 / R1+R2 = 进/出图片缩放界面 */
        if ((g_key_down & (KMOD_L1 | KMOD_L2)) == (KMOD_L1 | KMOD_L2)) { g_shoulder_combo = 1; return A_PIC; }
        if ((g_key_down & (KMOD_R1 | KMOD_R2)) == (KMOD_R1 | KMOD_R2)) { g_shoulder_combo = 1; return A_PIC; }
        /* 普通键：若正按住肩键则标记组合（避免抬起误翻页） */
        enum Action ka = key_to_action(sym);
        if (ka != A_NONE && (g_key_down & (KMOD_L1 | KMOD_L2 | KMOD_R1 | KMOD_R2))) g_shoulder_combo = 1;
        return ka;
    }
    if (ev->type == SDL_JOYBUTTONDOWN || ev->type == SDL_JOYBUTTONUP) {
        int b = ev->jbutton.button;
        int down = (ev->type == SDL_JOYBUTTONDOWN);
        if (!down) {
            g_btn_down &= ~(1 << b);
            /* 肩键抬起：本轮无组合 → 单按翻页 */
            int sh = (1 << BTN_L1) | (1 << BTN_L2) | (1 << BTN_R1) | (1 << BTN_R2);
            if ((b==BTN_L1||b==BTN_L2||b==BTN_R1||b==BTN_R2) && (g_btn_down & sh)==0 && !g_shoulder_combo) {
                if (b==BTN_L1||b==BTN_R1) return A_DOWN;  /* L1/R1 = 下一页 */
                else return A_UP;                          /* L2/R2 = 上一页 */
            }
            return A_NONE;
        }
        if (!((g_btn_down >> b) & 1)) g_shoulder_combo = 0; /* 新一次按下 */
        g_btn_down |= (1 << b);
        /* 组合键：L1/L2 + START = 强制退出（退出时自动加书签） */
        if (b == BTN_START) {
            if (g_btn_down & (1 << BTN_L1)) { g_shoulder_combo = 1; return A_QUIT_FORCE; }
            if (g_btn_down & (1 << BTN_L2)) { g_shoulder_combo = 1; return A_QUIT_FORCE; }
        }
        if ((b == BTN_L1 || b == BTN_L2) && (g_btn_down & (1 << BTN_START))) { g_shoulder_combo = 1; return A_QUIT_FORCE; }
        /* 图片层：L1+L2 / R1+R2 = 进/出图片缩放界面 */
        if ((g_btn_down & ((1 << BTN_L1) | (1 << BTN_L2))) == ((1 << BTN_L1) | (1 << BTN_L2))) { g_shoulder_combo = 1; return A_PIC; }
        if ((g_btn_down & ((1 << BTN_R1) | (1 << BTN_R2))) == ((1 << BTN_R1) | (1 << BTN_R2))) { g_shoulder_combo = 1; return A_PIC; }
        int sh = (1 << BTN_L1) | (1 << BTN_L2) | (1 << BTN_R1) | (1 << BTN_R2);
        if (g_btn_down & sh) g_shoulder_combo = 1; /* 按住肩键时按其它键=组合 */
        /* 单键映射 */
        switch (b) {
            case BTN_A: return A_SELECT;
            case BTN_B: return A_BACK;
            case BTN_X: return A_BOOKMARK;
            case BTN_Y: return A_MENU;
            case BTN_L1: case BTN_L2: case BTN_R1: case BTN_R2: return A_NONE; /* 单肩无操作，专用于图片功能 */
            default: return A_NONE;
        }
    }
    if (ev->type == SDL_JOYHATMOTION) {
        int sh = g_btn_down & ((1 << BTN_L1) | (1 << BTN_L2) | (1 << BTN_R1) | (1 << BTN_R2));
        if (sh) g_shoulder_combo = 1; /* 肩键+方向=组合（选图），避免抬起误翻页 */
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

/* ================= 配置读写 ================= */
static void load_config(cfg_t *c) {
    c->font_index = 1; c->fg_index = 0; c->bright_index = 4; /* 默认 中/白/100% */
    FILE *f = fopen("./config.cfg", "r");
    if (f) {
        int v; char line[128];
        while (fgets(line, sizeof(line), f)) {
            if      (sscanf(line, "font_index=%d", &v) == 1) c->font_index = v;
            else if (sscanf(line, "fg_index=%d", &v) == 1)   c->fg_index = v;
            else if (sscanf(line, "bright_index=%d", &v) == 1) c->bright_index = v;
        }
        fclose(f);
    }
    if (c->font_index < 0 || c->font_index > 3) c->font_index = 1;
    if (c->fg_index   < 0 || c->fg_index   > 4) c->fg_index = 0;
    if (c->bright_index < 0 || c->bright_index > 4) c->bright_index = 4;
}
static void save_config(const cfg_t *c) {
    FILE *f = fopen("./config.cfg", "w");
    if (f) {
        fprintf(f, "font_index=%d\nfg_index=%d\nbright_index=%d\n", c->font_index, c->fg_index, c->bright_index);
        fclose(f);
    }
}
static void apply_config(reader_ui_t *ui, const cfg_t *c) {
    fprintf(stderr,"[diag] apply_config 入口 font_index=%d\n", c->font_index); fflush(stderr);
    ui_set_font_size(ui, font_sizes[c->font_index]);
    fprintf(stderr,"[diag] apply_config: set_font_size 返回\n"); fflush(stderr);
    ui_set_fg(ui, fg_colors[c->fg_index][0], fg_colors[c->fg_index][1], fg_colors[c->fg_index][2]);
    ui_set_brightness(ui, brights[c->bright_index]);
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
/* 多书签：.bookmark 文件每行一条 "spine页 page页" */
#define MAX_BM 32
typedef struct { int sp, pg; } bm_t;
static int load_bookmarks(const char *book, bm_t *bms) {
    int n = 0;
    char *pp = sidecar_path(book, ".bookmark");
    FILE *f = fopen(pp, "r");
    if (f) {
        int sp, pg;
        while (n < MAX_BM && fscanf(f, "%d %d", &sp, &pg) == 2)
            if (sp >= 0) { bms[n].sp = sp; bms[n].pg = pg; n++; }
        fclose(f);
    }
    free(pp);
    return n;
}
static void save_bookmarks(const char *book, const bm_t *bms, int n) {
    char *pp = sidecar_path(book, ".bookmark");
    FILE *f = fopen(pp, "w");
    if (f) {
        for (int i = 0; i < n; i++) fprintf(f, "%d %d\n", bms[i].sp, bms[i].pg);
        fclose(f);
    }
    free(pp);
}
static int bm_find(const bm_t *bms, int n, int sp, int pg) {
    for (int i = 0; i < n; i++) if (bms[i].sp == sp && bms[i].pg == pg) return i;
    return -1;
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

/* 取某章排版结果：命中缓存直接返回，否则排版并缓存（缓存满则淘汰最旧项） */
static layout_t *get_layout(reader_ui_t *ui, epub_t *ep, int idx) {
    for (int i = 0; i < LAY_CACHE_N; i++)
        if (g_lay_cache[i].lay && g_lay_cache[i].spine == idx) return g_lay_cache[i].lay;
    char *html = epub_read_html(ep, ep->spine[idx]);
    if (!html) html = strdup("<p>(空章节)</p>");
    layout_t *L = layout_chapter(ui, ep, html, ep->spine[idx]);
    free(html);
    int slot = -1;
    for (int i = 0; i < LAY_CACHE_N; i++) if (!g_lay_cache[i].lay) { slot = i; break; }
    if (slot < 0) { layout_free(g_lay_cache[0].lay); slot = 0; }
    g_lay_cache[slot].spine = idx;
    g_lay_cache[slot].lay = L;
    return L;
}

/* 清空并释放排版缓存（换书 / 改字号等需重排时调用） */
static void invalidate_layout_cache(void) {
    for (int i = 0; i < LAY_CACHE_N; i++)
        if (g_lay_cache[i].lay) { layout_free(g_lay_cache[i].lay); g_lay_cache[i].lay = NULL; }
}

static void open_chapter(reader_ui_t *ui, reading_t *r, int idx) {
    if (idx<0 || idx>=r->ep->n_spine) return;
    r->spine_idx = idx;
    r->lay = get_layout(ui, r->ep, idx);
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

typedef enum { ST_READ, ST_TOC, ST_MENU, ST_COLOR, ST_FONTSZ, ST_BRIGHT, ST_MODE, ST_BMLIST, ST_PICVIEW } rstate;

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
/* 按新缩放系数重建显示 surface（仅 zoom 变化时调用，平移不重建） */
static void zoom_picview(reader_ui_t *ui, SDL_Surface **disp, int *zoom, int *ox, int *oy, SDL_Surface *full, int newz) {
    (void)ox; (void)oy;
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
    ui_flip(ui);
}

static void read_book(reader_ui_t *ui, epub_t *ep, const char *book, cfg_t *cfg) {
    reading_t r; memset(&r,0,sizeof(r)); r.ep=ep;
    invalidate_layout_cache();  /* 释放上一本书遗留的排版缓存 */
    g_btn_down = 0; g_key_down = 0; /* 清空按键状态，避免跨文件残留触发组合键 */
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
    int quit=0;

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
        if (st == ST_READ) {
            int pct = total_pages>0 ? (r.page+1)*100/total_pages : 0;
            int focus_line = (np>0) ? r.lay->pics[page_pics[pic_focus]].line : -1;
            char flbl[32]; if (np>0) snprintf(flbl, sizeof(flbl), "图 %d/%d", pic_focus+1, np); else flbl[0]=0;
            ui_draw_reader_layout(ui, r.lay, r.page, r.title, pct, at_bm, focus_line, np>0?flbl:NULL);
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
            static char blbl[MAX_BM][64];
            menu_item_t items[MAX_BM + 1];
            if (n_bm == 0) {
                items[0] = (menu_item_t){"(无书签, 阅读中按 X 添加)","",0};
                ui_draw_menu(ui,"书签",items,1,0);
            } else {
                for (int i=0;i<n_bm;i++) {
                    snprintf(blbl[i], sizeof(blbl[0]), "书签%d  第%d章 第%d页", i+1, bms[i].sp+1, bms[i].pg+1);
                    items[i]=(menu_item_t){blbl[i],"",1};
                }
                if (bm_sel >= n_bm) bm_sel = n_bm - 1;
                ui_draw_menu(ui,"书签 [A跳转 X删除]",items,n_bm,bm_sel);
            }
        } else if (st == ST_MENU) {
            char bmv[16]; snprintf(bmv,sizeof(bmv),"%d",n_bm);
            menu_item_t items[8]; int mn=0;
            items[mn++]=(menu_item_t){"目录","",1};
            items[mn++]=(menu_item_t){"书签",bmv,1};
            items[mn++]=(menu_item_t){"正文颜色",fg_labels[cfg->fg_index],1};
            items[mn++]=(menu_item_t){"字号",font_labels[cfg->font_index],1};
            items[mn++]=(menu_item_t){"亮度",bright_labels[cfg->bright_index],1};
            items[mn++]=(menu_item_t){"退出App","自动书签",1};
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
            menu_item_t items[5];
            for (int i=0;i<5;i++) items[i]=(menu_item_t){bright_labels[i], i==cfg->bright_index?"●":"",1};
            ui_draw_menu(ui,"亮度",items,5,sub_sel);
        } else if (st == ST_MODE) {
            menu_item_t items[1] = {{"阅读模式: 敬请期待","",0}};
            ui_draw_menu(ui,"阅读模式",items,1,0);
        }

        SDL_Event ev;
        if (!wait_event_timeout(&ev,300)) continue;
        enum Action act = event_to_action(&ev);
        if (act == A_NONE) continue;

        /* 强制退出（L1/L2 + START）：任意界面生效，直接退出整个 App，自动加书签 */
        if (act == A_QUIT_FORCE) {
            if (bm_find(bms, n_bm, r.spine_idx, r.page) < 0 && n_bm < MAX_BM) {
                bms[n_bm].sp = r.spine_idx; bms[n_bm].pg = r.page; n_bm++;
                save_bookmarks(book, bms, n_bm);
            }
            save_progress(book, r.spine_idx, r.page);
            g_quit_app = 1;
            quit = 1;
            continue;
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
            int shoulder = (g_key_down & (KMOD_L1 | KMOD_R1));
            if (shoulder && np > 0) {
                int dir = 0;
                if (act == A_UP || act == A_LEFT || act == A_MENU || act == A_BOOKMARK) dir = -1;
                else if (act == A_DOWN || act == A_RIGHT || act == A_BACK || act == A_SELECT) dir = +1;
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
                case A_BACK:  quit=1; break;
                case A_MENU:  st=ST_MENU; menu_sel=0; break;
                case A_BOOKMARK: {
                    int at = bm_find(bms, n_bm, r.spine_idx, r.page);
                    if (at >= 0) { /* 已有 → 删除（切换语义） */
                        for (int i=at;i<n_bm-1;i++) bms[i]=bms[i+1];
                        n_bm--;
                    } else if (n_bm < MAX_BM) {
                        bms[n_bm].sp=r.spine_idx; bms[n_bm].pg=r.page; n_bm++;
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
            int shoulder = (g_key_down & (KMOD_L1 | KMOD_R1));
            if (shoulder) {
                int nz = pic_zoom;
                if (act == A_UP || act == A_BOOKMARK)        nz += 1;   /* L1+↑ / R1+Y 放大1% */
                else if (act == A_DOWN || act == A_BACK)     nz -= 1;   /* L1+↓ / R1+B 缩小1% */
                else if (act == A_RIGHT || act == A_SELECT)  nz += 10;  /* L1+→ / R1+A 放大10% */
                else if (act == A_LEFT || act == A_MENU)     nz -= 10;  /* L1+← / R1+X 缩小10% */
                if (nz < 20) nz = 20; if (nz > 400) nz = 400;
                if (nz != pic_zoom) zoom_picview(ui, &pic_disp, &pic_zoom, &pic_ox, &pic_oy, pic_full, nz);
            } else {
                int step = 24;
                /* 镜头语义：↑=镜头上→图下；↓=镜头下→图上；←=镜头左→图右；→=镜头右→图左 */
                if (act == A_UP || act == A_BOOKMARK)       pic_oy += step;  /* 图下 */
                else if (act == A_DOWN || act == A_BACK)    pic_oy -= step;  /* 图上 */
                else if (act == A_LEFT || act == A_MENU)    pic_ox += step;  /* 图右 */
                else if (act == A_RIGHT || act == A_SELECT) pic_ox -= step;  /* 图左 */
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
            int n = 6;
            switch (act) {
                case A_UP:   if (menu_sel>0) menu_sel--; break;
                case A_DOWN: if (menu_sel<n-1) menu_sel++; break;
                case A_SELECT: case A_MENU:
                    if      (menu_sel==0) { st=ST_TOC; toc_sel=0; }
                    else if (menu_sel==1) { st=ST_BMLIST; bm_sel=0; }
                    else if (menu_sel==2) { st=ST_COLOR;  sub_sel=cfg->fg_index; }
                    else if (menu_sel==3) { st=ST_FONTSZ; sub_sel=cfg->font_index; }
                    else if (menu_sel==4) { st=ST_BRIGHT; sub_sel=cfg->bright_index; }
                    else if (menu_sel==5) { /* 退出App（整个程序），自动书签 */
                        if (bm_find(bms, n_bm, r.spine_idx, r.page) < 0 && n_bm < MAX_BM) {
                            bms[n_bm].sp=r.spine_idx; bms[n_bm].pg=r.page; n_bm++;
                            save_bookmarks(book, bms, n_bm);
                        }
                        save_progress(book, r.spine_idx, r.page);
                        g_quit_app=1; quit=1;
                    }
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
                case A_UP:   if (sub_sel>0) sub_sel--; break;
                case A_DOWN: if (sub_sel<4) sub_sel++; break;
                case A_SELECT: case A_MENU:
                    cfg->bright_index=sub_sel; ui_set_brightness(ui,brights[sub_sel]); save_config(cfg); st=ST_MENU; break;
                case A_BACK: st=ST_MENU; break;
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
    if (ep) {
        if (ep->n_spine == 0) fprintf(stderr, "[dbg] warning: spine empty\n");
        read_book(ui, ep, path, cfg);
        epub_close(ep);
    } else {
        ui_draw_error(ui, "解析失败", "EPUB 内部结构无法解析\n(缺失 OPF / spine 为空等)\n详见 run.log");
        wait_back(ui);
    }
    zip_close(z);
}

/* ================= 浏览器主循环 ================= */
static void run_browser(reader_ui_t *ui, cfg_t *cfg) {
    g_btn_down = 0; g_key_down = 0;
    /* 默认打开目录：优先 /media/sdcard/Ebook（用户书库），逐级回退 */
    const char *candidate_roots[] = {"/media/sdcard/Ebook","/media/roms/Ebook","/media/sdcard","/media/roms","/mnt/sd","/media",".",NULL};
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
            ui_draw_browser(ui, cwd, list.e, list.n, sel, first, first>0, (first+visible)<list.n);
            SDL_Event ev;
            if (!wait_event_timeout(&ev,300)) continue;
            enum Action act = event_to_action(&ev);
            if (act==A_NONE) continue;
            if (act==A_QUIT_FORCE) { quit=1; break; }
            if (act==A_UP) { if (sel>0) sel--; }
            else if (act==A_DOWN) { if (sel<list.n-1) sel++; }
            else if (act==A_BACK) {
                char *p=parent_dir(cwd);
                if (!strcmp(p,cwd)) { quit=1; free(p); } else { free(cwd); cwd=p; }
                break;
            }
            else if (act==A_SELECT) {
                nav_ent_t *e=&list.e[sel];
                if (e->is_dir) {
                    if (!strcmp(e->name,"..")) { char *p=parent_dir(cwd); free(cwd); cwd=p; }
                    else { char *nw=malloc(strlen(cwd)+strlen(e->name)+2); sprintf(nw,"%s/%s",cwd,e->name); free(cwd); cwd=nw; }
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

    fprintf(stderr,"[diag] 进入 run_browser\n"); fflush(stderr);
    run_browser(ui, &cfg);
    ui_quit(ui);
    return 0;
}
