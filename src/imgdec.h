#ifndef IMGDEC_H
#define IMGDEC_H
#include "SDL.h"
#include <stddef.h>

/* 解码图片（当前支持 WebP）并按需缩放到 maxw 宽以内。
   返回 SDL_Surface（调用者 SDL_FreeSurface），失败返回 NULL。 */
SDL_Surface *img_decode_scaled(const unsigned char *buf, size_t sz, int maxw, int maxh);

#endif
