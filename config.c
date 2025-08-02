#include <stddef.h>

#include "trex.h"

/* Global configuration cache */
const game_config_t *g_cfg = NULL;

/* Static configuration instance */
static const game_config_t game_config = {
    /* Timing configuration */
    .timing =
        {
            .target_fps = 60,
            .frame_time = 16.67, /* 1000.0 / 60 */
            .update_ms = 10.0,
            .anim_ms = 150.0,
            .sleep_us = 1000,
        },

    /* Physics configuration */
    .physics =
        {
            .jump_height = 15,
            .fall_depth = -5,
            .bounds_buffer = 50,
        },

    /* Power-up configuration */
    .powerups = {.duration = 600.0, .duck_timeout = 500.0},

    /* Color configuration */
    .colors =
        {
            /* T-Rex colors */
            .trex_normal = {34, 177, 76},
            .trex_dead = {163, 163, 163},
            .trex_invincible = {30, 240, 180},
            .trex_fire = {240, 120, 30},

            /* Object colors */
            .cactus = {18, 117, 48},
            .rock = {110, 110, 110},
            .egg_base = {151, 178, 159},
            .pterodactyl = {50, 80, 50},
            .fireball = {255, 100, 0},

            /* Ground colors */
            .ground_normal_primary = {111, 74, 53},
            .ground_normal_secondary = {48, 99, 48},
            .ground_dead_primary = {178, 178, 178},
            .ground_dead_secondary = {100, 100, 100},
            .ground_speck = {255, 224, 51},

            /* UI colors */
            .menu_title = {100, 255, 150},
            .menu_selected = {255, 255, 255},
            .menu_unselected = {140, 140, 140},
            .menu_help = {200, 200, 200},
            .score_text = {255, 255, 255},
        },

    /* Scoring configuration */
    .scoring = {.fireball_kill = 15, .powerup_collect = 10, .per_frame = 1},

    /* Spatial collision configuration */
    .spatial = {.bucket_size = 64, .bucket_count = 32},

    /* Sprite dimensions */
    /* UI layout configuration */
    .ui =
        {
            .menu_options = 2,
            .menu_spacing = 2,
            .trex_offset_x = 35,
            .trex_offset_y = 8,
            .content_offset_x = 5,
        },

    /* Rendering configuration */
    .render =
        {
            .max_colors = 256,
            .text_base = 50,
            .block_base = 115,
            .text_bg_base = 200,
            .speck_interval_1 = 25,
            .speck_interval_2 = 36,
        },

    /* Game limits */
    .limits = {.max_level = 10, .max_objects = 100, .object_types = 6},
};

/* Level configuration data */
static const level_config_t level_configs[] = {
    {.level = 1, .spawn_min = 1200, .spawn_max = 3000, .score_next = 100},
    {.level = 2, .spawn_min = 1200, .spawn_max = 2500, .score_next = 150},
    {.level = 3, .spawn_min = 1000, .spawn_max = 2200, .score_next = 200},
    {.level = 4, .spawn_min = 1000, .spawn_max = 2000, .score_next = 250},
    {.level = 5, .spawn_min = 1000, .spawn_max = 1900, .score_next = 270},
    {.level = 6, .spawn_min = 1000, .spawn_max = 1800, .score_next = 300},
    {.level = 7, .spawn_min = 1000, .spawn_max = 1700, .score_next = 350},
    {.level = 8, .spawn_min = 800, .spawn_max = 1700, .score_next = 400},
    {.level = 9, .spawn_min = 800, .spawn_max = 1600, .score_next = 450},
    {.level = 10, .spawn_min = 800, .spawn_max = 1500, .score_next = 500},
};

/* Object probability configuration */
static const object_probability_t object_probabilities[] = {
    {OBJECT_CACTUS, 0, 3500},            /* 35% chance */
    {OBJECT_ROCK, 3501, 5500},           /* 20% chance */
    {OBJECT_PTERODACTYL, 5501, 7000},    /* 15% chance */
    {OBJECT_GROUND_HOLE, 7001, 9000},    /* 20% chance */
    {OBJECT_EGG_INVINCIBLE, 9001, 9500}, /* 5% chance */
    {OBJECT_EGG_FIRE, 9501, 10000},      /* 5% chance */
};

/* Player spawn configuration */
static const player_spawn_t player_spawn = {.x = 30, .y_offset = 5};

/* Configuration access functions */
const game_config_t *config_get(void)
{
    return &game_config;
}

const level_config_t *config_get_level(int level)
{
    if (level < 1 || level > game_config.limits.max_level)
        return &level_configs[0];
    return &level_configs[level - 1];
}

const object_probability_t *config_get_probs(void)
{
    return object_probabilities;
}

int config_get_prob_count(void)
{
    return sizeof(object_probabilities) / sizeof(object_probabilities[0]);
}

const player_spawn_t *config_get_spawn(void)
{
    return &player_spawn;
}
