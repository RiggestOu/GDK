#include "render.h"
#include "epub.h"
#include "zip.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

enum Action { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_SELECT, A_BACK };

static enum Action key_to_action(int key) {
    switch (key) {
        case SDLK_UP:    case SDLK_w: return A_UP;
        case SDLK_DOWN:  case SDLK_s: return A_DOWN;
        case SDLK_LEFT:  case SDLK_a: return A_LEFT;
        case SDLK_RIGHT: case SDLK_d: return A_RIGHT;
        case SDLK_RETURN:case SDLK_SPACE: return A_SELECT;
        case SDLK_ESCAPE:case SDLK_q: return A_BACK;
        default: return A_NONE;
    }
}

/* SDL1.2 没有 SDL_WaitEventTimeout,用 PollEvent + Delay 模拟相同的带超时等待 */
static int wait_event_timeout(SDL_Event *ev, Uint32 timeout_ms) {
    Uint32 start = SDL_GetTicks();
    for (;;) {
        if (SDL_PollEvent(ev)) return 1;
        if ((SDL_GetTicks() - start) >= timeout_ms) return 0;
        SDL_Delay(10);
    }
}

/* IUX/OpenDingux 设备无桌面环境，SDL 必须显式指定帧缓冲驱动。
   在程序启动最早期设置，确保即使 IUX 直接启动本二进制（绕过 .dge 启动器）
   也能正确初始化显示设备。动态库路径由链接期 rpath 负责，这里不处理。 */
static void setup_runtime_env(void) {
    if (!getenv("SDL_VIDEODRIVER")) setenv("SDL_VIDEODRIVER", "fbcon", 1);
    if (!getenv("SDL_FBDEV"))       setenv("SDL_FBDEV", "/dev/fb0", 1);
    if (!getenv("SDL_NOMOUSE"))     setenv("SDL_NOMOUSE", "1", 1);
}

/* ---------- 可导航文件浏览器（支持进入任意目录） ---------- */
static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    return strcasecmp(s + ls - lf, suf) == 0;
}

typedef struct { char *name; int is_dir; } nav_ent_t;
typedef struct { nav_ent_t *e; int n, cap; } nav_list_t;

static void nav_add(nav_list_t *l, const char *name, int is_dir) {
    if (l->n >= l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->e = realloc(l->e, l->cap * sizeof(nav_ent_t)); }
    l->e[l->n].name = strdup(name);
    l->e[l->n].is_dir = is_dir;
    l->n++;
}
static void nav_free(nav_list_t *l) {
    for (int i = 0; i < l->n; i++) free(l->e[i].name);
    free(l->e);
    l->e = NULL; l->n = l->cap = 0;
}
/* 返回父目录（新分配字符串；根 "/" 的父仍是 "/"） */
static char *parent_dir(const char *d) {
    size_t n = strlen(d);
    while (n > 1 && d[n-1] == '/') n--;
    const char *slash = strrchr(d, '/');
    if (!slash || slash == d) return strdup("/");
    char *res = malloc(slash - d + 1);
    memcpy(res, d, slash - d);
    res[slash - d] = '\0';
    return res;
}
/* 构建当前目录列表：先 ".."（非根），再子目录，再 .epub 文件（不限深度） */
static void build_list(const char *dir, nav_list_t *l) {
    if (strcmp(dir, "/") != 0) nav_add(l, "..", 1);
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *ent;
    char buf[4096];
    while ((ent = readdir(dp))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(buf, sizeof(buf), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(buf, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) nav_add(l, ent->d_name, 1);
        else if (ends_with(ent->d_name, ".epub")) nav_add(l, ent->d_name, 0);
    }
    closedir(dp);
}
/* read_book 在本文件较后面定义，这里前置声明以便 open_epub 调用 */
static void read_book(reader_ui_t *ui, epub_t *ep, const char *book);

/* 打开一个 epub 的完整路径 */
static void open_epub(reader_ui_t *ui, const char *path) {
    zip_t *z = zip_open(path);
    if (!z) return;
    epub_t *ep = epub_open(z);
    if (ep) { read_book(ui, ep, path); epub_close(ep); }
    zip_close(z);
}

/* ---------- 绘制：可导航文件浏览器 ---------- */
static void draw_nav(reader_ui_t *ui, const char *cwd, nav_list_t *l, int sel) {
    ui_clear(ui);
    ui_rect(ui, 0, 0, SCREEN_W, ui->title_h, 40, 60, 110);
    char title[60];
    snprintf(title, sizeof(title), "浏览: %s", cwd);
    ui_text(ui, ui->margin, 1, title);
    int y = ui->title_h + 2;
    int visible = (SCREEN_H - ui->title_h - ui->prog_h) / ui->line_h;
    int first = sel - visible / 2; if (first < 0) first = 0;
    for (int i = first; i < l->n && (i - first) < visible; i++) {
        char disp[80];
        if (l->e[i].is_dir) {
            if (!strcmp(l->e[i].name, "..")) snprintf(disp, sizeof(disp), "[..] 上级目录");
            else snprintf(disp, sizeof(disp), "[%s]", l->e[i].name);
        } else {
            snprintf(disp, sizeof(disp), "%s", l->e[i].name);
        }
        if (i == sel) { ui_rect(ui, 0, y, SCREEN_W, ui->line_h, 60, 90, 150); ui_text(ui, ui->margin, y, disp); }
        else ui_text(ui, ui->margin, y, disp);
        y += ui->line_h;
    }
    ui_flip(ui);
}

/* ---------- 进度存取 ---------- */
static char *progress_path(const char *book) {
    size_t n = strlen(book);
    char *p = malloc(n + 9);
    strcpy(p, book);
    if (ends_with(p, ".epub")) strcpy(p + n - 5, ".progress");
    else strcat(p, ".progress");
    return p;
}
static void load_progress(const char *book, int *spine, int *page) {
    *spine = 0; *page = 0;
    char *pp = progress_path(book);
    FILE *f = fopen(pp, "r");
    if (f) { fscanf(f, "%d %d", spine, page); fclose(f); }
    free(pp);
}
static void save_progress(const char *book, int spine, int page) {
    char *pp = progress_path(book);
    FILE *f = fopen(pp, "w");
    if (f) { fprintf(f, "%d %d\n", spine, page); fclose(f); }
    free(pp);
}

/* ---------- 阅读状态 ---------- */
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
    if (r->lines) { for (int i = 0; i < r->n_lines; i++) free(r->lines[i]); free(r->lines); r->lines = NULL; }
    r->n_lines = 0;
}
static void open_chapter(reader_ui_t *ui, reading_t *r, int idx) {
    if (idx < 0 || idx >= r->ep->n_spine) return;
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

/* draw_nav() 已取代旧的 draw_browser() */

/* ---------- 绘制：目录 ---------- */
static void draw_toc(reader_ui_t *ui, reading_t *r, int sel) {
    ui_clear(ui);
    ui_rect(ui, 0, 0, SCREEN_W, ui->title_h, 40, 60, 110);
    ui_text(ui, ui->margin, 1, "Table of Contents");
    int y = ui->title_h + 2;
    int visible = (SCREEN_H - ui->title_h - ui->prog_h) / ui->line_h;
    int first = sel - visible / 2; if (first < 0) first = 0;
    for (int i = first; i < r->ep->n_toc && (i - first) < visible; i++) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%d. %s", i + 1, r->ep->toc[i].label);
        if (i == sel) { ui_rect(ui, 0, y, SCREEN_W, ui->line_h, 60, 90, 150); ui_text(ui, ui->margin, y, buf); }
        else ui_text(ui, ui->margin, y, buf);
        y += ui->line_h;
    }
    ui_flip(ui);
}

/* ---------- 阅读一本书 ---------- */
static void read_book(reader_ui_t *ui, epub_t *ep, const char *book) {
    reading_t r;
    memset(&r, 0, sizeof(r));
    r.ep = ep;
    char *nm = strrchr(book, '/'); nm = nm ? nm + 1 : (char *)book;
    r.title = strdup(nm);

    int sp = 0, pg = 0;
    load_progress(book, &sp, &pg);
    if (sp >= ep->n_spine) sp = 0;
    open_chapter(ui, &r, sp);
    r.page = pg;

    enum { ST_READ, ST_TOC } st = ST_READ;
    int toc_sel = 0;
    int quit = 0;
    while (!quit) {
        if (st == ST_READ) {
            int pct = r.total_pages > 0 ? (r.page + 1) * 100 / r.total_pages : 0;
            ui_render_page(ui, r.lines, r.n_lines, r.page, r.per_page, r.title, pct);
        } else {
            draw_toc(ui, &r, toc_sel);
        }
        SDL_Event ev;
        if (!wait_event_timeout(&ev, 300)) continue;
        enum Action act = A_NONE;
        if (ev.type == SDL_KEYDOWN) act = key_to_action(ev.key.keysym.sym);
        else if (ev.type == SDL_JOYBUTTONDOWN) {
            if (ev.jbutton.button == 0) act = A_SELECT;
            else if (ev.jbutton.button == 1) act = A_BACK;
        } else if (ev.type == SDL_JOYHATMOTION) {
            if (ev.jhat.value & SDL_HAT_UP) act = A_UP;
            else if (ev.jhat.value & SDL_HAT_DOWN) act = A_DOWN;
            else if (ev.jhat.value & SDL_HAT_LEFT) act = A_LEFT;
            else if (ev.jhat.value & SDL_HAT_RIGHT) act = A_RIGHT;
        }
        if (act == A_NONE) continue;

        if (st == ST_READ) {
            switch (act) {
                case A_UP:    if (r.page > 0) { r.page--; save_progress(book, r.spine_idx, r.page); } break;
                case A_DOWN:  if (r.page < r.total_pages - 1) { r.page++; save_progress(book, r.spine_idx, r.page); } break;
                case A_RIGHT: if (r.spine_idx < ep->n_spine - 1) { open_chapter(ui, &r, r.spine_idx + 1); r.page = 0; save_progress(book, r.spine_idx, r.page); } break;
                case A_LEFT:  if (r.spine_idx > 0) { open_chapter(ui, &r, r.spine_idx - 1); r.page = 0; save_progress(book, r.spine_idx, r.page); } break;
                case A_SELECT: st = ST_TOC; toc_sel = 0; break;
                case A_BACK:   quit = 1; break;
                default: break;
            }
        } else { /* ST_TOC */
            switch (act) {
                case A_UP:    if (toc_sel > 0) toc_sel--; break;
                case A_DOWN:  if (toc_sel < ep->n_toc - 1) toc_sel++; break;
                case A_SELECT: {
                    int idx = epub_find_spine(ep, ep->toc[toc_sel].href);
                    if (idx >= 0) { open_chapter(ui, &r, idx); r.page = 0; save_progress(book, r.spine_idx, r.page); }
                    st = ST_READ;
                    break;
                }
                case A_BACK:   st = ST_READ; break;
                default: break;
            }
        }
    }
    free_lines(&r);
    free(r.title);
}

/* ---------- 候选根目录 ---------- */
static const char *candidate_roots[] = {
    "/media/roms", "/media/sdcard", "/mnt/sd", "/media", "/run/media", ".", NULL
};

int main(int argc, char **argv) {
    setup_runtime_env();
    const char *font = NULL;
    const char *direct_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc) font = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] == '-') continue;
        else {
            struct stat st;
            if (stat(argv[i], &st) == 0 && ends_with(argv[i], ".epub")) direct_file = argv[i];
        }
    }

    reader_ui_t *ui = ui_init(font);
    if (!ui) {
        fprintf(stderr, "SDL/TTF 初始化失败 (缺少字体或显示设备)\n");
        return 1;
    }

    if (direct_file) {
        zip_t *z = zip_open(direct_file);
        if (!z) { fprintf(stderr, "无法打开 %s\n", direct_file); ui_quit(ui); return 1; }
        epub_t *ep = epub_open(z);
        if (!ep) { fprintf(stderr, "EPUB 解析失败: %s\n", direct_file); zip_close(z); ui_quit(ui); return 1; }
        read_book(ui, ep, direct_file);
        epub_close(ep); zip_close(z);
        ui_quit(ui);
        return 0;
    }

    /* ---------- 可导航文件浏览器：可进入 SD 卡任意目录 ---------- */
    char *cwd = NULL;
    for (int i = 0; candidate_roots[i]; i++) {
        struct stat st;
        if (stat(candidate_roots[i], &st) == 0 && S_ISDIR(st.st_mode)) { cwd = strdup(candidate_roots[i]); break; }
    }
    if (!cwd) cwd = strdup("/");

    int quit = 0;
    while (!quit) {
        nav_list_t list = {0};
        build_list(cwd, &list);
        if (list.n == 0) {
            /* 当前目录为空：提示，等待返回上级 / 退出 */
            int empty_wait = 1;
            while (empty_wait) {
                ui_clear(ui);
                ui_rect(ui, 0, 0, SCREEN_W, ui->title_h, 40, 60, 110);
                ui_text(ui, ui->margin, 1, "该目录为空");
                ui_text(ui, 6, 40, cwd);
                ui_text(ui, 6, 60, "按 B/ESC 返回上级");
                ui_flip(ui);
                SDL_Event ev;
                if (!wait_event_timeout(&ev, 300)) continue;
                enum Action act = A_NONE;
                if (ev.type == SDL_KEYDOWN) act = key_to_action(ev.key.keysym.sym);
                else if (ev.type == SDL_JOYBUTTONDOWN) { if (ev.jbutton.button == 1) act = A_BACK; }
                if (act == A_BACK) {
                    char *p = parent_dir(cwd);
                    if (!strcmp(p, cwd)) { quit = 1; free(p); }
                    else { free(cwd); cwd = p; }
                    empty_wait = 0;
                }
            }
            nav_free(&list);
            continue;
        }
        int sel = 0;
        while (1) {
            draw_nav(ui, cwd, &list, sel);
            SDL_Event ev;
            if (!wait_event_timeout(&ev, 300)) continue;
            enum Action act = A_NONE;
            if (ev.type == SDL_KEYDOWN) act = key_to_action(ev.key.keysym.sym);
            else if (ev.type == SDL_JOYBUTTONDOWN) {
                if (ev.jbutton.button == 0) act = A_SELECT;
                else if (ev.jbutton.button == 1) act = A_BACK;
            } else if (ev.type == SDL_JOYHATMOTION) {
                if (ev.jhat.value & SDL_HAT_UP) act = A_UP;
                else if (ev.jhat.value & SDL_HAT_DOWN) act = A_DOWN;
            }
            if (act == A_NONE) continue;
            if (act == A_UP) { if (sel > 0) sel--; }
            else if (act == A_DOWN) { if (sel < list.n - 1) sel++; }
            else if (act == A_BACK) {
                char *p = parent_dir(cwd);
                if (!strcmp(p, cwd)) { quit = 1; free(p); }
                else { free(cwd); cwd = p; }
                break;
            }
            else if (act == A_SELECT) {
                nav_ent_t *e = &list.e[sel];
                if (e->is_dir) {
                    if (!strcmp(e->name, "..")) {
                        char *p = parent_dir(cwd); free(cwd); cwd = p;
                    } else {
                        char *nw = malloc(strlen(cwd) + strlen(e->name) + 2);
                        sprintf(nw, "%s/%s", cwd, e->name);
                        free(cwd); cwd = nw;
                    }
                    break;
                } else {
                    char *path = malloc(strlen(cwd) + strlen(e->name) + 2);
                    sprintf(path, "%s/%s", cwd, e->name);
                    open_epub(ui, path);
                    free(path);
                }
            }
        }
        nav_free(&list);
        if (quit) break;
    }
    free(cwd);
    ui_quit(ui);
    return 0;
}
