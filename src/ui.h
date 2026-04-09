#ifndef UI_H
#define UI_H

#include <curses.h>
#include "map.h"
#include "route.h"

/* Color pair IDs */
#define CP_WATER     1
#define CP_LAND      2
#define CP_BORDER    3
#define CP_GRID      4
#define CP_ROUTE     5
#define CP_MARKER    6
#define CP_STATUS    7
#define CP_SIDEBAR   8
#define CP_INPUT     9
#define CP_TITLE     10

/* UI state */
typedef struct {
    WINDOW *map_win;
    WINDOW *status_win;
    WINDOW *sidebar_win;
    int     sidebar_visible;
    int     sidebar_width;
    int     sidebar_scroll;
    int     term_w, term_h;
} UIState;

/* Initialize curses, colors, windows */
void ui_init(UIState *ui);

/* Cleanup curses */
void ui_cleanup(UIState *ui);

/* Resize handler — rebuild windows */
void ui_resize(UIState *ui);

/* Draw the status bar at the bottom */
void ui_draw_status(UIState *ui, MapView *mv, const char *message);

/* Draw the directions sidebar */
void ui_draw_sidebar(UIState *ui, RouteResult *rr);

/* Prompt the user for text input. Returns 1 if OK, 0 if cancelled. */
int ui_input_dialog(UIState *ui, const char *prompt, char *buf, size_t buf_sz);

/* Show a brief message overlay */
void ui_show_message(UIState *ui, const char *msg);

/* Toggle sidebar visibility */
void ui_toggle_sidebar(UIState *ui);

#endif /* UI_H */