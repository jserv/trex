#include <stdlib.h>
#include <string.h>

#include "trex.h"

/* Color management */
static int total_text_colors = 0;
static color_t **v_text_colors = NULL;

static int total_block_colors = 0;
static color_t **v_block_colors = NULL;

/* Configuration is now handled globally via ensure_cfg() in config.h */

/* Double buffering */
static render_buffer_t render_buffer = {NULL, NULL, true};

/* Dirty region tracking */
static int dirty_min_x = 0, dirty_min_y = 0;
static int dirty_max_x = 0, dirty_max_y = 0;
static bool has_dirty_region = false;

/* Helper function to create and initialize a color */
static color_t *create_color(short r, short g, short b, int color_id)
{
    color_t *new_color = malloc(sizeof(color_t));
    if (!new_color)
        return NULL;

    new_color->r = r;
    new_color->g = g;
    new_color->b = b;
    new_color->color_id = color_id;

    return new_color;
}

int draw_get_color_id(color_t **colors,
                      short r,
                      short g,
                      short b,
                      short r2,
                      short g2,
                      short b2,
                      color_type_t type)
{
    const game_config_t *cfg = ensure_cfg();

    /* Search for existing color */
    for (int i = 0; i < cfg->render.max_colors; ++i) {
        color_t *color = colors[i];
        if (!color)
            continue;

        bool match = (type == COLOR_TYPE_TEXT_WITH_BG)
                         ? (color->r == r + r2 && color->g == g + g2 &&
                            color->b == b + b2)
                         : (color->r == r && color->g == g && color->b == b);

        if (match)
            return color->color_id;
    }

    /* Create new color based on type */
    color_t *new_color = NULL;
    int color_id = -1;
    int *counter = NULL;
    color_t ***array = NULL;

    switch (type) {
    case COLOR_TYPE_TEXT:
        if (total_text_colors >= cfg->render.max_colors)
            return -1;
        color_id = cfg->render.text_base + total_text_colors;
        counter = &total_text_colors;
        array = &v_text_colors;
        break;

    case COLOR_TYPE_BLOCK:
        if (total_block_colors >= cfg->render.max_colors)
            return -1;
        color_id = cfg->render.block_base + total_block_colors;
        counter = &total_block_colors;
        array = &v_block_colors;
        break;

    case COLOR_TYPE_TEXT_WITH_BG:
        if (total_text_colors >= cfg->render.max_colors - 1)
            return -1;
        color_id = cfg->render.text_bg_base + total_text_colors;
        counter = &total_text_colors;
        array = &v_text_colors;
        break;
    }

    /* Create color structure */
    new_color =
        create_color((type == COLOR_TYPE_TEXT_WITH_BG) ? r + r2 : r,
                     (type == COLOR_TYPE_TEXT_WITH_BG) ? g + g2 : g,
                     (type == COLOR_TYPE_TEXT_WITH_BG) ? b + b2 : b, color_id);

    if (!new_color)
        return -1;

    /* Initialize color pairs and colors based on type */
    switch (type) {
    case COLOR_TYPE_TEXT:
        tui_init_pair(color_id, color_id, TUI_COLOR_BLACK);
        tui_init_color(color_id, (r * 1000) / 255, (g * 1000) / 255,
                       (b * 1000) / 255);
        break;

    case COLOR_TYPE_BLOCK:
        tui_init_pair(color_id, TUI_COLOR_BLACK, color_id);
        tui_init_color(color_id, (r * 1000) / 255, (g * 1000) / 255,
                       (b * 1000) / 255);
        break;

    case COLOR_TYPE_TEXT_WITH_BG:
        tui_init_pair(color_id, color_id, color_id + 1);
        tui_init_color(color_id, (r * 1000) / 255, (g * 1000) / 255,
                       (b * 1000) / 255);
        tui_init_color(color_id + 1, (r2 * 1000) / 255, (g2 * 1000) / 255,
                       (b2 * 1000) / 255);
        break;
    }

    /* Store in appropriate array */
    (*array)[(*counter)++] = new_color;
    if (type == COLOR_TYPE_TEXT_WITH_BG)
        (*counter)++;

    return new_color->color_id;

    return -1;
}

/* Render buffer management */
void draw_init_buffers(void)
{
    const game_config_t *cfg = ensure_cfg();

    /* Initialize color arrays if not already done */
    if (!v_text_colors)
        v_text_colors = calloc(cfg->render.max_colors, sizeof(color_t *));
    if (!v_block_colors)
        v_block_colors = calloc(cfg->render.max_colors, sizeof(color_t *));

    render_buffer.front_buffer = tui_stdscr;
    render_buffer.back_buffer = tui_stdscr;

    /* Enable keypad */
    tui_set_keypad(tui_stdscr, true);

    /* Set non-blocking mode */
    tui_set_nodelay(tui_stdscr, true);

    /* Clear screen */
    tui_clear_window(tui_stdscr);

    /* Initial refresh */
    tui_refresh(tui_stdscr);
}

void draw_cleanup_buffers(void)
{
    /* Since we're using tui_stdscr, don't delete it */
    render_buffer.front_buffer = NULL;
    render_buffer.back_buffer = NULL;
}

void draw_swap_buffers(void)
{
    /* Since we're using tui_stdscr directly, just refresh it */
    if (render_buffer.needs_refresh) {
        tui_refresh(tui_stdscr);

        /* Reset dirty tracking */
        has_dirty_region = false;
        render_buffer.needs_refresh = false;
    }
}

static tui_window_t *get_draw_buffer(void)
{
    return render_buffer.back_buffer ? render_buffer.back_buffer : tui_stdscr;
}

static void mark_dirty(int x, int y, int width, int height)
{
    if (!has_dirty_region) {
        dirty_min_x = x;
        dirty_min_y = y;
        dirty_max_x = x + width;
        dirty_max_y = y + height;
        has_dirty_region = true;
    } else {
        /* Expand dirty region using ternary operators */
        dirty_min_x = (x < dirty_min_x) ? x : dirty_min_x;
        dirty_min_y = (y < dirty_min_y) ? y : dirty_min_y;
        dirty_max_x = (x + width > dirty_max_x) ? x + width : dirty_max_x;
        dirty_max_y = (y + height > dirty_max_y) ? y + height : dirty_max_y;
    }

    render_buffer.needs_refresh = true;
}

void draw_clear_back_buffer(void)
{
    if (render_buffer.back_buffer) {
        tui_clear_window(render_buffer.back_buffer);
        render_buffer.needs_refresh = true;
        has_dirty_region = false;
    }
}

/* Core rendering functions with buffering */
void draw_text(int x, int y, char *text, int flags)
{
    tui_window_t *buffer = get_draw_buffer();
    int text_len = strlen(text);

    tui_wattron(buffer, flags);
    tui_print_at(buffer, y, x, "%s", text);
    tui_wattroff(buffer, flags);

    mark_dirty(x, y, text_len, 1);
}

void draw_text_color(int x,
                     int y,
                     char *text,
                     int flags,
                     short r,
                     short g,
                     short b)
{
    tui_window_t *buffer = get_draw_buffer();
    int color_pair =
        draw_get_color_id(v_text_colors, r, g, b, 0, 0, 0, COLOR_TYPE_TEXT);
    int text_len = strlen(text);

    tui_wattron(buffer, TUI_COLOR_PAIR(color_pair) | flags);
    tui_print_at(buffer, y, x, "%s", text);
    tui_wattroff(buffer, TUI_COLOR_PAIR(color_pair) | flags);

    mark_dirty(x, y, text_len, 1);
}

void draw_block(int x, int y, int cols, int rows, int flags)
{
    tui_window_t *buffer = get_draw_buffer();
    tui_wattron(buffer, flags);

    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i)
            tui_print_at(buffer, y + j, x + i, " ");
    }

    tui_wattroff(buffer, flags);

    mark_dirty(x, y, cols, rows);
}

void draw_block_color(int x,
                      int y,
                      int cols,
                      int rows,
                      short r,
                      short g,
                      short b)
{
    tui_window_t *buffer = get_draw_buffer();
    int color_pair =
        draw_get_color_id(v_block_colors, r, g, b, 0, 0, 0, COLOR_TYPE_BLOCK);

    tui_wattron(buffer, TUI_COLOR_PAIR(color_pair));

    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i)
            tui_print_at(buffer, y + j, x + i, " ");

    tui_wattroff(buffer, TUI_COLOR_PAIR(color_pair));

    mark_dirty(x, y, cols, rows);
}

void draw_text_bg(int x,
                  int y,
                  char *text,
                  int flags,
                  short r,
                  short g,
                  short b,
                  short r2,
                  short g2,
                  short b2)
{
    tui_window_t *buffer = get_draw_buffer();
    int color_pair = draw_get_color_id(v_text_colors, r, g, b, r2, g2, b2,
                                       COLOR_TYPE_TEXT_WITH_BG);
    int text_len = strlen(text);

    tui_wattron(buffer, TUI_COLOR_PAIR(color_pair) | flags);
    tui_print_at(buffer, y, x, "%s", text);
    tui_wattroff(buffer, TUI_COLOR_PAIR(color_pair) | flags);

    mark_dirty(x, y, text_len, 1);
}

void draw_logo(int x, int y)
{
    draw_text(
        x, y,
        "  _____ _                                _______     _____       "
        "                ",
        TUI_COLOR_PAIR(1));
    draw_text(
        x, y + 1,
        " / ____| |                              |__   __|   |  __ \\     "
        "       _     _   ",
        TUI_COLOR_PAIR(1));
    draw_text(x, y + 2,
              "| |    | |__  _ __ ___  _ __ ___   ___     | |______| |__) "
              "|_____  ___| |_ _| |_ ",
              TUI_COLOR_PAIR(1));
    draw_text(
        x, y + 3,
        "| |    | '_ \\| '__/ _ \\| '_ ` _ \\ / _ \\    | |______|  _  // "
        "_ \\ \\/ |_   _|_   _|",
        TUI_COLOR_PAIR(1));
    draw_text(x, y + 4,
              "| |____| | | | | | (_) | | | | | |  __/    | |      | | \\ |  "
              "__/>  <  |_|   |_|  ",
              TUI_COLOR_PAIR(1));
    draw_text(x, y + 5,
              " \\_____|_| |_|_|  \\___/|_| |_| |_|\\___|    |_|      |_|  "
              "\\_\\___/_/\\_\\            ",
              TUI_COLOR_PAIR(1));
}

/* Color management cleanup */
void draw_cleanup_colors(void)
{
    /* Free all text colors */
    const game_config_t *cfg = ensure_cfg();

    for (int i = 0; i < total_text_colors && i < cfg->render.max_colors; i++) {
        if (v_text_colors[i]) {
            free(v_text_colors[i]);
            v_text_colors[i] = NULL;
        }
    }

    /* Free all block colors */
    for (int i = 0; i < total_block_colors && i < cfg->render.max_colors; i++) {
        if (v_block_colors[i]) {
            free(v_block_colors[i]);
            v_block_colors[i] = NULL;
        }
    }

    /* Reset counters */
    total_text_colors = 0;
    total_block_colors = 0;
}
