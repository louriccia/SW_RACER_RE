// SANM movie decoder. Blocky16 (video) and VIMA (audio) are ported from ScummVM's Grim engine,
// engines/grim/movie/codecs/blocky16.cpp and vima.cpp (GPLv3 or later), little-endian only.

#include "sanm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../stb_image.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

// ---------------------------------------------------------------------------------------------
// Blocky16
// ---------------------------------------------------------------------------------------------

static const int8_t table_small1[] = {0, 1, 2, 3, 3, 3, 3, 2, 1, 0, 0, 0, 1, 2, 2, 1};
static const int8_t table_small2[] = {0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 2, 1, 1, 1, 2, 2};
static const int8_t table_big1[] = {0, 2, 5, 7, 7, 7, 7, 7, 7, 5, 2, 0, 0, 0, 0, 0};
static const int8_t table_big2[] = {0, 0, 0, 0, 1, 3, 4, 6, 7, 7, 7, 7, 6, 4, 3, 1};

static const int8_t motion_table[] = {
    0, 0, -1, -43, 6, -43, -9, -42, 13, -41, -16, -40, 19, -39, -23, -36, 26, -34, -2, -33,
    4, -33, -29, -32, -9, -32, 11, -31, -16, -29, 32, -29, 18, -28, -34, -26, -22, -25, -1, -25,
    3, -25, -7, -24, 8, -24, 24, -23, 36, -23, -12, -22, 13, -21, -38, -20, 0, -20, -27, -19,
    -4, -19, 4, -19, -17, -18, -8, -17, 8, -17, 18, -17, 28, -17, 39, -17, -12, -15, 12, -15,
    -21, -14, -1, -14, 1, -14, -41, -13, -5, -13, 5, -13, 21, -13, -31, -12, -15, -11, -8, -11,
    8, -11, 15, -11, -2, -10, 1, -10, 31, -10, -23, -9, -11, -9, -5, -9, 4, -9, 11, -9,
    42, -9, 6, -8, 24, -8, -18, -7, -7, -7, -3, -7, -1, -7, 2, -7, 18, -7, -43, -6,
    -13, -6, -4, -6, 4, -6, 8, -6, -33, -5, -9, -5, -2, -5, 0, -5, 2, -5, 5, -5,
    13, -5, -25, -4, -6, -4, -3, -4, 3, -4, 9, -4, -19, -3, -7, -3, -4, -3, -2, -3,
    -1, -3, 0, -3, 1, -3, 2, -3, 4, -3, 6, -3, 33, -3, -14, -2, -10, -2, -5, -2,
    -3, -2, -2, -2, -1, -2, 0, -2, 1, -2, 2, -2, 3, -2, 5, -2, 7, -2, 14, -2,
    19, -2, 25, -2, 43, -2, -7, -1, -3, -1, -2, -1, -1, -1, 0, -1, 1, -1, 2, -1,
    3, -1, 10, -1, -5, 0, -3, 0, -2, 0, -1, 0, 1, 0, 2, 0, 3, 0, 5, 0,
    7, 0, -10, 1, -7, 1, -3, 1, -2, 1, -1, 1, 0, 1, 1, 1, 2, 1, 3, 1,
    -43, 2, -25, 2, -19, 2, -14, 2, -5, 2, -3, 2, -2, 2, -1, 2, 0, 2, 1, 2,
    2, 2, 3, 2, 5, 2, 7, 2, 10, 2, 14, 2, -33, 3, -6, 3, -4, 3, -2, 3,
    -1, 3, 0, 3, 1, 3, 2, 3, 4, 3, 19, 3, -9, 4, -3, 4, 3, 4, 7, 4,
    25, 4, -13, 5, -5, 5, -2, 5, 0, 5, 2, 5, 5, 5, 9, 5, 33, 5, -8, 6,
    -4, 6, 4, 6, 13, 6, 43, 6, -18, 7, -2, 7, 0, 7, 2, 7, 7, 7, 18, 7,
    -24, 8, -6, 8, -42, 9, -11, 9, -4, 9, 5, 9, 11, 9, 23, 9, -31, 10, -1, 10,
    2, 10, -15, 11, -8, 11, 8, 11, 15, 11, 31, 12, -21, 13, -5, 13, 5, 13, 41, 13,
    -1, 14, 1, 14, 21, 14, -12, 15, 12, 15, -39, 17, -28, 17, -18, 17, -8, 17, 8, 17,
    17, 18, -4, 19, 0, 19, 4, 19, 27, 19, 38, 20, -13, 21, 12, 22, -36, 23, -24, 23,
    -8, 24, 7, 24, -3, 25, 1, 25, 22, 25, 34, 26, -18, 28, -32, 29, 16, 29, -11, 31,
    9, 32, 29, 32, -4, 33, 2, 33, -26, 34, 23, 36, -19, 39, 16, 40, -13, 41, 9, 42,
    -6, 43, 1, 43, 0, 0, 0, 0, 0, 0, 0, 0, 0};

typedef struct Blocky16 {
    uint8_t *delta_bufs[2];
    uint8_t *delta_buf;
    uint8_t *cur_buf;
    int32_t prev_seq;
    int last_table_width;
    const uint8_t *src;
    const uint8_t *param;
    const uint8_t *param67;
    int pitch;
    int32_t offset1, offset2;
    uint8_t table_big[99328];
    uint8_t table_small[32768];
    int16_t table[256];
    int32_t frame_size, buf_size;
    int width, height, blocks_w, blocks_h;
} Blocky16;

static void make_tables_interpolation(Blocky16 *b, int param)
{
    const int8_t *t1, *t2;
    uint8_t *ptr;
    int32_t small_big[64];
    int s = 0;

    if (param == 8) {
        t1 = table_big1;
        t2 = table_big2;
        for (int i = 0; i < 256; i++) {
            b->table_big[384 + i * 388] = 0;
            b->table_big[385 + i * 388] = 0;
        }
    } else {
        t1 = table_small1;
        t2 = table_small2;
        for (int i = 0; i < 256; i++) {
            b->table_small[96 + i * 128] = 0;
            b->table_small[97 + i * 128] = 0;
        }
    }

    for (int x = 0; x < 16; x++) {
        int32_t v11 = t1[x], v21 = t2[x];
        for (int y = 0; y < 16; y++) {
            int32_t v12 = t1[y], v22 = t2[y];
            int32_t b1, b2;
            if (v21 == 0)
                b1 = 0;
            else if (v21 == param - 1)
                b1 = 1;
            else if (v11 == 0)
                b1 = 2;
            else if (v11 == param - 1)
                b1 = 3;
            else
                b1 = 4;
            if (v22 == 0)
                b2 = 0;
            else if (v22 == param - 1)
                b2 = 1;
            else if (v12 == 0)
                b2 = 2;
            else if (v12 == param - 1)
                b2 = 3;
            else
                b2 = 4;

            memset(small_big, 0, param * param * 4);
            int32_t var2 = abs(v22 - v21);
            int32_t tmp = abs(v12 - v11);
            if (var2 <= tmp)
                var2 = tmp;

            for (int32_t var1 = 0; var1 <= var2; var1++) {
                int32_t var3, var4;
                if (var2 > 0) {
                    var4 = (v11 * var1 + v12 * (var2 - var1) + var2 / 2) / var2;
                    var3 = (v21 * var1 + v22 * (var2 - var1) + var2 / 2) / var2;
                } else {
                    var4 = v11;
                    var3 = v21;
                }
                int32_t *p = &small_big[param * var3 + var4];
                *p = 1;
                int i;
                if ((b1 == 2 && b2 == 3) || (b2 == 2 && b1 == 3) || (b1 == 0 && b2 != 1) || (b2 == 0 && b1 != 1)) {
                    if (var3 >= 0) {
                        i = var3 + 1;
                        while (i--) {
                            *p = 1;
                            p -= param;
                        }
                    }
                } else if ((b2 != 0 && b1 == 1) || (b1 != 0 && b2 == 1)) {
                    if (param > var3) {
                        i = param - var3;
                        while (i--) {
                            *p = 1;
                            p += param;
                        }
                    }
                } else if ((b1 == 2 && b2 != 3) || (b2 == 2 && b1 != 3)) {
                    if (var4 >= 0) {
                        i = var4 + 1;
                        while (i--)
                            *(p--) = 1;
                    }
                } else if ((b1 == 0 && b2 == 1) || (b2 == 0 && b1 == 1) || (b1 == 3 && b2 != 2) || (b2 == 3 && b1 != 2)) {
                    if (param > var4) {
                        i = param - var4;
                        while (i--)
                            *(p++) = 1;
                    }
                }
            }

            if (param == 8) {
                for (int i = 64 - 1; i >= 0; i--) {
                    if (small_big[i] != 0) {
                        b->table_big[256 + s + b->table_big[384 + s]] = (uint8_t)i;
                        b->table_big[384 + s]++;
                    } else {
                        b->table_big[320 + s + b->table_big[385 + s]] = (uint8_t)i;
                        b->table_big[385 + s]++;
                    }
                }
                s += 388;
            } else {
                for (int i = 16 - 1; i >= 0; i--) {
                    if (small_big[i] != 0) {
                        b->table_small[64 + s + b->table_small[96 + s]] = (uint8_t)i;
                        b->table_small[96 + s]++;
                    } else {
                        b->table_small[80 + s + b->table_small[97 + s]] = (uint8_t)i;
                        b->table_small[97 + s]++;
                    }
                }
                s += 128;
            }
        }
    }
    (void)ptr;
}

static void make_tables47(Blocky16 *b, int width)
{
    if (b->last_table_width == width)
        return;
    b->last_table_width = width;

    for (int l = 0; l < 512; l += 2)
        b->table[l / 2] = (int16_t)(motion_table[l + 1] * width + motion_table[l]);

    int32_t a = 0, c = 0;
    do {
        int16_t tmp;
        for (int d = 0; d < b->table_small[96 + c]; d++) {
            tmp = b->table_small[64 + c + d];
            tmp = (int16_t)((uint8_t)(tmp >> 2) * width + (tmp & 3));
            b->table_small[c + d * 2] = (uint8_t)tmp;
            b->table_small[c + d * 2 + 1] = (uint8_t)(tmp >> 8);
        }
        for (int d = 0; d < b->table_small[97 + c]; d++) {
            tmp = b->table_small[80 + c + d];
            tmp = (int16_t)((uint8_t)(tmp >> 2) * width + (tmp & 3));
            b->table_small[32 + c + d * 2] = (uint8_t)tmp;
            b->table_small[32 + c + d * 2 + 1] = (uint8_t)(tmp >> 8);
        }
        for (int d = 0; d < b->table_big[384 + a]; d++) {
            tmp = b->table_big[256 + a + d];
            tmp = (int16_t)((uint8_t)(tmp >> 3) * width + (tmp & 7));
            b->table_big[a + d * 2] = (uint8_t)tmp;
            b->table_big[a + d * 2 + 1] = (uint8_t)(tmp >> 8);
        }
        for (int d = 0; d < b->table_big[385 + a]; d++) {
            tmp = b->table_big[320 + a + d];
            tmp = (int16_t)((uint8_t)(tmp >> 3) * width + (tmp & 7));
            b->table_big[128 + a + d * 2] = (uint8_t)tmp;
            b->table_big[128 + a + d * 2 + 1] = (uint8_t)(tmp >> 8);
        }
        a += 388;
        c += 128;
    } while (c < 32768);
}

static void copy4(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, 4);
}
static void put2(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)v;
    dst[1] = (uint8_t)(v >> 8);
}
static void put4(uint8_t *dst, uint32_t v)
{
    memcpy(dst, &v, 4);
}

static void level3(Blocky16 *b, uint8_t *d)
{
    int32_t off;
    uint32_t t;
    uint8_t code = *b->src++;

    if (code <= 0xF5) {
        if (code == 0xF5) {
            off = (int16_t)le16(b->src) * 2;
            b->src += 2;
        } else {
            off = b->table[code] * 2;
        }
        off += b->offset1;
        for (int i = 0; i < 2; i++) {
            copy4(d, d + off);
            d += b->pitch;
        }
    } else if (code == 0xFF || code == 0xF8) {
        put2(d + 0, le16(b->src + 0));
        put2(d + 2, le16(b->src + 2));
        d += b->pitch;
        put2(d + 0, le16(b->src + 4));
        put2(d + 2, le16(b->src + 6));
        b->src += 8;
    } else if (code == 0xFD) {
        t = *b->src++;
        t = le16(b->param67 + t * 2);
        t = (t << 16) | t;
        for (int i = 0; i < 2; i++) {
            put4(d, t);
            d += b->pitch;
        }
    } else if (code == 0xFE) {
        t = le16(b->src);
        b->src += 2;
        t = (t << 16) | t;
        for (int i = 0; i < 2; i++) {
            put4(d, t);
            d += b->pitch;
        }
    } else if (code == 0xF6) {
        for (int i = 0; i < 2; i++) {
            copy4(d, d + b->offset2);
            d += b->pitch;
        }
    } else if (code == 0xF7) {
        uint32_t v = le32(b->src);
        b->src += 4;
        put2(d + 0, le16(b->param67 + (uint8_t)v * 2));
        put2(d + 2, le16(b->param67 + (uint8_t)(v >> 8) * 2));
        v >>= 16;
        d += b->pitch;
        put2(d + 0, le16(b->param67 + (uint8_t)v * 2));
        put2(d + 2, le16(b->param67 + (uint8_t)(v >> 8) * 2));
    } else {// 0xF9..0xFC
        t = le16(b->param + (code - 0xF9) * 2);
        t = (t << 16) | t;
        for (int i = 0; i < 2; i++) {
            put4(d, t);
            d += b->pitch;
        }
    }
}

// The 0xF7 / 0xF8 two-colour pattern of an n x n block (n = 4 small, 8 big).
static void pattern(Blocky16 *b, uint8_t *d, uint8_t code, const uint8_t *tables, int stride, int count0, int count1,
                    int list1)
{
    uint32_t val;
    uint8_t shape = *b->src++;
    if (code == 0xF8) {
        val = le32(b->src);
        b->src += 4;
    } else {
        uint16_t idx = le16(b->src);
        val = (uint32_t)le16(b->param67 + (uint8_t)(idx >> 8) * 2) << 16;
        val |= le16(b->param67 + (uint8_t)idx * 2);
        b->src += 2;
    }
    const uint8_t *tp = tables + shape * stride;
    // as in the original, at least one entry is written even when a list is empty
    int l = tp[count0];
    const uint8_t *e = tp;
    do {
        put2(d + le16(e) * 2, val);
        e += 2;
    } while (l-- > 1);
    l = tp[count1];
    val >>= 16;
    e = tp + list1;
    do {
        put2(d + le16(e) * 2, val);
        e += 2;
    } while (l-- > 1);
}

static uint32_t fill_value(Blocky16 *b, uint8_t code)
{
    uint32_t t = 0;
    if (code == 0xFD) {
        t = *b->src++;
        t = le16(b->param67 + t * 2);
    } else if (code == 0xFE) {
        t = le16(b->src);
        b->src += 2;
    } else if (code >= 0xF9 && code <= 0xFC) {
        t = le16(b->param + (code - 0xF9) * 2);
    }
    return (t << 16) | t;
}

static void level2(Blocky16 *b, uint8_t *d)
{
    int32_t off;
    uint8_t code = *b->src++;

    if (code <= 0xF5) {
        if (code == 0xF5) {
            off = (int16_t)le16(b->src) * 2;
            b->src += 2;
        } else {
            off = b->table[code] * 2;
        }
        off += b->offset1;
        for (int i = 0; i < 4; i++) {
            copy4(d + 0, d + off + 0);
            copy4(d + 4, d + off + 4);
            d += b->pitch;
        }
    } else if (code == 0xFF) {
        level3(b, d);
        d += 4;
        level3(b, d);
        d += b->pitch * 2 - 4;
        level3(b, d);
        d += 4;
        level3(b, d);
    } else if (code == 0xF6) {
        for (int i = 0; i < 4; i++) {
            copy4(d + 0, d + b->offset2 + 0);
            copy4(d + 4, d + b->offset2 + 4);
            d += b->pitch;
        }
    } else if (code == 0xF7 || code == 0xF8) {
        pattern(b, d, code, b->table_small, 128, 96, 97, 32);
    } else {
        uint32_t t = fill_value(b, code);
        for (int i = 0; i < 4; i++) {
            put4(d + 0, t);
            put4(d + 4, t);
            d += b->pitch;
        }
    }
}

static void level1(Blocky16 *b, uint8_t *d)
{
    int32_t off;
    uint8_t code = *b->src++;

    if (code <= 0xF5) {
        if (code == 0xF5) {
            off = (int16_t)le16(b->src) * 2;
            b->src += 2;
        } else {
            off = b->table[code] * 2;
        }
        off += b->offset1;
        for (int i = 0; i < 8; i++) {
            memcpy(d, d + off, 16);
            d += b->pitch;
        }
    } else if (code == 0xFF) {
        level2(b, d);
        d += 8;
        level2(b, d);
        d += b->pitch * 4 - 8;
        level2(b, d);
        d += 8;
        level2(b, d);
    } else if (code == 0xF6) {
        for (int i = 0; i < 8; i++) {
            memcpy(d, d + b->offset2, 16);
            d += b->pitch;
        }
    } else if (code == 0xF7 || code == 0xF8) {
        pattern(b, d, code, b->table_big, 388, 384, 385, 128);
    } else {
        uint32_t t = fill_value(b, code);
        for (int i = 0; i < 8; i++) {
            put4(d + 0, t);
            put4(d + 4, t);
            put4(d + 8, t);
            put4(d + 12, t);
            d += b->pitch;
        }
    }
}

static void decode2(Blocky16 *b, uint8_t *dst, const uint8_t *src, const uint8_t *param, const uint8_t *param67)
{
    b->src = src;
    b->param = param;
    b->param67 = param67;
    b->pitch = b->width * 2;
    int next_line = b->width * 2 * 7;
    for (int bh = b->blocks_h; bh > 0; bh--) {
        for (int bw = b->blocks_w; bw > 0; bw--) {
            level1(b, dst);
            dst += 16;
        }
        dst += next_line;
    }
}

// BOMP run-length decoding (subcodecs 5 and 8).
typedef struct Bomp {
    int left, num, color;
    const uint8_t *src;
} Bomp;

static uint8_t bomp_decode(Bomp *s)
{
    uint8_t result;
    if (s->left == 2) {
        s->num = (*s->src >> 1) + 1;
        uint8_t code = *s->src++ & 1;
        if (code != 0) {
            s->left = 1;
            s->color = *s->src++;
        } else {
            s->left = 0;
        }
    }
    if (s->left != 0)
        result = s->left == 1 ? (uint8_t)s->color : 255;
    else
        result = *s->src++;
    if (--s->num == 0)
        s->left = 2;
    return result;
}

static int blocky16_init(Blocky16 *b, int width, int height)
{
    memset(b, 0, sizeof(*b));
    b->width = width;
    b->height = height;
    b->last_table_width = -1;
    make_tables_interpolation(b, 4);
    make_tables_interpolation(b, 8);
    b->blocks_w = (width + 7) / 8;
    b->blocks_h = (height + 7) / 8;
    b->frame_size = width * height * 2;
    b->buf_size = width * b->blocks_h * 8 * 2;
    // the decoder may read a little past the buffers (ScummVM: +200 bytes)
    b->delta_buf = (uint8_t *)calloc((size_t)b->buf_size * 3 + 200, 1);
    if (!b->delta_buf)
        return 0;
    b->delta_bufs[0] = b->delta_buf;
    b->delta_bufs[1] = b->delta_buf + b->buf_size;
    b->cur_buf = b->delta_buf + b->buf_size * 2;
    return 1;
}

static void blocky16_decode(Blocky16 *b, uint8_t *dst, const uint8_t *src)
{
    b->offset1 = (int32_t)(((b->delta_bufs[1] - b->cur_buf) / 2) * 2);
    b->offset2 = (int32_t)(((b->delta_bufs[0] - b->cur_buf) / 2) * 2);
    int32_t seq = le16(src + 16);
    const uint8_t *gfx = src + 560;

    if (seq == 0) {
        make_tables47(b, b->width);
        if (src[32] == src[33]) {
            memset(b->delta_bufs[0], src[32], b->buf_size);
            memset(b->delta_bufs[1], src[32], b->buf_size);
        } else {
            uint16_t v = le16(src + 32);
            uint16_t *p1 = (uint16_t *)b->delta_bufs[0], *p2 = (uint16_t *)b->delta_bufs[1];
            for (int n = b->buf_size / 2; n > 0; n--) {
                *p1++ = v;
                *p2++ = v;
            }
        }
        b->prev_seq = -1;
    }

    switch (src[18]) {
        case 0:
            memcpy(b->cur_buf, gfx, b->frame_size);
            break;
        case 2:
            if (seq == b->prev_seq + 1)
                decode2(b, b->cur_buf, gfx, src + 24, src + 40);
            break;
        case 3:
            memcpy(b->cur_buf, b->delta_bufs[1], b->frame_size);
            break;
        case 4:
            memcpy(b->cur_buf, b->delta_bufs[0], b->frame_size);
            break;
        case 5: {
            Bomp s = {2, 0, 0, gfx};
            uint8_t *d = b->cur_buf;
            for (int n = (int)le32(src + 36) / 2; n > 0; n--) {
                d[0] = bomp_decode(&s);
                d[1] = bomp_decode(&s);
                d += 2;
            }
            break;
        }
        case 6: {
            uint16_t *p = (uint16_t *)b->cur_buf;
            for (int n = b->frame_size / 2; n > 0; n--)
                *p++ = le16(src + 40 + *gfx++ * 2);
            break;
        }
        case 8: {
            Bomp s = {2, 0, 0, gfx};
            uint16_t *p = (uint16_t *)b->cur_buf;
            for (int n = b->frame_size / 2; n > 0; n--)
                *p++ = le16(src + 40 + bomp_decode(&s) * 2);
            break;
        }
        default:// 1 and 7 are not used by any known movie
            break;
    }

    memcpy(dst, b->cur_buf, b->frame_size);

    if (seq == b->prev_seq + 1) {
        uint8_t *t;
        if (src[19] == 1) {
            t = b->cur_buf;
            b->cur_buf = b->delta_bufs[1];
            b->delta_bufs[1] = t;
        } else if (src[19] == 2) {
            t = b->delta_bufs[0];
            b->delta_bufs[0] = b->delta_bufs[1];
            b->delta_bufs[1] = b->cur_buf;
            b->cur_buf = t;
        }
    }
    b->prev_seq = seq;
}

// ---------------------------------------------------------------------------------------------
// VIMA
// ---------------------------------------------------------------------------------------------

static const int16_t imc_table1[] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t imc_table2[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

static const int8_t imc_other1[] = {-1, 4, -1, 4};
static const int8_t imc_other2[] = {-1, -1, 2, 6, -1, -1, 2, 6};
static const int8_t imc_other3[] = {-1, -1, -1, -1, 1, 2, 4, 6, -1, -1, -1, -1, 1, 2, 4, 6};
static const int8_t imc_other4[] = {-1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, 2, 2, 4, 5, 6,
                                    -1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, 2, 2, 4, 5, 6};
static const int8_t imc_other5[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 5, 5, 6, 6,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 5, 5, 6, 6};
static const int8_t imc_other6[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2,
                                    2, 2, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2,
                                    2, 2, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6};
static const int8_t *const imc_offsets[] = {imc_other1, imc_other2, imc_other3, imc_other4, imc_other5, imc_other6};

static void vima_init(uint16_t *dest)
{
    for (int start = 0; start < 64; start++) {
        for (unsigned t1 = 0, pos = start; t1 < sizeof(imc_table1) / sizeof(imc_table1[0]); t1++, pos += 64) {
            int put = 0;
            int value = imc_table1[t1];
            for (int count = 32; count != 0; count >>= 1, value >>= 1)
                if (start & count)
                    put += value;
            dest[pos] = (uint16_t)put;
        }
    }
}

static void vima_decompress(const uint8_t *src, int16_t *dest, int dest_len, const uint16_t *table)
{
    int channels = 1;
    uint8_t sbytes[2] = {0, 0};
    int16_t swords[2] = {0, 0};

    sbytes[0] = *src++;
    if (sbytes[0] & 0x80) {
        sbytes[0] = (uint8_t)~sbytes[0];
        channels = 2;
    }
    swords[0] = (int16_t)be16(src);
    src += 2;
    if (channels > 1) {
        sbytes[1] = *src++;
        swords[1] = (int16_t)be16(src);
        src += 2;
    }
    int samples = dest_len / (channels * 2);

    if (be32(src) == TAG('I', 'M', 'A', '4')) {
        static const int deltas[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
        src += 4;
        int out = (int16_t)le16(src);
        int pos = src[2];
        src += 3;
        int curai = 7;
        uint8_t in = 0;
        int high = 0;
        int16_t *d = dest;
        for (int s = 0; s < samples; s++) {
            uint8_t nib;
            if (!high) {
                in = *src++;
                nib = in >> 4;
            } else {
                nib = in & 0xf;
            }
            high = !high;
            pos += deltas[nib & 0xf];
            pos = pos < 0 ? 0 : (pos > 88 ? 88 : pos);
            int delta = curai >> 3;
            if (nib & 4)
                delta += curai;
            if (nib & 2)
                delta += curai >> 1;
            if (nib & 1)
                delta += curai >> 2;
            if (nib & 8)
                delta = -delta;
            out += delta;
            out = out < -0x8000 ? -0x8000 : (out > 0x7fff ? 0x7fff : out);
            if (pos > 0)
                curai = imc_table1[pos];
            for (int c = 0; c < channels; c++)
                d[c] = (int16_t)out;
            d += channels;
        }
        return;
    }

    int bits = be16(src);
    int bitptr = 0;
    src += 2;
    for (int c = 0; c < channels; c++) {
        int16_t *d = dest + c;
        int pos = sbytes[c];
        int out = swords[c];
        for (int s = 0; s < samples; s++) {
            int nbits = imc_table2[pos];
            bitptr += nbits;
            int high = 1 << (nbits - 1);
            int low = high - 1;
            int val = (bits >> (16 - bitptr)) & (high | low);
            if (bitptr > 7) {
                bits = ((bits & 0xff) << 8) | *src++;
                bitptr -= 8;
            }
            if (val & high)
                val ^= high;
            else
                high = 0;
            if (val == low) {
                out = (int16_t)(bits << bitptr) & (int)0xffffff00;
                bits = ((bits & 0xff) << 8) | *src++;
                out |= (bits >> (8 - bitptr)) & 0xff;
                bits = ((bits & 0xff) << 8) | *src++;
            } else {
                int delta = table[(val << (7 - nbits)) | (pos << 6)];
                if (val)
                    delta += imc_table1[pos] >> (nbits - 1);
                if (high)
                    delta = -delta;
                out += delta;
                out = out < -0x8000 ? -0x8000 : (out > 0x7fff ? 0x7fff : out);
            }
            *d = (int16_t)out;
            d += channels;
            pos += imc_offsets[nbits - 2][val];
            pos = pos < 0 ? 0 : (pos > 88 ? 88 : pos);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------------------------

struct SanmMovie {
    uint8_t *data;
    size_t size;
    int width, height, frames, frame_us;
    uint32_t *frame_offsets;
    int next;
    uint16_t *image;
    int16_t *audio;
    int audio_samples, audio_channels, audio_rate;
    Blocky16 blocky;
};

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

// gzip: header, raw deflate, CRC32, ISIZE.
static uint8_t *gunzip(const uint8_t *in, size_t in_size, size_t *out_size)
{
    if (in_size < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return NULL;
    uint8_t flags = in[3];
    size_t p = 10;
    if (flags & 4)
        p += 2 + le16(in + p);
    if (flags & 8)
        while (p < in_size && in[p++]) {}
    if (flags & 16)
        while (p < in_size && in[p++]) {}
    if (flags & 2)
        p += 2;
    if (p >= in_size - 8)
        return NULL;
    uint32_t isize = le32(in + in_size - 4);
    uint8_t *out = (uint8_t *)malloc(isize ? isize : 1);
    if (!out)
        return NULL;
    int got = stbi_zlib_decode_noheader_buffer((char *)out, (int)isize, (const char *)in + p, (int)(in_size - 8 - p));
    if (got != (int)isize) {
        free(out);
        return NULL;
    }
    *out_size = isize;
    return out;
}

static void decode_audio(SanmMovie *m)
{
    static uint16_t table[5786];
    static int table_built;
    if (!table_built) {
        vima_init(table);
        table_built = 1;
    }
    size_t cap = 0, used = 0;// in samples (int16)
    for (int f = 0; f < m->frames; f++) {
        const uint8_t *fr = m->data + m->frame_offsets[f];
        uint32_t size = be32(fr + 4);
        const uint8_t *q = fr + 8, *end = fr + 8 + size;
        while (q + 8 <= end) {
            uint32_t tag = be32(q), sub = be32(q + 4);
            if (tag == TAG('W', 'a', 'v', 'e')) {
                const uint8_t *body = q + 8;
                int32_t count = (int32_t)be32(body);
                const uint8_t *vdata = body + 4;
                if (count < 0) {
                    count = (int32_t)be32(body + 8);
                    vdata = body + 12;
                }
                int ch = (vdata[0] & 0x80) ? 2 : 1;
                if (!m->audio_channels)
                    m->audio_channels = ch;
                if (ch == m->audio_channels && count > 0) {
                    size_t need = used + (size_t)count * ch;
                    if (need > cap) {
                        cap = need * 2;
                        m->audio = (int16_t *)realloc(m->audio, cap * sizeof(int16_t));
                    }
                    vima_decompress(vdata, m->audio + used, count * ch * 2, table);
                    used = need;
                }
            }
            q += 8 + sub + (sub & 1);
        }
    }
    m->audio_samples = m->audio_channels ? (int)(used / m->audio_channels) : 0;
}

SanmMovie *sanm_open(const char *path)
{
    size_t raw_size = 0;
    uint8_t *raw = read_file(path, &raw_size);
    if (!raw)
        return NULL;
    SanmMovie *m = (SanmMovie *)calloc(1, sizeof(SanmMovie));
    if (!m) {
        free(raw);
        return NULL;
    }
    if (raw_size > 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        m->data = gunzip(raw, raw_size, &m->size);
        free(raw);
    } else {
        m->data = raw;
        m->size = raw_size;
    }
    if (!m->data || m->size < 16 || be32(m->data) != TAG('S', 'A', 'N', 'M'))
        goto fail;

    size_t p = 8;
    if (be32(m->data + p) != TAG('S', 'H', 'D', 'R'))
        goto fail;
    {
        const uint8_t *h = m->data + p + 8;
        m->frames = (int)le32(h + 2);
        m->width = le16(h + 8);
        m->height = le16(h + 10);
        m->frame_us = (int)le32(h + 14);
        uint32_t sz = be32(m->data + p + 4);
        p += 8 + sz + (sz & 1);
    }
    m->audio_rate = 22050;
    if (p + 8 <= m->size && be32(m->data + p) == TAG('F', 'L', 'H', 'D')) {
        uint32_t sz = be32(m->data + p + 4);
        const uint8_t *h = m->data + p + 8;
        for (uint32_t q = 0; q + 8 <= sz;) {
            uint32_t bsz = be32(h + q + 4);
            if (be32(h + q) == TAG('W', 'a', 'v', 'e') && bsz >= 4)
                m->audio_rate = (int)le32(h + q + 8);
            q += 8 + bsz;
        }
        p += 8 + sz + (sz & 1);
    }

    m->frame_offsets = (uint32_t *)calloc(m->frames > 0 ? m->frames : 1, sizeof(uint32_t));
    int n = 0;
    while (p + 8 <= m->size && n < m->frames) {
        uint32_t sz = be32(m->data + p + 4);
        if (be32(m->data + p) == TAG('F', 'R', 'M', 'E'))
            m->frame_offsets[n++] = (uint32_t)p;
        p += 8 + sz + (sz & 1);
    }
    m->frames = n;
    if (m->width <= 0 || m->height <= 0 || m->frame_us <= 0)
        goto fail;
    m->image = (uint16_t *)calloc((size_t)m->width * m->height, 2);
    if (!m->image || !blocky16_init(&m->blocky, m->width, m->height))
        goto fail;
    decode_audio(m);
    return m;

fail:
    sanm_close(m);
    return NULL;
}

void sanm_close(SanmMovie *m)
{
    if (!m)
        return;
    free(m->data);
    free(m->frame_offsets);
    free(m->image);
    free(m->audio);
    free(m->blocky.delta_buf);
    free(m);
}

int sanm_width(const SanmMovie *m)
{
    return m->width;
}
int sanm_height(const SanmMovie *m)
{
    return m->height;
}
int sanm_frame_count(const SanmMovie *m)
{
    return m->frames;
}
int sanm_frame_us(const SanmMovie *m)
{
    return m->frame_us;
}
const uint16_t *sanm_image(const SanmMovie *m)
{
    return m->image;
}

const int16_t *sanm_audio(const SanmMovie *m, int *samples, int *channels, int *rate)
{
    *samples = m->audio_samples;
    *channels = m->audio_channels;
    *rate = m->audio_rate;
    return m->audio_samples ? m->audio : NULL;
}

int sanm_next_frame(SanmMovie *m)
{
    if (m->next >= m->frames)
        return -1;
    const uint8_t *fr = m->data + m->frame_offsets[m->next++];
    uint32_t size = be32(fr + 4);
    const uint8_t *q = fr + 8, *end = fr + 8 + size;
    int picture = 0;
    while (q + 8 <= end) {
        uint32_t tag = be32(q), sub = be32(q + 4);
        if (tag == TAG('B', 'l', '1', '6')) {
            blocky16_decode(&m->blocky, (uint8_t *)m->image, q + 8);
            picture = 1;
        }
        q += 8 + sub + (sub & 1);
    }
    return picture;
}
