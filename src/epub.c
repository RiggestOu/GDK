#include "epub.h"
#include "util.h"
#include <stdlib.h>

static char *strip_fragment(const char *href) {
    char *h = strdup(href);
    char *f = strchr(h, '#');
    if (f) *f = 0;
    return h;
}

char *epub_resolve(epub_t *e, const char *href) {
    char *h = strip_fragment(href);
    char *out;
    if (h[0] == '/') {
        out = strdup(h + 1);
    } else {
        size_t bl = strlen(e->base_dir);
        out = malloc(bl + strlen(h) + 2);
        strcpy(out, e->base_dir);
        if (bl && e->base_dir[bl - 1] != '/') strcat(out, "/");
        strcat(out, h);
    }
    free(h);
    return out;
}

int epub_find_spine(epub_t *e, const char *href) {
    char *path = strip_fragment(href);
    char *base = strrchr(path, '/');
    const char *bn = base ? base + 1 : path;
    for (int i = 0; i < e->n_spine; i++) {
        char *sp = strip_fragment(e->spine[i]);
        char *sb = strrchr(sp, '/');
        const char *sbn = sb ? sb + 1 : sp;
        if (strcasecmp(sbn, bn) == 0) { free(sp); free(path); return i; }
        free(sp);
    }
    free(path);
    return -1;
}

char *epub_read_text(epub_t *e, const char *href) {
    char *resolved = epub_resolve(e, href);
    size_t sz = 0;
    unsigned char *data = zip_read(e->zip, resolved, &sz);
    free(resolved);
    if (!data) return NULL;
    char *text = strip_tags((const char *)data);
    free(data);
    return text;
}

/* 解析 manifest：遍历 <item ...>，跳过 <itemref */
static void parse_manifest(epub_t *e, const char *opf) {
    const char *p = opf;
    while ((p = stristr(p, "<item")) != NULL) {
        if (p[5] == 'r') { p += 5; continue; } /* <itemref，跳过 */
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char *tag = strndup_(p, (size_t)(gt - p + 1));
        char *id = get_attr(tag, "id");
        char *href = get_attr(tag, "href");
        char *mt = get_attr(tag, "media-type");
        char *prop = get_attr(tag, "properties");
        if (href) {
            e->items = realloc(e->items, (e->n_items + 1) * sizeof(manifest_item_t));
            manifest_item_t *it = &e->items[e->n_items++];
            it->id = id ? id : strdup("");
            it->href = href;
            it->media_type = mt ? mt : strdup("");
            it->properties = prop ? prop : strdup("");
            id = href = mt = prop = NULL; /* 已移交 */
        }
        free(id); free(href); free(mt); free(prop);
        free(tag);
        p = gt + 1;
    }
}

static const char *manifest_href_by_id(epub_t *e, const char *idref) {
    for (int i = 0; i < e->n_items; i++)
        if (strcmp(e->items[i].id, idref) == 0) return e->items[i].href;
    return NULL;
}

/* 解析 spine：遍历 <itemref idref=...> */
static void parse_spine(epub_t *e, const char *opf) {
    const char *p = opf;
    while ((p = stristr(p, "<itemref")) != NULL) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char *tag = strndup_(p, (size_t)(gt - p + 1));
        char *idref = get_attr(tag, "idref");
        if (idref) {
            const char *href = manifest_href_by_id(e, idref);
            if (href) {
                e->spine = realloc(e->spine, (e->n_spine + 1) * sizeof(char *));
                e->spine[e->n_spine++] = strdup(href);
            }
        }
        free(idref);
        free(tag);
        p = gt + 1;
    }
}

/* 解析 EPUB3 nav 文档（扫描 <a href>...）</a>） */
static void parse_nav(epub_t *e, const char *navtext) {
    const char *p = navtext;
    while ((p = stristr(p, "<a ")) != NULL) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char *tag = strndup_(p, (size_t)(gt - p + 1));
        char *href = get_attr(tag, "href");
        const char *a_start = gt + 1;
        const char *a_end = stristr(a_start, "</a>");
        char *raw = a_end ? strndup_(a_start, (size_t)(a_end - a_start)) : strdup("");
        char *label = strip_tags(raw);
        if (href && label && *trim(label)) {
            e->toc = realloc(e->toc, (e->n_toc + 1) * sizeof(toc_entry_t));
            e->toc[e->n_toc].label = label;
            e->toc[e->n_toc].href = href;
            e->n_toc++;
            label = NULL; href = NULL;
        }
        free(tag); free(raw); free(label); free(href);
        p = gt + 1;
    }
}

/* 解析 EPUB2 NCX（<navPoint><navLabel><text>..</text></navLabel><content src=..></navPoint>） */
static void parse_ncx(epub_t *e, const char *ncx) {
    const char *p = ncx;
    while ((p = stristr(p, "<navPoint")) != NULL) {
        const char *end = stristr(p, "</navPoint>");
        if (!end) break;
        char *block = strndup_(p, (size_t)(end - p) + 12);
        char *navlabel = tag_content(block, "navLabel");
        char *label = NULL;
        if (navlabel) {
            char *t = tag_content(navlabel, "text");
            if (t) { label = trim(t); free(t); }
            free(navlabel);
        }
        char *content = stristr(block, "<content");
        char *href = NULL;
        if (content) {
            const char *gt = strchr(content, '>');
            if (gt) {
                char *ctag = strndup_(content, (size_t)(gt - content + 1));
                href = get_attr(ctag, "src");
                free(ctag);
            }
        }
        if (label && href && *label) {
            e->toc = realloc(e->toc, (e->n_toc + 1) * sizeof(toc_entry_t));
            e->toc[e->n_toc].label = label;
            e->toc[e->n_toc].href = href;
            e->n_toc++;
            label = NULL; href = NULL;
        }
        free(block); free(label); free(href);
        p = end + 12;
    }
}

static void parse_toc(epub_t *e) {
    /* 优先 EPUB3 nav */
    const char *nav_href = NULL;
    for (int i = 0; i < e->n_items; i++) {
        if (strstr(e->items[i].properties, "nav") &&
            strstr(e->items[i].media_type, "xhtml")) {
            nav_href = e->items[i].href;
            break;
        }
    }
    if (nav_href) {
        char *resolved = epub_resolve(e, nav_href);
        size_t sz = 0;
        unsigned char *data = zip_read(e->zip, resolved, &sz);
        free(resolved);
        if (data) { parse_nav(e, (const char *)data); free(data); }
    }
    /* 回退 EPUB2 NCX */
    if (e->n_toc == 0) {
        for (int i = 0; i < e->n_items; i++) {
            if (strstr(e->items[i].media_type, "x-dtbncx+xml")) {
                char *resolved = epub_resolve(e, e->items[i].href);
                size_t sz = 0;
                unsigned char *data = zip_read(e->zip, resolved, &sz);
                free(resolved);
                if (data) { parse_ncx(e, (const char *)data); free(data); }
                break;
            }
        }
    }
    /* 都没有则按 spine 生成章节列表 */
    if (e->n_toc == 0) {
        for (int i = 0; i < e->n_spine; i++) {
            e->toc = realloc(e->toc, (e->n_toc + 1) * sizeof(toc_entry_t));
            char buf[32];
            snprintf(buf, sizeof(buf), "Chapter %d", i + 1);
            e->toc[e->n_toc].label = strdup(buf);
            e->toc[e->n_toc].href = strdup(e->spine[i]);
            e->n_toc++;
        }
    }
}

epub_t *epub_open(zip_t *z) {
    epub_t *e = calloc(1, sizeof(epub_t));
    if (!e) return NULL;
    e->zip = z;

    size_t sz = 0;
    unsigned char *cont = zip_read(z, "META-INF/container.xml", &sz);
    if (!cont) { epub_close(e); return NULL; }
    /* <rootfile> 是自闭合标签，属性在标签内，需从标签本身取 full-path */
    const char *rp = stristr((const char *)cont, "<rootfile");
    char *opf = NULL;
    if (rp) {
        const char *gt = strchr(rp, '>');
        if (gt) {
            char *tag = strndup_(rp, (size_t)(gt - rp + 1));
            opf = get_attr(tag, "full-path");
            free(tag);
        }
    }
    if (!opf) { free(cont); epub_close(e); return NULL; }
    e->opf_path = strdup(opf);
    free(cont); free(opf);

    char *slash = strrchr(e->opf_path, '/');
    if (slash) {
        size_t bl = (size_t)(slash - e->opf_path + 1);
        e->base_dir = malloc(bl + 1);
        memcpy(e->base_dir, e->opf_path, bl);
        e->base_dir[bl] = 0;
    } else {
        e->base_dir = strdup("");
    }

    unsigned char *opftxt = zip_read(z, e->opf_path, &sz);
    if (!opftxt) { epub_close(e); return NULL; }
    parse_manifest(e, (const char *)opftxt);
    parse_spine(e, (const char *)opftxt);
    parse_toc(e);
    free(opftxt);
    return e;
}

void epub_close(epub_t *e) {
    if (!e) return;
    for (int i = 0; i < e->n_items; i++) {
        free(e->items[i].id); free(e->items[i].href);
        free(e->items[i].media_type); free(e->items[i].properties);
    }
    free(e->items);
    for (int i = 0; i < e->n_spine; i++) free(e->spine[i]);
    free(e->spine);
    for (int i = 0; i < e->n_toc; i++) {
        free(e->toc[i].label); free(e->toc[i].href);
    }
    free(e->toc);
    free(e->opf_path);
    free(e->base_dir);
    free(e);
}
