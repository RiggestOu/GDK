#include "render.h"
#include "epub.h"
#include "zip.h"
#include "util.h"
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
enum Action { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_SELECT, A_BACK, A_MENU, A_BOOKMARK, A_QUIT_FORCE };

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    return strcasecmp(s + ls - lf, suf) == 0;
}

static enum Action key_to_action(int key) {
    switch (key) {
        case SDLK_UP:    case SDLK_w: return A_UP;
        case SDLK_DOWN:  case SDLK_s: return A_DOWN;
        case SDLK_LEFT:  case SDLK_a: return A_LEFT;
        case SDLK_RIGHT: case SDLK_d: return A_RIGHT;
        case SDLK_RETURN:case SDLK_SPACE: return A_SELECT;
        case SDLK_ESCAPE:case SDLK_q: return A_BACK;
        case SDLK_m: return A_MENU;
        case SDLK_k: return A_BOOKMARK;
        case SDLK_PAGEUP:   return A_UP;     /* 键盘：上一页 */
        case SDLK_PAGEDOWN: return A_DOWN;   /* 键盘：下一页 */
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

/* 把 SDL 事件转成 Action：joystick 按钮 0/1/2/3 = A/B/X/Y，HAT=方向；键盘 keymap 兼容 */
static enum Action event_to_action(const SDL_Event *ev) {
    if (ev->type == SDL_KEYDOWN) return key_to_action(ev->key.keysym.sym);
    if (ev->type == SDL_JOYBUTTONDOWN || ev->type == SDL_JOYBUTTONUP) {
        int b = ev->jbutton.button;
        int down = (ev->type == SDL_JOYBUTTONDOWN);
        if (!down) { g_btn_down &= ~(1 << b); return A_NONE; }
        g_btn_down |= (1 << b);
        /* 组合键：L1/L2 + START = 强制退出（退出时自动加书签） */
        if (b == BTN_START) {
            if (g_btn_down & (1 << BTN_L1)) return A_QUIT_FORCE;
            if (g_btn_down & (1 << BTN_L2)) return A_QUIT_FORCE;
        }
        if ((b == BTN_L1 || b == BTN_L2) && (g_btn_down & (1 << BTN_START)))
            return A_QUIT_FORCE;
        /* 单键映射 */
        switch (b) {
            case BTN_A: return A_SELECT;
            case BTN_B: return A_BACK;
            case BTN_X: return A_BOOKMARK;
            case BTN_Y: return A_MENU;
            case BTN_L1: case BTN_R1: return A_DOWN;   /* 下一页 */
            case BTN_L2: case BTN_R2: return A_UP;     /* 上一页 */
            default: return A_NONE;
        }
    }
    if (ev->type == SDL_JOYHATMOTION) {
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
static void load_bookmark(const char *book, int *sp, int *pg) {
    *sp = -1; *pg = 0;
    char *pp = sidecar_path(book, ".bookmark");
    FILE *f = fopen(pp, "r");
    if (f) { fscanf(f, "%d %d", sp, pg); fclose(f); }
    free(pp);
}
static void save_bookmark(const char *book, int sp, int pg) {
    char *pp = sidecar_path(book, ".bookmark");
    FILE *f = fopen(pp, "w");
    if (f) { fprintf(f, "%d %d\n", sp, pg); fclose(f); }
    free(pp);
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
    char **lines;
    int n_lines;
    int per_page;
    int total_pages;
    char *title;
} reading_t;

static void free_lines(reading_t *r) {
    if (r->lines) { for (int i=0;i<r->n_lines;i++) free(r->lines[i]); free(r->lines); r->lines=NULL; }
    r->n_lines=0;
}
static void open_chapter(reader_ui_t *ui, reading_t *r, int idx) {
    if (idx<0 || idx>=r->ep->n_spine) return;
    free_lines(r);
    r->spine_idx = idx;
    char *text = epub_read_text(r->ep, r->ep->spine[idx]);
    if (!text) text = strdup("(空章节)");
    r->lines = wrap_text(ui, text, &r->n_lines);
    free(text);
    r->per_page = ui_lines_per_page(ui);
    r->total_pages = (r->n_lines + r->per_page - 1) / r->per_page;
    if (r->page >= r->total_pages) r->page = r->total_pages - 1;
    if (r->page < 0) r->page = 0;
}

typedef enum { ST_READ, ST_TOC, ST_MENU, ST_COLOR, ST_FONTSZ, ST_BRIGHT, ST_MODE } rstate;

static void read_book(reader_ui_t *ui, epub_t *ep, const char *book, cfg_t *cfg) {
    reading_t r; memset(&r,0,sizeof(r)); r.ep=ep;
    g_btn_down = 0; /* 清空按键状态，避免跨文件残留触发组合键 */
    char *nm = strrchr(book,'/'); nm = nm?nm+1:(char*)book;
    r.title = strdup(nm);

    int sp=0, pg=0; load_progress(book,&sp,&pg);
    if (sp>=ep->n_spine) sp=0;
    open_chapter(ui,&r,sp);
    r.page = pg;

    int bm_sp=-1, bm_pg=0; load_bookmark(book,&bm_sp,&bm_pg);
    int has_bm = (bm_sp>=0);

    rstate st = ST_READ;
    int toc_sel=0, menu_sel=0, sub_sel=0;
    int quit=0;
    while (!quit) {
        if (st == ST_READ) {
            int pct = r.total_pages>0 ? (r.page+1)*100/r.total_pages : 0;
            ui_draw_reader(ui, r.lines, r.n_lines, r.page, r.per_page, r.title, pct, has_bm);
        } else if (st == ST_TOC) {
            int n = ep->n_toc<64?ep->n_toc:64;
            menu_item_t items[64];
            for (int i=0;i<n;i++) items[i]=(menu_item_t){ep->toc[i].label,"",1};
            ui_draw_menu(ui,"目录",items,n,toc_sel);
        } else if (st == ST_MENU) {
            menu_item_t items[8]; int mn=0;
            items[mn++]=(menu_item_t){"目录","",1};
            items[mn++]=(menu_item_t){"文字颜色",fg_labels[cfg->fg_index],1};
            items[mn++]=(menu_item_t){"字号",font_labels[cfg->font_index],1};
            items[mn++]=(menu_item_t){"亮度",bright_labels[cfg->bright_index],1};
            items[mn++]=(menu_item_t){"阅读模式","敬请期待",0};
            if (has_bm) items[mn++]=(menu_item_t){"跳到书签","",1};
            items[mn++]=(menu_item_t){"退出App","自动书签",1};
            ui_draw_menu(ui,"菜单",items,mn,menu_sel);
        } else if (st == ST_COLOR) {
            menu_item_t items[5];
            for (int i=0;i<5;i++) items[i]=(menu_item_t){fg_labels[i], i==cfg->fg_index?"●":"",1};
            ui_draw_menu(ui,"文字颜色",items,5,sub_sel);
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

        /* 强制退出（L1/L2 + START）：任意界面生效，退出时自动加书签 */
        if (act == A_QUIT_FORCE) {
            save_bookmark(book, r.spine_idx, r.page);
            has_bm = 1;
            quit = 1;
            continue;
        }

        if (st == ST_READ) {
            switch (act) {
                case A_UP:    if (r.page>0){r.page--; save_progress(book,r.spine_idx,r.page);} break;
                case A_DOWN:  if (r.page<r.total_pages-1){r.page++; save_progress(book,r.spine_idx,r.page);} break;
                case A_SELECT:if (r.page<r.total_pages-1){r.page++; save_progress(book,r.spine_idx,r.page);} break; /* A=下一页 */
                case A_LEFT:  if (r.spine_idx>0){open_chapter(ui,&r,r.spine_idx-1); r.page=0; save_progress(book,r.spine_idx,r.page);} break;
                case A_RIGHT: if (r.spine_idx<ep->n_spine-1){open_chapter(ui,&r,r.spine_idx+1); r.page=0; save_progress(book,r.spine_idx,r.page);} break;
                case A_BACK:  quit=1; break;
                case A_MENU:  st=ST_MENU; menu_sel=0; break;
                case A_BOOKMARK:
                    if (has_bm && bm_sp==r.spine_idx && bm_pg==r.page) { save_bookmark(book,-1,0); has_bm=0; }
                    else { save_bookmark(book,r.spine_idx,r.page); bm_sp=r.spine_idx; bm_pg=r.page; has_bm=1; }
                    break;
                default: break;
            }
        } else if (st == ST_TOC) {
            int n = ep->n_toc;
            switch (act) {
                case A_UP:   if (toc_sel>0) toc_sel--; break;
                case A_DOWN: if (toc_sel<n-1) toc_sel++; break;
                case A_SELECT: case A_MENU: {
                    int idx = epub_find_spine(ep, ep->toc[toc_sel].href);
                    if (idx>=0){ open_chapter(ui,&r,idx); r.page=0; save_progress(book,r.spine_idx,r.page); }
                    st=ST_READ; break;
                }
                case A_BACK: st=ST_MENU; break;
                default: break;
            }
        } else if (st == ST_MENU) {
            int n = 6 + (has_bm?1:0);
            int exit_idx = n - 1;
            switch (act) {
                case A_UP:   if (menu_sel>0) menu_sel--; break;
                case A_DOWN: if (menu_sel<n-1) menu_sel++; break;
                case A_SELECT: case A_MENU:
                    if (menu_sel == exit_idx) { save_bookmark(book, r.spine_idx, r.page); has_bm=1; quit=1; break; }
                    if      (menu_sel==0) { st=ST_TOC; toc_sel=0; }
                    else if (menu_sel==1) { st=ST_COLOR;  sub_sel=cfg->fg_index; }
                    else if (menu_sel==2) { st=ST_FONTSZ; sub_sel=cfg->font_index; }
                    else if (menu_sel==3) { st=ST_BRIGHT; sub_sel=cfg->bright_index; }
                    else if (menu_sel==4) { st=ST_MODE; }
                    else if (menu_sel==5 && has_bm) {
                        if (bm_sp>=0 && bm_sp<ep->n_spine){ open_chapter(ui,&r,bm_sp); r.page=bm_pg; if(r.page>=r.total_pages)r.page=r.total_pages-1; save_progress(book,r.spine_idx,r.page); }
                        st=ST_READ;
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
                    open_chapter(ui,&r,r.spine_idx); r.page=0; save_progress(book,r.spine_idx,r.page);
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
    free_lines(&r);
    free(r.title);
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
    g_btn_down = 0;
    const char *candidate_roots[] = {"/media/roms","/media/sdcard","/mnt/sd","/media",".",NULL};
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
