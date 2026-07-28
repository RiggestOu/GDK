#include "imgdec.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "webp/decode.h"   /* libwebp（静态链接，-O0 交叉编译） */

/* RGBA8888 → SDL_Surface（32bpp），最近邻缩放到 dw x dh */
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
            /* 白底合成 alpha（阅读器深色背景下直接丢 alpha 会出黑边，书籍插图多为白底） */
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

SDL_Surface *img_decode_scaled(const unsigned char *buf, size_t sz, int maxw, int maxh) {
    if (!buf || sz < 16) return NULL;
    /* WebP: RIFF....WEBP */
    if (memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WEBP", 4) == 0) {
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
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        SDL_Surface *s = rgba_to_surface_scaled(rgba, w, h, dw, dh);
        WebPFree(rgba);
        return s;
    }
    /* 其它格式暂不支持（PNG/JPG 的系统解码库带 MXU 雷） */
    return NULL;
}
