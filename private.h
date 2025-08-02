#pragma once

/*
 * This header contains private implementation details that should not
 * be exposed in the public API. Include this only in implementation files.
 */

#include "trex.h"

/* Menu option IDs */
typedef enum {
    MENUID_START = 0,
    MENUID_EXIT = 1,
} menu_id_t;

/* Menu functions */
void menu_update(double elapsed);
void menu_render();
void menu_handle_input(int input);
void menu_handle_selection(menu_id_t menu);

/* Sprite descriptor structure */
typedef struct {
    const int *data;
    int rows, cols;
} sprite_t;

/* Sprite descriptors */
extern const sprite_t sprite_cactus;
extern const sprite_t sprite_rock;
extern const sprite_t sprite_egg;
extern const sprite_t sprite_pterodactyl;
extern const sprite_t sprite_trex_normal;
extern const sprite_t sprite_trex_duck;

/* Get sprite pixel at position */
static inline int sprite_get_pixel(const sprite_t *sprite, int row, int col)
{
    if (row < 0 || row >= sprite->rows || col < 0 || col >= sprite->cols)
        return 0;
    return sprite->data[row * sprite->cols + col];
}
