#ifndef ZIP_H
#define ZIP_H

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    char            *name;       /* 条目路径（zip 内部） */
    unsigned long   comp_size;   /* 压缩后大小 */
    unsigned long   uncomp_size; /* 解压后大小 */
    unsigned short  method;      /* 0=存储 8=deflate */
    unsigned long   local_offset;/* 本地文件头偏移 */
} zip_entry_t;

typedef struct {
    FILE         *fp;
    zip_entry_t  *entries;
    int           n;
} zip_t;

/* 打开一个 zip（如 .epub），成功返回 zip_t*，失败返回 NULL */
zip_t *zip_open(const char *path);

/* 关闭并释放 */
void zip_close(zip_t *z);

/* 按名字查找条目（大小写不敏感；也支持后缀匹配，如 "content.opf"） */
const zip_entry_t *zip_find(zip_t *z, const char *name);

/* 读取某条目完整内容到新分配缓冲区，*size 设为其长度；失败返回 NULL（需 free） */
unsigned char *zip_read(zip_t *z, const char *name, size_t *size);

#endif /* ZIP_H */
