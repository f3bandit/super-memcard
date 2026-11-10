/* Super MemCard: a save icon from the game's box art, as flash-cart menus make their thumbnails (docs/05-design.md).
 * Copyright (C) 2026 f3bandit. All rights reserved.
 *
 * The art (PNG or JPEG, Console Mode's media/<game>.png) fitted into 32x32, centred, the rest transparent, in 15
 * colours plus transparent: icon.bin's layout (the PS1 save icon's: 4 bits a pixel, a 16-colour PS1 palette), so
 * Console Mode and SS1 Tool draw it with their PS1 icon code, 1:1 at the 32x32 they draw icons at. */
#include "smc_sync.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNM /* the tests' art */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO_WARNINGS
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"

#define S 32 /* the icon's side */

struct box { int lo, hi; }; /* a run of colours in col[] (median cut) */
static uint8_t col[S * S][3];

static int widest(const struct box *b, int *range) {
    int ch = 0, best = -1;
    for (int c = 0; c < 3; c++) {
        int mn = 255, mx = 0;
        for (int i = b->lo; i < b->hi; i++) {
            if (col[i][c] < mn)
                mn = col[i][c];
            if (col[i][c] > mx)
                mx = col[i][c];
        }
        if (mx - mn > best) {
            best = mx - mn;
            ch = c;
        }
    }
    *range = best;
    return ch;
}

static int sort_ch;
static int by_ch(const void *a, const void *b) { return ((const uint8_t *)a)[sort_ch] - ((const uint8_t *)b)[sort_ch]; }

size_t smc_art_icon(const char *path, uint8_t *out, size_t out_max) {
    const size_t need = 8 + 32 + S * S / 2;
    if (out_max < need)
        return 0;
    int w, h, n;
    uint8_t *img = stbi_load(path, &w, &h, &n, 4);
    if (!img)
        return 0;
    /* fit inside SxS, keeping its shape */
    int tw = S, th = S;
    if (w > h)
        th = (int)((long)S * h / w);
    else
        tw = (int)((long)S * w / h);
    if (tw < 1)
        tw = 1;
    if (th < 1)
        th = 1;
    int ox = (S - tw) / 2, oy = (S - th) / 2;

    /* each icon pixel: the average of the art's pixels under it (alpha-weighted) */
    static uint8_t px[S][S][4];
    memset(px, 0, sizeof(px));
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long)x * w / tw), x1 = (int)((long)(x + 1) * w / tw);
            int y0 = (int)((long)y * h / th), y1 = (int)((long)(y + 1) * h / th);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (y1 <= y0)
                y1 = y0 + 1;
            unsigned long r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    const uint8_t *p = img + 4 * ((size_t)yy * w + xx);
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    cnt++;
                }
            uint8_t *q = px[oy + y][ox + x];
            if (a) {
                q[0] = (uint8_t)(r / a);
                q[1] = (uint8_t)(g / a);
                q[2] = (uint8_t)(b / a);
            }
            q[3] = (uint8_t)(cnt ? a / cnt : 0);
        }
    stbi_image_free(img);

    /* 15 colours by median cut over the opaque pixels */
    int nc = 0;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++)
            if (px[y][x][3] >= 128)
                memcpy(col[nc++], px[y][x], 3);
    struct box boxes[15];
    int nb = 0;
    if (nc)
        boxes[nb++] = (struct box){0, nc};
    while (nb < 15) {
        int pick = -1, pick_range = 0, pick_ch = 0;
        for (int i = 0; i < nb; i++) {
            int range, ch = widest(&boxes[i], &range);
            if (boxes[i].hi - boxes[i].lo > 1 && range > pick_range) {
                pick = i;
                pick_range = range;
                pick_ch = ch;
            }
        }
        if (pick < 0)
            break;
        struct box *b = &boxes[pick];
        sort_ch = pick_ch;
        qsort(col[b->lo], (size_t)(b->hi - b->lo), 3, by_ch);
        int mid = (b->lo + b->hi) / 2;
        boxes[nb++] = (struct box){mid, b->hi};
        b->hi = mid;
    }
    uint8_t pal[16][3] = {{0}};
    uint16_t pal16[16] = {0}; /* 0: transparent */
    for (int i = 0; i < nb; i++) {
        unsigned long s[3] = {0};
        for (int k = boxes[i].lo; k < boxes[i].hi; k++)
            for (int c = 0; c < 3; c++)
                s[c] += col[k][c];
        int cnt = boxes[i].hi - boxes[i].lo;
        for (int c = 0; c < 3; c++)
            pal[i + 1][c] = (uint8_t)(s[c] / (unsigned long)cnt);
        uint16_t v = (uint16_t)((pal[i + 1][0] >> 3) | (pal[i + 1][1] >> 3) << 5 | (pal[i + 1][2] >> 3) << 10);
        pal16[i + 1] = v ? v : 0x8000; /* black that isn't transparent: the PS1's semi-transparency bit */
    }

    memcpy(out, "SMIC", 4);
    out[4] = 1;
    out[5] = 1; /* one frame */
    out[6] = S;
    out[7] = S;
    for (int i = 0; i < 16; i++) {
        out[8 + 2 * i] = (uint8_t)pal16[i];
        out[9 + 2 * i] = (uint8_t)(pal16[i] >> 8);
    }
    uint8_t *pix = out + 40;
    memset(pix, 0, S * S / 2);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int idx = 0;
            if (px[y][x][3] >= 128) {
                long best = -1;
                for (int i = 1; i <= nb; i++) {
                    long dr = px[y][x][0] - pal[i][0], dg = px[y][x][1] - pal[i][1], db = px[y][x][2] - pal[i][2];
                    long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                    if (best < 0 || d < best) {
                        best = d;
                        idx = i;
                    }
                }
            }
            pix[(y * S + x) / 2] |= (uint8_t)(x & 1 ? idx << 4 : idx);
        }
    return need;
}
