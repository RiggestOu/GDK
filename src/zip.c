#include "zip.h"
#include <string.h>
#include <zlib.h>

static unsigned long get32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned short get16(const unsigned char *p) {
    return (unsigned short)(p[0] | (p[1] << 8));
}

zip_t *zip_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize < 22) { fclose(fp); return NULL; }

    /* 在文件末尾扫描 EOCD 签名 0x06054b50 */
    long maxback = fsize < 65557 ? fsize : 65557;
    unsigned char *buf = malloc(maxback);
    if (!buf) { fclose(fp); return NULL; }
    fseek(fp, fsize - maxback, SEEK_SET);
    fread(buf, 1, maxback, fp);

    long eocd = -1;
    for (long i = maxback - 22; i >= 0; i--) {
        if (buf[i] == 0x50 && buf[i+1] == 0x4b &&
            buf[i+2] == 0x05 && buf[i+3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { free(buf); fclose(fp); return NULL; }

    unsigned long cd_offset = get32(buf + eocd + 16);
    unsigned short cd_count = get16(buf + eocd + 10);
    free(buf);

    fseek(fp, cd_offset, SEEK_SET);

    zip_t *z = calloc(1, sizeof(zip_t));
    if (!z) { fclose(fp); return NULL; }
    z->fp = fp;
    z->entries = calloc(cd_count ? cd_count : 1, sizeof(zip_entry_t));
    z->n = 0;
    if (!z->entries) { free(z); fclose(fp); return NULL; }

    for (int i = 0; i < cd_count; i++) {
        unsigned char hdr[46];
        if (fread(hdr, 1, 46, fp) != 46) break;
        if (!(hdr[0] == 0x50 && hdr[1] == 0x4b && hdr[2] == 0x01 && hdr[3] == 0x02))
            break;
        unsigned short nlen = get16(hdr + 28);
        unsigned short elen = get16(hdr + 30);
        unsigned short clen = get16(hdr + 32);
        unsigned long  csize = get32(hdr + 20);
        unsigned long  usize = get32(hdr + 24);
        unsigned short method = get16(hdr + 10);
        unsigned long  local = get32(hdr + 42);

        char *name = malloc(nlen + 1);
        if (!name) break;
        if (fread(name, 1, nlen, fp) != nlen) { free(name); break; }
        name[nlen] = 0;
        fseek(fp, (long)elen + clen, SEEK_CUR); /* 跳过 extra + comment */

        zip_entry_t *e = &z->entries[z->n++];
        e->name = name;
        e->comp_size = csize;
        e->uncomp_size = usize;
        e->method = method;
        e->local_offset = local;
    }
    return z;
}

void zip_close(zip_t *z) {
    if (!z) return;
    for (int i = 0; i < z->n; i++) free(z->entries[i].name);
    free(z->entries);
    if (z->fp) fclose(z->fp);
    free(z);
}

const zip_entry_t *zip_find(zip_t *z, const char *name) {
    if (!z || !name) return NULL;
    size_t nl = strlen(name);
    for (int i = 0; i < z->n; i++) {
        if (strcasecmp(z->entries[i].name, name) == 0) return &z->entries[i];
    }
    /* 后缀匹配：支持只给文件名，如 "content.opf" */
    for (int i = 0; i < z->n; i++) {
        char *slash = strrchr(z->entries[i].name, '/');
        const char *base = slash ? slash + 1 : z->entries[i].name;
        if (strcasecmp(base, name) == 0) return &z->entries[i];
        size_t el = strlen(z->entries[i].name);
        if (el >= nl && strcasecmp(z->entries[i].name + el - nl, name) == 0)
            return &z->entries[i];
    }
    return NULL;
}

unsigned char *zip_read(zip_t *z, const char *name, size_t *size) {
    const zip_entry_t *e = zip_find(z, name);
    if (!e) return NULL;

    fseek(z->fp, (long)e->local_offset, SEEK_SET);
    unsigned char lh[30];
    if (fread(lh, 1, 30, z->fp) != 30) return NULL;
    unsigned short nlen = get16(lh + 26);
    unsigned short elen = get16(lh + 28);
    fseek(z->fp, (long)nlen + elen, SEEK_CUR);

    if (e->method == 0) { /* 存储 */
        unsigned char *out = malloc(e->comp_size + 1);
        if (!out) return NULL;
        if (fread(out, 1, e->comp_size, z->fp) != e->comp_size) { free(out); return NULL; }
        out[e->comp_size] = 0;
        if (size) *size = e->comp_size;
        return out;
    }
    if (e->method == 8) { /* deflate */
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return NULL;

        size_t cap = e->uncomp_size ? e->uncomp_size : (1u << 20);
        unsigned char *buf = malloc(cap);
        if (!buf) { inflateEnd(&strm); return NULL; }
        unsigned char in[8192];
        size_t total = 0;
        int ret = Z_OK;

        while (ret != Z_STREAM_END) {
            if (strm.avail_in == 0) {
                size_t r = fread(in, 1, sizeof(in), z->fp);
                strm.next_in = in;
                strm.avail_in = (uInt)r;
                if (r == 0 && total > 0) break; /* 输入耗尽 */
            }
            strm.next_out = buf + total;
            strm.avail_out = (uInt)(cap - total);
            ret = inflate(&strm, Z_NO_FLUSH);
            total = cap - strm.avail_out;
            if (ret == Z_STREAM_END) break;
            if (ret != Z_OK) { free(buf); inflateEnd(&strm); return NULL; }
            if (strm.avail_out == 0) { /* 输出空间不足，扩容 */
                cap *= 2;
                unsigned char *nb = realloc(buf, cap);
                if (!nb) { free(buf); inflateEnd(&strm); return NULL; }
                buf = nb;
            }
        }
        inflateEnd(&strm);
        unsigned char *out = realloc(buf, total + 1);
        if (out) out[total] = 0;
        if (size) *size = total;
        return out ? out : buf;
    }
    return NULL;
}
