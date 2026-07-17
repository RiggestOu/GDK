#include "util.h"
#include <stdlib.h>

char *get_attr(const char *tag, const char *attr) {
    if (!tag || !attr) return NULL;
    size_t al = strlen(attr);
    const char *p = tag;
    while ((p = stristr(p, attr)) != NULL) {
        /* 属性名前必须是空白或 '<' */
        const char *before = (p == tag) ? " " : p - 1;
        if (!isspace((unsigned char)*before) && *before != '<') { p += al; continue; }
        const char *q = p + al;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != '=') { p += al; continue; }
        q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != '"' && *q != '\'') { p += al; continue; }
        char qc = *q; q++;
        const char *start = q;
        while (*q && *q != qc) q++;
        return strndup_(start, q - start);
    }
    return NULL;
}

char *tag_content(const char *buf, const char *tag) {
    if (!buf || !tag) return NULL;
    char open[64];
    snprintf(open, sizeof(open), "<%s", tag);
    const char *p = stristr(buf, open);
    if (!p) return NULL;
    const char *q = p;
    while (*q && *q != '>') q++;
    if (*q != '>') return NULL;
    q++;
    char close[64];
    snprintf(close, sizeof(close), "</%s", tag);
    const char *end = stristr(q, close);
    if (!end) return NULL;
    return strndup_(q, end - q);
}

char *decode_entities(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (s[i] == '&') {
            const char *semi = strchr(s + i, ';');
            if (semi) {
                size_t el = (size_t)(semi - (s + i));
                if (el > 1 && el < 64) {
                    char ent[64];
                    memcpy(ent, s + i + 1, el - 1);
                    ent[el - 1] = 0;
                    unsigned long cp = 0; int ok = 0;
                    if      (strcmp(ent, "amp")  == 0) { cp = '&';  ok = 1; }
                    else if (strcmp(ent, "lt")   == 0) { cp = '<';  ok = 1; }
                    else if (strcmp(ent, "gt")   == 0) { cp = '>';  ok = 1; }
                    else if (strcmp(ent, "quot") == 0) { cp = '"';  ok = 1; }
                    else if (strcmp(ent, "apos") == 0) { cp = '\''; ok = 1; }
                    else if (ent[0] == '#') {
                        cp = (ent[1] == 'x' || ent[1] == 'X')
                             ? strtoul(ent + 2, NULL, 16)
                             : strtoul(ent + 1, NULL, 10);
                        ok = 1;
                    }
                    if (ok) {
                        if      (cp < 0x80)       out[o++] = (char)cp;
                        else if (cp < 0x800)      { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                        else if (cp < 0x10000)    { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                        else                      { out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                        i = (size_t)(semi - s) + 1;
                        continue;
                    }
                }
            }
        }
        out[o++] = s[i++];
    }
    out[o] = 0;
    return out;
}

char *strip_tags(const char *html) {
    if (!html) return NULL;
    size_t len = strlen(html);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (html[i] == '<') {
            size_t j = i + 1;
            while (j < len && (isalnum((unsigned char)html[j]) || html[j] == '/')) j++;
            size_t tlen = j - (i + 1);
            char tname[32];
            size_t tl = tlen < 31 ? tlen : 31;
            memcpy(tname, html + i + 1, tl);
            tname[tl] = 0;
            while (i < len && html[i] != '>') i++;
            if (strcasecmp(tname, "p") == 0 || strcasecmp(tname, "div") == 0 ||
                strcasecmp(tname, "br") == 0 || strcasecmp(tname, "li") == 0 ||
                strcasecmp(tname, "tr") == 0 || strcasecmp(tname, "h1") == 0 ||
                strcasecmp(tname, "h2") == 0 || strcasecmp(tname, "h3") == 0 ||
                strcasecmp(tname, "h4") == 0 || strcasecmp(tname, "h5") == 0 ||
                strcasecmp(tname, "h6") == 0 || strcasecmp(tname, "section") == 0 ||
                strcasecmp(tname, "/p") == 0 || strcasecmp(tname, "/div") == 0) {
                out[o++] = '\n';
            }
        } else {
            out[o++] = html[i];
        }
    }
    out[o] = 0;
    char *dec = decode_entities(out);
    free(out);
    return dec;
}
