#include <poll.h>
#include <unistd.h>

#include "trex.h"

int main()
{
    /* Get configuration */
    const game_config_t *cfg = ensure_cfg();

    /* Initialize sprites */
    sprites_init();

    /* Initialize TUI */
    tui_init();
    tui_raw();
    tui_set_nodelay(tui_stdscr, true);
    tui_set_keypad(tui_stdscr, true);
    tui_noecho();
    tui_set_cursor(0);
    tui_start_color();
    tui_cbreak();

    /* Initialize the game */
    state_initialize();

    double last_frame_time = state_get_time_ms();
    double accumulator = 0.0;

    /* While the game is active */
    while (state_is_running()) {
        /* Check for pending shutdown signals */
        if (tui_check_shutdown())
            break; /* This won't be reached, but for clarity */

        /* Check for pending resize signals */
        tui_check_resize();

        double current_time = state_get_time_ms();
        double delta_time = current_time - last_frame_time;
        last_frame_time = current_time;

        /* Accumulate time for frame rate limiting */
        accumulator += delta_time;

        /* Only update and render at target frame rate */
        if (accumulator >= cfg->timing.frame_time) {
            /* Process all available input events to reduce latency.
             * This prevents input lag when multiple keys are pressed quickly
             */
            int max_inputs = 8; /* Process up to 8 inputs per frame */
            while (max_inputs-- > 0 && tui_has_input()) {
                int ch = tui_getch();
                if (ch != -1)
                    state_handle_input(ch);
            }

            /* Update the game */
            state_update_frame();

            /* Render the game */
            state_render_frame();

            accumulator -= cfg->timing.frame_time;
        } else {
            /* Use poll() with 4ms timeout for low-latency input polling.
             * This matches the optimized tui_getch() implementation
             */
            struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};

            /* 4ms timeout for optimal latency vs CPU trade-off */
            poll(&pfd, 1, 4);
        }
    }

    /* Cleanup render buffers and colors */
    draw_cleanup_buffers();
    draw_cleanup_colors();

    /* Finalize TUI */
    tui_noraw();
    tui_set_cursor(1);
    tui_echo();
    tui_clear_screen();
    tui_cleanup();

    return 0;
}
