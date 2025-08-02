#include "private.h"
#include "trex.h"

static screen_type_t current_screen = SCREEN_MENU, previous_screen;

static double last_update_time = 0.0;

double state_get_time_ms()
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;

    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

void state_initialize()
{
    /* Create enhanced color pairs for the game */
    tui_init_pair(1, TUI_COLOR_GREEN,
                  TUI_COLOR_BLACK); /* Logo - keep existing green */
    tui_init_pair(2, TUI_COLOR_CYAN, TUI_COLOR_BLACK);   /* Accent elements */
    tui_init_pair(3, TUI_COLOR_YELLOW, TUI_COLOR_BLACK); /* Highlights */
    tui_init_pair(4, TUI_COLOR_WHITE, TUI_COLOR_BLACK);  /* Main text */

    /* Initialize double buffering */
    draw_init_buffers();

    /* Initialize the World */
    play_init_world();
}

void state_update_frame()
{
    /* Check the active screen, and call its update */
    switch (current_screen) {
    case SCREEN_MENU:
        menu_update(TICKCOUNT - last_update_time);
        break;
    case SCREEN_WORLD:
        play_update_world(TICKCOUNT - last_update_time);
        break;
    default:
        break;
    }

    last_update_time = TICKCOUNT;
}

void state_render_frame()
{
    /* Clear the back buffer instead of clearing the screen directly */
    draw_clear_back_buffer();

    /* Check the active screen, and call its render */
    switch (current_screen) {
    case SCREEN_MENU:
        menu_render();
        break;
    case SCREEN_WORLD:
        play_render_world();
        break;
    }

    /* Swap buffers to display the rendered frame */
    draw_swap_buffers();
}

screen_type_t state_get_screen_type()
{
    return current_screen;
}

screen_type_t state_restore_screen_type()
{
    return previous_screen;
}

void state_set_screen_type(screen_type_t screen)
{
    previous_screen = current_screen;
    current_screen = screen;
    last_update_time = TICKCOUNT;

    if (screen == SCREEN_WORLD)
        play_init_world();
}

int state_get_resolution(int type)
{
    int height = tui_get_max_y(tui_stdscr);
    int width = tui_get_max_x(tui_stdscr);

    return type == 0 ? height : width;
}

void state_handle_input(int key_code)
{
    if (current_screen == SCREEN_WORLD) {
        switch (key_code) {
        case 'q':
        case 'Q':
            state_set_screen_type(SCREEN_MENU);
            break;
        }
    }

    switch (current_screen) {
    case SCREEN_MENU:
        menu_handle_input(key_code);
        break;
    case SCREEN_WORLD:
        play_handle_input(key_code);
        break;
    }
}

static bool is_game_running = true; /* 0 when closing the game */
void state_quit_game()
{
    is_game_running = false;
}

bool state_is_running()
{
    return is_game_running;
}
