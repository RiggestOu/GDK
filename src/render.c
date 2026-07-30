#include "render.h"
#include <SDL/SDL_joystick.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* HUD 覆盖层数据（电量/时间由 main.c 提供），在绘制末尾叠加到所有界面 */
static int g_hud_batt = -1;
static char g_hud_clock[8] = "--:--";
static void draw_hud(reader_ui_t *ui);

/* 保存打开的 joystick 句柄：SDL1.2 必须显式打开设备，才会投递 JOYBUTTONDOWN / JOYHATMOTION 事件 */
static SDL_Joystick *g_joy = NULL;

/* ---------- UTF-8 辅助 ---------- */
static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
static unsigned long utf8_cp(const char *p) {
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80) return c;
    int n = utf8_len(c);
    unsigned long cp = c & ((1u << (7 - n)) - 1);
    for (int i = 1; i < n; i++) cp = (cp << 6) | (p[i] & 0x3F);
    return cp;
}

/* 字体候选：彻底移除自定义 font.ttf，只用设备系统字体。
   顺序即加载优先级；首个能成功打开且含中文(通过下方CJK闸门)的即被采用，并记入 run.log 的 [diag] 日志。
   首选 /usr/share/fonts/SourceHanSans-Regular-04.ttf(思源黑体，已验证含CJK)——设备 /usr/share/fonts 下唯一含中文的字体。
   ./system.ttf 为可选覆盖项(若用户想放本地副本到部署目录)；其余系统路径仅作留底。 */
static const char *try_fonts[] = {
    "/usr/share/fonts/SourceHanSans-Regular-04.ttf",   /* 设备系统默认中文字体(思源黑体) — 主用 */
    "./system.ttf",                                   /* 可选：用户若想用本地副本可放此文件到部署目录 */
    "/usr/share/fonts/default.ttf",
    "/usr/share/fonts/opendingux/default.ttf",
    NULL
};

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    return strcasecmp(s + ls - lf, suf) == 0;
}

/* 应用亮度系数到颜色 */
static void dim(int f, int *r, int *g, int *bl) {
    *r = *r * f / 100;
    *g = *g * f / 100;
    *bl = *bl * f / 100;
}

/* ---------- 初始化 / 释放 ---------- */
reader_ui_t *ui_init(const char *font_path) {
    /* NOPARACHUTE：禁止 SDL 自装信号处理器覆盖 main 里的 [CRASH] 黑匣子 */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_NOPARACHUTE) < 0) { fprintf(stderr,"[diag] SDL_Init FAILED: %s\n",SDL_GetError()); return NULL; }
    /* 关键：光初始化 JOYSTICK 子系统不够，必须打开设备，否则收不到按键事件 */
    SDL_JoystickEventState(SDL_ENABLE);
    if (SDL_NumJoysticks() > 0) g_joy = SDL_JoystickOpen(0);
    if (TTF_Init() < 0) { fprintf(stderr,"[diag] TTF_Init FAILED: %s\n",TTF_GetError()); if (g_joy) SDL_JoystickClose(g_joy); SDL_Quit(); return NULL; }
    fprintf(stderr,"[diag] SDL_Init+TTF_Init OK\n");

    reader_ui_t *ui = calloc(1, sizeof(reader_ui_t));
    if (!ui) { if (g_joy) SDL_JoystickClose(g_joy); TTF_Quit(); SDL_Quit(); return NULL; }

    ui->screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    if (!ui->screen) { fprintf(stderr,"[diag] SetVideoMode FAILED: %s\n",SDL_GetError()); free(ui); if (g_joy) SDL_JoystickClose(g_joy); TTF_Quit(); SDL_Quit(); return NULL; }
    fprintf(stderr,"[diag] SetVideoMode OK (%dx%d)\n", SCREEN_W, SCREEN_H);

    ui->font = NULL; ui->font_path = NULL;
    const char *fp = font_path;
    int i = 0;
    while ((fp || try_fonts[i])) {
        const char *p = fp ? fp : try_fonts[i];
        fprintf(stderr,"[diag] TTF_OpenFont(%s)... ", p);
        TTF_Font *f = TTF_OpenFont(p, 14);
        if (f) {
            /* 中文可用性闸门：测试'中'(U+4E2D)，不含中文的字体(如DejaVuSans)跳过，避免方块 */
            if (TTF_GlyphIsProvided(f, 0x4E2D)) {
                ui->font = f; ui->font_path = strdup(p);
                fprintf(stderr,"OK(含CJK)\n");
                break;
            } else {
                fprintf(stderr,"SKIP(不含中文)\n");
                TTF_CloseFont(f);
            }
        } else {
            fprintf(stderr,"FAIL(%s)\n", TTF_GetError());
        }
        if (fp) fp = NULL; else i++;
    }
    if (!ui->font) { fprintf(stderr,"[diag] ALL fonts failed\n"); free(ui); if (g_joy) SDL_JoystickClose(g_joy); TTF_Quit(); SDL_Quit(); return NULL; }
    fprintf(stderr,"[diag] 采用字体路径: %s\n", ui->font_path ? ui->font_path : "(null)");

    ui->font_size   = 14;
    ui->fg_r = 235; ui->fg_g = 235; ui->fg_b = 235;
    ui->bg_r = 16;  ui->bg_g = 18;  ui->bg_b = 26;
    ui->brightness  = 100;
    ui->accent_r = 90; ui->accent_g = 160; ui->accent_b = 240;
    ui->margin  = 6;
    fprintf(stderr,"[diag] TTF_FontHeight... "); fflush(stderr);
    ui->line_h  = TTF_FontHeight(ui->font) + 2;
    fprintf(stderr,"OK line_h=%d\n", ui->line_h); fflush(stderr);
    ui->title_h = TITLE_H;
    ui->status_h = STATUS_H;
    return ui;
}

/* 标题字体懒加载（h1=正文+7，h2=正文+4） */
static void open_heading_fonts(reader_ui_t *ui) {
    const char *p = ui->font_path ? ui->font_path : "/usr/share/fonts/SourceHanSans-Regular-04.ttf";
    if (!ui->font_h1) ui->font_h1 = TTF_OpenFont(p, ui->font_size + 7);
    if (!ui->font_h2) ui->font_h2 = TTF_OpenFont(p, ui->font_size + 4);
}
TTF_Font *ui_style_font(reader_ui_t *ui, int style, int *bold) {
    if (bold) *bold = 0;
    if (style == 1) { open_heading_fonts(ui); if (bold) *bold = 1; return ui->font_h1 ? ui->font_h1 : ui->font; }
    if (style == 2) { open_heading_fonts(ui); if (bold) *bold = 1; return ui->font_h2 ? ui->font_h2 : ui->font; }
    if (style == 3) { if (bold) *bold = 1; return ui->font; }
    return ui->font;
}
void ui_text_font(reader_ui_t *ui, TTF_Font *f, int bold, int x, int y,
                  const char *text, int r, int g, int b) {
    if (!text || !*text || !f) return;
    int rr = r, gg = g, bb = b; dim(ui->brightness, &rr, &gg, &bb);
    SDL_Color fg = { (Uint8)rr, (Uint8)gg, (Uint8)bb, 255 };
    int old = TTF_GetFontStyle(f);
    if (bold) TTF_SetFontStyle(f, old | TTF_STYLE_BOLD);
    SDL_Surface *s = TTF_RenderUTF8_Solid(f, text, fg);
    if (bold) TTF_SetFontStyle(f, old);
    if (!s) return;
    SDL_Rect dst = { x, y, 0, 0 };
    SDL_BlitSurface(s, NULL, ui->screen, &dst);
    SDL_FreeSurface(s);
}

void ui_quit(reader_ui_t *ui) {
    if (!ui) return;
    if (ui->font_h1) TTF_CloseFont(ui->font_h1);
    if (ui->font_h2) TTF_CloseFont(ui->font_h2);
    if (ui->font) TTF_CloseFont(ui->font);
    if (ui->font_path) free(ui->font_path);
    if (g_joy) { SDL_JoystickClose(g_joy); g_joy = NULL; }
    free(ui);
    TTF_Quit();
    SDL_Quit();
}

/* ---------- 基础绘制 ---------- */
void ui_clear(reader_ui_t *ui) {
    SDL_FillRect(ui->screen, NULL, SDL_MapRGB(ui->screen->format, ui->bg_r, ui->bg_g, ui->bg_b));
}
void ui_flip(reader_ui_t *ui) { SDL_Flip(ui->screen); }

void ui_rect(reader_ui_t *ui, int x, int y, int w, int h, int r, int g, int b) {
    int rr = r, gg = g, bb = b; dim(ui->brightness, &rr, &gg, &bb);
    SDL_Rect rc = { x, y, w, h };
    SDL_FillRect(ui->screen, &rc, SDL_MapRGB(ui->screen->format, rr, gg, bb));
}
void ui_text_rgb(reader_ui_t *ui, int x, int y, const char *text, int r, int g, int b) {
    if (!text || !*text) return;
    int rr = r, gg = g, bb = b; dim(ui->brightness, &rr, &gg, &bb);
    SDL_Color fg = { (Uint8)rr, (Uint8)gg, (Uint8)bb, 255 };
    static int first_render = 1;
    if (first_render) { fprintf(stderr,"[diag] 首次 TTF_RenderUTF8_Solid(\"%.20s\")... ", text); fflush(stderr); }
    SDL_Surface *s = TTF_RenderUTF8_Solid(ui->font, text, fg);
    if (first_render) { fprintf(stderr,"%s\n", s ? "OK" : "FAIL"); fflush(stderr); first_render = 0; }
    if (!s) return;
    SDL_Rect dst = { x, y, 0, 0 };
    SDL_BlitSurface(s, NULL, ui->screen, &dst);
    SDL_FreeSurface(s);
}
void ui_text(reader_ui_t *ui, int x, int y, const char *text) {
    ui_text_rgb(ui, x, y, text, ui->fg_r, ui->fg_g, ui->fg_b);
}

/* ---------- 配置变更 ---------- */
void ui_set_font_size(reader_ui_t *ui, int size) {
    fprintf(stderr,"[diag] set_font_size 入口 size=%d ui=%p\n", size, (void*)ui); fflush(stderr);
    if (size < 10) size = 10;
    if (size > 28) size = 28;
    const char *p = ui->font_path ? ui->font_path : "/usr/share/fonts/SourceHanSans-Regular-04.ttf";
    /* 若字号相同则无需重开字体（也规避二次 OpenFont 触发的崩溃） */
    if (ui->font && size == ui->font_size) { fprintf(stderr,"[diag] set_font_size(%d) 同字号跳过\n", size); fflush(stderr); return; }
    fprintf(stderr,"[diag] set_font_size: OpenFont(%s,%d)... ", p, size); fflush(stderr);
    TTF_Font *f = TTF_OpenFont(p, size);
    if (!f) f = TTF_OpenFont("/usr/share/fonts/SourceHanSans-Regular-04.ttf", size);
    fprintf(stderr,"%s\n", f ? "OK" : "FAIL"); fflush(stderr);
    if (!f) return;
    fprintf(stderr,"[diag] set_font_size: CloseFont 旧字体... "); fflush(stderr);
    if (ui->font) TTF_CloseFont(ui->font);
    fprintf(stderr,"OK\n"); fflush(stderr);
    ui->font = f; ui->font_size = size;
    /* 字号变了，标题字体作废（下次用时按新字号重开） */
    if (ui->font_h1) { TTF_CloseFont(ui->font_h1); ui->font_h1 = NULL; }
    if (ui->font_h2) { TTF_CloseFont(ui->font_h2); ui->font_h2 = NULL; }
    fprintf(stderr,"[diag] set_font_size: FontHeight... "); fflush(stderr);
    ui->line_h = TTF_FontHeight(ui->font) + 2;
    fprintf(stderr,"OK line_h=%d\n", ui->line_h); fflush(stderr);
}
void ui_set_fg(reader_ui_t *ui, int r, int g, int b) { ui->fg_r = r; ui->fg_g = g; ui->fg_b = b; }
void ui_set_brightness(reader_ui_t *ui, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ui->brightness = pct;
}

/* ---------- 小图形 ---------- */
static void draw_title(reader_ui_t *ui, const char *title) {
    ui_rect(ui, 0, 0, SCREEN_W, TITLE_H, ui->accent_r, ui->accent_g, ui->accent_b);
    ui_rect(ui, 0, TITLE_H - 1, SCREEN_W, 1, 0, 0, 0);
    ui_text_rgb(ui, ui->margin, 3, title, 255, 255, 255);
}
static void draw_tri_up(reader_ui_t *ui, int x, int y) {
    ui_rect(ui, x, y + 4, 7, 2, 200, 200, 200);
    ui_rect(ui, x + 1, y + 2, 5, 2, 200, 200, 200);
    ui_rect(ui, x + 2, y, 3, 2, 200, 200, 200);
}
static void draw_tri_dn(reader_ui_t *ui, int x, int y) {
    ui_rect(ui, x + 2, y, 3, 2, 200, 200, 200);
    ui_rect(ui, x + 1, y + 2, 5, 2, 200, 200, 200);
    ui_rect(ui, x, y + 4, 7, 2, 200, 200, 200);
}

/* ---------- 浏览器 ---------- */
void ui_draw_browser(reader_ui_t *ui, const char *cwd,
                     const nav_ent_t *items, int n, int sel, int first,
                     int can_up, int can_down) {
    ui_clear(ui);
    char title[64];
    snprintf(title, sizeof(title), "浏览: %s", cwd ? cwd : "/");
    /* 地址栏下移到 HUD 时钟之下，避免遮挡顶部居中时间 */
    int addr_y = TITLE_H + 2;
    ui_rect(ui, 0, addr_y - 2, SCREEN_W, ui->line_h, ui->accent_r, ui->accent_g, ui->accent_b);
    ui_rect(ui, 0, addr_y - 3 + ui->line_h, SCREEN_W, 1, 0, 0, 0);
    ui_text_rgb(ui, ui->margin, addr_y, title, 255, 255, 255);

    int list_top = addr_y + ui->line_h + 3;
    int list_bottom = SCREEN_H - STATUS_H - 3;
    int avail = list_bottom - list_top;
    int visible = avail / ui->line_h;
    if (visible < 1) visible = 1;

    for (int i = first; i < n && (i - first) < visible; i++) {
        int y = list_top + (i - first) * ui->line_h;
        int is_dir = items[i].is_dir;
        const char *nm = items[i].name;
        int is_up = (!strcmp(nm, ".."));
        int is_epub = (!is_dir && ends_with(nm, ".epub"));

        if (i == sel) {
            ui_rect(ui, 0, y - 1, SCREEN_W, ui->line_h, ui->accent_r / 2, ui->accent_g / 2, ui->accent_b / 2);
            ui_rect(ui, 0, y - 1, 3, ui->line_h, ui->accent_r, ui->accent_g, ui->accent_b);
        }
        int tx = ui->margin + 4;
        if (is_up) {
            ui_text_rgb(ui, tx, y, "^ 上级目录", 190, 190, 210);
        } else if (is_dir) {
            ui_rect(ui, tx, y + 3, 9, 7, ui->accent_r, ui->accent_g, ui->accent_b);
            ui_rect(ui, tx, y + 3, 4, 2, ui->accent_r, ui->accent_g, ui->accent_b);
            ui_text_rgb(ui, tx + 13, y, nm, 150, 200, 255);
        } else if (is_epub) {
            ui_text_rgb(ui, tx, y, nm, 255, 205, 110);
        } else {
            ui_text_rgb(ui, tx, y, nm, 165, 165, 165);
        }
    }
    if (can_up)   draw_tri_up(ui, SCREEN_W - 10, list_top + 1);
    if (can_down) draw_tri_dn(ui, SCREEN_W - 10, list_bottom - 8);

    ui_draw_status(ui, "A 打开", "B 返回");
    draw_hud(ui);
    ui_flip(ui);
}

/* ---------- 阅读页 ---------- */
void ui_draw_reader(reader_ui_t *ui, char **lines, int n_lines,
                    int page, int per_page, const char *title, int pct,
                    int bookmark_on) {
    ui_clear(ui);
    char t[32];
    snprintf(t, sizeof(t), "%.28s", title ? title : "");
    draw_title(ui, t);
    if (bookmark_on) ui_text_rgb(ui, SCREEN_W - ui->margin - 12, 3, "★", 255, 220, 80);

    int body_top = TITLE_H + 3;
    int body_bottom = SCREEN_H - STATUS_H - PROG_H - 3;
    for (int i = 0; i < per_page; i++) {
        int idx = page * per_page + i;
        if (idx >= n_lines) break;
        ui_text(ui, ui->margin, body_top + i * ui->line_h, lines[idx]);
    }

    int py = SCREEN_H - STATUS_H - PROG_H - 1;
    ui_rect(ui, 0, py - 1, SCREEN_W, 1, 0, 0, 0);
    int bar_w = SCREEN_W - 2 * ui->margin - 28;
    ui_rect(ui, ui->margin, py, bar_w, PROG_H, 40, 42, 50);
    int w = (int)((long)bar_w * pct / 100);
    if (w > bar_w) w = bar_w;
    ui_rect(ui, ui->margin, py, w, PROG_H, ui->accent_r, ui->accent_g, ui->accent_b);
    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    ui_text_rgb(ui, SCREEN_W - ui->margin - 24, py, pbuf, 200, 210, 230);

    ui_draw_status(ui, "B 退出", "Y 菜单 X 书签");
    draw_hud(ui);
    ui_flip(ui);
}

/* ---------- 菜单 ---------- */
void ui_draw_menu(reader_ui_t *ui, const char *title,
                 const menu_item_t *items, int n, int sel) {
    ui_clear(ui);
    ui_rect(ui, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0); /* 暗化遮罩 */
    int px = 10, py = 22, pw = SCREEN_W - 20, ph = SCREEN_H - 22 - STATUS_H - 6;
    ui_rect(ui, px, py, pw, ph, 24, 28, 40);
    ui_rect(ui, px, py, pw, 1, ui->accent_r, ui->accent_g, ui->accent_b);
    ui_rect(ui, px, py, 1, ph, ui->accent_r, ui->accent_g, ui->accent_b);
    ui_rect(ui, px + pw - 1, py, 1, ph, ui->accent_r, ui->accent_g, ui->accent_b);
    ui_text_rgb(ui, px + 6, py + 3, title, ui->accent_r, ui->accent_g, ui->accent_b);

    int ly = py + 16;
    /* 滚动：以选中项为中心，超出面板的条目滚动显示 */
    int visible = (ph - 16) / ui->line_h;
    if (visible < 1) visible = 1;
    int first = 0;
    if (n > visible) {
        first = sel - visible / 2;
        if (first < 0) first = 0;
        if (first + visible > n) first = n - visible;
    }
    for (int i = first; i < n && (i - first) < visible; i++) {
        int y = ly + (i - first) * ui->line_h;
        if (i == sel)
            ui_rect(ui, px + 3, y - 1, pw - 6, ui->line_h, ui->accent_r / 2, ui->accent_g / 2, ui->accent_b / 2);
        int col = items[i].enabled ? 235 : 110;
        char buf[128];
        if (items[i].value && *items[i].value)
            snprintf(buf, sizeof(buf), "%s: %s", items[i].label, items[i].value);
        else
            snprintf(buf, sizeof(buf), "%s", items[i].label);
        ui_text_rgb(ui, px + 8, y, buf, col, col, col);
    }
    if (first > 0)           draw_tri_up(ui, px + pw - 12, py + 4);
    if (first + visible < n) draw_tri_dn(ui, px + pw - 12, py + ph - 8);
    ui_draw_status(ui, "A/Y 选择", "B 返回");
    draw_hud(ui);
    ui_flip(ui);
}

/* ---------- 状态栏 ---------- */
void ui_draw_status(reader_ui_t *ui, const char *left, const char *right) {
    int y = SCREEN_H - STATUS_H;
    ui_rect(ui, 0, y, SCREEN_W, STATUS_H, 30, 32, 40);
    ui_rect(ui, 0, y, SCREEN_W, 1, 0, 0, 0);
    ui_text_rgb(ui, ui->margin, y + 2, left, 200, 210, 230);
    if (right) {
        int w = 0, h = 0;
        TTF_SizeUTF8(ui->font, right, &w, &h);
        ui_text_rgb(ui, SCREEN_W - ui->margin - w, y + 2, right, 200, 210, 230);
    }
}

/* ---------- HUD 覆盖层（电量 / 时间 / 亮度） ---------- */
void ui_set_hud(int batt_pct, const char *clock) {
    g_hud_batt = batt_pct;
    if (clock) { strncpy(g_hud_clock, clock, 7); g_hud_clock[7] = 0; }
}
static void draw_hud(reader_ui_t *ui) {
    int tw = 0, th = 0;
    /* 顶部居中：当前时间（24h，时:分） */
    TTF_SizeUTF8(ui->font, g_hud_clock, &tw, &th);
    ui_text_rgb(ui, (SCREEN_W - tw) / 2, 3, g_hud_clock, 235, 235, 245);
    /* 右上角：剩余电量 */
    char bbuf[16];
    if (g_hud_batt >= 0) snprintf(bbuf, sizeof(bbuf), "%d%%", g_hud_batt);
    else snprintf(bbuf, sizeof(bbuf), "?");
    int bw = 0; TTF_SizeUTF8(ui->font, bbuf, &bw, &th);
    ui_text_rgb(ui, SCREEN_W - ui->margin - bw, 3, bbuf, 150, 255, 150);
    /* 底部居中：当前亮度 */
    char lbuf[16]; snprintf(lbuf, sizeof(lbuf), "亮%d%%", ui->brightness);
    int lw = 0; TTF_SizeUTF8(ui->font, lbuf, &lw, &th);
    ui_text_rgb(ui, (SCREEN_W - lw) / 2, SCREEN_H - STATUS_H + 2, lbuf, 200, 210, 230);
}
/* 供 layout.c / main.c 跨文件叠加 HUD（内部调用 static draw_hud） */
void ui_draw_hud(reader_ui_t *ui) { draw_hud(ui); }

/* ---------- 错误屏 ---------- */
void ui_draw_error(reader_ui_t *ui, const char *title, const char *msg) {
    ui_clear(ui);
    ui_rect(ui, 0, 0, SCREEN_W, TITLE_H, 200, 60, 60);
    ui_text_rgb(ui, ui->margin, 3, title, 255, 255, 255);
    int y = TITLE_H + 6;
    const char *p = msg;
    while (p && *p && y < SCREEN_H - STATUS_H - 4) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char buf[256];
        int l = len < 255 ? len : 255;
        memcpy(buf, p, l); buf[l] = 0;
        ui_text_rgb(ui, ui->margin, y, buf, 220, 220, 220);
        y += ui->line_h;
        p = nl ? nl + 1 : p + strlen(p);
    }
    ui_draw_status(ui, "", "B 返回");
    draw_hud(ui);
    ui_flip(ui);
}

/* ---------- 文本折行 ---------- */
char **wrap_text(reader_ui_t *ui, const char *text, int *out_n) {
    return wrap_text_font(ui->font, text, SCREEN_W - 2 * ui->margin, out_n);
}
char **wrap_text_font(TTF_Font *font, const char *text, int maxw, int *out_n) {
    typedef struct { char *s; int w; int cjk; } tok_t;
    tok_t *toks = NULL; int nt = 0, cap = 0;

    char *cur = malloc(8); int cl = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\r') {
            if (cl) { toks = realloc(toks, ++cap * sizeof(tok_t)); toks[nt].s = cur; toks[nt].cjk = 0; toks[nt].w = 0; nt++; cl = 0; cur = malloc(8); }
            i++; continue;
        }
        if (isspace(c)) {
            if (cl) { toks = realloc(toks, ++cap * sizeof(tok_t)); toks[nt].s = cur; toks[nt].cjk = 0; toks[nt].w = 0; nt++; cl = 0; cur = malloc(8); }
            i++; continue;
        }
        unsigned long cp = utf8_cp(text + i);
        int bl = utf8_len(c);
        int is_cjk = (cp >= 0x2E80);
        if (is_cjk) {
            if (cl) { toks = realloc(toks, ++cap * sizeof(tok_t)); toks[nt].s = cur; toks[nt].cjk = 0; toks[nt].w = 0; nt++; cl = 0; cur = malloc(8); }
            char *t = malloc(bl + 1); memcpy(t, text + i, bl); t[bl] = 0;
            toks = realloc(toks, ++cap * sizeof(tok_t));
            toks[nt].s = t; toks[nt].cjk = 1; toks[nt].w = 0; nt++;
        } else {
            cur = realloc(cur, cl + bl + 1);
            memcpy(cur + cl, text + i, bl); cl += bl; cur[cl] = 0;
        }
        i += bl;
    }
    if (cl) { toks = realloc(toks, ++cap * sizeof(tok_t)); toks[nt].s = cur; toks[nt].cjk = 0; toks[nt].w = 0; nt++; }
    else free(cur);

    int space_w = 0, h = 0;
    TTF_SizeUTF8(font, " ", &space_w, &h);
    for (int i = 0; i < nt; i++) TTF_SizeUTF8(font, toks[i].s, &toks[i].w, &h);

    char **lines = NULL; int nl = 0;
    char *line = malloc(1); line[0] = 0; int lw = 0; int prev_cjk = 0;
    for (int i = 0; i < nt; i++) {
        int sep = (lw > 0 && !toks[i].cjk && !prev_cjk) ? space_w : 0;
        int add = toks[i].w + sep;
        if (lw + add > maxw && lw > 0) {
            lines = realloc(lines, (nl + 1) * sizeof(char *));
            lines[nl++] = line;
            line = malloc(1); line[0] = 0; lw = 0; sep = 0; prev_cjk = 0;
        }
        if (sep) { size_t L = strlen(line); line = realloc(line, L + 2); line[L] = ' '; line[L+1] = 0; lw += sep; }
        size_t L = strlen(line);
        line = realloc(line, L + strlen(toks[i].s) + 1);
        strcat(line, toks[i].s);
        lw += toks[i].w;
        prev_cjk = toks[i].cjk;
    }
    lines = realloc(lines, (nl + 1) * sizeof(char *));
    lines[nl++] = line;
    for (int i = 0; i < nt; i++) free(toks[i].s);
    free(toks);
    *out_n = nl;
    return lines;
}

int ui_lines_per_page(reader_ui_t *ui) {
    int body = SCREEN_H - TITLE_H - STATUS_H - PROG_H - 6;
    int pp = body / ui->line_h;
    return pp > 1 ? pp : 1;
}
