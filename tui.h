#pragma once

#include "trex.h"

/* Terminal capabilities structure */
typedef struct {
    /* Color capabilities */
    bool supports_color;
    bool supports_256_colors;
    bool supports_true_color;
    bool can_change_colors;
    uint16_t max_colors;
    uint16_t max_pairs;

    /* Cursor capabilities */
    bool can_hide_cursor;
    bool can_blink_cursor;
    bool has_block_cursor;
    bool supports_cursor_shapes;

    /* Screen capabilities */
    bool can_clear_screen;
    bool can_scroll;
    bool has_alt_screen;
    bool supports_save_restore;
    bool alt_screen;

    /* Input capabilities */
    bool has_mouse;
    bool has_function_keys;
    bool has_meta_key;
    bool supports_paste_bracketing;
    bool supports_mouse;
    bool supports_bracketed_paste;

    /* Advanced features */
    bool supports_unicode;
    bool supports_wide_chars;
    bool supports_sixel;
    bool supports_kitty_graphics;

    /* Text attributes */
    bool supports_bold;
    bool supports_underline;
    bool supports_reverse;
    bool supports_dim;
    bool supports_blink;
    bool supports_italic;
    bool strikethrough;

    /* Terminal specific features */
    bool supports_ech;
    bool supports_rep;

    /* Terminal identification */
    char term_name[64];
    char term_version[32];
    uint16_t term_width;
    uint16_t term_height;

    /* Performance flags */
    bool fast_scrolling;
    bool fast_color_changes;
    bool hw_accel;

    /* Detection metadata */
    bool detection_complete;
    uint64_t detection_time_ms;
    uint32_t checksum;
} tui_term_caps_t;

/* Window structure - already typedef'd in trex.h */
struct tui_window_t {
    int begy, begx;
    int maxy, maxx;
    int cury, curx;
    int keypad_mode;
    int delay;
    int attr;
    int bkgd;
    unsigned char *dirty;
};

/* Color pairs constant */
#define TUI_COLOR_PAIRS 256

/* External variables */
extern tui_window_t *tui_stdscr;
extern int tui_lines;
extern int tui_cols;
