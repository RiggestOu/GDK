#include "layout.h"
#include "util.h"
#include "imgdec.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================= XHTML → 块序列 ================= */

typedef struct {
    block_t *b; int n, cap;
} blist_t;

static void bl_push(blist_t *L, block_t blk) {
    if (L->n >= L->cap) { L->cap = L->cap ? L->cap * 2 : 16; L->b = realloc(L->b, L->cap * sizeof(block_t)); }
    L->b[L->n++] = blk;
}

/* 从标签文本里取对齐（style="text-align:center" 或 align="center"） */
static blk_align tag_align(const char *tag) {
    char *style = get_attr(tag, "style");
    blk_align a = AL_LEFT;
    if (style) {
        char *ta = stristr(style, "text-align");
        if (ta) {
            if (stristr(ta, "center")) a = AL_CENTER;
            else if (stristr(ta, "right")) a = AL_RIGHT;
        }
        free(style);
        if (a != AL_LEFT) return a;
    }
    char *al = get_attr(tag, "align");
    if (al) {
        if (!strcasecmp(al, "center")) a = AL_CENTER;
        else if (!strcasecmp(al, "right")) a = AL_RIGHT;
        free(al);
    }
    /* 常见 class 命名兜底 */
    if (a == AL_LEFT) {
        char *cls = get_attr(tag, "class");
        if (cls) {
            if (stristr(cls, "center") || stristr(cls, "cover")) a = AL_CENTER;
            else if (stristr(cls, "right")) a = AL_RIGHT;
            free(cls);
        }
    }
    return a;
}

/* 当前积累的文本块状态 */
typedef struct {
    char *buf; size_t len, cap;
    blk_type type; blk_align align; int bold;
} cur_t;

static void cur_append(cur_t *c, const char *s, size_t n) {
    if (c->len + n + 1 > c->cap) { c->cap = (c->len + n + 1) * 2 + 64; c->buf = realloc(c->buf, c->cap); }
    memcpy(c->buf + c->len, s, n); c->len += n; c->buf[c->len] = 0;
}

static int is_blank(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isspace(c)) {
            /* 全角空格 U+3000 = E3 80 80 也算空白 */
            if (c == 0xE3 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x80) { s += 2; continue; }
            return 0;
        }
    }
    return 1;
}

static void flush_cur(blist_t *L, cur_t *c) {
    if (c->len > 0 && !is_blank(c->buf)) {
        char *dec = decode_entities(c->buf);
        if (dec && !is_blank(dec)) {
            block_t b;
            b.type = c->type; b.align = c->align; b.bold = c->bold;
            b.text = strdup(trim(dec)); b.img_href = NULL;
            bl_push(L, b);
        }
        free(dec);
    }
    c->len = 0; if (c->buf) c->buf[0] = 0;
}

static int tag_is(const char *name, const char *t) { return strcasecmp(name, t) == 0; }

block_t *html_to_blocks(const char *html, int *out_n) {
    blist_t L = {0};
    cur_t cur = {0};
    cur.type = BLK_PARA; cur.align = AL_LEFT; cur.bold = 0;

    const char *p = stristr(html, "<body");
    if (!p) p = html; else { const char *g = strchr(p, '>'); p = g ? g + 1 : p; }

    while (*p) {
        if (*p != '<') {
            const char *lt = strchr(p, '<');
            size_t n = lt ? (size_t)(lt - p) : strlen(p);
            cur_append(&cur, p, n);
            p += n;
            continue;
        }
        /* 注释 */
        if (!strncmp(p, "<!--", 4)) { const char *e = strstr(p, "-->"); p = e ? e + 3 : p + strlen(p); continue; }
        /* 声明/PI */
        if (p[1] == '!' || p[1] == '?') { const char *g = strchr(p, '>'); p = g ? g + 1 : p + strlen(p); continue; }

        const char *gt = strchr(p, '>');
        if (!gt) break;
        /* 取标签名 */
        const char *q = p + 1; int closing = 0;
        if (*q == '/') { closing = 1; q++; }
        char name[16]; int ni = 0;
        while (q < gt && ni < 15 && (isalnum((unsigned char)*q) || *q == ':')) name[ni++] = *q++;
        name[ni] = 0;
        char *tag = strndup_(p, (size_t)(gt - p + 1));

        if (!closing && (tag_is(name, "head") )) {
            const char *e = stristr(gt, "</head");
            p = e ? (strchr(e, '>') ? strchr(e, '>') + 1 : e + 6) : gt + 1;
            free(tag); continue;
        }
        if (!closing && (tag_is(name, "style") || tag_is(name, "script"))) {
            const char *e = stristr(gt, tag_is(name, "style") ? "</style" : "</script");
            p = e ? (strchr(e, '>') ? strchr(e, '>') + 1 : e + 8) : gt + 1;
            free(tag); continue;
        }
        if (!closing && (tag_is(name, "img") || tag_is(name, "image"))) {
            flush_cur(&L, &cur);
            char *src = get_attr(tag, "src");
            if (!src) src = get_attr(tag, "xlink:href");
            if (!src) src = get_attr(tag, "href");
            if (src) {
                block_t b; memset(&b, 0, sizeof(b));
                b.type = BLK_IMG; b.align = AL_CENTER; b.img_href = src;
                bl_push(&L, b);
            }
            free(tag); p = gt + 1; continue;
        }
        /* 块级标签 */
        int h = 0;
        if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) h = name[1] - '0';
        int is_block = h || tag_is(name, "p") || tag_is(name, "div") || tag_is(name, "li") ||
                       tag_is(name, "blockquote") || tag_is(name, "section") || tag_is(name, "tr") ||
                       tag_is(name, "td") || tag_is(name, "title") || tag_is(name, "table") ||
                       tag_is(name, "ul") || tag_is(name, "ol") || tag_is(name, "br") || tag_is(name, "hr");
        if (is_block) {
            flush_cur(&L, &cur);
            if (!closing) {
                if (h == 1) { cur.type = BLK_H1; cur.bold = 1; cur.align = AL_CENTER; }
                else if (h == 2) { cur.type = BLK_H2; cur.bold = 1; cur.align = AL_LEFT; }
                else if (h >= 3) { cur.type = BLK_H3; cur.bold = 1; cur.align = AL_LEFT; }
                else if (tag_is(name, "blockquote")) { cur.type = BLK_QUOTE; cur.bold = 0; cur.align = AL_LEFT; }
                else if (!tag_is(name, "br") && !tag_is(name, "hr")) { cur.type = BLK_PARA; cur.bold = 0; cur.align = AL_LEFT; }
                /* 标签自带对齐覆盖默认 */
                if (!tag_is(name, "br") && !tag_is(name, "hr")) {
                    blk_align a = tag_align(tag);
                    if (a != AL_LEFT) cur.align = a;
                }
            } else {
                cur.type = BLK_PARA; cur.bold = 0; cur.align = AL_LEFT;
            }
        } else if ((tag_is(name, "b") || tag_is(name, "strong")) && !closing) {
            if (cur.len == 0 || is_blank(cur.buf)) cur.bold = 1; /* 整块加粗近似 */
        }
        free(tag);
        p = gt + 1;
    }
    flush_cur(&L, &cur);
    free(cur.buf);
    *out_n = L.n;
    return L.b;
}

void blocks_free(block_t *b, int n) {
    for (int i = 0; i < n; i++) { free(b[i].text); free(b[i].img_href); }
    free(b);
}

/* ================= 排版（块 → 像素行 + 分页） ================= */

typedef struct { rline_t *l; int n, cap; } rlist_t;
static rline_t *rl_push(rlist_t *L) {
    if (L->n >= L->cap) { L->cap = L->cap ? L->cap * 2 : 64; L->l = realloc(L->l, L->cap * sizeof(rline_t)); }
    rline_t *r = &L->l[L->n++];
    memset(r, 0, sizeof(*r));
    return r;
}

layout_t *layout_chapter(reader_ui_t *ui, epub_t *ep, const char *html, const char *doc_href) {
    layout_t *L = calloc(1, sizeof(layout_t));
    L->page_h = SCREEN_H - TITLE_H - STATUS_H - PROG_H - 6;

    int nb = 0;
    block_t *blocks = html_to_blocks(html, &nb);

    rlist_t rl = {0};
    int y = 0;
    int maxw_full = SCREEN_W - 2 * ui->margin;

    /* 本章图片原图收集（供缩放查看器放大细节），复用 layout.h 的 pic_ent_t */
    pic_ent_t *picbuf = NULL; int npic = 0, cap_pic = 0;

    for (int i = 0; i < nb; i++) {
        block_t *b = &blocks[i];
        if (b->type == BLK_IMG) {
            SDL_Surface *img = NULL;
            if (ep && b->img_href) {
                size_t isz = 0;
                unsigned char *ibuf = epub_read_file_rel(ep, doc_href, b->img_href, &isz);
                fprintf(stderr, "[img] src=%s read=%s size=%zu\n", b->img_href, ibuf ? "OK" : "NULL", isz);
                if (ibuf) {
                    img = img_decode_scaled(ibuf, isz, maxw_full, L->page_h - 4);
                    fprintf(stderr, "[img] decode=%s\n", img ? "OK" : "NULL");
                    free(ibuf);
                }
            }
            if (img) {
                /* 解码原图（供缩放查看器放大细节；失败存 NULL，缩放时显示占位） */
                SDL_Surface *full = NULL;
                if (ep && b->img_href) {
                    size_t fsz = 0;
                    unsigned char *fbuf = epub_read_file_rel(ep, doc_href, b->img_href, &fsz);
                    if (fbuf) { full = img_decode(fbuf, fsz); free(fbuf); }
                }
                if (npic >= cap_pic) { cap_pic = cap_pic ? cap_pic * 2 : 8; picbuf = realloc(picbuf, cap_pic * sizeof(pic_ent_t)); }
                picbuf[npic].line = rl.n;   /* 即将 push 的行索引 */
                picbuf[npic].page = -1;
                picbuf[npic].full = full;
                npic++;

                y += 4;
                rline_t *r = rl_push(&rl);
                r->img = img; r->h = (short)(img->h + 4);
                r->x = (short)(ui->margin + (maxw_full - img->w) / 2);
                r->y = y;
                y += r->h;
            } else {
                /* 解码失败/不支持 → 灰色占位行 */
                rline_t *r = rl_push(&rl);
                r->text = strdup("[图片无法显示]");
                r->style = 4; r->h = (short)ui->line_h;
                r->x = (short)(ui->margin + 40); r->y = y;
                y += r->h + 2;
            }
            continue;
        }
        if (!b->text || !*b->text) continue;

        int bold = 0;
        TTF_Font *f = ui_style_font(ui, (int)b->type, &bold);
        if (b->bold) bold = 1;
        int fh = TTF_FontHeight(f) + 2;
        int indent = 0;
        int maxw = maxw_full;
        int x0 = ui->margin;
        if (b->type == BLK_QUOTE) { x0 += 12; maxw -= 24; }

        /* 段落首行缩进：中文排版习惯，加两个全角空格 */
        char *text = b->text;
        char *tmp = NULL;
        if (b->type == BLK_PARA && b->align == AL_LEFT) {
            tmp = malloc(strlen(text) + 8);
            strcpy(tmp, "\xE3\x80\x80\xE3\x80\x80"); /* 　　 */
            strcat(tmp, text);
            text = tmp;
        }

        /* 块顶部间距 */
        if (b->type == BLK_H1) y += 10;
        else if (b->type == BLK_H2 || b->type == BLK_H3) y += 6;
        else y += 2;

        int nl = 0;
        char **lines = wrap_text_font(f, text, maxw, &nl);
        free(tmp);
        for (int k = 0; k < nl; k++) {
            if (!lines[k]) continue;
            rline_t *r = rl_push(&rl);
            r->text = lines[k];
            r->style = (unsigned char)b->type;
            r->bold = (unsigned char)bold;
            r->h = (short)fh;
            r->y = y;
            int lx = x0;
            if (b->align != AL_LEFT) {
                int w = 0, hh = 0;
                TTF_SizeUTF8(f, lines[k], &w, &hh);
                if (b->align == AL_CENTER) lx = x0 + (maxw - w) / 2;
                else lx = x0 + (maxw - w);
                if (lx < x0) lx = x0;
            }
            r->x = (short)lx;
            y += fh;
        }
        free(lines);
        /* 块底部间距 */
        if (b->type == BLK_H1) y += 6;
        else if (b->type == BLK_H2 || b->type == BLK_H3) y += 4;
    }
    blocks_free(blocks, nb);

    if (rl.n == 0) {
        rline_t *r = rl_push(&rl);
        r->text = strdup("(空章节)");
        r->h = (short)ui->line_h; r->x = (short)ui->margin; r->y = 0;
        y = ui->line_h;
    }

    L->lines = rl.l;
    L->n_lines = rl.n;
    L->doc_h = y;

    /* 分页：贪心装行，装不下的行开新页 */
    int cap = 16, np = 0;
    int *ps = malloc(cap * sizeof(int));
    int page_top = 0;
    ps[np++] = 0;
    for (int i = 0; i < L->n_lines; i++) {
        rline_t *r = &L->lines[i];
        if (r->y + r->h - page_top > L->page_h && r->y > page_top) {
            if (np >= cap) { cap *= 2; ps = realloc(ps, cap * sizeof(int)); }
            ps[np++] = i;
            page_top = r->y;
        }
    }
    L->page_start = ps;
    L->n_pages = np;

    /* 填每张图片所属页 */
    for (int k = 0; k < npic; k++) {
        int ln = picbuf[k].line;
        int pg = 0;
        while (pg + 1 < np && L->page_start[pg + 1] <= ln) pg++;
        picbuf[k].page = pg;
    }
    L->pics = picbuf;
    L->n_pics = npic;
    return L;
}

void layout_free(layout_t *L) {
    if (!L) return;
    for (int i = 0; i < L->n_lines; i++) {
        free(L->lines[i].text);
        if (L->lines[i].img) SDL_FreeSurface(L->lines[i].img);
    }
    free(L->lines);
    free(L->page_start);
    if (L->pics) {
        for (int i = 0; i < L->n_pics; i++)
            if (L->pics[i].full) SDL_FreeSurface(L->pics[i].full);
        free(L->pics);
    }
    free(L);
}

/* ================= 绘制一页 ================= */
void ui_draw_reader_layout(reader_ui_t *ui, layout_t *L, int page,
                           const char *title, int pct, int bookmark_on,
                           int focus_line, const char *focus_label) {
    ui_clear(ui);
    char t[32];
    snprintf(t, sizeof(t), "%.28s", title ? title : "");
    /* 标题栏 */
    ui_rect(ui, 0, 0, SCREEN_W, TITLE_H, ui->accent_r, ui->accent_g, ui->accent_b);
    ui_rect(ui, 0, TITLE_H - 1, SCREEN_W, 1, 0, 0, 0);
    ui_text_rgb(ui, ui->margin, 3, t, 255, 255, 255);
    if (bookmark_on) ui_text_rgb(ui, SCREEN_W - ui->margin - 12, 3, "★", 255, 220, 80);

    int body_top = TITLE_H + 3;
    if (L && L->n_pages > 0) {
        if (page < 0) page = 0;
        if (page >= L->n_pages) page = L->n_pages - 1;
        int start = L->page_start[page];
        int page_top = L->lines[start].y;
        int end = (page + 1 < L->n_pages) ? L->page_start[page + 1] : L->n_lines;
        for (int i = start; i < end; i++) {
            rline_t *r = &L->lines[i];
            int dy = body_top + (r->y - page_top);
            if (r->img) {
                SDL_Rect dst = { r->x, (Sint16)(dy + 2), 0, 0 };
                SDL_BlitSurface(r->img, NULL, ui->screen, &dst);
                if (i == focus_line && focus_label) {
                    /* 焦点高亮：金色双层边框 + 标签 */
                    ui_rect(ui, r->x - 2, dy + 1, r->img->w + 4, r->img->h + 4, 255, 210, 60);
                    ui_rect(ui, r->x,     dy + 3, r->img->w,     r->img->h,     255, 210, 60);
                    int ly = dy - 12; if (ly < TITLE_H + 1) ly = dy + r->img->h + 4;
                    ui_text_rgb(ui, r->x, ly, focus_label, 255, 210, 60);
                }
                continue;
            }
            if (!r->text) continue;
            int cr, cg, cb;
            if (r->style == 1 || r->style == 2 || r->style == 3) {
                /* 标题：固定色，不随正文颜色设置走 */
                cr = 245; cg = 245; cb = 250;
            } else if (r->style == 4) {
                cr = 180; cg = 186; cb = 200;
            } else {
                cr = ui->fg_r; cg = ui->fg_g; cb = ui->fg_b;
            }
            int bold = 0;
            TTF_Font *f = ui_style_font(ui, (int)r->style, &bold);
            if (r->bold) bold = 1;
            ui_text_font(ui, f, bold, r->x, dy, r->text, cr, cg, cb);
        }
    }

    /* 进度条 */
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
    ui_flip(ui);
}
