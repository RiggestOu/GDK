#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* 大小写不敏感子串查找 */
static inline char *stristr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char*)hay;
    for (; *hay; ++hay) {
        if (strncasecmp(hay, needle, nlen) == 0) return (char*)hay;
    }
    return NULL;
}

/* 复制子串 [s, s+n) */
static inline char *strndup_(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* 去除首尾空白（原地） */
static inline char *trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* 从一段 tag 文本中取出属性 attr 的值，返回新分配字符串（需 free）或 NULL。
   例如 get_attr("<item href=\"a\"/>", "href") -> "a" */
char *get_attr(const char *tag, const char *attr);

/* 取出第一个 <tag ...> ... </tag> 之间的内容，返回新分配字符串（需 free）或 NULL。
   大小写不敏感匹配 tag 名。 */
char *tag_content(const char *buf, const char *tag);

/* 把 XML 实体解码为 UTF-8 文本，返回新分配字符串（需 free）。
   支持 &amp; &lt; &gt; &quot; &apos; &#NNN; &#xHH; */
char *decode_entities(const char *s);

/* 去掉所有 XML/HTML 标签，保留文本，返回新分配字符串（需 free）。
   会把常见块级标签替换为换行。 */
char *strip_tags(const char *html);

#endif /* UTIL_H */
