#include "ui.h"
#include <string.h>
#include <stdio.h>

#define SIDEBAR_WIDTH 36

void ui_init(UIState *ui)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, FALSE);
    timeout(100); /* non-blocking with 100ms timeout for resize */

    if (has_colors()) {
        start_color();
        use_default_colors();

        /* Color pairs */
        init_pair(CP_WATER,   COLOR_CYAN,    -1);
        init_pair(CP_LAND,    COLOR_GREEN,   -1);
        init_pair(CP_BORDER,  COLOR_YELLOW,  -1);
        init_pair(CP_GRID,    COLOR_BLUE,    -1);
        init_pair(CP_ROUTE,   COLOR_RED,     -1);
        init_pair(CP_MARKER,  COLOR_MAGENTA, -1);
        init_pair(CP_STATUS,  COLOR_WHITE,   COLOR_BLUE);
        init_pair(CP_SIDEBAR, COLOR_WHITE,   -1);
        init_pair(CP_INPUT,   COLOR_WHITE,   COLOR_BLACK);
        init_pair(CP_TITLE,   COLOR_YELLOW,  COLOR_BLUE);
    }

    getmaxyx(stdscr, ui->term_h, ui->term_w);

    ui->sidebar_visible = 0;
    ui->sidebar_width = SIDEBAR_WIDTH;
    ui->sidebar_scroll = 0;

    /* Create windows */
    int map_w = ui->term_w;
    int map_h = ui->term_h - 2; /* leave 2 rows for status */

    ui->map_win = newwin(map_h, map_w, 0, 0);
    ui->status_win = newwin(2, ui->term_w, map_h, 0);
    ui->sidebar_win = NULL;
}

void ui_cleanup(UIState *ui)
{
    if (ui->map_win) delwin(ui->map_win);
    if (ui->status_win) delwin(ui->status_win);
    if (ui->sidebar_win) delwin(ui->sidebar_win);
    endwin();
}

void ui_resize(UIState *ui)
{
    getmaxyx(stdscr, ui->term_h, ui->term_w);

    int map_w = ui->term_w;
    int map_h = ui->term_h - 2;

    if (ui->sidebar_visible) {
        map_w -= ui->sidebar_width;
        if (map_w < 20) map_w = 20;
    }

    /* Recreate windows */
    if (ui->map_win) delwin(ui->map_win);
    if (ui->status_win) delwin(ui->status_win);
    if (ui->sidebar_win) delwin(ui->sidebar_win);

    ui->map_win = newwin(map_h, map_w, 0, 0);
    ui->status_win = newwin(2, ui->term_w, map_h, 0);

    if (ui->sidebar_visible) {
        ui->sidebar_win = newwin(map_h, ui->sidebar_width,
                                 0, map_w);
    } else {
        ui->sidebar_win = NULL;
    }
}

void ui_draw_status(UIState *ui, MapView *mv, const char *message)
{
    werase(ui->status_win);
    wbkgd(ui->status_win, COLOR_PAIR(CP_STATUS));

    /* Top status line: coordinates & zoom */
    char line1[256];
    snprintf(line1, sizeof(line1),
             " Lat: %.4f  Lon: %.4f  Zoom: %d",
             mv->center_lat, mv->center_lon, mv->zoom);
    mvwprintw(ui->status_win, 0, 0, "%-*s", ui->term_w, line1);

    /* Bottom status line: controls & message */
    char line2[256];
    if (message && message[0]) {
        snprintf(line2, sizeof(line2), " %s", message);
    } else {
        snprintf(line2, sizeof(line2),
                 " [Arrows]Pan [+/-]Zoom [g]Goto [r]Route [d]Directions [c]Clear [q]Quit");
    }
    mvwprintw(ui->status_win, 1, 0, "%-*s", ui->term_w, line2);

    /* Title */
    const char *title = " RouteASCII ";
    int title_x = ui->term_w - (int)strlen(title) - 1;
    if (title_x > 0) {
        wattron(ui->status_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(ui->status_win, 0, title_x, "%s", title);
        wattroff(ui->status_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    }

    wrefresh(ui->status_win);
}

void ui_draw_sidebar(UIState *ui, RouteResult *rr)
{
    if (!ui->sidebar_win || !rr) return;

    werase(ui->sidebar_win);
    wbkgd(ui->sidebar_win, COLOR_PAIR(CP_SIDEBAR));
    box(ui->sidebar_win, 0, 0);

    /* Title */
    wattron(ui->sidebar_win, A_BOLD);
    mvwprintw(ui->sidebar_win, 0, 2, " Directions ");
    wattroff(ui->sidebar_win, A_BOLD);

    if (!rr->valid) {
        mvwprintw(ui->sidebar_win, 2, 2, "No route loaded");
        wrefresh(ui->sidebar_win);
        return;
    }

    /* Summary */
    char dist_buf[64], dur_buf[64];
    route_format_distance(rr->total_distance_m, dist_buf, sizeof(dist_buf), 1);
    route_format_duration(rr->total_duration_s, dur_buf, sizeof(dur_buf));

    wattron(ui->sidebar_win, A_BOLD | COLOR_PAIR(CP_ROUTE));
    mvwprintw(ui->sidebar_win, 1, 2, "%s | %s", dist_buf, dur_buf);
    wattroff(ui->sidebar_win, A_BOLD | COLOR_PAIR(CP_ROUTE));

    int max_y, max_x;
    getmaxyx(ui->sidebar_win, max_y, max_x);

    /* Steps list */
    int y = 3;
    int start = ui->sidebar_scroll;
    for (int i = start; i < rr->step_count && y < max_y - 1; i++) {
        RouteStep *s = &rr->steps[i];

        /* Step number & instruction */
        wattron(ui->sidebar_win, A_BOLD);
        mvwprintw(ui->sidebar_win, y, 1, "%2d.", i + 1);
        wattroff(ui->sidebar_win, A_BOLD);

        /* Truncate instruction to fit */
        char inst[64];
        snprintf(inst, sizeof(inst), "%.28s", s->instruction);
        mvwprintw(ui->sidebar_win, y, 5, "%s", inst);
        y++;

        if (y < max_y - 1 && s->road_name[0]) {
            char road[64];
            snprintf(road, sizeof(road), "%.30s", s->road_name);
            mvwprintw(ui->sidebar_win, y, 5, "%s", road);
            y++;
        }

        if (y < max_y - 1) {
            char sd[32], st[32];
            route_format_distance(s->distance_m, sd, sizeof(sd), 1);
            route_format_duration(s->duration_s, st, sizeof(st));
            mvwprintw(ui->sidebar_win, y, 5, "%s  %s", sd, st);
            y++;
        }

        y++; /* blank line between steps */
    }

    wrefresh(ui->sidebar_win);
}

int ui_input_dialog(UIState *ui, const char *prompt, char *buf, size_t buf_sz)
{
    int dw = 50;
    int dh = 5;
    int dx = (ui->term_w - dw) / 2;
    int dy = (ui->term_h - dh) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;

    WINDOW *dlg = newwin(dh, dw, dy, dx);
    wbkgd(dlg, COLOR_PAIR(CP_INPUT));
    box(dlg, 0, 0);

    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, 2, " %s ", prompt);
    wattroff(dlg, A_BOLD);

    mvwprintw(dlg, 2, 2, "> ");
    wrefresh(dlg);

    /* Enable cursor and echo for input */
    curs_set(1);
    echo();
    nodelay(stdscr, FALSE);
    timeout(-1);

    /* Read input */
    int max_input = (int)buf_sz - 1;
    if (max_input > dw - 6) max_input = dw - 6;

    memset(buf, 0, buf_sz);
    mvwgetnstr(dlg, 2, 4, buf, max_input);

    /* Restore settings */
    noecho();
    curs_set(0);
    timeout(100);

    delwin(dlg);

    /* Check if user cancelled (empty input) */
    return (buf[0] != '\0') ? 1 : 0;
}

void ui_show_message(UIState *ui, const char *msg)
{
    int len = (int)strlen(msg);
    int dw = len + 6;
    if (dw > ui->term_w - 4) dw = ui->term_w - 4;
    int dh = 3;
    int dx = (ui->term_w - dw) / 2;
    int dy = (ui->term_h - dh) / 2;

    WINDOW *dlg = newwin(dh, dw, dy, dx);
    wbkgd(dlg, COLOR_PAIR(CP_INPUT));
    box(dlg, 0, 0);
    mvwprintw(dlg, 1, 3, "%.*s", dw - 6, msg);
    wrefresh(dlg);

    timeout(-1);
    wgetch(dlg);
    timeout(100);

    delwin(dlg);
}

void ui_toggle_sidebar(UIState *ui)
{
    ui->sidebar_visible = !ui->sidebar_visible;
    ui->sidebar_scroll = 0;
    ui_resize(ui);
}