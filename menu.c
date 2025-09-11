#include <stddef.h>

#include "private.h"
#include "trex.h"

/* Sprite rectangle definition */
typedef struct {
    int x, y, w, h;
} sprite_rect_t;

/* T-Rex sprite coordinate data */
static const sprite_rect_t menu_trex_parts[] = {
    /* Head */
    {10, 0, 8, 1},
    {9, 1, 11, 1},
    {9, 2, 11, 1},

    /* Neck and body */
    {9, 3, 6, 1},
    {9, 4, 9, 1},

    /* Arms */
    {0, 5, 1, 1},
    {9, 5, 7, 1},
    {0, 6, 2, 1},
    {7, 6, 11, 1},

    /* Main body */
    {0, 7, 3, 1},
    {6, 7, 10, 1},
    {0, 8, 15, 1},
    {2, 9, 11, 1},
    {3, 10, 8, 1},
    {4, 11, 4, 1},
    {9, 11, 2, 1},

    /* Legs */
    {4, 12, 2, 1},
    {10, 12, 1, 1},
    {4, 13, 1, 1},
    {10, 13, 1, 1},
    {4, 14, 2, 1},
    {10, 14, 2, 1},
};

/* Draw a simplified T-Rex sprite for the menu */
static void menu_draw_trex(int x, int y)
{
    const game_config_t *cfg = ensure_cfg();
    const rgb_color_t *color = &cfg->colors.trex_normal;

    /* Draw all T-Rex parts from coordinate table */
    for (size_t i = 0; i < sizeof(menu_trex_parts) / sizeof(menu_trex_parts[0]);
         i++) {
        const sprite_rect_t *rect = &menu_trex_parts[i];
        draw_block_color(x + rect->x, y + rect->y, rect->w, rect->h, color->r,
                         color->g, color->b);
    }

    /* Eye - special color */
    const rgb_color_t *eye_color = &cfg->colors.menu_selected;
    draw_block_color(x + 12, y + 1, 1, 1, eye_color->r, eye_color->g,
                     eye_color->b);
}

#define MENU_NUMOPTIONS 2 /* Total number of menu options */

/* Menu strings */
static const char *menu_options[MENU_NUMOPTIONS] = {
    "Start Game",
    "Exit",
};

/* Variable to store the current selected option */
static menu_id_t selected_menu_id = MENUID_START;

void menu_update(double elapsed)
{
    (void) elapsed; /* Mark parameter as unused */
}

void menu_handle_selection(menu_id_t menu)
{
    switch (menu) {
    case MENUID_START:
        state_set_screen_type(SCREEN_WORLD);
        break;
    case MENUID_EXIT:
        state_quit_game();
        break;
    }
}

void menu_render()
{
    const game_config_t *cfg = ensure_cfg();

    int center_x = RESOLUTION_COLS >> 1;
    int center_y = RESOLUTION_ROWS >> 1;

    /* Layout: T-Rex on left, menu items on right */
    /* Position T-Rex on the left */
    int trex_x = center_x - cfg->ui.trex_offset_x;
    /* Vertically center the content */
    int trex_y = center_y - cfg->ui.trex_offset_y;
    /* Menu content starts to the right of T-Rex */
    int content_x = center_x - cfg->ui.content_offset_x;

    /* Draw T-Rex sprite on the left */
    menu_draw_trex(trex_x, trex_y);

    /* Title aligned with top of T-Rex */
    draw_text_color(content_x, trex_y, "T-Rex Runner", 0,
                    cfg->colors.menu_title.r, cfg->colors.menu_title.g,
                    cfg->colors.menu_title.b);

    /* Controls section */
    int controls_y = trex_y + 3;
    draw_text_color(content_x, controls_y, "Controls:", 0,
                    cfg->colors.menu_title.r, cfg->colors.menu_title.g,
                    cfg->colors.menu_title.b);
    draw_text_color(content_x + 2, controls_y + 1, "Jump: SPACE or UP", 0,
                    cfg->colors.menu_help.r, cfg->colors.menu_help.g,
                    cfg->colors.menu_help.b);
    draw_text_color(content_x + 2, controls_y + 2, "Crouch: DOWN", 0,
                    cfg->colors.menu_help.r, cfg->colors.menu_help.g,
                    cfg->colors.menu_help.b);
    draw_text_color(content_x + 2, controls_y + 3, "Quit: ESC or Q", 0,
                    cfg->colors.menu_help.r, cfg->colors.menu_help.g,
                    cfg->colors.menu_help.b);

    /* Menu options */
    int menu_y = controls_y + 5;

    for (int i = 0; i < MENU_NUMOPTIONS; i++) {
        int y_pos = menu_y + i * 2; /* Reduced spacing for compact layout */

        if (i == (int) selected_menu_id) {
            /* Selected item with simple highlight */
            draw_text_color(content_x - 2, y_pos, ">", 0,
                            cfg->colors.menu_title.r, cfg->colors.menu_title.g,
                            cfg->colors.menu_title.b);
            draw_text_color(content_x, y_pos, (char *) menu_options[i], 0,
                            cfg->colors.menu_selected.r,
                            cfg->colors.menu_selected.g,
                            cfg->colors.menu_selected.b);
        } else {
            /* Unselected item */
            draw_text_color(content_x, y_pos, (char *) menu_options[i], 0,
                            cfg->colors.menu_unselected.r,
                            cfg->colors.menu_unselected.g,
                            cfg->colors.menu_unselected.b);
        }
    }
}

void menu_handle_input(int key_code)
{
    switch (key_code) {
    case 10:
    case TUI_KEY_ENTER:
        menu_handle_selection(selected_menu_id);
        break;
    case TUI_KEY_UP:
        if (selected_menu_id != MENUID_START)
            selected_menu_id--;
        break;
    case TUI_KEY_DOWN:
        if (selected_menu_id != MENUID_EXIT)
            selected_menu_id++;
        break;
    }
}
