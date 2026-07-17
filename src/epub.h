#ifndef EPUB_H
#define EPUB_H
#include "zip.h"

typedef struct {
    char *id;          /* manifest id */
    char *href;        /* 相对 OPF 的路径 */
    char *media_type;
    char *properties;  /* 例如 "nav" */
} manifest_item_t;

typedef struct {
    char *label;       /* 目录显示名 */
    char *href;        /* 目标（可能含 #fragment，相对 OPF 目录） */
} toc_entry_t;

typedef struct {
    zip_t  *zip;
    char   *opf_path;  /* zip 内完整路径 */
    char   *base_dir;  /* OPF 所在目录（结尾带 '/'，可能为空串） */
    manifest_item_t *items;
    int     n_items;
    char  **spine;     /* 阅读顺序中的 href（相对 OPF） */
    int     n_spine;
    toc_entry_t *toc;
    int     n_toc;
} epub_t;

/* 打开并解析 epub（z 由调用者 zip_open 得到，epub_close 时不会关闭 z） */
epub_t *epub_open(zip_t *z);
void epub_close(epub_t *e);

/* 解析相对路径（相对 OPF 目录）为 zip 内完整路径，返回 malloc（需 free） */
char *epub_resolve(epub_t *e, const char *href);

/* 取出某个内容文档的纯文本（strip_tags + decode），返回 malloc 字符串（需 free）。
   href 可带 #fragment，会被忽略。 */
char *epub_read_text(epub_t *e, const char *href);

/* 在 spine 中查找与给定 href（路径部分）匹配的章节索引，找不到返回 -1 */
int epub_find_spine(epub_t *e, const char *href);

#endif /* EPUB_H */
