#ifndef SUBPIXEL_H
#define SUBPIXEL_H

#define _MAP_NEEDS_CURSES
#include <curses.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Subpixel braille canvas. Each character cell holds a 2x4 grid of dots,
 * packed into a Unicode braille glyph (U+2800..U+28FF). This gives us 8x
 * more "pixels" than direct character rendering for line primitives like
 * coastlines and roads, which is the single biggest win for visual fidelity
 * at any zoom level.
 *
 * Fills are still done at character resolution; only strokes route through
 * the braille canvas. At end-of-frame we flush the canvas over whatever the
 * fill pass already wrote — any cell with no dots is left alone.
 * -------------------------------------------------------------------------*/

typedef struct {
    unsigned char *dots;   /* [h * w] — bitmask; 0 means cell untouched */
    unsigned char *color;  /* [h * w] — curses color pair id */
    unsigned char *attr;   /* [h * w] — attribute byte (A_BOLD, A_DIM -> 1,2) */
    int w, h;              /* canvas size in *cells* */
    int pw, ph;            /* canvas size in *pixels* (2w, 4h) */
} BrailleCanvas;

/* Allocate / reallocate to match window size. Safe to call every frame. */
void bc_resize(BrailleCanvas *bc, int w, int h);

/* Free internal buffers. */
void bc_free(BrailleCanvas *bc);

/* Clear all dots (keeps allocations). */
void bc_clear(BrailleCanvas *bc);

/* Set a single pixel. Clamps to canvas. */
void bc_set(BrailleCanvas *bc, int px, int py, int cp, int attr);

/* Draw a line in pixel coordinates (Bresenham). Endpoints may be outside
 * the canvas — they're clipped internally. */
void bc_line(BrailleCanvas *bc, int x0, int y0, int x1, int y1,
             int cp, int attr);

/* Emit the canvas to a curses window. For each cell with non-zero dots,
 * write the corresponding braille glyph at (col, row). Cells with zero
 * dots are left untouched so the fill pass shows through. */
void bc_flush(BrailleCanvas *bc, WINDOW *win);

#endif /* SUBPIXEL_H */
