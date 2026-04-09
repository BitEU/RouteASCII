#include "subpixel.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Braille dot bitmask layout (U+2800 base):
 *   (0,0)=dot1 (1,0)=dot4
 *   (0,1)=dot2 (1,1)=dot5
 *   (0,2)=dot3 (1,2)=dot6
 *   (0,3)=dot7 (1,3)=dot8
 */
static const unsigned char BRAILLE_BIT[4][2] = {
    {0x01, 0x08},
    {0x02, 0x10},
    {0x04, 0x20},
    {0x40, 0x80},
};

/* Encode a simple attr flag byte: 1 = A_BOLD, 2 = A_DIM, 0 = none. */
static unsigned char pack_attr(int attr)
{
    if (attr & A_BOLD) return 1;
    if (attr & A_DIM)  return 2;
    return 0;
}

static int unpack_attr(unsigned char a)
{
    if (a == 1) return A_BOLD;
    if (a == 2) return A_DIM;
    return 0;
}

void bc_resize(BrailleCanvas *bc, int w, int h)
{
    if (w == bc->w && h == bc->h && bc->dots) {
        bc_clear(bc);
        return;
    }
    free(bc->dots); free(bc->color); free(bc->attr);
    bc->w = w; bc->h = h;
    bc->pw = w * 2; bc->ph = h * 4;
    size_t n = (size_t)w * (size_t)h;
    bc->dots  = calloc(n, 1);
    bc->color = calloc(n, 1);
    bc->attr  = calloc(n, 1);
}

void bc_free(BrailleCanvas *bc)
{
    free(bc->dots); free(bc->color); free(bc->attr);
    memset(bc, 0, sizeof(*bc));
}

void bc_clear(BrailleCanvas *bc)
{
    if (!bc->dots) return;
    size_t n = (size_t)bc->w * (size_t)bc->h;
    memset(bc->dots, 0, n);
    memset(bc->color, 0, n);
    memset(bc->attr, 0, n);
}

void bc_set(BrailleCanvas *bc, int px, int py, int cp, int attr)
{
    if (!bc->dots) return;
    if (px < 0 || py < 0 || px >= bc->pw || py >= bc->ph) return;
    int cx = px >> 1;       /* /2 */
    int cy = py >> 2;       /* /4 */
    int sx = px & 1;
    int sy = py & 3;
    int idx = cy * bc->w + cx;
    bc->dots[idx]  |= BRAILLE_BIT[sy][sx];
    bc->color[idx]  = (unsigned char)cp;   /* last writer wins */
    bc->attr[idx]   = pack_attr(attr);
}

/* Bresenham in pixel space, bounds-checked per-pixel via bc_set. */
void bc_line(BrailleCanvas *bc, int x0, int y0, int x1, int y1,
             int cp, int attr)
{
    if (!bc->dots) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    /* Cap on iterations in case caller passes insane endpoints. In pixel
     * space the canvas is maybe 500x250 = 750 chars worth — cap generously. */
    int cap = (bc->pw + bc->ph) * 4;
    int steps = 0;

    for (;;) {
        bc_set(bc, x0, y0, cp, attr);
        if (x0 == x1 && y0 == y1) break;
        if (++steps > cap) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void bc_flush(BrailleCanvas *bc, WINDOW *win)
{
    if (!bc->dots) return;
    int mx, my;
    getmaxyx(win, my, mx);
    int W = bc->w < mx ? bc->w : mx;
    int H = bc->h < my ? bc->h : my;

    /* 3-byte UTF-8 buffer for braille glyphs in U+2800..U+28FF:
     *   byte0 = 0xE2
     *   byte1 = 0xA0 + (code >> 6)   -- braille codes are 0x2800+dots
     *                                    0x2800 >> 6 = 0xA0
     *   byte2 = 0x80 + (code & 0x3F) = 0x80 + (dots & 0x3F)
     * But dots can be 0..0xFF; we want the full 0x2800+dots.
     * 0x2800 + dots => high bits depend on bits 6-7 of dots.
     * Cleaner: compute codepoint then encode. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y * bc->w + x;
            unsigned char d = bc->dots[idx];
            if (d == 0) continue;
            if (x == mx - 1 && y == my - 1) continue; /* avoid cursor wrap */
            unsigned int cp_code = 0x2800u + d;
            /* UTF-8 for U+0800..U+FFFF: 1110xxxx 10xxxxxx 10xxxxxx */
            char buf[4];
            buf[0] = (char)(0xE0 | (cp_code >> 12));
            buf[1] = (char)(0x80 | ((cp_code >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp_code & 0x3F));
            buf[3] = '\0';
            int cp_pair = bc->color[idx];
            int attr = unpack_attr(bc->attr[idx]);
            wattr_set(win, attr, (short)cp_pair, NULL);
            mvwaddstr(win, y, x, buf);
        }
    }
    wattr_set(win, A_NORMAL, 0, NULL);
}
