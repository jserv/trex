#pragma once

/*
 * T-Rex Game Public API
 *
 * This header provides the public interface for the T-Rex game,
 * including game logic, rendering infrastructure, and configuration.
 */

#include <stdbool.h>

#define LOGO_START_Y 9

/* Type aliases for clarity */
typedef struct tui_term_cap_t tui_term_cap_t;
typedef struct tui_window_t tui_window_t;

/* Predefined windows */
extern tui_window_t *tui_stdscr;
extern tui_window_t *tui_curscr;

/* Key constants and macros */
#define TUI_KEY_CODE_YES 256
#define TUI_KEY_MIN 257
#define TUI_KEY_BREAK 257
#define TUI_KEY_SRESET 344
#define TUI_KEY_RESET 345
#define TUI_KEY_DOWN 258
#define TUI_KEY_UP 259
#define TUI_KEY_LEFT 260
#define TUI_KEY_RIGHT 261
#define TUI_KEY_HOME 262
#define TUI_KEY_BACKSPACE 263
#define TUI_KEY_ENTER 0x10C
#define TUI_KEY_F0 264
#define TUI_KEY_MAX 400
#define TUI_KEY_ESC 27

/* Function key macros */
#define TUI_KEY_F(n) (TUI_KEY_F0 + (n))

/* Color constants */
#define TUI_COLOR_BLACK 0
#define TUI_COLOR_RED 1
#define TUI_COLOR_GREEN 2
#define TUI_COLOR_YELLOW 3
#define TUI_COLOR_BLUE 4
#define TUI_COLOR_MAGENTA 5
#define TUI_COLOR_CYAN 6
#define TUI_COLOR_WHITE 7

/* Attribute constants */
#define TUI_A_NORMAL 0x00000000
#define TUI_A_UNDERLINE 0x00020000
#define TUI_A_REVERSE 0x00040000
#define TUI_A_BLINK 0x00080000
#define TUI_A_DIM 0x00100000
#define TUI_A_BOLD 0x00200000
#define TUI_A_ALTCHARSET 0x00400000
#define TUI_A_INVISIBLE 0x00800000
#define TUI_A_PROTECT 0x01000000
#define TUI_A_HORIZONTAL 0x02000000
#define TUI_A_LEFT 0x04000000
#define TUI_A_LOW 0x08000000
#define TUI_A_RIGHT 0x10000000
#define TUI_A_TOP 0x20000000
#define TUI_A_VERTICAL 0x40000000
#define TUI_A_ITALIC 0x80000000
#define TUI_A_COLOR 0xFF00

/* Color pair macros */
#define TUI_COLOR_PAIR(n) ((int) ((n) << 8))
#define TUI_PAIR_NUMBER(a) (((int) (a) >> 8) & 0xff)

/* Special return values */
#define TUI_ERR (-1)
#define TUI_OK 0

/* TUI initialization and cleanup */
tui_window_t *tui_init(void);
int tui_cleanup(void);
bool tui_check_shutdown(void);
bool tui_check_resize(void);

/* Terminal capability functions */
tui_term_cap_t *tui_term_cap_new(void);
void tui_term_cap_delete(tui_term_cap_t *cap);
int tui_get_term_ncols(tui_term_cap_t *cap);
int tui_get_term_nrows(tui_term_cap_t *cap);

/* Screen management */
void tui_clear(tui_window_t *win);
void tui_erase(tui_window_t *win);
int tui_clear_window(tui_window_t *win);
int tui_refresh(tui_window_t *win);
int tui_endwin(void);

/* Input configuration */
int tui_raw(void);
int tui_cbreak(void);
int tui_noecho(void);
int tui_echo(void);
int tui_set_nodelay(tui_window_t *win, bool enable);
int tui_set_keypad(tui_window_t *win, bool enable);

/* Cursor control */
int tui_set_cursor(int visibility);
void tui_move(tui_window_t *win, int row, int col);

/* Color management */
int tui_start_color(void);
int tui_has_colors(void);
int tui_init_pair(short pair, short f, short b);
int tui_init_color(short color, short r, short g, short b);

/* Character input */
int tui_getch(void);
bool tui_has_input(void);

/* Terminal control */
int tui_noraw(void);
int tui_clear_screen(void);

/* Text output */
void tui_wprintw(tui_window_t *win, const char *fmt, ...);
int tui_print_at(tui_window_t *win, int row, int col, const char *fmt, ...);

/* Attribute management */
int tui_wattron(tui_window_t *win, int attrs);
int tui_wattroff(tui_window_t *win, int attrs);

/* Window properties */
int tui_get_max_x(tui_window_t *win);
int tui_get_max_y(tui_window_t *win);

/* Debug statistics */
void tui_debug_writev_stats(void);
void tui_debug_rle_stats(void);
void tui_debug_lru_cache(void);
void tui_debug_string_interning(void);

/* ========== Game Configuration (from config.h) ========== */

/* Frame rate and timing configuration */
typedef struct {
    int target_fps;
    double frame_time;
    double update_ms; /* Physics update interval */
    double anim_ms;   /* Animation frame interval */
    int sleep_us;
} timing_config_t;

/* Physics configuration */
typedef struct {
    int jump_height;
    int fall_depth;
    int bounds_buffer;
} physics_config_t;

/* Power-up configuration */
typedef struct {
    double duration;
    double duck_timeout;
} powerup_config_t;

/* Color configuration (RGB values 0-255) */
typedef struct {
    unsigned char r;
    unsigned char g;
    unsigned char b;
} rgb_color_t;

typedef struct {
    /* T-Rex colors */
    rgb_color_t trex_normal;
    rgb_color_t trex_dead;
    rgb_color_t trex_invincible;
    rgb_color_t trex_fire;

    /* Object colors */
    rgb_color_t cactus;
    rgb_color_t rock;
    rgb_color_t egg_base;
    rgb_color_t pterodactyl;
    rgb_color_t fireball;

    /* Ground colors */
    rgb_color_t ground_normal_primary;
    rgb_color_t ground_normal_secondary;
    rgb_color_t ground_dead_primary;
    rgb_color_t ground_dead_secondary;
    rgb_color_t ground_speck;

    /* UI colors */
    rgb_color_t menu_title;
    rgb_color_t menu_selected;
    rgb_color_t menu_unselected;
    rgb_color_t menu_help;
    rgb_color_t score_text;
} colors_config_t;

/* Level configuration */
typedef struct {
    int level;
    int spawn_min;
    int spawn_max;
    int score_next;
} level_config_t;

/* Object probability configuration */
typedef struct {
    int object_type;
    int range_start;
    int range_end;
} object_probability_t;

/* Scoring configuration */
typedef struct {
    int fireball_kill;
    int powerup_collect;
    int per_frame;
} scoring_config_t;

/* Spatial collision configuration */
typedef struct {
    int bucket_size;
    int bucket_count;
} spatial_config_t;

/* UI layout configuration */
typedef struct {
    int menu_options;
    int menu_spacing;
    int trex_offset_x;
    int trex_offset_y;
    int content_offset_x;
} ui_layout_t;

/* Rendering configuration */
typedef struct {
    int max_colors;
    int text_base;
    int block_base;
    int text_bg_base;
    int speck_interval_1;
    int speck_interval_2;
} render_config_t;

/* Game limits */
typedef struct {
    int max_level;
    int max_objects;
    int object_types;
} game_limits_t;

/* Main configuration structure */
typedef struct {
    timing_config_t timing;
    physics_config_t physics;
    powerup_config_t powerups;
    colors_config_t colors;
    scoring_config_t scoring;
    spatial_config_t spatial;
    ui_layout_t ui;
    render_config_t render;
    game_limits_t limits;
} game_config_t;

/* Configuration access functions */
const game_config_t *config_get(void);
const level_config_t *config_get_level(int level);
const object_probability_t *config_get_probs(void);
int config_get_prob_count(void);

/* Global configuration cache */
extern const game_config_t *g_cfg;

/* Ensure configuration is loaded */
static inline const game_config_t *ensure_cfg(void)
{
    if (!g_cfg)
        g_cfg = config_get();
    return g_cfg;
}

/* Player spawn configuration */
typedef struct {
    int x;
    int y_offset;
} player_spawn_t;

const player_spawn_t *config_get_spawn(void);

/* Color type enums for get_color_id */
typedef enum {
    COLOR_TYPE_TEXT = 0,
    COLOR_TYPE_BLOCK = 1,
    COLOR_TYPE_TEXT_WITH_BG = 2
} color_type_t;

/* Structure to store the color and its ID */
typedef struct color {
    short r, g, b;
    int color_id;
} color_t;

/* Double buffering support */
typedef struct {
    tui_window_t *front_buffer; /* Displayed buffer */
    tui_window_t *back_buffer;  /* Drawing buffer */
    bool needs_refresh;         /* Flag to track if refresh is needed */
} render_buffer_t;

/* Draw text with ncurses flags */
void draw_render_text(int x, int y, char *text, int flags);

/* Draw text with RGB colors */
void draw_render_colored_text(int x,
                              int y,
                              char *text,
                              int flags,
                              short r,
                              short g,
                              short b);

/* Draw text with RGB colors and RGB background */
void draw_render_text_with_background(int x,
                                      int y,
                                      char *text,
                                      int flags,
                                      short r,
                                      short g,
                                      short b,
                                      short r2,
                                      short g2,
                                      short b2);

/* Draw an empty block with ncurses flags */
void draw_render_block(int x, int y, int cols, int rows, int flags);

/* Draw an empty block with RGB colors */
void draw_render_colored_block(int x,
                               int y,
                               int cols,
                               int rows,
                               short r,
                               short g,
                               short b);

/* Draw the game logo */
void draw_render_logo();

/* Get color ID for given RGB values - Used internally by render system */
int draw_get_color_id(color_t **colors,
                      short r,
                      short g,
                      short b,
                      short r2,
                      short g2,
                      short b2,
                      color_type_t type);

/* Render buffer management functions */
void draw_init_buffers(void);
void draw_cleanup_buffers(void);
void draw_swap_buffers(void);
void draw_clear_back_buffer(void);

/* Color management cleanup */
void draw_cleanup_colors(void);

/* Resolution is now dynamically obtained via state_get_resolution() */

/* Forward declarations */
typedef struct object object_t;
typedef struct bounding_box bounding_box_t;

/* Game object types */
typedef enum {
    OBJECT_TREX = 0,
    OBJECT_CACTUS = 1,
    OBJECT_ROCK = 2,
    OBJECT_PTERODACTYL = 3,
    OBJECT_GROUND_HOLE = 4,
    OBJECT_EGG_INVINCIBLE = 5,
    OBJECT_EGG_FIRE = 6,
    OBJECT_FIRE_BALL = 7
} object_type_t;

/* Game states */
typedef enum {
    STATE_IDLE = 0,
    STATE_RUNNING = 1,
    STATE_JUMPING = 2,
    STATE_FALLING = 3,
    STATE_DUCK = 4
} state_t;

/* Bounding box for collision detection */
struct bounding_box {
    int x, y, width, height;
};

/* Game object structure */
struct object {
    int x, y;
    int cols, rows;
    object_type_t type;
    state_t state;
    int frame;
    int max_frames;
    int height;

    bool enemy;
    bounding_box_t bounding_box;
};

/* Object management functions */
void play_init_object(object_t *object);
int play_find_free_slot();
void play_add_object(int x, int y, object_type_t type);
void play_cleanup_objects();

/* Rendering functions */
void play_render_object(object_t const *object);

/* World management */
void play_init_world();
void play_update_world(double elapsed);
void play_render_world();
void play_adjust_for_resize();

/* Input handling */
void play_handle_input(int input);

/* Object generation */
object_type_t play_random_object(bool b_generate_egg);

/* Kill the player */
void play_kill_player();

/* Game screen types */
typedef enum {
    SCREEN_MENU = 0,
    SCREEN_WORLD = 1,
} screen_type_t;

/* Time management */
double state_get_time_ms();

/* State initialization and main loop functions */
void state_initialize();
void state_update_frame();
void state_render_frame();

/* Screen management */
void state_set_screen_type(screen_type_t screen_type);
screen_type_t state_get_screen_type();
screen_type_t state_restore_screen_type();

/* Resolution management */
int state_get_resolution(int type); /* 0 for rows, 1 for cols */
int state_get_resolution_rows();
int state_get_resolution_cols();

/* Input handling */
void state_handle_input(int key_code);

/* Game control */
void state_quit_game();
bool state_is_running();

/* Initialize sprite data */
void sprites_init(void);

/* Convenience macros */
#define RESOLUTION_ROWS (state_get_resolution(0))
#define RESOLUTION_COLS (state_get_resolution(1))
#define TICKCOUNT (state_get_time_ms())
