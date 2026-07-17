#include "render.h"
#include <stdlib.h>
#include <string.h>

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

static const char *try_fonts[] = {
    "./font.ttf",
    "font.ttf",
    "/usr/share/fonts/truetype/wenquanyi/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL
};

reader_ui_t *ui_init(const char *font_path) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) return NULL;
    if (TTF_Init() < 0) { SDL_Quit(); return NULL; }

    reader_ui_t *ui = calloc(1, sizeof(reader_ui_t));
    if (!ui) { TTF_Quit(); SDL_Quit(); return NULL; }

    ui->screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    if (!ui->screen) { free(ui); TTF_Quit(); SDL_Quit(); return NULL; }

    const char *fp = font_path;
    int i = 0;
    while (!ui->font && (fp || try_fonts[i])) {
        const char *p = fp ? fp : try_fonts[i];
        ui->font = TTF_OpenFont(p, 14);
        if (fp) break;
        fp = NULL; i++;
    }
    if (!ui->font) { free(ui); TTF_Quit(); SDL_Quit(); return NULL; }

    ui->fg_r = 235; ui->fg_g = 235; ui->fg_b = 235;
    ui->bg_r = 18;  ui->bg_g = 18;  ui->bg_b = 18;
    ui->margin = 6;
    ui->line_h = TTF_FontHeight(ui->font) + 2;
    ui->title_h = 16;
    ui->prog_h  = 10;
    return ui;
}

void ui_quit(reader_ui_t *ui) {
    if (!ui) return;
    if (ui->font) TTF_CloseFont(ui->font);
    free(ui);
    TTF_Quit();
    SDL_Quit();
}

void ui_clear(reader_ui_t *ui) {
    SDL_FillRect(ui->screen, NULL, SDL_MapRGB(ui->screen->format, ui->bg_r, ui->bg_g, ui->bg_b));
}
void ui_flip(reader_ui_t *ui) { SDL_Flip(ui->screen); }

void ui_rect(reader_ui_t *ui, int x, int y, int w, int h, int r, int g, int b) {
    SDL_Rect rc = { x, y, w, h };
    SDL_FillRect(ui->screen, &rc, SDL_MapRGB(ui->screen->format, r, g, b));
}

void ui_text(reader_ui_t *ui, int x, int y, const char *text) {
    if (!text || !*text) return;
    SDL_Color fg = { ui->fg_r, ui->fg_g, ui->fg_b, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Solid(ui->font, text, fg);
    if (!s) return;
    SDL_Rect dst = { x, y, 0, 0 };
    SDL_BlitSurface(s, NULL, ui->screen, &dst);
    SDL_FreeSurface(s);
}

char **wrap_text(reader_ui_t *ui, const char *text, int *out_n) {
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

    /* 测量每个 token 宽度 */
    int space_w = 0, h = 0;
    TTF_SizeUTF8(ui->font, " ", &space_w, &h);
    for (int i = 0; i < nt; i++) TTF_SizeUTF8(ui->font, toks[i].s, &toks[i].w, &h);

    int maxw = SCREEN_W - 2 * ui->margin;
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
    int body = SCREEN_H - ui->title_h - ui->prog_h;
    int pp = body / ui->line_h;
    return pp > 1 ? pp : 1;
}

void ui_render_page(reader_ui_t *ui, char **lines, int n_lines, int page, int per_page,
                    const char *title, int pct) {
    ui_clear(ui);
    /* 标题栏 */
    ui_rect(ui, 0, 0, SCREEN_W, ui->title_h, 40, 60, 110);
    if (title) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.28s", title);
        SDL_Color fg = { 255, 255, 255, 255 };
        SDL_Surface *s = TTF_RenderUTF8_Solid(ui->font, buf, fg);
        if (s) { SDL_Rect d = { ui->margin, 1, 0, 0 }; SDL_BlitSurface(s, NULL, ui->screen, &d); SDL_FreeSurface(s); }
    }
    /* 正文 */
    int start = page * per_page;
    for (int i = 0; i < per_page; i++) {
        int idx = start + i;
        if (idx >= n_lines) break;
        ui_text(ui, ui->margin, ui->title_h + i * ui->line_h, lines[idx]);
    }
    /* 进度条 */
    int y = SCREEN_H - ui->prog_h;
    ui_rect(ui, 0, y, SCREEN_W, ui->prog_h, 30, 30, 30);
    int w = (SCREEN_W * pct) / 100;
    ui_rect(ui, 0, y, w, ui->prog_h, 80, 140, 220);
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    ui_text(ui, SCREEN_W - 36, y + 1, pbuf);
    ui_flip(ui);
}
