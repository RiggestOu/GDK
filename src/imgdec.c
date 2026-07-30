#include "imgdec.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "webp/decode.h"   /* libwebp（静态链接，-O0 交叉编译） */
#include "SDL.h"
/* 工具链缺 SDL_image.h 头，但构建已链接 -lSDL_image；此处自声明所需符号 */
extern SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc);

/* 任意 SDL_Surface → 屏幕格式(16bpp) + 白底合成 alpha，供缩放查看器 / 内嵌图使用。
   ⚠️ 本函数【不释放】src——由调用方统一管理生命周期。
   （历史 bug：旧版成功时内部 SDL_FreeSurface(src)，调用方又释放一次 → double free
    → 堆损坏 → uClibc abort()，表现为 JPEG/PNG 封面书籍开书即"Aborted"闪退。） */
static SDL_Surface *to_screen(SDL_Surface *src) {
    if (!src) return NULL;
    SDL_Surface *white = SDL_CreateRGBSurface(SDL_SWSURFACE, src->w, src->h, 32,
                                              0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (white) {
        SDL_FillRect(white, NULL, SDL_MapRGBA(white->format, 255, 255, 255, 255));
        SDL_BlitSurface(src, NULL, white, NULL); /* alpha 合成到白底（插图多为白底，避免深色背景透出） */
    }
    SDL_Surface *conv = white ? SDL_DisplayFormat(white) : SDL_DisplayFormat(src);
    if (white) SDL_FreeSurface(white);
    return conv ? conv : src;   /* 失败时原样返回 src，调用方据指针是否相同决定释放 */
}

/* RGBA8888 → SDL_Surface（32bpp），最近邻缩放到 dw x dh（webp 专用路径） */
static SDL_Surface *rgba_to_surface_scaled(const unsigned char *rgba, int w, int h, int dw, int dh) {
    SDL_Surface *s = SDL_CreateRGBSurface(SDL_SWSURFACE, dw, dh, 32,
                                          0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    if (!s) return NULL;
    if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * h / dh);
        Uint32 *dst = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        const unsigned char *srow = rgba + (size_t)sy * w * 4;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * w / dw);
            const unsigned char *px = srow + sx * 4;
            unsigned a = px[3];
            unsigned r = (px[0] * a + 255 * (255 - a)) / 255;
            unsigned g = (px[1] * a + 255 * (255 - a)) / 255;
            unsigned b = (px[2] * a + 255 * (255 - a)) / 255;
            dst[x] = (Uint32)(r | (g << 8) | (b << 16));
        }
    }
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    SDL_Surface *conv = SDL_DisplayFormat(s); /* 转屏幕格式(16bpp)加速 blit */
    if (conv) { SDL_FreeSurface(s); return conv; }
    return s;
}

static int is_webp(const unsigned char *buf, size_t sz) {
    return (sz >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WEBP", 4) == 0);
}

/* 按格式缩放并转屏幕格式。支持 EPUB 常见嵌入格式：WebP / PNG / JPG / GIF / BMP / TIFF 等。 */
SDL_Surface *img_decode_scaled(const unsigned char *buf, size_t sz, int maxw, int maxh) {
    if (!buf || sz < 8) return NULL;
    if (is_webp(buf, sz)) {
        int w = 0, h = 0;
        if (!WebPGetInfo(buf, sz, &w, &h) || w <= 0 || h <= 0) {
            fprintf(stderr, "[img] WebPGetInfo 失败\n");
            return NULL;
        }
        unsigned char *rgba = WebPDecodeRGBA(buf, sz, &w, &h);
        if (!rgba) { fprintf(stderr, "[img] WebPDecodeRGBA 失败 (%dx%d)\n", w, h); return NULL; }
        fprintf(stderr, "[img] WebP 解码成功 %dx%d\n", w, h);
        int dw = w, dh = h;
        if (dw > maxw) { dh = (int)((long)dh * maxw / dw); dw = maxw; }
        if (maxh > 0 && dh > maxh) { dw = (int)((long)dw * maxh / dh); dh = maxh; }
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        SDL_Surface *s = rgba_to_surface_scaled(rgba, w, h, dw, dh);
        WebPFree(rgba);
        return s;
    }
    /* 其它格式：SDL_image 解码（PNG/JPG/GIF/BMP/TIFF…），再适配缩放并转屏幕格式 */
    SDL_RWops *rw = SDL_RWFromMem((void *)buf, (int)sz);
    if (!rw) return NULL;
    SDL_Surface *raw = IMG_Load_RW(rw, 1);
    if (!raw) { fprintf(stderr, "[img] IMG_Load_RW 失败 (sz=%zu)\n", sz); return NULL; }
    fprintf(stderr, "[img] IMG 解码成功 %dx%d\n", raw->w, raw->h);
    int dw = raw->w, dh = raw->h;
    if (dw > maxw) { dh = (int)((long)dh * maxw / dw); dw = maxw; }
    if (maxh > 0 && dh > maxh) { dw = (int)((long)dw * maxh / dh); dh = maxh; }
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    SDL_Surface *sc = SDL_CreateRGBSurface(SDL_SWSURFACE, dw, dh,
                                           raw->format->BitsPerPixel,
                                           raw->format->Rmask, raw->format->Gmask,
                                           raw->format->Bmask, raw->format->Amask);
    if (sc) SDL_SoftStretch(raw, NULL, sc, NULL);
    SDL_Surface *conv = to_screen(sc ? sc : raw);   /* to_screen 不释放入参 */
    if (sc && conv != sc) SDL_FreeSurface(sc);
    if (conv != raw) SDL_FreeSurface(raw);          /* 仅当 raw 不是最终结果时才释放 */
    return conv;
}

/* 解码原图（不缩放），供缩放查看器放大细节。同样兼容多格式。 */
SDL_Surface *img_decode(const unsigned char *buf, size_t sz) {
    if (!buf || sz < 8) return NULL;
    if (is_webp(buf, sz)) {
        int w = 0, h = 0;
        if (!WebPGetInfo(buf, sz, &w, &h) || w <= 0 || h <= 0) {
            fprintf(stderr, "[img] WebPGetInfo 失败\n");
            return NULL;
        }
        unsigned char *rgba = WebPDecodeRGBA(buf, sz, &w, &h);
        if (!rgba) { fprintf(stderr, "[img] WebPDecodeRGBA 失败 (%dx%d)\n", w, h); return NULL; }
        fprintf(stderr, "[img] WebP 原图解码 %dx%d\n", w, h);
        SDL_Surface *s = rgba_to_surface_scaled(rgba, w, h, w, h);
        WebPFree(rgba);
        return s;
    }
    /* 非 webp：SDL_image 解码原图，转屏幕格式（含白底合成） */
    SDL_RWops *rw = SDL_RWFromMem((void *)buf, (int)sz);
    if (!rw) return NULL;
    SDL_Surface *raw = IMG_Load_RW(rw, 1);
    if (!raw) { fprintf(stderr, "[img] IMG_Load_RW 失败 (sz=%zu)\n", sz); return NULL; }
    fprintf(stderr, "[img] IMG 原图解码 %dx%d\n", raw->w, raw->h);
    SDL_Surface *conv = to_screen(raw);
    if (conv != raw) SDL_FreeSurface(raw);
    return conv;
}
