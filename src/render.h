#ifndef RENDER_H
#define RENDER_H
#include "SDL.h"
#include "SDL_ttf.h"

#define SCREEN_W 320
#define SCREEN_H 240

typedef struct {
    SDL_Surface *screen;
    TTF_Font    *font;
    int  fg_r, fg_g, fg_b;
    int  bg_r, bg_g, bg_b;
    int  margin;
    int  line_h;
    int  title_h;
    int  prog_h;
} reader_ui_t;

/* 初始化 SDL + 字体。font_path 为 NULL 时尝试 ./font.ttf 等默认位置。 */
reader_ui_t *ui_init(const char *font_path);
void ui_quit(reader_ui_t *ui);

void ui_clear(reader_ui_t *ui);
void ui_flip(reader_ui_t *ui);
/* 在 (x,y) 以前景色绘制一行文本 */
void ui_text(reader_ui_t *ui, int x, int y, const char *text);
/* 填充矩形（0-255 RGB） */
void ui_rect(reader_ui_t *ui, int x, int y, int w, int h, int r, int g, int b);

/* 把文本按屏幕宽度折行，返回行指针数组与行数（需 free 每行及数组） */
char **wrap_text(reader_ui_t *ui, const char *text, int *out_n);
int ui_lines_per_page(reader_ui_t *ui);
/* 渲染第 page 页（每页 per_page 行），显示标题与进度百分比 */
void ui_render_page(reader_ui_t *ui, char **lines, int n_lines, int page, int per_page,
                    const char *title, int pct);

#endif /* RENDER_H */
