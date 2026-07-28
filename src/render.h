#ifndef RENDER_H
#define RENDER_H
#include "SDL.h"
#include "SDL_ttf.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define TITLE_H  18
#define STATUS_H 13
#define PROG_H   8

typedef struct {
    SDL_Surface *screen;
    TTF_Font    *font;
    TTF_Font    *font_h1;       /* 标题字体（大） */
    TTF_Font    *font_h2;       /* 标题字体（中） */
    char        *font_path;     /* 用于按字号重开字体 */
    int  font_size;             /* 当前字号 */
    int  fg_r, fg_g, fg_b;      /* 文字（前景）色 */
    int  bg_r, bg_g, bg_b;      /* 背景色 */
    int  brightness;            /* 亮度 30..100(%) */
    int  accent_r, accent_g, accent_b; /* 强调色（标题栏/选中条） */
    int  margin;
    int  line_h;
    int  title_h;
    int  status_h;
} reader_ui_t;

/* 浏览器条目 */
typedef struct { const char *name; int is_dir; } nav_ent_t;
/* 菜单条目 */
typedef struct { const char *label; const char *value; int enabled; } menu_item_t;

reader_ui_t *ui_init(const char *font_path);
void ui_quit(reader_ui_t *ui);

void ui_clear(reader_ui_t *ui);
void ui_flip(reader_ui_t *ui);
/* 以指定颜色绘制文本（前景色由调用方给定，内部统一应用亮度） */
void ui_text(reader_ui_t *ui, int x, int y, const char *text);
void ui_text_rgb(reader_ui_t *ui, int x, int y, const char *text, int r, int g, int b);
/* 填充矩形（0-255 RGB，内部应用亮度） */
void ui_rect(reader_ui_t *ui, int x, int y, int w, int h, int r, int g, int b);

/* 配置变更（即时生效，下次绘制即见） */
void ui_set_font_size(reader_ui_t *ui, int size);
void ui_set_fg(reader_ui_t *ui, int r, int g, int b);
void ui_set_brightness(reader_ui_t *ui, int pct);

/* 把文本按屏幕宽度折行，返回行指针数组与行数（需 free 每行及数组） */
char **wrap_text(reader_ui_t *ui, const char *text, int *out_n);
/* 通用折行：指定字体与最大宽度 */
char **wrap_text_font(TTF_Font *font, const char *text, int maxw, int *out_n);
int ui_lines_per_page(reader_ui_t *ui);

/* 按样式取字体：style 0=正文 1=h1 2=h2 3=h3 4=引用；*bold 输出是否加粗 */
TTF_Font *ui_style_font(reader_ui_t *ui, int style, int *bold);
/* 指定字体/加粗绘制一行文本 */
void ui_text_font(reader_ui_t *ui, TTF_Font *f, int bold, int x, int y,
                  const char *text, int r, int g, int b);

/* 绘制：浏览器 */
void ui_draw_browser(reader_ui_t *ui, const char *cwd,
                     const nav_ent_t *items, int n, int sel, int first,
                     int can_up, int can_down);
/* 绘制：阅读页 */
void ui_draw_reader(reader_ui_t *ui, char **lines, int n_lines,
                    int page, int per_page, const char *title, int pct,
                    int bookmark_on);
/* 绘制：菜单/子菜单 */
void ui_draw_menu(reader_ui_t *ui, const char *title,
                 const menu_item_t *items, int n, int sel);
/* 绘制：底部状态栏（left 左对齐，right 右对齐） */
void ui_draw_status(reader_ui_t *ui, const char *left, const char *right);
/* 绘制：错误屏（msg 支持 \n 多行） */
void ui_draw_error(reader_ui_t *ui, const char *title, const char *msg);

#endif /* RENDER_H */
