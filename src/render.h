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
    TTF_Font    *font_hud;      /* HUD 簇专用小号字体（不受正文字号影响，始终紧凑） */
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
    int  render_epoch;     /* 渲染失效计数：字体/前景色/亮度变化时自增，供阅读页整页缓存判定 */
} reader_ui_t;

/* 浏览器条目 */
typedef struct { const char *name; int is_dir; } nav_ent_t;
/* 菜单条目 */
typedef struct { const char *label; const char *value; int enabled; } menu_item_t;

reader_ui_t *ui_init(const char *font_path);
void ui_quit(reader_ui_t *ui);

void ui_clear(reader_ui_t *ui);
void ui_flip(reader_ui_t *ui);
/* 诊断用：采样屏幕平均亮度（兼容任意 bpp），用于锁定浮层残留根因；-1 表示不可用 */
int ui_screen_luma(reader_ui_t *ui);
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
/* 计算底部状态区所需高度（HUD 一行 + 脚注折行行数），用于布局（body_bottom/进度条位置） */
int ui_status_height(reader_ui_t *ui, const char *left);
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
/* 绘制：菜单/子菜单；km_title=1 时“自定义快捷键”标题仅在 大/特大 折两行、小/中单行 */
void ui_draw_menu(reader_ui_t *ui, const char *title,
                 const menu_item_t *items, int n, int sel, int km_title);
/* 绘制：底部状态栏（left 左对齐，right 右对齐） */
void ui_draw_status(reader_ui_t *ui, const char *left, const char *right);
/* 绘制：错误屏（msg 支持 \n 多行） */
void ui_draw_error(reader_ui_t *ui, const char *title, const char *msg);

/* HUD 覆盖层（电量/时间/亮度）：由 main.c 提供数据，绘制在阅读/菜单/浏览器顶部与底部 */
void ui_set_hud(int batt_pct, const char *clock);
void ui_set_hud_bookmark(int on);
void ui_draw_hud(reader_ui_t *ui);
/* 读取当前 HUD 时钟/电量（供阅读页整页缓存判定是否需重绘） */
const char *ui_hud_clock(void);
int ui_hud_batt(void);

/* 圆4 半透明快捷键说明浮层（由 main.c 切换与绘制，render 在 flip 时回调） */
void ui_set_km_overlay(int on);
void ui_keymap_overlay_draw(reader_ui_t *ui);
/* 画面大面积变化后调用：下一次 ui_flip 会把同一帧写满 framebuffer 的全部轮转页，
   清除多缓冲残留页（= 浮层关闭后画面偶发整体变暗的根因） */
void ui_flush_frames(void);

#endif /* RENDER_H */
