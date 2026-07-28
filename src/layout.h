#ifndef LAYOUT_H
#define LAYOUT_H
#include "render.h"
#include "epub.h"

/* ---------- 块级解析（XHTML → 块序列） ---------- */
typedef enum { BLK_PARA = 0, BLK_H1, BLK_H2, BLK_H3, BLK_QUOTE, BLK_IMG } blk_type;
typedef enum { AL_LEFT = 0, AL_CENTER, AL_RIGHT } blk_align;

typedef struct {
    blk_type  type;
    blk_align align;
    int       bold;       /* 整块加粗 */
    char     *text;       /* 已解实体的纯文本（BLK_IMG 时为 NULL） */
    char     *img_href;   /* BLK_IMG：图片相对路径 */
} block_t;

/* 把 XHTML 解析成块序列（跳过 head/style/script）。返回块数组，需 blocks_free */
block_t *html_to_blocks(const char *html, int *out_n);
void blocks_free(block_t *b, int n);

/* ---------- 排好版的行（像素级） ---------- */
typedef struct {
    char        *text;    /* 行文本（NULL 表示图片行/空行） */
    SDL_Surface *img;     /* 图片行：已缩放好的 surface（由 layout 持有并释放） */
    short x;              /* 绘制 x（对齐已算好） */
    short h;              /* 行高（像素） */
    int   y;              /* 相对文档顶部 y（像素） */
    unsigned char style;  /* 0=正文 1=h1 2=h2 3=h3 4=引用 */
    unsigned char bold;
} rline_t;

/* 图片原图（供缩放查看器放大细节）。由 layout 持有并在 layout_free 释放 full/href。 */
typedef struct {
    int           line;   /* 在 layout_t.lines 中的行索引 */
    int           page;   /* 该图片落在第几页（分页后填） */
    SDL_Surface  *full;   /* 原始分辨率 surface（未缩放；进入缩放界面时按需解码并缓存） */
    char         *href;   /* 图片相对当前章节文档的引用路径，供按需解码 */
} pic_ent_t;

typedef struct {
    rline_t *lines;
    int      n_lines;
    int      doc_h;       /* 文档总高 */
    int     *page_start;  /* 每页起始行索引 */
    int      n_pages;
    int      page_h;      /* 单页可用高度 */
    pic_ent_t *pics;      /* 本章所有图片（原图）列表 */
    int       n_pics;
} layout_t;

/* 对一章排版：html → blocks → 折行/对齐/分页。ep 用于取图片资源（可为 NULL 则图片显示占位）。
   doc_href = 当前章节 XHTML 在 zip 内的路径，用于相对它解析图片 src。 */
layout_t *layout_chapter(reader_ui_t *ui, epub_t *ep, const char *html, const char *doc_href);
void layout_free(layout_t *L);

/* 绘制排好版的一页（替代旧 ui_draw_reader）。正文用 ui->fg 色，标题/引用用固定色。
   focus_line>=0 且 focus_label 非空时，在对应图片行画金色高亮框 + 标签。 */
void ui_draw_reader_layout(reader_ui_t *ui, layout_t *L, int page,
                           const char *title, int pct, int bookmark_on,
                           int focus_line, const char *focus_label);

#endif /* LAYOUT_H */
