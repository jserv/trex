#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <termios.h>

#include "tui.h"

/* Forward declarations */
static void apply_attributes(int attr);

tui_window_t *tui_stdscr = NULL;
int tui_lines = 0;
int tui_cols = 0;

static struct termios orig_termios, saved_termios;
static int term_initialized = 0, cursor_visibility = 1, colors_initialized = 0;

/* Terminal capabilities cache */
static tui_term_caps_t g_terminal_caps = {0};
static bool g_caps_loaded = false, g_caps_initialized = false;

/* Color mode based on detected capabilities */
typedef enum {
    COLOR_MODE_MONO,
    COLOR_MODE_16,
    COLOR_MODE_256,
    COLOR_MODE_TRUECOLOR
} color_mode_t;
static color_mode_t g_color_mode = COLOR_MODE_16;

/* Vectored I/O for optimal performance */
#define MAX_IOVECS 128
#define WRITEV_BUFFER_SIZE 4096
#define VEC_FLUSH_THRESHOLD 64 /* Flush when we have many vectors */

/* Data storage for vectors - ensures data lifetime */
#define WRITEV_DATA_POOL_SIZE 8192

typedef struct {
    struct iovec vecs[MAX_IOVECS];
    int count;
    size_t total_bytes;
    bool auto_flush_enabled;
    /* Data pool to store copied data */
    char data_pool[WRITEV_DATA_POOL_SIZE];
    size_t data_pool_used;
} writev_buffer_t;

static writev_buffer_t writev_buf = {
    .count = 0,
    .total_bytes = 0,
    .auto_flush_enabled = true,
    .data_pool_used = 0,
};

/* Parameters for vectored output buffer */
static struct {
    uint64_t writev_calls;
    uint64_t total_vectors;
    uint64_t total_bytes;
    uint64_t fallback_writes;
    uint64_t partial_writes;
} writev_stats = {0};

/* Fallback buffering for compatibility */
#define OUTPUT_BUFFER_SIZE 8192
#define BUFFER_FLUSH_THRESHOLD (OUTPUT_BUFFER_SIZE * 3 / 4) /* Flush at 75% */
static struct {
    char data[OUTPUT_BUFFER_SIZE];
    size_t len;
    bool auto_flush_enabled;
    bool use_writev;
} output_buffer = {.len = 0, .auto_flush_enabled = true, .use_writev = true};

/* Cursor position caching */
#define CURSOR_CACHE_ROWS 100
#define CURSOR_CACHE_COLS 200
static struct {
    char sequences[CURSOR_CACHE_ROWS][CURSOR_CACHE_COLS][16];
    int lengths[CURSOR_CACHE_ROWS][CURSOR_CACHE_COLS];
    bool initialized;
    int last_row;
    int last_col;
} cursor_cache = {.initialized = false, .last_row = -1, .last_col = -1};

/* Hierarchical dirty region tracking with 2-level tile system */
#define TILE_L1_SIZE 8     /* Level 1: 8x8 character tiles */
#define TILE_L2_SIZE 32    /* Level 2: 32x32 character blocks (4x4 L1 tiles) */
#define MAX_L1_TILES_X 128 /* Max tiles in X direction for Level 1 */
#define MAX_L1_TILES_Y 64  /* Max tiles in Y direction for Level 1 */
#define MAX_L2_BLOCKS_X 32 /* Max blocks in X direction for Level 2 */
#define MAX_L2_BLOCKS_Y 16 /* Max blocks in Y direction for Level 2 */

/* Sparse dirty tile tracking for better cache efficiency */
typedef struct dirty_tile {
    uint16_t row, col;
    struct dirty_tile *next;
} dirty_tile_t;

#define DIRTY_TILE_POOL_SIZE 512
static dirty_tile_t dirty_tile_pool[DIRTY_TILE_POOL_SIZE];
static dirty_tile_t *dirty_tile_free_list = NULL;
static dirty_tile_t *dirty_l1_tiles = NULL;
static dirty_tile_t *dirty_l2_blocks = NULL;
static int dirty_tile_pool_used = 0;

/* Bitmap for O(1) membership checks in sparse tracking */
#define BITMAP_WORDS ((MAX_L1_TILES_X * MAX_L1_TILES_Y + 63) / 64)
#define BITMAP_L2_WORDS ((MAX_L2_BLOCKS_X * MAX_L2_BLOCKS_Y + 63) / 64)
static uint64_t l1_tile_bitmap[BITMAP_WORDS];
static uint64_t l2_block_bitmap[BITMAP_L2_WORDS];

static struct {
    int min_row, max_row;
    int min_col, max_col;
    bool has_changes;

    /* Level 1: 8x8 tiles for fine-grained tracking */
    bool l1_tiles[MAX_L1_TILES_Y][MAX_L1_TILES_X];
    int l1_tiles_x, l1_tiles_y;

    /* Level 2: 32x32 blocks for coarse-grained tracking */
    bool l2_blocks[MAX_L2_BLOCKS_Y][MAX_L2_BLOCKS_X];
    int l2_blocks_x, l2_blocks_y;

    /* Optimization flags */
    bool use_hierarchical_tiles;
    bool use_sparse_tracking;

    /* Performance metrics for algorithm tuning */
    uint64_t l1_scans_avoided;
    uint64_t l2_scans_avoided;
    uint64_t total_scans;
    uint64_t sparse_hits;

    /* Adaptive mode switching */
    uint64_t frame_count;
    uint64_t sparse_beneficial_count;
    bool prefer_sparse_mode;
} dirty_region = {
    .min_row = INT_MAX,
    .max_row = -1,
    .min_col = INT_MAX,
    .max_col = -1,
    .has_changes = false,
    .l1_tiles_x = 0,
    .l1_tiles_y = 0,
    .l2_blocks_x = 0,
    .l2_blocks_y = 0,
    .use_hierarchical_tiles = false,
    .l1_scans_avoided = 0,
    .l2_scans_avoided = 0,
    .total_scans = 0,
};

/* Attribute state tracking */
static struct {
    short last_fg;
    short last_bg;
    int last_attrs;
    bool initialized;
} attr_state = {
    .last_fg = -1,
    .last_bg = -1,
    .last_attrs = -1,
    .initialized = false,
};

typedef struct {
    short fg;
    short bg;
} color_pair_t;

/* Color pair node for lazy allocation */
typedef struct color_pair_node {
    uint16_t fg_bg;               /* Packed fg/bg (8 bits each) */
    short pair_num;               /* Allocated pair number */
    short fg;                     /* Foreground color */
    short bg;                     /* Background color */
    struct color_pair_node *next; /* Hash collision chain */
} color_pair_node_t;

/* Hash table for lazy color pair allocation */
#define COLOR_PAIR_HASH_SIZE 256
#define COMMON_PAIRS_CACHE_SIZE 16

/* Pre-computed common color pairs */
typedef struct {
    short fg, bg;
    short pair_num;
    int usage_count;
} common_pair_t;

static struct {
    color_pair_node_t *table[COLOR_PAIR_HASH_SIZE];
    short next_pair;          /* Next available pair number */
    int allocated_count;      /* Number of allocated pairs */
    color_pair_node_t *nodes; /* Pre-allocated node pool */
    int node_count;           /* Number of nodes in pool */
    int node_used;            /* Number of used nodes */

    /* Usage statistics */
    int cache_hits;
    int cache_misses;
    int hash_collisions;

    /* Common pairs cache */
    common_pair_t common_pairs[COMMON_PAIRS_CACHE_SIZE];
    int common_pairs_count;
} color_pair_cache = {
    .next_pair = 1, /* Start from 1, 0 is default */
    .allocated_count = 0,
    .nodes = NULL,
    .node_count = 0,
    .node_used = 0,
    .cache_hits = 0,
    .cache_misses = 0,
    .hash_collisions = 0,
    .common_pairs_count = 0,
};

/* Legacy static array for backward compatibility */
static color_pair_t color_pairs[TUI_COLOR_PAIRS];

/* String interning for escape sequences */
#define ESC_SEQ_POOL_SIZE 2048 /* Increased for better coverage */
#define ESC_SEQ_MAX_LEN 64
#define ESC_SEQ_HASH_SIZE 512      /* Increased for less collisions */
#define ATTR_COMBO_CACHE_SIZE 1024 /* Increased for more combinations */

/* Pre-computed sequence pools */
#define CURSOR_POS_POOL_SIZE 256 /* Pool for common cursor positions */
#define COLOR_SEQ_POOL_SIZE 128  /* Pool for common color sequences */

/* LRU Escape Sequence Cache */
#define ESC_LRU_CACHE_SIZE 128
#define ESC_LRU_HASH_BITS 7 /* 2^7 = 128 buckets */
#define ESC_LRU_HASH_SIZE (1 << ESC_LRU_HASH_BITS)

typedef struct esc_seq_entry {
    char sequence[ESC_SEQ_MAX_LEN];
    int length;
    uint32_t hash;
    int ref_count;
    struct esc_seq_entry *next;
} esc_seq_entry_t;

/* LRU cache entry for escape sequences */
typedef struct esc_lru_entry {
    /* Cache key components */
    uint32_t key_hash; /* Hash of (row, col, attr, fg, bg) */
    int row, col;      /* Position */
    int attr;          /* Attributes */
    short fg, bg;      /* Colors */

    /* Cached sequence */
    char sequence[ESC_SEQ_MAX_LEN];
    int length;

    /* LRU management */
    struct esc_lru_entry *lru_prev;
    struct esc_lru_entry *lru_next;
    struct esc_lru_entry *hash_next;

    /* Usage tracking */
    uint32_t access_count;
    uint64_t last_access;
} esc_lru_entry_t;

/* Performance metrics for optimization monitoring */
static struct {
    uint64_t precomputed_hits;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t total_sequences;
} esc_seq_stats = {0};

/* LRU cache for complete escape sequences */
static struct {
    esc_lru_entry_t entries[ESC_LRU_CACHE_SIZE];
    esc_lru_entry_t *hash_table[ESC_LRU_HASH_SIZE];
    esc_lru_entry_t *lru_head; /* Most recently used */
    esc_lru_entry_t *lru_tail; /* Least recently used */
    int used_entries;
    bool initialized;

    /* Statistics */
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t total_access;
} esc_lru_cache = {0};

/* RLE compression statistics */
static struct {
    uint64_t space_runs_optimized;
    uint64_t space_chars_saved;
    uint64_t char_runs_optimized;
    uint64_t char_repeats_saved;
    uint64_t total_chars_output;
} rle_stats = {0};

/* Attribute combination cache entry */
typedef struct attr_combo_entry {
    short fg;
    short bg;
    int attrs;
    const char *sequence; /* Points to interned sequence */
    int seq_length;
    struct attr_combo_entry *next;
} attr_combo_entry_t;

static struct {
    esc_seq_entry_t *hash_table[ESC_SEQ_HASH_SIZE];
    esc_seq_entry_t *pool;
    int pool_size;
    int pool_used;
    bool initialized;

    /* Attribute combination cache */
    attr_combo_entry_t *attr_combo_table[ATTR_COMBO_CACHE_SIZE];
    attr_combo_entry_t *attr_combo_pool;
    int attr_combo_pool_size;
    int attr_combo_pool_used;

    /* Pre-computed sequence pools */
    struct {
        char cursor_positions[CURSOR_POS_POOL_SIZE][16]; /* "\033[row;colH" */
        int cursor_pos_lengths[CURSOR_POS_POOL_SIZE];
        char basic_colors[16][32]; /* Basic ANSI color sequences */
        int color_lengths[16];
        char attributes[8][16]; /* Common attribute combinations */
        int attr_lengths[8];
        bool initialized;
    } precomputed;
} esc_seq_cache = {
    .initialized = false,
    .pool_size = 0,
    .pool_used = 0,
    .attr_combo_pool_size = 0,
    .attr_combo_pool_used = 0,
};

typedef struct {
    short r, g, b;
} color_def_t;

#define MAX_CUSTOM_COLORS 256
static color_def_t color_defs[MAX_CUSTOM_COLORS] = {
    {0, 0, 0},          /* BLACK */
    {1000, 0, 0},       /* RED */
    {0, 1000, 0},       /* GREEN */
    {1000, 1000, 0},    /* YELLOW */
    {0, 0, 1000},       /* BLUE */
    {1000, 0, 1000},    /* MAGENTA */
    {0, 1000, 1000},    /* CYAN */
    {1000, 1000, 1000}, /* WHITE */
};

static char **screen_buf = NULL, **prev_screen_buf = NULL;
static int **attr_buf = NULL, **prev_attr_buf = NULL;
static int buf_rows = 0, buf_cols = 0;

/* Static buffers for common escape sequences */
static const char ESC_RESET[] = "\x1b[0m";
static const char ESC_CLEAR[] = "\x1b[2J\x1b[H";
static const char ESC_HIDE_CURSOR[] = "\x1b[?25l";
static const char ESC_SHOW_CURSOR[] = "\x1b[?25h";

/* Fast access macros for pre-computed sequences */
#define PRECOMP_RESET (esc_seq_cache.precomputed.attributes[0])
#define PRECOMP_RESET_LEN (esc_seq_cache.precomputed.attr_lengths[0])
#define PRECOMP_BOLD (esc_seq_cache.precomputed.attributes[1])
#define PRECOMP_BOLD_LEN (esc_seq_cache.precomputed.attr_lengths[1])

/* Forward declarations for lazy color allocation */
static void init_color_pair_cache(void);
static void free_color_pair_cache(void);

/* Forward declarations for string interning */
static void init_esc_seq_cache(void);
static void free_esc_seq_cache(void);
static const char *intern_esc_sequence(const char *seq, int len);
static const char *get_cached_attr_sequence(short fg,
                                            short bg,
                                            int attrs,
                                            int *out_len);

/* Forward declarations for LRU escape sequence cache */
static void init_esc_lru_cache(void);
static void free_esc_lru_cache(void);

/* Forward declaration for color utilities */
static void get_rgb_values(short r,
                           short g,
                           short b,
                           int *out_r,
                           int *out_g,
                           int *out_b);

/* Forward declarations for output functions */
static void output_buffered_run(int y,
                                int start_x,
                                int end_x,
                                char **screen_buf,
                                char **prev_screen_buf,
                                int **prev_attr_buf);

/* Fast row-level dirty checking using memcmp */
static inline bool row_has_changes(int y, int start_col, int end_col)
{
    if (y >= buf_rows || start_col >= buf_cols || end_col >= buf_cols)
        return false;

    /* Fast check: compare character data in bulk */
    int row_width = end_col - start_col + 1;
    if (!memcmp(&screen_buf[y][start_col], &prev_screen_buf[y][start_col],
                row_width)) {
        /* If characters are same, check attributes */
        if (!memcmp(&attr_buf[y][start_col], &prev_attr_buf[y][start_col],
                    row_width * sizeof(int))) {
            return false; /* No changes detected */
        }
    }

    return true; /* Changes detected */
}

/* Terminal capability detection constants */
#define DEFAULT_DETECTION_TIMEOUT 100
#define PROBE_RESPONSE_TIMEOUT 50

/* Terminal capability detection helpers */
static struct termios g_orig_termios;
static bool g_termios_saved = false;

/* Common terminal identifiers */
static const struct {
    const char *name;
    const char *pattern;
    bool supports_truecolor;
    bool supports_256_color;
    bool fast_color_changes;
} known_terminals[] = {
    {"xterm-256color", "xterm", true, true, true},
    {"screen-256color", "screen", true, true, false},
    {"tmux-256color", "tmux", true, true, false},
    {"alacritty", "alacritty", true, true, true},
    {"kitty", "kitty", true, true, true},
    {"wezterm", "wezterm", true, true, true},
    {"iterm2", "iterm", true, true, true},
    {"vte", "vte", true, true, true},
    {"konsole", "konsole", true, true, true},
    {"gnome-terminal", "gnome", true, true, true},
    {NULL, NULL, false, false, false},
};

/* Utility functions for terminal capability detection */
static uint64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static void setup_raw_mode(void)
{
    if (g_termios_saved)
        return;

    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
        struct termios raw = g_orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;

        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_termios_saved = true;
    }
}

static void restore_terminal_mode(void)
{
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_termios_saved = false;
    }
}

static bool send_query_and_wait_response(const char *query,
                                         char *response,
                                         size_t response_size,
                                         int timeout_ms)
{
    if (!query || !response)
        return false;

    /* Send query */
    size_t query_len = strlen(query);
    if (write(STDOUT_FILENO, query, query_len) != (ssize_t) query_len)
        return false;

    /* Wait for response */
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return false;

    /* Read response */
    ssize_t bytes_read = read(STDIN_FILENO, response, response_size - 1);
    if (bytes_read > 0) {
        response[bytes_read] = '\0';
        return true;
    }

    return false;
}

static bool detect_truecolor_support(void)
{
    /* Check environment variables first */
    const char *colorterm = getenv("COLORTERM");
    if (colorterm &&
        (strstr(colorterm, "truecolor") || strstr(colorterm, "24bit"))) {
        return true;
    }

    const char *term = getenv("TERM");
    if (term) {
        for (int i = 0; known_terminals[i].name; i++) {
            if (strstr(term, known_terminals[i].pattern))
                return known_terminals[i].supports_truecolor;
        }
    }

    /* Test with actual color query */
    char response[64] = {0};
    if (send_query_and_wait_response("\033]4;1;?\033\\", response,
                                     sizeof(response),
                                     PROBE_RESPONSE_TIMEOUT)) {
        /* Look for RGB response format */
        return strstr(response, "rgb:");
    }

    return false;
}

static bool detect_256_color_support(void)
{
    const char *term = getenv("TERM");
    if (term) {
        if (strstr(term, "256color") || strstr(term, "256"))
            return true;

        for (int i = 0; known_terminals[i].name; i++) {
            if (strstr(term, known_terminals[i].pattern))
                return known_terminals[i].supports_256_color;
        }
    }

    return false;
}

static uint16_t detect_max_colors(bool supports_truecolor,
                                  bool supports_256_color)
{
    if (supports_truecolor)
        return 65535; /* Use max uint16_t for truecolor indicator */
    if (supports_256_color)
        return 256;

    /* Check for basic 16-color support */
    const char *term = getenv("TERM");
    if (term && strncmp(term, "dumb", 4))
        return 16;
    return 8;
}

static void detect_terminal_identification(tui_term_caps_t *caps)
{
    const char *term = getenv("TERM");
    if (term) {
        strncpy(caps->term_name, term, sizeof(caps->term_name) - 1);
        caps->term_name[sizeof(caps->term_name) - 1] = '\0';
    } else {
        strncpy(caps->term_name, "unknown", sizeof(caps->term_name) - 1);
        caps->term_name[sizeof(caps->term_name) - 1] = '\0';
    }

    /* Simple version detection */
    strncpy(caps->term_version, "unknown", sizeof(caps->term_version) - 1);
    caps->term_version[sizeof(caps->term_version) - 1] = '\0';
}

static void detect_perf_caps(tui_term_caps_t *caps)
{
    const char *term = getenv("TERM");
    if (!term)
        return;

    for (int i = 0; known_terminals[i].name; i++) {
        if (strstr(term, known_terminals[i].pattern)) {
            caps->fast_color_changes = known_terminals[i].fast_color_changes;
            break;
        }
    }

    /* GPU acceleration heuristics */
    caps->hw_accel = strstr(term, "alacritty") || strstr(term, "kitty") ||
                     strstr(term, "wezterm") || strstr(term, "gpu");

    /* Fast scrolling for modern terminals */
    caps->fast_scrolling = strstr(term, "xterm") || strstr(term, "alacritty") ||
                           strstr(term, "kitty") || strstr(term, "wezterm");
}

static uint32_t calculate_checksum(const tui_term_caps_t *caps)
{
    /* Simple checksum of capability flags */
    uint32_t checksum = 0;
    const uint8_t *data = (const uint8_t *) caps;

    /* Only checksum the boolean flags */
    for (size_t i = 0; i < sizeof(bool) * 20; i++)
        checksum = checksum * 31 + data[i];

    return checksum;
}

/* Terminal capability detection API */
static int tui_term_caps_init(void)
{
    if (g_caps_initialized)
        return 0;

    memset(&g_terminal_caps, 0, sizeof(g_terminal_caps));
    g_caps_initialized = true;

    return 0;
}

static int tui_term_caps_detect(uint32_t timeout_ms)
{
    uint64_t start_time = get_time_ms();
    uint64_t timeout = timeout_ms ? timeout_ms : DEFAULT_DETECTION_TIMEOUT;

    memset(&g_terminal_caps, 0, sizeof(g_terminal_caps));

    /* Setup raw mode for terminal queries */
    setup_raw_mode();

    /* Detect capabilities in order of importance/speed */
    g_terminal_caps.supports_unicode =
        getenv("LANG") &&
        (strstr(getenv("LANG"), "UTF-8") || strstr(getenv("LANG"), "utf8"));
    g_terminal_caps.supports_256_colors = detect_256_color_support();
    g_terminal_caps.supports_true_color = detect_truecolor_support();
    g_terminal_caps.max_colors =
        detect_max_colors(g_terminal_caps.supports_true_color,
                          g_terminal_caps.supports_256_colors);

    /* Quick checks that don't require terminal queries */
    const char *term = getenv("TERM");
    g_terminal_caps.supports_mouse =
        term && (strstr(term, "xterm") || strstr(term, "screen") ||
                 strstr(term, "tmux"));
    g_terminal_caps.alt_screen = term && strncmp(term, "dumb", 4);
    g_terminal_caps.supports_bracketed_paste = g_terminal_caps.supports_mouse;

    /* Text attributes - assume supported for most terminals */
    bool is_basic_term =
        term && (!strncmp(term, "dumb", 4) || !strncmp(term, "unknown", 7));

    g_terminal_caps.supports_bold = !is_basic_term;
    g_terminal_caps.supports_underline = !is_basic_term;
    g_terminal_caps.supports_reverse = !is_basic_term;
    g_terminal_caps.supports_dim = !is_basic_term;
    g_terminal_caps.supports_blink = !is_basic_term;
    g_terminal_caps.supports_italic = g_terminal_caps.supports_true_color;
    g_terminal_caps.strikethrough = g_terminal_caps.supports_true_color;

    /* Control sequences - most modern terminals support these */
    g_terminal_caps.supports_ech =
        !is_basic_term && g_terminal_caps.supports_256_colors;
    g_terminal_caps.supports_rep = g_terminal_caps.supports_256_colors;

    /* Advanced features */
    g_terminal_caps.supports_wide_chars = g_terminal_caps.supports_unicode;
    g_terminal_caps.supports_cursor_shapes =
        g_terminal_caps.supports_256_colors;
    g_terminal_caps.supports_sixel = false;
    g_terminal_caps.supports_kitty_graphics = term && strstr(term, "kitty");

    /* Terminal identification and performance */
    detect_terminal_identification(&g_terminal_caps);
    detect_perf_caps(&g_terminal_caps);

    /* Get terminal size */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        g_terminal_caps.term_width = ws.ws_col;
        g_terminal_caps.term_height = ws.ws_row;
    } else {
        g_terminal_caps.term_width = 80;
        g_terminal_caps.term_height = 24;
    }

    /* Finalize detection */
    uint64_t end_time = get_time_ms();
    g_terminal_caps.detection_time_ms = end_time - start_time;
    g_terminal_caps.detection_complete =
        (g_terminal_caps.detection_time_ms <= timeout);
    g_terminal_caps.checksum = calculate_checksum(&g_terminal_caps);

    /* Restore terminal mode */
    restore_terminal_mode();

    return 0;
}

static const char *tui_get_cap_sequence(const char *seq_type)
{
    if (!seq_type)
        return NULL;

    /* Return sequences based on terminal capabilities */
    if (!strcmp(seq_type, "clear"))
        return "\033[2J\033[H";
    if (!strcmp(seq_type, "home"))
        return "\033[H";
    if (!strcmp(seq_type, "reset"))
        return "\033[0m";
    if (!strcmp(seq_type, "hide_cursor"))
        return "\033[?25l";
    if (!strcmp(seq_type, "show_cursor"))
        return "\033[?25h";
    if (!strcmp(seq_type, "alt_screen_on") && g_terminal_caps.alt_screen)
        return "\033[?1049h";
    if (!strcmp(seq_type, "alt_screen_off") && g_terminal_caps.alt_screen)
        return "\033[?1049l";

    return NULL;
}

/* Terminal capability caching functions removed - not used */

/* Load terminal capabilities with caching */
static void load_terminal_capabilities(void)
{
    if (g_caps_loaded)
        return;

    /* Initialize capability system */
    tui_term_caps_init();

    /* Detect capabilities directly - caching removed */
    if (tui_term_caps_detect(100) == 0) {
        g_caps_loaded = true;
    }

    /* Configure color mode based on capabilities */
    if (g_terminal_caps.supports_true_color) {
        g_color_mode = COLOR_MODE_TRUECOLOR;
    } else if (g_terminal_caps.supports_256_colors) {
        g_color_mode = COLOR_MODE_256;
    } else if (g_terminal_caps.max_colors >= 16) {
        g_color_mode = COLOR_MODE_16;
    } else {
        g_color_mode = COLOR_MODE_MONO;
    }
}

/* Vectored output buffering functions */
static void adjust_iovecs_after_partial_write(struct iovec *vecs,
                                              int *count,
                                              ssize_t bytes_written)
{
    ssize_t remaining = bytes_written;
    int i = 0;

    while (remaining > 0 && i < *count) {
        if (remaining >= (ssize_t) vecs[i].iov_len) {
            /* This vector was completely written */
            remaining -= vecs[i].iov_len;
            i++;
        } else {
            /* Partial write of this vector */
            vecs[i].iov_base = (char *) vecs[i].iov_base + remaining;
            vecs[i].iov_len -= remaining;
            remaining = 0;
        }
    }

    /* Shift remaining vectors */
    if (i > 0) {
        memmove(&vecs[0], &vecs[i], (*count - i) * sizeof(struct iovec));
        *count -= i;
    }
}

static void tui_flush_vectored(void)
{
    if (writev_buf.count == 0)
        return;

    int vecs_remaining = writev_buf.count;
    struct iovec *current_vecs = writev_buf.vecs;
    ssize_t total_bytes = writev_buf.total_bytes;
    ssize_t written_total = 0;

    /* Track statistics */
    writev_stats.writev_calls++;
    writev_stats.total_vectors += writev_buf.count;
    writev_stats.total_bytes += writev_buf.total_bytes;

    /* Robust partial write handling loop */
    while (written_total < total_bytes && vecs_remaining > 0) {
        ssize_t written = writev(STDOUT_FILENO, current_vecs, vecs_remaining);

        if (written < 0) {
            if (errno == EINTR) /* Interrupted by signal, retry */
                continue;

            /* Unrecoverable error, break and fallback */
            writev_stats.fallback_writes++;
            break;
        }

        if (written == 0) /* No progress made, avoid infinite loop */
            break;

        written_total += written;

        if (written < total_bytes)
            writev_stats.partial_writes++;

        /* Adjust vectors for next iteration */
        adjust_iovecs_after_partial_write(current_vecs, &vecs_remaining,
                                          written);

        /* Update remaining total for accurate loop condition */
        total_bytes -= written;
    }

    /* Reset buffer */
    writev_buf.count = 0;
    writev_buf.total_bytes = 0;
    writev_buf.data_pool_used = 0;
}

static void tui_flush(void)
{
    if (output_buffer.use_writev) {
        tui_flush_vectored();
        return;
    }

    /* Fallback implementation */
    if (output_buffer.len > 0) {
        size_t written = 0;
        while (written < output_buffer.len) {
            ssize_t n = write(STDOUT_FILENO, output_buffer.data + written,
                              output_buffer.len - written);
            if (n > 0) {
                written += n;
            } else if (n < 0 && errno != EINTR) {
                break;
            }
        }
        output_buffer.len = 0;
    }
}

static void tui_write_vectored(const char *data, size_t len)
{
    /* If we're at vector capacity or data pool is full, flush first */
    if (writev_buf.count >= MAX_IOVECS ||
        writev_buf.data_pool_used + len > WRITEV_DATA_POOL_SIZE) {
        tui_flush_vectored();
    }

    /* Copy data into our pool to ensure lifetime */
    char *pool_ptr = writev_buf.data_pool + writev_buf.data_pool_used;
    memcpy(pool_ptr, data, len);

    /* Add to vector array pointing to our pool */
    writev_buf.vecs[writev_buf.count].iov_base = pool_ptr;
    writev_buf.vecs[writev_buf.count].iov_len = len;
    writev_buf.count++;
    writev_buf.total_bytes += len;
    writev_buf.data_pool_used += len;

    /* Auto-flush based on vector count or total bytes */
    if (writev_buf.auto_flush_enabled &&
        (writev_buf.count >= VEC_FLUSH_THRESHOLD ||
         writev_buf.total_bytes >= WRITEV_BUFFER_SIZE)) {
        tui_flush_vectored();
    }
}

static void tui_write(const char *data, size_t len)
{
    if (output_buffer.use_writev) {
        tui_write_vectored(data, len);
        return;
    }

    /* Fallback buffered implementation */
    writev_stats.fallback_writes++;

    if (output_buffer.len + len > OUTPUT_BUFFER_SIZE) {
        tui_flush();

        /* If data is still too large, write directly */
        if (len > OUTPUT_BUFFER_SIZE) {
            size_t written = 0;
            while (written < len) {
                ssize_t n = write(STDOUT_FILENO, data + written, len - written);
                if (n > 0) {
                    written += n;
                } else if (n < 0 && errno != EINTR) {
                    break;
                }
            }
            return;
        }
    }

    /* Buffer the data */
    memcpy(output_buffer.data + output_buffer.len, data, len);
    output_buffer.len += len;

    /* Auto-flush when buffer reaches threshold */
    if (output_buffer.auto_flush_enabled &&
        output_buffer.len >= BUFFER_FLUSH_THRESHOLD) {
        tui_flush();
    }
}

static void tui_puts(const char *str)
{
    tui_write(str, strlen(str));
}

/* Enable/disable auto-flushing for batch operations */
static void tui_set_auto_flush(bool enabled)
{
    if (output_buffer.use_writev) {
        writev_buf.auto_flush_enabled = enabled;
    } else {
        output_buffer.auto_flush_enabled = enabled;
    }
}

/* Force flush and re-enable auto-flush */
static void tui_force_flush(void)
{
    tui_flush();
    if (output_buffer.use_writev) {
        writev_buf.auto_flush_enabled = true;
    } else {
        output_buffer.auto_flush_enabled = true;
    }
}

static void tui_putchar(char c)
{
    tui_write(&c, 1);
}

static void init_cursor_cache(void)
{
    if (cursor_cache.initialized)
        return;

    /* Pre-compute escape sequences for common screen positions */
    for (int row = 0; row < CURSOR_CACHE_ROWS; row++) {
        for (int col = 0; col < CURSOR_CACHE_COLS; col++) {
            cursor_cache.lengths[row][col] =
                snprintf(cursor_cache.sequences[row][col], 16, "\x1b[%d;%dH",
                         row + 1, col + 1);
        }
    }
    cursor_cache.initialized = true;
}

/* Fast cursor position lookup */
static inline int get_cursor_pool_index(int row, int col)
{
    /* Check if position is in pre-computed pool */
    if (row < 2 && col < 40)
        return row * 40 + col;

    /* Check game area positions */
    if (row >= 10 && row < 16 && col < 80 && (col % 5) == 0)
        return 80 + (row - 10) * 16 + (col / 5);
    return -1;
}

static void tui_move_cached(int row, int col)
{
    /* Skip if already at position */
    if (row == cursor_cache.last_row && col == cursor_cache.last_col)
        return;

    /* Use relative movement heuristic for small cursor jumps */
    if (cursor_cache.last_row >= 0 && cursor_cache.last_col >= 0) {
        int row_diff = row - cursor_cache.last_row;
        int col_diff = col - cursor_cache.last_col;
        bool use_relative = false;

        /* Simple heuristic: use relative moves for small deltas */
        if (abs(row_diff) <= 5 && abs(col_diff) <= 5) {
            /* Special optimized cases first */
            if (row_diff == 1 && col == 0) {
                /* Next line start - cheapest possible */
                tui_write("\r\n", 2);
                use_relative = true;
            } else if (row_diff == 0 && col == 0 && cursor_cache.last_col > 0) {
                /* Beginning of current line */
                tui_write("\r", 1);
                use_relative = true;
            } else {
                /* Generate relative moves for small jumps */
                char buf[32];
                int total_len = 0;

                /* Vertical movement first */
                if (row_diff > 0) {
                    if (row_diff == 1) {
                        memcpy(buf + total_len, "\033[B", 3);
                        total_len += 3;
                    } else {
                        int len =
                            snprintf(buf + total_len, sizeof(buf) - total_len,
                                     "\033[%dB", row_diff);
                        total_len += len;
                    }
                } else if (row_diff < 0) {
                    if (row_diff == -1) {
                        memcpy(buf + total_len, "\033[A", 3);
                        total_len += 3;
                    } else {
                        int len =
                            snprintf(buf + total_len, sizeof(buf) - total_len,
                                     "\033[%dA", -row_diff);
                        total_len += len;
                    }
                }

                /* Horizontal movement second */
                if (col_diff > 0) {
                    if (col_diff == 1) {
                        memcpy(buf + total_len, "\033[C", 3);
                        total_len += 3;
                    } else {
                        int len =
                            snprintf(buf + total_len, sizeof(buf) - total_len,
                                     "\033[%dC", col_diff);
                        total_len += len;
                    }
                } else if (col_diff < 0) {
                    if (col_diff == -1) {
                        memcpy(buf + total_len, "\033[D", 3);
                        total_len += 3;
                    } else {
                        int len =
                            snprintf(buf + total_len, sizeof(buf) - total_len,
                                     "\033[%dD", -col_diff);
                        total_len += len;
                    }
                }

                if (total_len > 0) {
                    tui_write(buf, total_len);
                    use_relative = true;
                }
            }
        }

        if (use_relative) {
            cursor_cache.last_row = row;
            cursor_cache.last_col = col;
            /* Count as cache hit since it's optimized */
            esc_seq_stats.cache_hits++;
            esc_seq_stats.total_sequences++;
            return;
        }
    }

    /* First, try pre-computed pool */
    if (esc_seq_cache.precomputed.initialized) {
        int pool_idx = get_cursor_pool_index(row, col);
        if (pool_idx >= 0 && pool_idx < CURSOR_POS_POOL_SIZE) {
            tui_write(esc_seq_cache.precomputed.cursor_positions[pool_idx],
                      esc_seq_cache.precomputed.cursor_pos_lengths[pool_idx]);
            cursor_cache.last_row = row;
            cursor_cache.last_col = col;
            esc_seq_stats.precomputed_hits++;
            esc_seq_stats.total_sequences++;
            return;
        }
    }

    /* Then try runtime cache for positions within cache bounds */
    if (cursor_cache.initialized && row >= 0 && row < CURSOR_CACHE_ROWS &&
        col >= 0 && col < CURSOR_CACHE_COLS) {
        tui_write(cursor_cache.sequences[row][col],
                  cursor_cache.lengths[row][col]);
        esc_seq_stats.cache_hits++;
        esc_seq_stats.total_sequences++;
    } else {
        /* Fall back to dynamic generation and intern the sequence */
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row + 1, col + 1);
        const char *interned = intern_esc_sequence(buf, len);
        tui_write(interned, len);
        esc_seq_stats.cache_misses++;
        esc_seq_stats.total_sequences++;
    }

    /* Update last position */
    cursor_cache.last_row = row;
    cursor_cache.last_col = col;
}

static void get_terminal_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        tui_lines = ws.ws_row;
        tui_cols = ws.ws_col;
    } else {
        tui_lines = 24;
        tui_cols = 80;
    }
}

static void reset_cursor_tracking(void)
{
    cursor_cache.last_row = -1;
    cursor_cache.last_col = -1;
}

/* Initialize sparse dirty tile tracking */
static void init_sparse_dirty_tracking(void)
{
    /* Initialize free list */
    dirty_tile_free_list = NULL;
    for (int i = 0; i < DIRTY_TILE_POOL_SIZE - 1; i++) {
        dirty_tile_pool[i].next = &dirty_tile_pool[i + 1];
    }
    dirty_tile_pool[DIRTY_TILE_POOL_SIZE - 1].next = NULL;
    dirty_tile_free_list = &dirty_tile_pool[0];

    dirty_l1_tiles = dirty_l2_blocks = NULL;
    dirty_tile_pool_used = 0;
    dirty_region.use_sparse_tracking = true;
}

/* Sparse tile allocation */
static dirty_tile_t *alloc_dirty_tile(uint16_t row, uint16_t col)
{
    if (!dirty_tile_free_list || dirty_tile_pool_used >= DIRTY_TILE_POOL_SIZE)
        return NULL;

    dirty_tile_t *tile = dirty_tile_free_list;
    dirty_tile_free_list = tile->next;
    tile->row = row;
    tile->col = col;
    tile->next = NULL;
    dirty_tile_pool_used++;
    return tile;
}

static void free_dirty_tile(dirty_tile_t *tile)
{
    if (!tile)
        return;

    tile->next = dirty_tile_free_list;
    dirty_tile_free_list = tile;
    dirty_tile_pool_used--;
}

/* Fast bitmap-based membership checks for sparse tracking */
static inline void set_l1_tile_bitmap(int tile_row, int tile_col)
{
    if (tile_row >= MAX_L1_TILES_Y || tile_col >= MAX_L1_TILES_X)
        return;
    int bit_index = tile_row * MAX_L1_TILES_X + tile_col;
    int word_index = bit_index / 64;
    int bit_offset = bit_index % 64;
    l1_tile_bitmap[word_index] |= (1ULL << bit_offset);
}

static inline bool check_l1_tile_bitmap(int tile_row, int tile_col)
{
    if (tile_row >= MAX_L1_TILES_Y || tile_col >= MAX_L1_TILES_X)
        return false;
    int bit_index = tile_row * MAX_L1_TILES_X + tile_col;
    int word_index = bit_index / 64;
    int bit_offset = bit_index % 64;
    return (l1_tile_bitmap[word_index] & (1ULL << bit_offset)) != 0;
}

static inline void set_l2_block_bitmap(int block_row, int block_col)
{
    if (block_row >= MAX_L2_BLOCKS_Y || block_col >= MAX_L2_BLOCKS_X)
        return;
    int bit_index = block_row * MAX_L2_BLOCKS_X + block_col;
    int word_index = bit_index / 64;
    int bit_offset = bit_index % 64;
    l2_block_bitmap[word_index] |= (1ULL << bit_offset);
}

static inline bool check_l2_block_bitmap(int block_row, int block_col)
{
    if (block_row >= MAX_L2_BLOCKS_Y || block_col >= MAX_L2_BLOCKS_X)
        return false;
    int bit_index = block_row * MAX_L2_BLOCKS_X + block_col;
    int word_index = bit_index / 64;
    int bit_offset = bit_index % 64;
    return (l2_block_bitmap[word_index] & (1ULL << bit_offset)) != 0;
}

static void reset_sparse_dirty_tracking(void)
{
    /* Return all tiles to free list */
    dirty_tile_t *current = dirty_l1_tiles;
    while (current) {
        dirty_tile_t *next = current->next;
        free_dirty_tile(current);
        current = next;
    }

    current = dirty_l2_blocks;
    while (current) {
        dirty_tile_t *next = current->next;
        free_dirty_tile(current);
        current = next;
    }

    dirty_l1_tiles = dirty_l2_blocks = NULL;

    /* Clear bitmaps for fast membership checks */
    memset(l1_tile_bitmap, 0, sizeof(l1_tile_bitmap));
    memset(l2_block_bitmap, 0, sizeof(l2_block_bitmap));
}

/* Initialize hierarchical tile system */
static void init_hierarchical_dirty_tracking(int screen_cols, int screen_rows)
{
    /* Calculate Level 1 tile dimensions (8x8 character tiles) */
    dirty_region.l1_tiles_x = (screen_cols + TILE_L1_SIZE - 1) / TILE_L1_SIZE;
    dirty_region.l1_tiles_y = (screen_rows + TILE_L1_SIZE - 1) / TILE_L1_SIZE;

    /* Calculate Level 2 block dimensions (32x32 character blocks) */
    dirty_region.l2_blocks_x = (screen_cols + TILE_L2_SIZE - 1) / TILE_L2_SIZE;
    dirty_region.l2_blocks_y = (screen_rows + TILE_L2_SIZE - 1) / TILE_L2_SIZE;

    /* Enable hierarchical tracking if dimensions fit within limits */
    if (dirty_region.l1_tiles_x <= MAX_L1_TILES_X &&
        dirty_region.l1_tiles_y <= MAX_L1_TILES_Y &&
        dirty_region.l2_blocks_x <= MAX_L2_BLOCKS_X &&
        dirty_region.l2_blocks_y <= MAX_L2_BLOCKS_Y) {
        dirty_region.use_hierarchical_tiles = true;

        /* Clear both levels */
        memset(dirty_region.l1_tiles, 0, sizeof(dirty_region.l1_tiles));
        memset(dirty_region.l2_blocks, 0, sizeof(dirty_region.l2_blocks));

        /* Reset statistics */
        dirty_region.l1_scans_avoided = 0;
        dirty_region.l2_scans_avoided = 0;
        dirty_region.total_scans = 0;
    }
}

/* Add sparse tile to linked list if not already present */
static void add_sparse_l1_tile(int tile_row, int tile_col)
{
    /* Fast bitmap check for membership */
    if (check_l1_tile_bitmap(tile_row, tile_col))
        return; /* Already tracked */

    /* Add new tile to sparse list */
    dirty_tile_t *new_tile = alloc_dirty_tile(tile_row, tile_col);
    if (new_tile) {
        new_tile->next = dirty_l1_tiles;
        dirty_l1_tiles = new_tile;
        set_l1_tile_bitmap(tile_row, tile_col);
    }
}

static void add_sparse_l2_block(int block_row, int block_col)
{
    /* Fast bitmap check for membership */
    if (check_l2_block_bitmap(block_row, block_col))
        return; /* Already tracked */

    /* Add new block to sparse list */
    dirty_tile_t *new_block = alloc_dirty_tile(block_row, block_col);
    if (new_block) {
        new_block->next = dirty_l2_blocks;
        dirty_l2_blocks = new_block;
        set_l2_block_bitmap(block_row, block_col);
    }
}

static void mark_dirty(int row, int col)
{
    if (row < dirty_region.min_row)
        dirty_region.min_row = row;
    if (row > dirty_region.max_row)
        dirty_region.max_row = row;
    if (col < dirty_region.min_col)
        dirty_region.min_col = col;
    if (col > dirty_region.max_col)
        dirty_region.max_col = col;
    dirty_region.has_changes = true;

    /* Update hierarchical tile system */
    if (dirty_region.use_hierarchical_tiles) {
        /* Level 1: Mark 8x8 tile as dirty */
        int l1_tile_row = row / TILE_L1_SIZE;
        int l1_tile_col = col / TILE_L1_SIZE;
        if (l1_tile_row < dirty_region.l1_tiles_y &&
            l1_tile_col < dirty_region.l1_tiles_x) {
            dirty_region.l1_tiles[l1_tile_row][l1_tile_col] = true;

            /* Also add to sparse tracking if enabled */
            if (dirty_region.use_sparse_tracking)
                add_sparse_l1_tile(l1_tile_row, l1_tile_col);
        }

        /* Level 2: Mark 32x32 block as dirty */
        int l2_block_row = row / TILE_L2_SIZE;
        int l2_block_col = col / TILE_L2_SIZE;
        if (l2_block_row < dirty_region.l2_blocks_y &&
            l2_block_col < dirty_region.l2_blocks_x) {
            dirty_region.l2_blocks[l2_block_row][l2_block_col] = true;

            /* Also add to sparse tracking if enabled */
            if (dirty_region.use_sparse_tracking)
                add_sparse_l2_block(l2_block_row, l2_block_col);
        }
    }
}

static void mark_dirty_region(int row1, int col1, int row2, int col2)
{
    if (row1 < dirty_region.min_row)
        dirty_region.min_row = row1;
    if (row2 > dirty_region.max_row)
        dirty_region.max_row = row2;
    if (col1 < dirty_region.min_col)
        dirty_region.min_col = col1;
    if (col2 > dirty_region.max_col)
        dirty_region.max_col = col2;
    dirty_region.has_changes = true;

    /* Update hierarchical tiles for rectangular regions */
    if (dirty_region.use_hierarchical_tiles) {
        /* Level 1: Mark all 8x8 tiles in the region as dirty */
        int l1_tile_row1 = row1 / TILE_L1_SIZE;
        int l1_tile_col1 = col1 / TILE_L1_SIZE;
        int l1_tile_row2 = row2 / TILE_L1_SIZE;
        int l1_tile_col2 = col2 / TILE_L1_SIZE;

        for (int tr = l1_tile_row1;
             tr <= l1_tile_row2 && tr < dirty_region.l1_tiles_y; tr++) {
            for (int tc = l1_tile_col1;
                 tc <= l1_tile_col2 && tc < dirty_region.l1_tiles_x; tc++) {
                dirty_region.l1_tiles[tr][tc] = true;

                /* Also add to sparse tracking if enabled */
                if (dirty_region.use_sparse_tracking) {
                    add_sparse_l1_tile(tr, tc);
                }
            }
        }

        /* Level 2: Mark all 32x32 blocks in the region as dirty */
        int l2_block_row1 = row1 / TILE_L2_SIZE;
        int l2_block_col1 = col1 / TILE_L2_SIZE;
        int l2_block_row2 = row2 / TILE_L2_SIZE;
        int l2_block_col2 = col2 / TILE_L2_SIZE;

        for (int br = l2_block_row1;
             br <= l2_block_row2 && br < dirty_region.l2_blocks_y; br++) {
            for (int bc = l2_block_col1;
                 bc <= l2_block_col2 && bc < dirty_region.l2_blocks_x; bc++) {
                dirty_region.l2_blocks[br][bc] = true;

                /* Also add to sparse tracking if enabled */
                if (dirty_region.use_sparse_tracking)
                    add_sparse_l2_block(br, bc);
            }
        }
    }
}

static void reset_dirty_region(void)
{
    dirty_region.min_row = INT_MAX;
    dirty_region.max_row = -1;
    dirty_region.min_col = INT_MAX;
    dirty_region.max_col = -1;
    dirty_region.has_changes = false;

    /* Reset hierarchical tile system */
    if (dirty_region.use_hierarchical_tiles) {
        memset(dirty_region.l1_tiles, 0, sizeof(dirty_region.l1_tiles));
        memset(dirty_region.l2_blocks, 0, sizeof(dirty_region.l2_blocks));
    }

    /* Reset sparse tracking */
    if (dirty_region.use_sparse_tracking) {
        reset_sparse_dirty_tracking();
    }
}

/* Helper functions for hierarchical scanning */
static inline bool has_l1_tile_changes(int tile_row, int tile_col)
{
    if (!dirty_region.use_hierarchical_tiles)
        return true; /* Fall back to full scanning */

    return (tile_row < dirty_region.l1_tiles_y &&
            tile_col < dirty_region.l1_tiles_x &&
            dirty_region.l1_tiles[tile_row][tile_col]);
}

static inline bool has_l2_block_changes(int block_row, int block_col)
{
    if (!dirty_region.use_hierarchical_tiles)
        return true; /* Fall back to full scanning */

    return (block_row < dirty_region.l2_blocks_y &&
            block_col < dirty_region.l2_blocks_x &&
            dirty_region.l2_blocks[block_row][block_col]);
}

/* Future optimization: quadtree-like region subdivision for very sparse updates
 */
#ifdef ENABLE_QUADTREE_SCANNING
static bool scan_region_recursive(int min_row,
                                  int max_row,
                                  int min_col,
                                  int max_col,
                                  int depth)
{
    /* Base case: if region is small enough, do direct scanning */
    if (depth > 3 || (max_row - min_row < 4 && max_col - min_col < 4)) {
        for (int y = min_row; y <= max_row; y++) {
            if (row_has_changes(y, min_col, max_col))
                return true;
        }
        return false;
    }

    /* Divide region into 4 quadrants and recursively check */
    int mid_row = (min_row + max_row) / 2;
    int mid_col = (min_col + max_col) / 2;

    bool has_changes = false;

    /* Check quadrants in order that's most likely to find changes first */
    /* Top-left quadrant */
    if (scan_region_recursive(min_row, mid_row, min_col, mid_col, depth + 1))
        has_changes = true;

    /* Top-right quadrant */
    if (scan_region_recursive(min_row, mid_row, mid_col + 1, max_col,
                              depth + 1)) {
        has_changes = true;
    }

    /* Bottom-left quadrant */
    if (scan_region_recursive(mid_row + 1, max_row, min_col, mid_col,
                              depth + 1)) {
        has_changes = true;
    }

    /* Bottom-right quadrant */
    if (scan_region_recursive(mid_row + 1, max_row, mid_col + 1, max_col,
                              depth + 1)) {
        has_changes = true;
    }

    return has_changes;
}
#endif

/* Scan a single L1 tile (8x8 characters) for changes */
static void scan_l1_tile(int tile_row,
                         int tile_col,
                         int buf_rows,
                         int buf_cols,
                         int scan_min_row,
                         int scan_max_row,
                         int scan_min_col,
                         int scan_max_col,
                         bool *has_changes)
{
    int start_row = tile_row * TILE_L1_SIZE;
    int end_row = (tile_row + 1) * TILE_L1_SIZE;
    int start_col = tile_col * TILE_L1_SIZE;
    int end_col = (tile_col + 1) * TILE_L1_SIZE;

    /* Clamp to buffer bounds */
    if (end_row > buf_rows)
        end_row = buf_rows;
    if (end_col > buf_cols)
        end_col = buf_cols;
    if (end_row > tui_lines)
        end_row = tui_lines;
    if (end_col > tui_cols)
        end_col = tui_cols;

    /* Clamp to scan region */
    if (start_row < scan_min_row)
        start_row = scan_min_row;
    if (end_row > scan_max_row + 1)
        end_row = scan_max_row + 1;
    if (start_col < scan_min_col)
        start_col = scan_min_col;
    if (end_col > scan_max_col + 1)
        end_col = scan_max_col + 1;

    for (int y = start_row; y < end_row; y++) {
        int x = start_col;
        while (x < end_col) {
            /* Check if cell has changed */
            if (screen_buf[y][x] != prev_screen_buf[y][x] ||
                attr_buf[y][x] != prev_attr_buf[y][x]) {
                *has_changes = true;

                /* Find the start of changed region */
                int start_x = x;
                int curr_attr = attr_buf[y][x];

                /* Find end of run with same attributes that have changed */
                int end_x = x;
                while (
                    end_x + 1 < end_col && end_x + 1 <= scan_max_col &&
                    attr_buf[y][end_x + 1] == curr_attr &&
                    (screen_buf[y][end_x + 1] !=
                         prev_screen_buf[y][end_x + 1] ||
                     attr_buf[y][end_x + 1] != prev_attr_buf[y][end_x + 1])) {
                    end_x++;
                }

                /* Move to start of changed run */
                tui_move_cached(y, start_x);

                /* Apply attributes for this run */
                apply_attributes(curr_attr);

                /* Output all changed characters in the run with RLE
                 * optimization */
                output_buffered_run(y, start_x, end_x, screen_buf,
                                    prev_screen_buf, prev_attr_buf);

                /* Update cached position after run */
                cursor_cache.last_col = end_x + 1;

                /* Move to next position */
                x = end_x + 1;
            } else {
                /* Cell unchanged, skip to next */
                x++;
            }
        }
    }
}

/* Column-level dirty detection */
static inline bool col_has_changes(int x, int start_row, int end_row)
{
    if (x >= buf_cols || start_row >= buf_rows || end_row >= buf_rows)
        return false;

    for (int y = start_row; y <= end_row; y++) {
        if (screen_buf[y][x] != prev_screen_buf[y][x] ||
            attr_buf[y][x] != prev_attr_buf[y][x]) {
            return true;
        }
    }
    return false;
}

/* Enhanced dirty region detection with tighter bounding box */
static void optimize_dirty_region(void)
{
    if (!dirty_region.has_changes)
        return;

    int original_min_row = dirty_region.min_row;
    int original_max_row = dirty_region.max_row;
    int original_min_col = dirty_region.min_col;
    int original_max_col = dirty_region.max_col;

    /* Scan from top to find first changed row - using memcmp optimization */
    dirty_region.min_row = INT_MAX;
    for (int y = original_min_row; y <= original_max_row && y < buf_rows; y++) {
        if (row_has_changes(y, original_min_col,
                            (original_max_col < buf_cols) ? original_max_col
                                                          : buf_cols - 1)) {
            dirty_region.min_row = y;
            break;
        }
    }

    /* Scan from bottom to find last changed row */
    dirty_region.max_row = -1;
    for (int y = original_max_row; y >= dirty_region.min_row && y >= 0; y--) {
        if (row_has_changes(y, original_min_col,
                            (original_max_col < buf_cols) ? original_max_col
                                                          : buf_cols - 1)) {
            dirty_region.max_row = y;
            break;
        }
    }

    /* If no changes found, mark region as clean */
    if (dirty_region.min_row == INT_MAX || dirty_region.max_row == -1) {
        dirty_region.has_changes = false;
        return;
    }

    /* Scan from left to find first changed column */
    dirty_region.min_col = INT_MAX;
    for (int x = original_min_col; x <= original_max_col && x < buf_cols; x++) {
        if (col_has_changes(x, dirty_region.min_row, dirty_region.max_row)) {
            dirty_region.min_col = x;
            break;
        }
    }

    /* Scan from right to find last changed column */
    dirty_region.max_col = -1;
    for (int x = original_max_col; x >= dirty_region.min_col && x >= 0; x--) {
        if (col_has_changes(x, dirty_region.min_row, dirty_region.max_row)) {
            dirty_region.max_col = x;
            break;
        }
    }

    /* Final validation */
    if (dirty_region.min_col == INT_MAX || dirty_region.max_col == -1)
        dirty_region.has_changes = false;
}

static void reset_attr_state(void)
{
    attr_state.last_fg = -1;
    attr_state.last_bg = -1;
    attr_state.last_attrs = -1;
    attr_state.initialized = false;
}

static void restore_terminal(void)
{
    if (term_initialized) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        if (cursor_visibility == 0)
            tui_puts(ESC_SHOW_CURSOR);
        tui_puts(ESC_RESET);
        tui_flush();
        term_initialized = 0;
    }
}

static void handle_signal(int sig)
{
    restore_terminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void handle_resize(int sig)
{
    (void) sig;
    get_terminal_size();
    if (tui_stdscr) {
        tui_stdscr->maxy = tui_lines;
        tui_stdscr->maxx = tui_cols;
    }
}

static int setup_terminal(void)
{
    if (term_initialized)
        return 0;

    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1)
        return -1;

    orig_termios = saved_termios;
    orig_termios.c_lflag &= ~(ECHO | ICANON);
    orig_termios.c_cc[VMIN] = 1;
    orig_termios.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios) == -1)
        return -1;

    term_initialized = 1;
    atexit(restore_terminal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGWINCH, handle_resize);

    return 0;
}

static void free_buffers(void)
{
    if (screen_buf) {
        for (int i = 0; i < buf_rows; i++)
            free(screen_buf[i]);
        free(screen_buf);
        screen_buf = NULL;
    }
    if (attr_buf) {
        for (int i = 0; i < buf_rows; i++)
            free(attr_buf[i]);
        free(attr_buf);
        attr_buf = NULL;
    }
    if (prev_screen_buf) {
        for (int i = 0; i < buf_rows; i++)
            free(prev_screen_buf[i]);
        free(prev_screen_buf);
        prev_screen_buf = NULL;
    }
    if (prev_attr_buf) {
        for (int i = 0; i < buf_rows; i++)
            free(prev_attr_buf[i]);
        free(prev_attr_buf);
        prev_attr_buf = NULL;
    }
    buf_rows = 0;
    buf_cols = 0;
}

static int allocate_buffers(void)
{
    free_buffers();

    buf_rows = tui_lines;
    buf_cols = tui_cols;

    screen_buf = calloc(buf_rows, sizeof(char *));
    attr_buf = calloc(buf_rows, sizeof(int *));
    prev_screen_buf = calloc(buf_rows, sizeof(char *));
    prev_attr_buf = calloc(buf_rows, sizeof(int *));

    if (!screen_buf || !attr_buf || !prev_screen_buf || !prev_attr_buf) {
        free_buffers();
        return -1;
    }

    for (int i = 0; i < buf_rows; i++) {
        screen_buf[i] = calloc(buf_cols + 1, sizeof(char));
        attr_buf[i] = calloc(buf_cols, sizeof(int));
        prev_screen_buf[i] = calloc(buf_cols + 1, sizeof(char));
        prev_attr_buf[i] = calloc(buf_cols, sizeof(int));
        if (!screen_buf[i] || !attr_buf[i] || !prev_screen_buf[i] ||
            !prev_attr_buf[i]) {
            free_buffers();
            return -1;
        }
        memset(screen_buf[i], ' ', buf_cols);
        memset(prev_screen_buf[i], '\0',
               buf_cols); /* Initialize to different state */
        memset(prev_attr_buf[i], 0xFF,
               buf_cols * sizeof(int)); /* Initialize to invalid attrs */
    }

    return 0;
}

/* Test if writev is available and functional */
static void detect_writev_support(void)
{
    /* Test writev with dummy data */
    struct iovec test_vec[2];
    char test1[] = "";
    char test2[] = "";

    test_vec[0].iov_base = test1;
    test_vec[0].iov_len = 0;
    test_vec[1].iov_base = test2;
    test_vec[1].iov_len = 0;

    ssize_t result = writev(STDOUT_FILENO, test_vec, 2);

    /* writev should return 0 for empty vectors, or work normally */
    if (result >= 0) {
        /* Enable writev by default - significant performance benefit */
        output_buffer.use_writev = true;

        /* Allow explicit disable for compatibility */
        if (getenv("TUI_DISABLE_WRITEV")) {
            output_buffer.use_writev = false;
            fprintf(stderr, "writev disabled via TUI_DISABLE_WRITEV\n");
        }
    } else {
        output_buffer.use_writev = false;
        fprintf(stderr, "writev not supported, using fallback buffering\n");
    }
}

tui_window_t *tui_init(void)
{
    if (tui_stdscr)
        return tui_stdscr;

    /* Load terminal capabilities with caching */
    load_terminal_capabilities();

    /* Test writev support */
    detect_writev_support();

    get_terminal_size();

    if (setup_terminal() == -1)
        return NULL;

    tui_stdscr = calloc(1, sizeof(tui_window_t));
    if (!tui_stdscr)
        return NULL;

    tui_stdscr->begy = 0;
    tui_stdscr->begx = 0;
    tui_stdscr->maxy = tui_lines;
    tui_stdscr->maxx = tui_cols;
    tui_stdscr->cury = 0;
    tui_stdscr->curx = 0;
    tui_stdscr->keypad_mode = 0;
    tui_stdscr->delay = -1;
    tui_stdscr->attr = TUI_A_NORMAL;
    tui_stdscr->bkgd = TUI_A_NORMAL;

    if (allocate_buffers() == -1) {
        free(tui_stdscr);
        tui_stdscr = NULL;
        return NULL;
    }

    /* Initialize cursor cache for performance */
    init_cursor_cache();

    /* Initialize hierarchical dirty region tracking */
    init_hierarchical_dirty_tracking(tui_cols, tui_lines);

    /* Initialize sparse dirty tracking */
    init_sparse_dirty_tracking();

    /* Initialize lazy color pair allocation */
    init_color_pair_cache();

    /* Initialize escape sequence interning */
    init_esc_seq_cache();

    /* Initialize LRU escape sequence cache */
    init_esc_lru_cache();

    /* Use alternate screen if supported */
    if (g_terminal_caps.alt_screen) {
        const char *alt_screen_on = tui_get_cap_sequence("alt_screen_on");
        if (alt_screen_on)
            tui_puts(alt_screen_on);
    }

    /* Clear screen */
    tui_puts(ESC_CLEAR);
    tui_flush();

    return tui_stdscr;
}

/* Debug functions removed - not used in production */

int tui_cleanup(void)
{
    if (!tui_stdscr)
        return -1;

    free_buffers();
    free_color_pair_cache();
    free_esc_seq_cache();
    free_esc_lru_cache();
    free(tui_stdscr);
    tui_stdscr = NULL;

    /* Reset colors and clear screen */
    tui_puts(ESC_RESET);
    tui_puts(ESC_CLEAR);
    if (cursor_visibility == 0)
        tui_puts(ESC_SHOW_CURSOR);

    /* Exit alternate screen if we used it */
    if (g_terminal_caps.alt_screen) {
        const char *alt_screen_off = tui_get_cap_sequence("alt_screen_off");
        if (alt_screen_off)
            tui_puts(alt_screen_off);
    }

    tui_flush();

    /* Cleanup capability system - removed as caching functions are unused */

    restore_terminal();
    return 0;
}

int tui_start_color(void)
{
    colors_initialized = 1;

    /* Initialize all color pairs to safe defaults */
    for (int i = 0; i < TUI_COLOR_PAIRS; i++) {
        color_pairs[i].fg = TUI_COLOR_WHITE;
        color_pairs[i].bg = TUI_COLOR_BLACK;
    }

    /* Don't redefine basic colors - they should keep their standard meanings */
    /* TUI_COLOR_BLACK should be black (0,0,0), not gray */
    /* Only define custom colors for indices >= 8 */

    return 0;
}

/* Lazy color pair allocation functions */

/* Pack foreground and background into 16-bit value */
static inline uint16_t pack_fg_bg(short fg, short bg)
{
    return ((uint16_t) (fg & 0xFF) << 8) | (bg & 0xFF);
}

/* Hash function for color pairs */
static inline unsigned int hash_color_pair(uint16_t fg_bg)
{
    /* FNV-1a hash for better distribution */
    uint32_t hash = 2166136261U;
    uint8_t const *bytes = (uint8_t const *) &fg_bg;
    for (int i = 0; i < 2; i++) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash % COLOR_PAIR_HASH_SIZE;
}

/* Initialize color pair cache */
/* Initialize common color pairs cache with game-specific pairs */
static void init_common_pairs_cache(void)
{
    /* Pre-define common color combinations used in the T-Rex game */
    struct {
        short fg, bg;
    } common_colors[] = {
        {TUI_COLOR_WHITE, TUI_COLOR_BLACK},   /* Default text */
        {TUI_COLOR_GREEN, TUI_COLOR_BLACK},   /* T-Rex, power-ups */
        {TUI_COLOR_RED, TUI_COLOR_BLACK},     /* Obstacles, fire */
        {TUI_COLOR_YELLOW, TUI_COLOR_BLACK},  /* Score, special items */
        {TUI_COLOR_BLUE, TUI_COLOR_BLACK},    /* Sky elements */
        {TUI_COLOR_CYAN, TUI_COLOR_BLACK},    /* Water, ice */
        {TUI_COLOR_MAGENTA, TUI_COLOR_BLACK}, /* Special effects */
        {TUI_COLOR_WHITE, TUI_COLOR_RED},     /* Danger indicators */
        {TUI_COLOR_BLACK, TUI_COLOR_WHITE},   /* Inverted text */
        {TUI_COLOR_BLACK, TUI_COLOR_GREEN},   /* Background elements */
    };

    color_pair_cache.common_pairs_count = 0;
    for (int i = 0; i < 10 && i < COMMON_PAIRS_CACHE_SIZE; i++) {
        color_pair_cache.common_pairs[i].fg = common_colors[i].fg;
        color_pair_cache.common_pairs[i].bg = common_colors[i].bg;
        /* Reserve 0 for default */
        color_pair_cache.common_pairs[i].pair_num = i + 1;
        color_pair_cache.common_pairs[i].usage_count = 0;
        color_pair_cache.common_pairs_count++;
    }
}

static void init_color_pair_cache(void)
{
    /* Clear hash table */
    memset(color_pair_cache.table, 0, sizeof(color_pair_cache.table));

    /* Allocate node pool */
    color_pair_cache.node_count = TUI_COLOR_PAIRS;
    color_pair_cache.nodes =
        calloc(color_pair_cache.node_count, sizeof(color_pair_node_t));
    color_pair_cache.node_used = 0;
    color_pair_cache.next_pair = 1;
    color_pair_cache.allocated_count = 0;

    /* Initialize statistics */
    color_pair_cache.cache_hits = 0;
    color_pair_cache.cache_misses = 0;
    color_pair_cache.hash_collisions = 0;

    /* Initialize common pairs cache */
    init_common_pairs_cache();
}

/* Free color pair cache */
static void free_color_pair_cache(void)
{
    if (color_pair_cache.nodes) {
        free(color_pair_cache.nodes);
        color_pair_cache.nodes = NULL;
    }
    memset(color_pair_cache.table, 0, sizeof(color_pair_cache.table));
    color_pair_cache.node_count = 0;
    color_pair_cache.node_used = 0;
    color_pair_cache.allocated_count = 0;
}

/* Get or allocate a color pair */
static short get_or_alloc_pair(short fg, short bg)
{
    /* Handle default case */
    if (fg == TUI_COLOR_WHITE && bg == TUI_COLOR_BLACK) {
        color_pair_cache.cache_hits++;
        return 0; /* Default pair */
    }

    /* First, check common pairs cache for fast lookup */
    for (int i = 0; i < color_pair_cache.common_pairs_count; i++) {
        if (color_pair_cache.common_pairs[i].fg == fg &&
            color_pair_cache.common_pairs[i].bg == bg) {
            color_pair_cache.common_pairs[i].usage_count++;
            color_pair_cache.cache_hits++;
            return color_pair_cache.common_pairs[i].pair_num;
        }
    }

    uint16_t fg_bg = pack_fg_bg(fg, bg);
    unsigned int hash = hash_color_pair(fg_bg);

    /* Search in hash table */
    color_pair_node_t *node = color_pair_cache.table[hash];
    int collision_count = 0;
    while (node) {
        if (node->fg_bg == fg_bg) {
            color_pair_cache.cache_hits++;
            return node->pair_num; /* Found existing pair */
        }
        node = node->next;
        collision_count++;
    }

    /* Count hash collisions for statistics */
    if (collision_count > 0)
        color_pair_cache.hash_collisions += collision_count;

    /* Not found, allocate new pair */
    color_pair_cache.cache_misses++;

    if (color_pair_cache.next_pair >= TUI_COLOR_PAIRS ||
        color_pair_cache.node_used >= color_pair_cache.node_count) {
        /* Out of pairs or nodes, return default */
        return 0;
    }

    /* Get new node from pool */
    node = &color_pair_cache.nodes[color_pair_cache.node_used++];
    node->fg_bg = fg_bg;
    node->fg = fg;
    node->bg = bg;
    node->pair_num = color_pair_cache.next_pair++;

    /* Add to hash table */
    node->next = color_pair_cache.table[hash];
    color_pair_cache.table[hash] = node;

    /* Also update legacy array for compatibility */
    if (node->pair_num < TUI_COLOR_PAIRS) {
        color_pairs[node->pair_num].fg = fg;
        color_pairs[node->pair_num].bg = bg;
    }

    color_pair_cache.allocated_count++;

    return node->pair_num;
}


/* Get color values from a pair number */
static bool get_pair_colors(short pair, short *fg, short *bg)
{
    if (pair == 0) {
        /* Default pair */
        *fg = TUI_COLOR_WHITE;
        *bg = TUI_COLOR_BLACK;
        return true;
    }

    /* Check legacy array first for compatibility */
    if (pair > 0 && pair < TUI_COLOR_PAIRS) {
        *fg = color_pairs[pair].fg;
        *bg = color_pairs[pair].bg;
        return true;
    }

    /* Then search in lazy allocation cache */
    for (int i = 0; i < color_pair_cache.node_used; i++) {
        if (color_pair_cache.nodes[i].pair_num == pair) {
            *fg = color_pair_cache.nodes[i].fg;
            *bg = color_pair_cache.nodes[i].bg;
            return true;
        }
    }

    /* Not found */
    *fg = TUI_COLOR_WHITE;
    *bg = TUI_COLOR_BLACK;
    return false;
}

int tui_init_pair(short pair, short fg, short bg)
{
    if (pair < 0 || pair >= TUI_COLOR_PAIRS)
        return -1;

    /* For compatibility, always update the legacy array */
    color_pairs[pair].fg = fg;
    color_pairs[pair].bg = bg;

    /* Also ensure it's in the lazy allocation cache for consistency */
    if (pair > 0 && pair < 10) {
        /* Pre-allocate commonly used pairs */
        get_or_alloc_pair(fg, bg);
    }

    return 0;
}

/* String interning functions */

/* Simple hash function for strings */
static uint32_t hash_string(const char *str, int len)
{
    uint32_t hash = 5381;
    for (int i = 0; i < len; i++)
        hash = ((hash << 5) + hash) + (unsigned char) str[i];
    return hash;
}

/* Initialize escape sequence cache */
/* Initialize pre-computed escape sequences */
static void init_precomputed_sequences(void)
{
    if (esc_seq_cache.precomputed.initialized)
        return;

    /* Pre-compute cursor positions for common screen locations */
    int idx = 0;
    /* First 80 positions: top two rows (0,0) to (1,39) */
    for (int row = 0; row < 2 && idx < CURSOR_POS_POOL_SIZE; row++) {
        for (int col = 0; col < 40 && idx < CURSOR_POS_POOL_SIZE; col++) {
            int len = snprintf(esc_seq_cache.precomputed.cursor_positions[idx],
                               16, "\033[%d;%dH", row + 1, col + 1);
            esc_seq_cache.precomputed.cursor_pos_lengths[idx] = len;
            idx++;
        }
    }

    /* Next 80 positions: common game area (10-15, 0-79) */
    for (int row = 10; row < 16 && idx < CURSOR_POS_POOL_SIZE; row++) {
        for (int col = 0; col < 80 && idx < CURSOR_POS_POOL_SIZE; col += 5) {
            int len = snprintf(esc_seq_cache.precomputed.cursor_positions[idx],
                               16, "\033[%d;%dH", row + 1, col + 1);
            esc_seq_cache.precomputed.cursor_pos_lengths[idx] = len;
            idx++;
        }
    }

    /* Pre-compute basic ANSI color sequences */
    for (int i = 0; i < 8; i++) {
        /* Foreground colors */
        int len = snprintf(esc_seq_cache.precomputed.basic_colors[i], 32,
                           "\033[3%dm", i);
        esc_seq_cache.precomputed.color_lengths[i] = len;

        /* Background colors */
        len = snprintf(esc_seq_cache.precomputed.basic_colors[i + 8], 32,
                       "\033[4%dm", i);
        esc_seq_cache.precomputed.color_lengths[i + 8] = len;
    }

    /* Pre-compute common attribute sequences */
    const char *attr_seqs[] = {
        "\033[0m",   /* Reset */
        "\033[1m",   /* Bold */
        "\033[2m",   /* Dim */
        "\033[4m",   /* Underline */
        "\033[5m",   /* Blink */
        "\033[7m",   /* Reverse */
        "\033[0;1m", /* Reset + Bold */
        "\033[0;7m", /* Reset + Reverse */
    };

    for (int i = 0; i < 8; i++) {
        strcpy(esc_seq_cache.precomputed.attributes[i], attr_seqs[i]);
        esc_seq_cache.precomputed.attr_lengths[i] = strlen(attr_seqs[i]);
    }

    esc_seq_cache.precomputed.initialized = true;
}

static void init_esc_seq_cache(void)
{
    if (esc_seq_cache.initialized)
        return;

    /* Clear hash tables */
    memset(esc_seq_cache.hash_table, 0, sizeof(esc_seq_cache.hash_table));
    memset(esc_seq_cache.attr_combo_table, 0,
           sizeof(esc_seq_cache.attr_combo_table));

    /* Allocate escape sequence pool */
    esc_seq_cache.pool_size = ESC_SEQ_POOL_SIZE;
    esc_seq_cache.pool =
        calloc(esc_seq_cache.pool_size, sizeof(esc_seq_entry_t));
    esc_seq_cache.pool_used = 0;

    /* Allocate attribute combination pool */
    esc_seq_cache.attr_combo_pool_size = ATTR_COMBO_CACHE_SIZE;
    esc_seq_cache.attr_combo_pool =
        calloc(esc_seq_cache.attr_combo_pool_size, sizeof(attr_combo_entry_t));
    esc_seq_cache.attr_combo_pool_used = 0;

    esc_seq_cache.initialized = true;

    /* Initialize pre-computed sequences */
    init_precomputed_sequences();

    /* Pre-cache common sequences */
    intern_esc_sequence("\x1b[0m", 4);  /* Reset */
    intern_esc_sequence("\x1b[1m", 4);  /* Bold */
    intern_esc_sequence("\x1b[7m", 4);  /* Reverse */
    intern_esc_sequence("\x1b[30m", 5); /* Black fg */
    intern_esc_sequence("\x1b[31m", 5); /* Red fg */
    intern_esc_sequence("\x1b[32m", 5); /* Green fg */
    intern_esc_sequence("\x1b[33m", 5); /* Yellow fg */
    intern_esc_sequence("\x1b[34m", 5); /* Blue fg */
    intern_esc_sequence("\x1b[35m", 5); /* Magenta fg */
    intern_esc_sequence("\x1b[36m", 5); /* Cyan fg */
    intern_esc_sequence("\x1b[37m", 5); /* White fg */
    intern_esc_sequence("\x1b[40m", 5); /* Black bg */
    intern_esc_sequence("\x1b[41m", 5); /* Red bg */
    intern_esc_sequence("\x1b[42m", 5); /* Green bg */
    intern_esc_sequence("\x1b[43m", 5); /* Yellow bg */
    intern_esc_sequence("\x1b[44m", 5); /* Blue bg */
    intern_esc_sequence("\x1b[45m", 5); /* Magenta bg */
    intern_esc_sequence("\x1b[46m", 5); /* Cyan bg */
    intern_esc_sequence("\x1b[47m", 5); /* White bg */

    /* Pre-cache common attribute combinations */
    intern_esc_sequence("\x1b[0;37;40m", 10); /* White on black (normal) */
    intern_esc_sequence("\x1b[1;37;40m", 10); /* Bold white on black */
    intern_esc_sequence("\x1b[7m", 4);        /* Reverse video */
    intern_esc_sequence("\x1b[1;7m", 6);      /* Bold reverse */
    intern_esc_sequence("\x1b[0;30;47m", 10); /* Black on white (inverse) */
    intern_esc_sequence("\x1b[0;32m", 6);     /* Green text */
    intern_esc_sequence("\x1b[0;31m", 6);     /* Red text */
    intern_esc_sequence("\x1b[1;33m", 6);     /* Bold yellow */
    intern_esc_sequence("\x1b[0;34m", 6);     /* Blue text */

    /* Pre-populate common attribute combinations in cache */
    get_cached_attr_sequence(TUI_COLOR_WHITE, TUI_COLOR_BLACK, TUI_A_NORMAL,
                             NULL);
    get_cached_attr_sequence(TUI_COLOR_WHITE, TUI_COLOR_BLACK, TUI_A_BOLD,
                             NULL);
    get_cached_attr_sequence(TUI_COLOR_BLACK, TUI_COLOR_WHITE, TUI_A_NORMAL,
                             NULL);
    get_cached_attr_sequence(TUI_COLOR_GREEN, TUI_COLOR_BLACK, TUI_A_NORMAL,
                             NULL);
    get_cached_attr_sequence(TUI_COLOR_RED, TUI_COLOR_BLACK, TUI_A_NORMAL,
                             NULL);
    get_cached_attr_sequence(TUI_COLOR_YELLOW, TUI_COLOR_BLACK, TUI_A_BOLD,
                             NULL);
}

/* Free escape sequence cache */
static void free_esc_seq_cache(void)
{
    if (esc_seq_cache.pool) {
        free(esc_seq_cache.pool);
        esc_seq_cache.pool = NULL;
    }
    if (esc_seq_cache.attr_combo_pool) {
        free(esc_seq_cache.attr_combo_pool);
        esc_seq_cache.attr_combo_pool = NULL;
    }
    memset(esc_seq_cache.hash_table, 0, sizeof(esc_seq_cache.hash_table));
    memset(esc_seq_cache.attr_combo_table, 0,
           sizeof(esc_seq_cache.attr_combo_table));
    esc_seq_cache.pool_size = 0;
    esc_seq_cache.pool_used = 0;
    esc_seq_cache.attr_combo_pool_size = 0;
    esc_seq_cache.attr_combo_pool_used = 0;
    esc_seq_cache.initialized = false;
}

/* Initialize LRU escape sequence cache */
static void init_esc_lru_cache(void)
{
    if (esc_lru_cache.initialized)
        return;

    /* Clear the cache */
    memset(&esc_lru_cache, 0, sizeof(esc_lru_cache));

    /* Initialize all entries */
    for (int i = 0; i < ESC_LRU_CACHE_SIZE; i++) {
        esc_lru_cache.entries[i].lru_prev = NULL;
        esc_lru_cache.entries[i].lru_next = NULL;
        esc_lru_cache.entries[i].hash_next = NULL;
        esc_lru_cache.entries[i].key_hash = 0;
    }

    esc_lru_cache.initialized = true;
}

/* Free LRU escape sequence cache */
static void free_esc_lru_cache(void)
{
    memset(&esc_lru_cache, 0, sizeof(esc_lru_cache));
}

/* Intern an escape sequence - returns pointer to cached copy */
static const char *intern_esc_sequence(const char *seq, int len)
{
    if (!esc_seq_cache.initialized || len >= ESC_SEQ_MAX_LEN)
        return seq; /* Fallback to original */

    uint32_t hash = hash_string(seq, len);
    unsigned int bucket = hash % ESC_SEQ_HASH_SIZE;

    /* Look for existing entry */
    esc_seq_entry_t *entry = esc_seq_cache.hash_table[bucket];
    while (entry) {
        if (entry->hash == hash && entry->length == len &&
            !memcmp(entry->sequence, seq, len)) {
            entry->ref_count++;
            return entry->sequence;
        }
        entry = entry->next;
    }

    /* Not found, create new entry if pool has space */
    if (esc_seq_cache.pool_used >= esc_seq_cache.pool_size)
        return seq; /* Pool full, fallback */

    entry = &esc_seq_cache.pool[esc_seq_cache.pool_used++];
    memcpy(entry->sequence, seq, len);
    entry->sequence[len] = '\0';
    entry->length = len;
    entry->hash = hash;
    entry->ref_count = 1;

    /* Add to hash table */
    entry->next = esc_seq_cache.hash_table[bucket];
    esc_seq_cache.hash_table[bucket] = entry;

    return entry->sequence;
}

/* Get or create cached attribute sequence */
static const char *get_cached_attr_sequence(short fg,
                                            short bg,
                                            int attrs,
                                            int *out_len)
{
    if (!esc_seq_cache.initialized)
        return NULL;

    /* Create hash key from attributes */
    uint32_t key =
        ((uint32_t) fg << 16) | ((uint32_t) bg << 8) | (attrs & 0xFF);
    unsigned int bucket = key % ATTR_COMBO_CACHE_SIZE;

    /* Look for existing entry */
    attr_combo_entry_t *entry = esc_seq_cache.attr_combo_table[bucket];
    while (entry) {
        if (entry->fg == fg && entry->bg == bg && entry->attrs == attrs) {
            if (out_len)
                *out_len = entry->seq_length;
            return entry->sequence;
        }
        entry = entry->next;
    }

    /* Not found, build the sequence */
    char seq_buf[ESC_SEQ_MAX_LEN];
    int seq_len = 0;

    /* Start escape sequence */
    seq_buf[seq_len++] = '\x1b';
    seq_buf[seq_len++] = '[';

    /* Reset first */
    seq_buf[seq_len++] = '0';

    /* Add text attributes */
    if (attrs & TUI_A_BOLD) {
        seq_len += snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len, ";1");
    }

    /* Add foreground color */
    if (fg != TUI_COLOR_WHITE) {
        if (fg >= 8 && fg < MAX_CUSTOM_COLORS) {
            /* Custom color - use RGB */
            int r, g, b;
            get_rgb_values(color_defs[fg].r, color_defs[fg].g, color_defs[fg].b,
                           &r, &g, &b);
            seq_len += snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len,
                                ";38;2;%d;%d;%d", r, g, b);
        } else if (fg < 8) {
            /* Basic ANSI color */
            seq_len += snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len,
                                ";3%d", fg);
        }
    }

    /* Add background color */
    if (bg != TUI_COLOR_BLACK) {
        if (bg >= 8 && bg < MAX_CUSTOM_COLORS) {
            /* Custom color - use RGB */
            int r, g, b;
            get_rgb_values(color_defs[bg].r, color_defs[bg].g, color_defs[bg].b,
                           &r, &g, &b);
            seq_len += snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len,
                                ";48;2;%d;%d;%d", r, g, b);
        } else if (bg < 8) {
            /* Basic ANSI color */
            seq_len += snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len,
                                ";4%d", bg);
        }
    }

    /* Close the sequence */
    seq_buf[seq_len++] = 'm';
    seq_buf[seq_len] = '\0';

    /* Intern the sequence */
    const char *interned_seq = intern_esc_sequence(seq_buf, seq_len);

    /* Cache the attribute combination if pool has space */
    if (esc_seq_cache.attr_combo_pool_used <
        esc_seq_cache.attr_combo_pool_size) {
        entry = &esc_seq_cache
                     .attr_combo_pool[esc_seq_cache.attr_combo_pool_used++];
        entry->fg = fg;
        entry->bg = bg;
        entry->attrs = attrs;
        entry->sequence = interned_seq;
        entry->seq_length = seq_len;

        /* Add to hash table */
        entry->next = esc_seq_cache.attr_combo_table[bucket];
        esc_seq_cache.attr_combo_table[bucket] = entry;
    }

    if (out_len)
        *out_len = seq_len;
    return interned_seq;
}

int tui_init_color(short color, short r, short g, short b)
{
    /* Store color definitions for all colors, not just basic 8 */
    if (color >= 0 && color < MAX_CUSTOM_COLORS) {
        color_defs[color].r = r;
        color_defs[color].g = g;
        color_defs[color].b = b;
    }
    return 0;
}

int tui_raw(void)
{
    orig_termios.c_lflag &= ~(ICANON | ISIG);
    orig_termios.c_cc[VMIN] = 1;
    orig_termios.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int tui_noraw(void)
{
    orig_termios.c_lflag |= ICANON | ISIG;
    return tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int tui_cbreak(void)
{
    orig_termios.c_lflag &= ~ICANON;
    orig_termios.c_cc[VMIN] = 1;
    orig_termios.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int tui_noecho(void)
{
    orig_termios.c_lflag &= ~ECHO;
    return tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int tui_echo(void)
{
    orig_termios.c_lflag |= ECHO;
    return tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int tui_set_cursor(int visibility)
{
    int prev = cursor_visibility;
    if (visibility == 0) {
        tui_puts(ESC_HIDE_CURSOR);
        cursor_visibility = 0;
    } else if (visibility == 1) {
        tui_puts(ESC_SHOW_CURSOR);
        cursor_visibility = 1;
    } else {
        return -1;
    }
    tui_flush();
    return prev;
}

static int parse_escape_sequence(void)
{
    char ch;
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};

    if (poll(&pfd, 1, 50) <= 0)
        return 27;

    if (read(STDIN_FILENO, &ch, 1) != 1)
        return 27;

    if (ch == '[') {
        if (read(STDIN_FILENO, &ch, 1) != 1)
            return 27;

        switch (ch) {
        case 'A':
            return TUI_KEY_UP;
        case 'B':
            return TUI_KEY_DOWN;
        case 'C':
            return TUI_KEY_RIGHT;
        case 'D':
            return TUI_KEY_LEFT;
        default:
            return 27;
        }
    }

    return 27;
}

int tui_getch(void)
{
    if (!tui_stdscr)
        return -1;

    if (tui_stdscr->delay >= 0) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&pfd, 1, tui_stdscr->delay) <= 0)
            return -1;
    }

    char ch;
    if (read(STDIN_FILENO, &ch, 1) != 1)
        return -1;

    if (ch == 27 && tui_stdscr->keypad_mode)
        return parse_escape_sequence();

    if (ch == '\r' || ch == '\n')
        return TUI_KEY_ENTER;

    return ch;
}

int tui_set_nodelay(tui_window_t *win, bool bf)
{
    if (!win)
        return -1;
    win->delay = bf ? 0 : -1;
    return 0;
}

int tui_set_keypad(tui_window_t *win, bool yes)
{
    if (!win)
        return -1;
    win->keypad_mode = yes ? 1 : 0;
    return 0;
}

int tui_clear_screen(void)
{
    if (!tui_stdscr || !screen_buf || !attr_buf || !prev_screen_buf ||
        !prev_attr_buf)
        return -1;

    for (int i = 0; i < buf_rows; i++) {
        memset(screen_buf[i], ' ', buf_cols);
        for (int j = 0; j < buf_cols; j++)
            attr_buf[i][j] = TUI_A_NORMAL;
        /* Invalidate previous buffer to force redraw */
        memset(prev_screen_buf[i], '\0', buf_cols);
        memset(prev_attr_buf[i], 0xFF, buf_cols * sizeof(int));
    }

    tui_stdscr->cury = 0;
    tui_stdscr->curx = 0;

    /* Mark entire screen as dirty */
    mark_dirty_region(0, 0, buf_rows - 1, buf_cols - 1);

    /* Reset tracking states since screen is cleared */
    reset_cursor_tracking();
    reset_attr_state();

    return 0;
}

tui_window_t *tui_newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    tui_window_t *win = calloc(1, sizeof(tui_window_t));
    if (!win)
        return NULL;

    win->begy = begin_y;
    win->begx = begin_x;
    win->maxy = nlines;
    win->maxx = ncols;
    win->cury = 0;
    win->curx = 0;
    win->keypad_mode = 0;
    win->delay = -1;
    win->attr = TUI_A_NORMAL;
    win->bkgd = TUI_A_NORMAL;

    win->dirty = calloc(nlines, sizeof(unsigned char));
    if (!win->dirty) {
        free(win);
        return NULL;
    }

    return win;
}

int tui_delwin(tui_window_t *win)
{
    if (!win)
        return -1;

    if (win->dirty)
        free(win->dirty);
    free(win);
    return 0;
}

int tui_clear_window(tui_window_t *win)
{
    if (!win || !screen_buf || !attr_buf || !prev_screen_buf || !prev_attr_buf)
        return -1;

    for (int y = 0; y < win->maxy; y++) {
        int screen_y = win->begy + y;
        if (screen_y >= 0 && screen_y < buf_rows) {
            for (int x = 0; x < win->maxx; x++) {
                int screen_x = win->begx + x;
                if (screen_x >= 0 && screen_x < buf_cols) {
                    screen_buf[screen_y][screen_x] = ' ';
                    attr_buf[screen_y][screen_x] = win->bkgd;
                    /* Invalidate previous buffer to force redraw */
                    prev_screen_buf[screen_y][screen_x] = '\0';
                    prev_attr_buf[screen_y][screen_x] = 0xFFFFFFFF;
                    mark_dirty(screen_y, screen_x);
                }
            }
        }
        if (win->dirty)
            win->dirty[y] = 1;
    }

    win->cury = 0;
    win->curx = 0;

    return 0;
}

/* Convert RGB from 0-1000 to 0-255 range for custom colors */
static void get_rgb_values(short r,
                           short g,
                           short b,
                           int *out_r,
                           int *out_g,
                           int *out_b)
{
    /* Normalize RGB from 0-1000 range to 0-255 */
    if (r > 255 || g > 255 || b > 255) {
        *out_r = (r * 255) / 1000;
        *out_g = (g * 255) / 1000;
        *out_b = (b * 255) / 1000;
    } else {
        *out_r = r;
        *out_g = g;
        *out_b = b;
    }
}

static void output_buffered_run_vectored(int y,
                                         int start_x,
                                         int end_x,
                                         char **screen_buf,
                                         char **prev_screen_buf,
                                         int **prev_attr_buf)
{
    /* Character buffer for this run - increased for better coalescing */
    static char run_buffer[512];
    int buf_len = 0;
    int run_len = end_x - start_x + 1;

    if (run_len <= (int) sizeof(run_buffer)) {
        /* Move cursor if needed - goes into vector buffer */
        if (cursor_cache.last_col != start_x)
            tui_move_cached(y, start_x);

        /* Collect all characters into buffer */
        for (int x = start_x; x <= end_x; x++) {
            run_buffer[buf_len++] = screen_buf[y][x];
            prev_screen_buf[y][x] = screen_buf[y][x];
            prev_attr_buf[y][x] = attr_buf[y][x];
        }

        /* Add character run to vector - this is key optimization */
        tui_write_vectored(run_buffer, buf_len);

        /* Enhanced batch multiple operations - increase threshold for better
         * coalescing */
        if (output_buffer.use_writev && writev_buf.count > 0) {
            /* More aggressive batching: flush less frequently to accumulate
             * more vectors */
            if (writev_buf.count >= (VEC_FLUSH_THRESHOLD * 3 / 4) ||
                writev_buf.total_bytes >= (WRITEV_BUFFER_SIZE * 3 / 4)) {
                tui_flush_vectored();
            }
        }
    } else {
        /* Fallback for very long runs */
        for (int x = start_x; x <= end_x; x++) {
            tui_putchar(screen_buf[y][x]);
            prev_screen_buf[y][x] = screen_buf[y][x];
            prev_attr_buf[y][x] = attr_buf[y][x];
        }
    }

    cursor_cache.last_col = end_x + 1;
    rle_stats.total_chars_output += run_len;
}

static void output_buffered_run(int y,
                                int start_x,
                                int end_x,
                                char **screen_buf,
                                char **prev_screen_buf,
                                int **prev_attr_buf)
{
    if (output_buffer.use_writev) {
        output_buffered_run_vectored(y, start_x, end_x, screen_buf,
                                     prev_screen_buf, prev_attr_buf);
        return;
    }

    /* Original implementation for fallback */
    if (cursor_cache.last_col != start_x)
        tui_move_cached(y, start_x);

    /* Buffer characters for this run to reduce write calls */
    static char run_buffer[256];
    int buf_len = 0;
    int run_len = end_x - start_x + 1;

    if (run_len <= (int) sizeof(run_buffer)) {
        /* Collect all characters into buffer */
        for (int x = start_x; x <= end_x; x++) {
            run_buffer[buf_len++] = screen_buf[y][x];
            prev_screen_buf[y][x] = screen_buf[y][x];
            prev_attr_buf[y][x] = attr_buf[y][x];
        }

        /* Write entire buffer in one call */
        tui_write(run_buffer, buf_len);
    } else {
        /* Fallback for very long runs */
        for (int x = start_x; x <= end_x; x++) {
            tui_putchar(screen_buf[y][x]);
            prev_screen_buf[y][x] = screen_buf[y][x];
            prev_attr_buf[y][x] = attr_buf[y][x];
        }
    }

    cursor_cache.last_col = end_x + 1;
    rle_stats.total_chars_output += run_len;
}

static void apply_attributes(int attr)
{
    /* Extract color information */
    short fg = TUI_COLOR_WHITE;
    short bg = TUI_COLOR_BLACK;

    if (colors_initialized && (attr & TUI_A_COLOR)) {
        short pair = TUI_PAIR_NUMBER(attr);
        /* Use lazy allocation to get colors */
        get_pair_colors(pair, &fg, &bg);
    }

    /* Extract text attributes */
    int text_attrs = attr & ~TUI_A_COLOR;

    /* Check if anything actually changed */
    if (attr_state.initialized && attr_state.last_fg == fg &&
        attr_state.last_bg == bg && attr_state.last_attrs == text_attrs) {
        return; /* Nothing changed, skip update */
    }

    /* Use cached attribute sequence */
    int seq_len;
    const char *cached_seq =
        get_cached_attr_sequence(fg, bg, text_attrs, &seq_len);

    if (cached_seq) {
        tui_write(cached_seq, seq_len);
    } else {
        /* Fallback to building sequence manually if cache fails */
        char seq_buf[128];
        seq_len = 0;

        /* Always start with reset to ensure clean state */
        seq_len +=
            snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len, "\x1b[0");

        /* Add text attributes if any */
        if (text_attrs != TUI_A_NORMAL) {
            if (text_attrs & TUI_A_BOLD)
                seq_len += snprintf(seq_buf + seq_len,
                                    sizeof(seq_buf) - seq_len, ";1");
        }

        /* Add foreground color */
        if (fg != TUI_COLOR_WHITE || !attr_state.initialized) {
            if (fg < 8 && esc_seq_cache.precomputed.initialized) {
                /* Use pre-computed basic ANSI color */
                memcpy(seq_buf + seq_len,
                       esc_seq_cache.precomputed.basic_colors[fg] + 1,
                       esc_seq_cache.precomputed.color_lengths[fg] - 1);
                seq_len += esc_seq_cache.precomputed.color_lengths[fg] - 1;
            } else if (fg >= 8 && fg < MAX_CUSTOM_COLORS) {
                /* Custom color - use RGB */
                int r, g, b;
                get_rgb_values(color_defs[fg].r, color_defs[fg].g,
                               color_defs[fg].b, &r, &g, &b);
                seq_len +=
                    snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len,
                             ";38;2;%d;%d;%d", r, g, b);
            } else if (fg < 8) {
                /* Fallback for basic ANSI color */
                seq_len += snprintf(seq_buf + seq_len,
                                    sizeof(seq_buf) - seq_len, ";3%d", fg);
            }
        }

        /* Add background color if not black (terminal default) */
        if (bg != TUI_COLOR_BLACK) {
            if (bg < 8 && esc_seq_cache.precomputed.initialized) {
                /* Use pre-computed basic ANSI background color */
                memcpy(seq_buf + seq_len,
                       esc_seq_cache.precomputed.basic_colors[bg + 8] + 1,
                       esc_seq_cache.precomputed.color_lengths[bg + 8] - 1);
                seq_len += esc_seq_cache.precomputed.color_lengths[bg + 8] - 1;
            } else if (bg >= 8 && bg < MAX_CUSTOM_COLORS) {
                /* Custom color - use RGB */
                int r, g, b;
                get_rgb_values(color_defs[bg].r, color_defs[bg].g,
                               color_defs[bg].b, &r, &g, &b);
                seq_len +=
                    snprintf(seq_buf + seq_len, sizeof(seq_buf) - seq_len,
                             ";48;2;%d;%d;%d", r, g, b);
            } else if (bg < 8) {
                /* Fallback for basic ANSI color */
                seq_len += snprintf(seq_buf + seq_len,
                                    sizeof(seq_buf) - seq_len, ";4%d", bg);
            }
        }

        /* Close the escape sequence */
        seq_buf[seq_len++] = 'm';
        seq_buf[seq_len] = '\0';

        /* Intern the complete sequence and send it */
        cached_seq = intern_esc_sequence(seq_buf, seq_len);
        tui_write(cached_seq, seq_len);
    }

    /* Update cached state */
    attr_state.last_fg = fg;
    attr_state.last_bg = bg;
    attr_state.last_attrs = text_attrs;
    attr_state.initialized = true;
}

int tui_refresh(tui_window_t *win)
{
    if (!win || !screen_buf || !attr_buf || !prev_screen_buf || !prev_attr_buf)
        return -1;

    if (win == tui_stdscr) {
        /* Disable auto-flush during batch rendering for better performance */
        tui_set_auto_flush(false);

        /* Use double buffering with change detection */
        bool has_changes = false;

        /* Early exit if no dirty regions */
        if (!dirty_region.has_changes) {
            tui_set_auto_flush(true); /* Re-enable auto-flush */
            return 0;
        }

        /* Optimize dirty region to reduce unnecessary scanning */
        optimize_dirty_region();

        /* Optimize scan area using dirty region bounds */
        int scan_min_row = dirty_region.min_row >= 0 ? dirty_region.min_row : 0;
        int scan_max_row = dirty_region.max_row < buf_rows
                               ? dirty_region.max_row
                               : buf_rows - 1;
        int scan_min_col = dirty_region.min_col >= 0 ? dirty_region.min_col : 0;
        int scan_max_col = dirty_region.max_col < buf_cols
                               ? dirty_region.max_col
                               : buf_cols - 1;

        /* Clamp to screen bounds */
        scan_min_row = scan_min_row < tui_lines ? scan_min_row : tui_lines - 1;
        scan_max_row = scan_max_row < tui_lines ? scan_max_row : tui_lines - 1;
        scan_min_col = scan_min_col < tui_cols ? scan_min_col : tui_cols - 1;
        scan_max_col = scan_max_col < tui_cols ? scan_max_col : tui_cols - 1;

        /* Adaptive scanning strategy selection */
        bool use_sparse_scanning = false;
        int sparse_tile_count = 0;

        dirty_region.frame_count++;

        if (dirty_region.use_sparse_tracking) {
            /* Count sparse tiles */
            dirty_tile_t *current = dirty_l1_tiles;
            while (current &&
                   sparse_tile_count < 100) { /* Cap count for performance */
                sparse_tile_count++;
                current = current->next;
            }

            /* Calculate scan area and tile density */
            int scan_area = (scan_max_row - scan_min_row + 1) *
                            (scan_max_col - scan_min_col + 1);
            int tile_area = scan_area / (TILE_L1_SIZE * TILE_L1_SIZE);
            bool sparse_would_be_beneficial =
                (sparse_tile_count > 0 && sparse_tile_count < tile_area / 3);

            /* Adaptive decision with history */
            if (sparse_would_be_beneficial) {
                dirty_region.sparse_beneficial_count++;
            }

            /* Every 60 frames, evaluate if we should prefer sparse mode */
            if (dirty_region.frame_count % 60 == 0) {
                if (dirty_region.sparse_beneficial_count > 30) {
                    /* More than 50% beneficial */
                    dirty_region.prefer_sparse_mode = true;
                } else if (dirty_region.sparse_beneficial_count < 15) {
                    /* Less than 25% beneficial */
                    dirty_region.prefer_sparse_mode = false;
                }
                dirty_region.sparse_beneficial_count = 0;
            }

            /* Make scanning decision based on current frame + learned
             * preference */
            use_sparse_scanning =
                sparse_would_be_beneficial ||
                (dirty_region.prefer_sparse_mode && sparse_tile_count > 0 &&
                 sparse_tile_count < tile_area / 2);
        }

        if (use_sparse_scanning) {
            /* Sparse scanning: iterate only through dirty tiles */
            dirty_region.total_scans++;
            dirty_region.sparse_hits++;

            dirty_tile_t *current = dirty_l1_tiles;
            while (current) {
                int tile_row = current->row;
                int tile_col = current->col;

                /* Check if tile intersects with scan region */
                int tile_start_row = tile_row * TILE_L1_SIZE;
                int tile_end_row = tile_start_row + TILE_L1_SIZE - 1;
                int tile_start_col = tile_col * TILE_L1_SIZE;
                int tile_end_col = tile_start_col + TILE_L1_SIZE - 1;

                if (tile_end_row >= scan_min_row &&
                    tile_start_row <= scan_max_row &&
                    tile_end_col >= scan_min_col &&
                    tile_start_col <= scan_max_col) {
                    /* Scan this tile */
                    scan_l1_tile(tile_row, tile_col, buf_rows, buf_cols,
                                 scan_min_row, scan_max_row, scan_min_col,
                                 scan_max_col, &has_changes);
                }

                current = current->next;
            }
        } else if (dirty_region.use_hierarchical_tiles) {
            dirty_region.total_scans++;

            /* Sparse hierarchical scanning: only iterate via dirty L2 block */
            if (dirty_region.use_sparse_tracking) {
                dirty_tile_t *current_l2 = dirty_l2_blocks;
                while (current_l2) {
                    int l2_row = current_l2->row;
                    int l2_col = current_l2->col;

                    /* Check if L2 block intersects with scan region */
                    int l2_start_row_px = l2_row * TILE_L2_SIZE;
                    int l2_end_row_px = l2_start_row_px + TILE_L2_SIZE - 1;
                    int l2_start_col_px = l2_col * TILE_L2_SIZE;
                    int l2_end_col_px = l2_start_col_px + TILE_L2_SIZE - 1;

                    if (l2_end_row_px >= scan_min_row &&
                        l2_start_row_px <= scan_max_row &&
                        l2_end_col_px >= scan_min_col &&
                        l2_start_col_px <= scan_max_col) {
                        /* Sparse L1 scanning within this L2 block */
                        dirty_tile_t *current_l1 = dirty_l1_tiles;
                        while (current_l1) {
                            int l1_row = current_l1->row;
                            int l1_col = current_l1->col;

                            /* Check if L1 tile is within this L2 block */
                            int l1_l2_row =
                                l1_row / (TILE_L2_SIZE / TILE_L1_SIZE);
                            int l1_l2_col =
                                l1_col / (TILE_L2_SIZE / TILE_L1_SIZE);

                            if (l1_l2_row == l2_row && l1_l2_col == l2_col) {
                                /* Check if L1 tile intersects with scan reg */
                                int l1_start_row_px = l1_row * TILE_L1_SIZE;
                                int l1_end_row_px =
                                    l1_start_row_px + TILE_L1_SIZE - 1;
                                int l1_start_col_px = l1_col * TILE_L1_SIZE;
                                int l1_end_col_px =
                                    l1_start_col_px + TILE_L1_SIZE - 1;

                                if (l1_end_row_px >= scan_min_row &&
                                    l1_start_row_px <= scan_max_row &&
                                    l1_end_col_px >= scan_min_col &&
                                    l1_start_col_px <= scan_max_col) {
                                    /* Scan this L1 tile */
                                    scan_l1_tile(l1_row, l1_col, buf_rows,
                                                 buf_cols, scan_min_row,
                                                 scan_max_row, scan_min_col,
                                                 scan_max_col, &has_changes);
                                }
                            }
                            current_l1 = current_l1->next;
                        }
                    }
                    current_l2 = current_l2->next;
                }
            } else {
                /* Traditional hierarchical scanning: full array scan */
                int l2_start_row = scan_min_row / TILE_L2_SIZE;
                int l2_end_row = scan_max_row / TILE_L2_SIZE;
                int l2_start_col = scan_min_col / TILE_L2_SIZE;
                int l2_end_col = scan_max_col / TILE_L2_SIZE;

                for (int l2_row = l2_start_row; l2_row <= l2_end_row;
                     l2_row++) {
                    for (int l2_col = l2_start_col; l2_col <= l2_end_col;
                         l2_col++) {
                        /* Only scan L2 block if it's marked dirty */
                        if (!has_l2_block_changes(l2_row, l2_col)) {
                            dirty_region.l2_scans_avoided++;
                            continue;
                        }

                        /* Level 1: Scan 8x8 tiles within this L2 block */
                        int l1_start_row =
                            l2_row * (TILE_L2_SIZE / TILE_L1_SIZE);
                        int l1_end_row =
                            (l2_row + 1) * (TILE_L2_SIZE / TILE_L1_SIZE) - 1;
                        int l1_start_col =
                            l2_col * (TILE_L2_SIZE / TILE_L1_SIZE);
                        int l1_end_col =
                            (l2_col + 1) * (TILE_L2_SIZE / TILE_L1_SIZE) - 1;

                        /* Clamp L1 range to screen bounds */
                        if (l1_end_row >= dirty_region.l1_tiles_y)
                            l1_end_row = dirty_region.l1_tiles_y - 1;
                        if (l1_end_col >= dirty_region.l1_tiles_x)
                            l1_end_col = dirty_region.l1_tiles_x - 1;

                        /* Further constrain to intersect with actual dirty
                         * region
                         */
                        int l1_scan_start_row = scan_min_row / TILE_L1_SIZE;
                        int l1_scan_end_row = scan_max_row / TILE_L1_SIZE;
                        int l1_scan_start_col = scan_min_col / TILE_L1_SIZE;
                        int l1_scan_end_col = scan_max_col / TILE_L1_SIZE;

                        if (l1_start_row < l1_scan_start_row)
                            l1_start_row = l1_scan_start_row;
                        if (l1_end_row > l1_scan_end_row)
                            l1_end_row = l1_scan_end_row;
                        if (l1_start_col < l1_scan_start_col)
                            l1_start_col = l1_scan_start_col;
                        if (l1_end_col > l1_scan_end_col)
                            l1_end_col = l1_scan_end_col;

                        for (int l1_row = l1_start_row; l1_row <= l1_end_row;
                             l1_row++) {
                            for (int l1_col = l1_start_col;
                                 l1_col <= l1_end_col; l1_col++) {
                                /* Only scan L1 tile if it's marked dirty */
                                if (!has_l1_tile_changes(l1_row, l1_col)) {
                                    dirty_region.l1_scans_avoided++;
                                    continue;
                                }

                                /* Scan this 8x8 tile for actual changes */
                                scan_l1_tile(l1_row, l1_col, buf_rows, buf_cols,
                                             scan_min_row, scan_max_row,
                                             scan_min_col, scan_max_col,
                                             &has_changes);
                            }
                        }
                    }
                }
            }
        } else {
            /* Fallback: Traditional linear scanning with memcmp optimization */
            for (int y = scan_min_row; y <= scan_max_row; y++) {
                /* Fast row check: skip entire row if unchanged */
                if (!row_has_changes(y, scan_min_col,
                                     (scan_max_col < buf_cols)
                                         ? scan_max_col
                                         : buf_cols - 1)) {
                    continue;
                }

                int x = scan_min_col;
                while (x <= scan_max_col && x < buf_cols && x < tui_cols) {
                    /* Check if cell has changed - now only for rows we know
                     * have changes */
                    if (screen_buf[y][x] != prev_screen_buf[y][x] ||
                        attr_buf[y][x] != prev_attr_buf[y][x]) {
                        has_changes = true;

                        /* Find the start of changed region */
                        int start_x = x;
                        int curr_attr = attr_buf[y][x];

                        /* Find end of run with same attributes that have
                         * changed, with gap coalescing */
                        int end_x = x;
                        int unchanged_gap = 0;
                        /* Coalesce if gap is 3 chars or less */
                        const int MAX_GAP = 3;

                        while (end_x + 1 < buf_cols && end_x + 1 < tui_cols &&
                               attr_buf[y][end_x + 1] == curr_attr) {
                            if (screen_buf[y][end_x + 1] !=
                                    prev_screen_buf[y][end_x + 1] ||
                                attr_buf[y][end_x + 1] !=
                                    prev_attr_buf[y][end_x + 1]) {
                                /* Changed cell - continue the run */
                                end_x++;
                                unchanged_gap = 0;
                            } else if (unchanged_gap < MAX_GAP) {
                                /* Small gap of unchanged cells - include in run
                                 * to avoid cursor move */
                                end_x++;
                                unchanged_gap++;
                            } else {
                                /* Gap too large - end the run */
                                break;
                            }
                        }

                        /* Trim any trailing unchanged cells */
                        while (end_x > x &&
                               screen_buf[y][end_x] ==
                                   prev_screen_buf[y][end_x] &&
                               attr_buf[y][end_x] == prev_attr_buf[y][end_x]) {
                            end_x--;
                        }

                        /* Move to start of changed run */
                        tui_move_cached(y, start_x);

                        /* Apply attributes for this run */
                        apply_attributes(curr_attr);

                        /* Output all changed characters in the run with
                         * buffering optimization */
                        output_buffered_run(y, start_x, end_x, screen_buf,
                                            prev_screen_buf, prev_attr_buf);

                        /* Update cached position after run */
                        cursor_cache.last_col = end_x + 1;

                        /* Move to next position */
                        x = end_x + 1;
                    } else {
                        /* Cell unchanged, skip to next */
                        x++;
                    }
                }
            }
        }

        /* Only flush if we actually rendered something */
        if (has_changes) {
            /* Reset to normal using pre-computed sequence */
            if (esc_seq_cache.precomputed.initialized) {
                tui_write(PRECOMP_RESET, PRECOMP_RESET_LEN);
            } else {
                const char *reset_seq =
                    intern_esc_sequence(ESC_RESET, strlen(ESC_RESET));
                tui_puts(reset_seq);
            }
            tui_force_flush();

            /* Reset tracking states after rendering */
            reset_attr_state();
        }

        /* Re-enable auto-flush after batch rendering */
        tui_set_auto_flush(true);

        /* Always reset dirty region tracking */
        reset_dirty_region();
    } else {
        for (int y = 0; y < win->maxy; y++) {
            if (!win->dirty || win->dirty[y]) {
                int screen_y = win->begy + y;
                if (screen_y >= 0 && screen_y < tui_lines &&
                    screen_y < buf_rows) {
                    /* Find runs of cells with identical attributes */
                    int x = 0;

                    while (x < win->maxx) {
                        int screen_x = win->begx + x;
                        if (screen_x < 0 || screen_x >= tui_cols ||
                            screen_x >= buf_cols) {
                            x++;
                            continue;
                        }

                        /* Get attributes for current position */
                        int start_x = x;
                        int curr_attr = attr_buf[screen_y][screen_x];

                        /* Find end of run with same attributes */
                        int end_x = x;
                        while (end_x + 1 < win->maxx) {
                            int next_screen_x = win->begx + end_x + 1;
                            if (next_screen_x >= 0 &&
                                next_screen_x < tui_cols &&
                                next_screen_x < buf_cols &&
                                attr_buf[screen_y][next_screen_x] ==
                                    curr_attr) {
                                end_x++;
                            } else {
                                break;
                            }
                        }

                        /* Move to start of run */
                        tui_move_cached(screen_y, win->begx + start_x);

                        /* Apply attributes for this run */
                        apply_attributes(curr_attr);

                        /* Output all characters in the run */
                        for (int i = start_x; i <= end_x; i++) {
                            int sx = win->begx + i;
                            if (sx >= 0 && sx < tui_cols && sx < buf_cols)
                                tui_putchar(screen_buf[screen_y][sx]);
                        }

                        /* Update cached position after run */
                        cursor_cache.last_col = win->begx + end_x + 1;

                        /* Move to next run */
                        x = end_x + 1;
                    }
                }
                if (win->dirty)
                    win->dirty[y] = 0;
            }
        }

        /* Ensure we end with the reference background color */
        tui_puts("\x1b[0m\x1b[48;2;74;74;74m");
        tui_force_flush();
    }

    return 0;
}

/* Unused window functions removed - wnoutrefresh, doupdate, touchwin */

/* UTF-8 helper functions for proper Unicode character handling */
static int utf8_char_length(unsigned char byte)
{
    if (byte < 0x80)
        return 1; /* ASCII */
    if ((byte & 0xE0) == 0xC0)
        return 2; /* 110xxxxx */
    if ((byte & 0xF0) == 0xE0)
        return 3; /* 1110xxxx */
    if ((byte & 0xF8) == 0xF0)
        return 4; /* 11110xxx */

    return 1; /* Invalid UTF-8, treat as single byte */
}

static bool is_valid_utf8_sequence(const char *p, int len)
{
    if (len <= 0 || len > 4)
        return false;

    /* Check first byte */
    unsigned char first = (unsigned char) *p;
    if (len == 1)
        return first < 0x80;
    if (len == 2)
        return (first & 0xE0) == 0xC0 && first >= 0xC2;
    if (len == 3)
        return (first & 0xF0) == 0xE0;
    if (len == 4)
        return (first & 0xF8) == 0xF0 && first <= 0xF4;

    /* Check continuation bytes */
    for (int i = 1; i < len; i++) {
        if (((unsigned char) p[i] & 0xC0) != 0x80)
            return false;
    }

    return true;
}

static int utf8_display_width(const char *utf8_char, int char_len)
{
    /* For menu arrows and most common Unicode characters, assume width of 1 */
    /* This is a simplified approach - full implementation would use wcwidth()
     */
    if (char_len == 1)
        return 1; /* ASCII */

    /* Handle specific Unicode characters we use in menu */
    if (char_len == 3) {
        /* Check for ► (U+25BA) = E2 96 BA */
        if ((unsigned char) utf8_char[0] == 0xE2 &&
            (unsigned char) utf8_char[1] == 0x96 &&
            (unsigned char) utf8_char[2] == 0xBA) {
            return 1;
        }
        /* Check for ◄ (U+25C4) = E2 97 84 */
        if ((unsigned char) utf8_char[0] == 0xE2 &&
            (unsigned char) utf8_char[1] == 0x97 &&
            (unsigned char) utf8_char[2] == 0x84) {
            return 1;
        }
    }

    /* Default to width 1 for other multibyte characters */
    return 1;
}

int tui_print_at(tui_window_t *win, int y, int x, const char *fmt, ...)
{
    if (!win || !screen_buf || !attr_buf)
        return -1;

    va_list ap;
    va_start(ap, fmt);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    int screen_y = win->begy + y;
    int screen_x = win->begx + x;

    if (screen_y < 0 || screen_y >= buf_rows)
        return -1;

    int start_x = screen_x;

    /* Process UTF-8 aware character-by-character */
    if (g_terminal_caps.supports_unicode) {
        for (char *p = buffer; *p && screen_x < buf_cols;) {
            int char_len = utf8_char_length((unsigned char) *p);
            int remaining = strlen(p);

            /* Ensure we have enough bytes for this character */
            if (char_len > remaining) {
                char_len = 1; /* Treat as single byte if incomplete */
            }

            /* Validate UTF-8 sequence */
            if (char_len > 1 && !is_valid_utf8_sequence(p, char_len)) {
                char_len = 1; /* Fall back to single byte */
            }

            if (screen_x >= 0) {
                /* Store the complete UTF-8 sequence */
                if (char_len == 1) {
                    /* ASCII character */
                    screen_buf[screen_y][screen_x] = *p;
                } else {
                    /* Multi-byte UTF-8 character - store first byte, mark
                     * others as continuation */
                    screen_buf[screen_y][screen_x] = *p;
                    for (int i = 1; i < char_len && (screen_x + i) < buf_cols;
                         i++) {
                        if ((screen_x + i) >= 0) {
                            screen_buf[screen_y][screen_x + i] = p[i];
                            attr_buf[screen_y][screen_x + i] =
                                win->attr |
                                0x80000000; /* Mark as continuation */
                        }
                    }
                }

                attr_buf[screen_y][screen_x] = win->attr;

                /* Invalidate previous buffer entries */
                if (prev_screen_buf && prev_attr_buf) {
                    for (int i = 0; i < char_len && (screen_x + i) < buf_cols;
                         i++) {
                        if ((screen_x + i) >= 0) {
                            prev_screen_buf[screen_y][screen_x + i] = '\0';
                            prev_attr_buf[screen_y][screen_x + i] = 0xFFFFFFFF;
                        }
                    }
                }
            }

            p += char_len;
            screen_x += utf8_display_width(p - char_len, char_len);
        }
    } else {
        /* Fall back to byte-by-byte processing for non-UTF-8 terminals */
        for (char *p = buffer; *p && screen_x < buf_cols; p++, screen_x++) {
            if (screen_x >= 0) {
                screen_buf[screen_y][screen_x] = *p;
                attr_buf[screen_y][screen_x] = win->attr;
                /* Invalidate previous buffer entry to ensure redraw */
                if (prev_screen_buf && prev_attr_buf) {
                    prev_screen_buf[screen_y][screen_x] = '\0';
                    prev_attr_buf[screen_y][screen_x] = 0xFFFFFFFF;
                }
            }
        }
    }

    /* Mark the written region as dirty */
    if (screen_x > start_x)
        mark_dirty_region(screen_y, start_x, screen_y, screen_x - 1);

    if (win->dirty && y < win->maxy)
        win->dirty[y] = 1;

    win->cury = y;
    win->curx = screen_x - win->begx;

    return 0;
}

int tui_wattron(tui_window_t *win, int attrs)
{
    if (!win)
        return -1;

    if (attrs & TUI_A_COLOR)
        win->attr &= ~TUI_A_COLOR;
    win->attr |= attrs;
    return 0;
}

int tui_wattroff(tui_window_t *win, int attrs)
{
    if (!win)
        return -1;

    if (attrs & TUI_A_COLOR)
        win->attr &= ~TUI_A_COLOR;
    win->attr &= ~attrs;
    return 0;
}

/* Get window dimensions */
int tui_get_max_x(tui_window_t *win)
{
    return win ? win->maxx : tui_cols;
}

int tui_get_max_y(tui_window_t *win)
{
    return win ? win->maxy : tui_lines;
}
