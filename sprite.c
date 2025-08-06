#include <stddef.h>
#include <stdint.h>

#include "private.h"
#include "trex.h"

/* RLE-compressed sprite data: [count, value, count, value, ...] */
static const uint8_t cactus_rle[] = {
    19, 0, 2, 1, 8, 0, 1, 1, 2, 0, 2, 1, 2, 0,  1, 1, 5, 0,  2, 1, 1, 0, 2,
    1,  1, 0, 2, 1, 6, 0, 6, 1, 9, 0, 2, 1, 11, 0, 2, 1, 11, 0, 2, 1, 5, 0,
};

static const uint8_t rock_rle[] = {
    7, 0, 2, 1, 5, 0, 7, 1, 2, 0, 10, 1,
};

static const uint8_t egg_rle[] = {
    5, 0, 3, 1, 8, 0, 7, 1, 6, 0, 7, 1, 5,
    0, 9, 1, 4, 0, 9, 1, 5, 0, 7, 1, 3, 0,
};

static const uint8_t pterodactyl_rle[] = {
    15, 0, 1,  1,  30, 0,  2,  1,  29, 0,  3,  1,  20, 0, 3,  1,  5,
    0,  4, 1,  18, 0,  53, 1,  19, 0,  21, 1,  12, 0,  5, 1,  28, 0,
    4,  1, 29, 0,  3,  1,  30, 0,  2,  1,  31, 0,  1,  1, 16, 0,
};

/* T-Rex sprites compressed */
static const uint8_t trex_normal_rle[] = {
    10, 0, 8, 1, 13, 0, 3,  1, 1,  0, 7, 1, 11, 0, 11, 1, 11, 0, 6,  1,
    16, 0, 9, 1, 4,  0, 1,  1, 8,  0, 7, 1, 6,  0, 2,  1, 5,  0, 11, 1,
    4,  0, 3, 1, 3,  0, 10, 1, 2,  0, 1, 1, 3,  0, 15, 1, 9,  0, 11, 1,
    12, 0, 8, 1, 15, 0, 4,  1, 2,  0, 2, 1, 14, 0, 2,  1, 4,  0, 1,  1,
    15, 0, 1, 1, 5,  0, 1,  1, 15, 0, 2, 1, 4,  0, 2,  1, 10, 0,
};

static const uint8_t trex_duck_rle[] = {
    201, 0, 6, 1, 3,  0, 1, 1, 17, 0, 5, 1, 1,  0, 5, 1, 1, 0, 3,  1, 6,  0,
    47,  1, 6, 0, 17, 1, 2, 0, 8,  1, 4, 0, 10, 1, 4, 0, 1, 1, 16, 0, 2,  1,
    4,   0, 1, 1, 23, 0, 1, 1, 5,  0, 1, 1, 23, 0, 2, 1, 4, 0, 2,  1, 18, 0,
};

/* Pre-compute sprite data buffers */
static int cactus_data[104], rock_data[33], egg_data[78], pterodactyl_data[384],
    trex_normal_data[330], trex_duck_data[450];

static void decompress_rle_to_buffer(const uint8_t *rle,
                                     size_t rle_size,
                                     int *buf)
{
    int pos = 0;
    for (size_t i = 0; i < rle_size; i += 2) {
        for (int j = 0; j < rle[i]; j++)
            buf[pos++] = rle[i + 1];
    }
}

static void decompress_sprite_data(void)
{
    static bool initialized = false;
    if (initialized)
        return;

    decompress_rle_to_buffer(cactus_rle, sizeof(cactus_rle), cactus_data);
    decompress_rle_to_buffer(rock_rle, sizeof(rock_rle), rock_data);
    decompress_rle_to_buffer(egg_rle, sizeof(egg_rle), egg_data);
    decompress_rle_to_buffer(pterodactyl_rle, sizeof(pterodactyl_rle),
                             pterodactyl_data);
    decompress_rle_to_buffer(trex_normal_rle, sizeof(trex_normal_rle),
                             trex_normal_data);
    decompress_rle_to_buffer(trex_duck_rle, sizeof(trex_duck_rle),
                             trex_duck_data);

    initialized = true;
}

/* Initialize sprites - call before using any sprites */
void sprites_init(void)
{
    decompress_sprite_data();
}

/* Sprite descriptors with lazy initialization */
const sprite_t sprite_cactus = {
    .data = cactus_data,
    .rows = 8,
    .cols = 13,
};
const sprite_t sprite_rock = {
    .data = rock_data,
    .rows = 3,
    .cols = 11,
};
const sprite_t sprite_egg = {
    .data = egg_data,
    .rows = 6,
    .cols = 13,
};
const sprite_t sprite_pterodactyl = {
    .data = pterodactyl_data,
    .rows = 12,
    .cols = 32,
};
const sprite_t sprite_trex_normal = {
    .data = trex_normal_data,
    .rows = 15,
    .cols = 22,
};
const sprite_t sprite_trex_duck = {
    .data = trex_duck_data,
    .rows = 15,
    .cols = 30,
};
