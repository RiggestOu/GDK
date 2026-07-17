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

/* ---------- 文件浏览 ---------- */
typedef struct { char **paths; int n, cap; } filelist_t;

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    return strcasecmp(s + ls - lf, suf) == 0;
}
static void fl_add(filelist_t *l, const char *p) {
    if (l->n >= l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->paths = realloc(l->paths, l->cap * sizeof(char *)); }
    l->paths[l->n++] = strdup(p);
}
static void scan_dir(const char *dir, filelist_t *l, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char buf[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(buf, sizeof(buf), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(buf, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 1) scan_dir(buf, l, depth + 1);
        } else if (ends_with(e->d_name, ".epub")) {
            fl_add(l, buf);
        }
    }
    closedir(d);
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

/* ---------- 绘制：文件浏览 ---------- */
static void draw_browser(reader_ui_t *ui, filelist_t *l, int sel) {
    ui_clear(ui);
    ui_rect(ui, 0, 0, SCREEN_W, ui->title_h, 40, 60, 110);
    ui_text(ui, ui->margin, 1, "EPUB Files (Up/Down, A=Open)");
    int y = ui->title_h + 2;
    int visible = (SCREEN_H - ui->title_h - ui->prog_h) / ui->line_h;
    int first = sel - visible / 2; if (first < 0) first = 0;
    for (int i = first; i < l->n && (i - first) < visible; i++) {
        const char *nm = strrchr(l->paths[i], '/');
        nm = nm ? nm + 1 : l->paths[i];
        if (i == sel) {
            ui_rect(ui, 0, y, SCREEN_W, ui->line_h, 60, 90, 150);
            ui_text(ui, ui->margin, y, nm);
        } else {
            ui_text(ui, ui->margin, y, nm);
        }
        y += ui->line_h;
    }
    ui_flip(ui);
}

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
        if (!SDL_WaitEventTimeout(&ev, 300)) continue;
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
    "/mnt/sd", "/media/sdcard", "/media", "/run/media", ".", NULL
};

int main(int argc, char **argv) {
    const char *font = NULL;
    const char *scan_root = NULL;
    const char *direct_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc) font = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] == '-') continue;
        else {
            struct stat st;
            if (stat(argv[i], &st) == 0) {
                if (S_ISDIR(st.st_mode)) scan_root = argv[i];
                else if (ends_with(argv[i], ".epub")) direct_file = argv[i];
            }
        }
    }
    if (!scan_root && !direct_file) {
        if (getenv("EPUB_ROOT")) scan_root = getenv("EPUB_ROOT");
        else for (int i = 0; candidate_roots[i]; i++) {
            struct stat st;
            if (stat(candidate_roots[i], &st) == 0) { scan_root = candidate_roots[i]; break; }
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

    filelist_t list = {0};
    if (scan_root) scan_dir(scan_root, &list, 0);
    if (list.n == 0) {
        ui_clear(ui);
        ui_text(ui, 6, 40, "未找到 .epub 文件");
        ui_text(ui, 6, 60, "把书放到 SD 卡，或用");
        ui_text(ui, 6, 80, "epubreader <目录|文件> 启动");
        ui_flip(ui);
        SDL_Delay(2500);
        for (int i = 0; i < list.n; i++) free(list.paths[i]);
        free(list.paths);
        ui_quit(ui);
        return 0;
    }

    int sel = 0, quit = 0;
    while (!quit) {
        draw_browser(ui, &list, sel);
        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 300)) continue;
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
        switch (act) {
            case A_UP:   if (sel > 0) sel--; break;
            case A_DOWN: if (sel < list.n - 1) sel++; break;
            case A_SELECT: {
                const char *book = list.paths[sel];
                zip_t *z = zip_open(book);
                if (z) {
                    epub_t *ep = epub_open(z);
                    if (ep) { read_book(ui, ep, book); epub_close(ep); }
                    else { /* 解析失败 */ }
                    zip_close(z);
                }
                break;
            }
            case A_BACK: quit = 1; break;
            default: break;
        }
    }
    for (int i = 0; i < list.n; i++) free(list.paths[i]);
    free(list.paths);
    ui_quit(ui);
    return 0;
}
