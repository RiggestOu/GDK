#ifndef IMGDEC_H
#define IMGDEC_H
#include "SDL.h"
#include <stddef.h>

/* 解码图片（当前支持 WebP）并按需缩放到 maxw 宽以内。
   返回 SDL_Surface（调用者 SDL_FreeSurface），失败返回 NULL。 */
SDL_Surface *img_decode_scaled(const unsigned char *buf, size_t sz, int maxw, int maxh);

/* 解码图片原始分辨率（不缩放），供缩放查看器放大细节用。
   同样白底合成 alpha → DisplayFormat。返回 SDL_Surface（调用者 SDL_FreeSurface），失败 NULL。 */
SDL_Surface *img_decode(const unsigned char *buf, size_t sz);

#endif
