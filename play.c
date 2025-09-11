#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "private.h"
#include "trex.h"

/* Forward declarations for spatial collision system */
static void collect_powerup(object_t *powerup);

/* Helper functions for collision detection */
static bool involves_ground_hole(object_t const *obj1, object_t const *obj2);
static bool is_player_enemy(object_t const *obj1, object_t const *obj2);
static bool is_player_powerup(object_t const *obj1, object_t const *obj2);

/* Spatial collision detection helpers */
typedef struct {
    int left, right, top, bottom;
} bounding_rect_t;

static bounding_rect_t get_bounds(const object_t *obj, bool is_player);
static bool bounds_overlap(const bounding_rect_t *rect1,
                           const bounding_rect_t *rect2);

/* Values used to initialize objects */
#define HEIGHT_ZERO 0

/* Jump buffer and coyote time constants (in milliseconds) */
#define JUMP_BUFFER_MS 120
#define COYOTE_TIME_MS 80

/* Duck hitbox adjustments */
#define DUCK_HITBOX_TOP_OFFSET 6
#define DUCK_HITBOX_RIGHT_EXTEND 10

/* Fast fall mechanics */
#define FAST_FALL_MULTIPLIER 2.5

/* Off-screen position for removed objects */
#define OFFSCREEN_X (RESOLUTION_COLS + 1)

/* Game state variables */
static int user_score = 0, distance = 0, current_level = 0;
static float powerup_time = -1, obstacle_time = -1;
static bool is_dead = false, is_falling_animation = false,
            can_throw_fireball = true;
static object_type_t powerup_type;
static double last_key_check_time = 0.0;
static object_t player;

/* Streak counter for consecutive aerial obstacle clears */
static int aerial_streak = 0;
static int max_streak = 0;
static bool was_airborne_last_frame = false;
static bool cleared_obstacle_while_airborne = false;

/* Jump buffer and coyote time state */
static double last_jump_keydown = 0.0;
static double left_ground_at = 0.0;

/* Fast-fall state */
static bool is_fast_falling = false;
static double fast_fall_multiplier = FAST_FALL_MULTIPLIER;
static double last_fast_fall_time = 0.0;

/*
 * Spatial collision detection optimization system
 *
 * This system divides the game world into horizontal buckets to reduce
 * collision detection complexity from O(n²) to approximately O(n).
 * Objects are placed into buckets based on their X coordinate, and
 * collision checks only occur between objects in nearby buckets.
 */
#define SPATIAL_BUCKET_SIZE 64  /* Each bucket covers 64 pixels horizontally */
#define SPATIAL_BUCKET_COUNT 32 /* Support screen width + some extra */
#define MAX_OBJECTS 100         /* Default - will use config */
#define SPATIAL_INVALID_INDEX -1

typedef struct spatial_node {
    object_t *object;
    struct spatial_node *next;
} spatial_node_t;

typedef struct {
    spatial_node_t **buckets;  /* Array of bucket heads */
    spatial_node_t *node_pool; /* Pre-allocated node pool */
    int bucket_count;          /* Number of buckets */
    int max_objects;           /* Maximum objects supported */
    int next_free_node;        /* Next available node in pool */
} spatial_hash_t;

static spatial_hash_t spatial_hash;

/**
 * Get spatial bucket index for x coordinate
 * @x : X coordinate to map to bucket
 *
 * Return bucket index for spatial hash, clamped to valid range
 */
static inline int spatial_get_bucket(int x)
{
    const game_config_t *cfg = ensure_cfg();

    /* Clamp negative coordinates to first bucket */
    if (x < 0)
        return 0;

    /* Calculate bucket index and clamp to maximum */
    int bucket = x / cfg->spatial.bucket_size;
    return (bucket >= spatial_hash.bucket_count) ? spatial_hash.bucket_count - 1
                                                 : bucket;
}

/**
 * Clear spatial hash (called each frame)
 *
 * Reset all buckets and node pool for next frame
 */
static void spatial_clear(void)
{
    const game_config_t *cfg = ensure_cfg();

    /* Initialize buckets on first use if needed */
    if (!spatial_hash.buckets) {
        spatial_hash.bucket_count = cfg->spatial.bucket_count;
        spatial_hash.buckets =
            calloc(spatial_hash.bucket_count, sizeof(spatial_node_t *));
        spatial_hash.node_pool =
            calloc(cfg->limits.max_objects, sizeof(spatial_node_t));
        spatial_hash.max_objects = cfg->limits.max_objects;
    }

    for (int i = 0; i < spatial_hash.bucket_count; i++) {
        spatial_hash.buckets[i] = NULL;
    }
    spatial_hash.next_free_node = 0;
}

/**
 * Add object to spatial hash for collision detection optimization
 * @object : Object to add to the spatial hash
 *
 * Objects are bucketed by X coordinate for faster collision queries
 */
static void spatial_add_object(object_t *object)
{
    const game_config_t *cfg = ensure_cfg();

    /* Early validation checks */
    if (!object || !spatial_hash.buckets || !spatial_hash.node_pool)
        return;

    /* Skip objects that are completely off-screen to reduce collision workload
     */
    if (object->x + object->cols < -cfg->physics.bounds_buffer ||
        object->x > RESOLUTION_COLS + cfg->physics.bounds_buffer)
        return;

    /* Prevent node pool overflow */
    if (spatial_hash.next_free_node >= spatial_hash.max_objects)
        return;

    int bucket_idx = spatial_get_bucket(object->x);

    /* Defensive bounds check */
    if (bucket_idx < 0 || bucket_idx >= spatial_hash.bucket_count)
        return;

    /* Allocate node from pool and link to bucket */
    spatial_node_t *node =
        &spatial_hash.node_pool[spatial_hash.next_free_node++];
    node->object = object;
    node->next = spatial_hash.buckets[bucket_idx];
    spatial_hash.buckets[bucket_idx] = node;
}

/**
 * Find closest object to fireball using spatial queries
 * @fireball : Fireball object to find target for
 *
 * Return closest targetable object or NULL if none found
 */
static object_t *find_closest_target(object_t const *fireball)
{
    if (!fireball || !spatial_hash.buckets)
        return NULL;

    object_t *closest = NULL;
    int min_distance = RESOLUTION_COLS;

    /* Check current bucket and adjacent buckets */
    int fireball_bucket = spatial_get_bucket(fireball->x);
    for (int bucket_offset = -1; bucket_offset <= 1; bucket_offset++) {
        int bucket_idx = fireball_bucket + bucket_offset;
        if (bucket_idx < 0 || bucket_idx >= spatial_hash.bucket_count)
            continue;

        for (spatial_node_t *node = spatial_hash.buckets[bucket_idx];
             node != NULL; node = node->next) {
            object_t *obj = node->object;
            if (obj && obj->x < min_distance &&
                obj->type < OBJECT_EGG_INVINCIBLE) {
                closest = obj;
                min_distance = obj->x;
            }
        }
    }

    return closest;
}


/**
 * Perform collision check between two specific objects
 * @obj1 : First object in collision check
 * @obj2 : Second object in collision check
 *
 * Check for overlap and handle collision effects if detected
 */
static void spatial_collision_check_pair(object_t *obj1, object_t *obj2)
{
    const game_config_t *cfg = ensure_cfg();
    if (obj1 == obj2)
        return;

    /* Get bounding rectangles for collision detection */
    bounding_rect_t bounds1, bounds2;

    /* Apply special duck adjustments for player vs non-ground-hole collisions
     */
    if (obj1 == &player && !involves_ground_hole(obj1, obj2)) {
        bounds1 = get_bounds(obj1, true);
        bounds2 = get_bounds(obj2, false);
    } else if (obj2 == &player && !involves_ground_hole(obj1, obj2)) {
        bounds1 = get_bounds(obj1, false);
        bounds2 = get_bounds(obj2, true);
    } else {
        /* Standard bounds for non-player or ground hole collisions */
        bounds1 = get_bounds(obj1, false);
        bounds2 = get_bounds(obj2, false);
    }

    /* Check for collision */
    if (!bounds_overlap(&bounds1, &bounds2))
        return; /* No collision detected */

    /* Handle collision effects based on object types */

    /* Fireball vs enemy collisions */
    if (obj1->type == OBJECT_FIRE_BALL && obj2->type < OBJECT_EGG_INVINCIBLE) {
        obj1->x = OFFSCREEN_X; /* Move fireball off-screen */
        obj2->x = OFFSCREEN_X; /* Move target off-screen */
        user_score += cfg->scoring.fireball_kill;
        return;
    }
    if (obj2->type == OBJECT_FIRE_BALL && obj1->type < OBJECT_EGG_INVINCIBLE) {
        obj2->x = OFFSCREEN_X; /* Move fireball off-screen */
        obj1->x = OFFSCREEN_X; /* Move target off-screen */
        user_score += cfg->scoring.fireball_kill;
        return;
    }

    /* Player vs enemy collisions */
    if (is_player_enemy(obj1, obj2)) {
        object_t const *enemy = (obj1 == &player) ? obj2 : obj1;

        if (enemy->type == OBJECT_GROUND_HOLE) {
            player.state = STATE_FALLING;
            is_falling_animation = true;
        } else if (powerup_time > 0.0f &&
                   powerup_type == OBJECT_EGG_INVINCIBLE) {
            /* Player is invincible, ignore collision */
        } else {
            play_kill_player();
        }
        return;
    }

    /* Player vs powerup collisions */
    if (is_player_powerup(obj1, obj2)) {
        object_t *powerup = (obj1 == &player) ? obj2 : obj1;
        collect_powerup(powerup);
    }
}

/**
 * Handle power-up collection
 * @powerup : Power-up object that was collected
 *
 * Apply power-up effects and remove from game
 */
static void collect_powerup(object_t *powerup)
{
    const game_config_t *cfg = ensure_cfg();

    if (powerup->type == OBJECT_EGG_INVINCIBLE) {
        powerup_time = cfg->powerups.duration;
        powerup_type = OBJECT_EGG_INVINCIBLE;
        powerup->x = OFFSCREEN_X;
        user_score += cfg->scoring.powerup_collect;
    } else if (powerup->type == OBJECT_EGG_FIRE) {
        powerup_time = cfg->powerups.duration;
        powerup_type = OBJECT_EGG_FIRE;
        powerup->x = OFFSCREEN_X;
        user_score += cfg->scoring.powerup_collect;
    }
}

/* Array to store the objects in the game - allocated dynamically */
static object_t **objects = NULL;

/**
 * Generate a random object type based on probability
 * @b_generate_egg : Whether egg generation is allowed
 *
 * Return randomly selected object type
 */
object_type_t play_random_object(bool b_generate_egg)
{
    /* Generate random value between 1 and 10000 with overflow protection */
    long rand_val = random();
    int random_value = (int) (rand_val % 10000) + 1;

    const object_probability_t *probs = config_get_probs();
    int count = config_get_prob_count();

    for (int i = 0; i < count; i++) {
        /* Is the generated value within range of any object? */
        if (random_value >= probs[i].range_start &&
            random_value < probs[i].range_end) {
            /* Generated an egg but shouldn't have? Then generate again */
            if (probs[i].object_type >= OBJECT_EGG_INVINCIBLE &&
                !b_generate_egg)
                return play_random_object(b_generate_egg);
            return probs[i].object_type;
        }
    }

    return OBJECT_CACTUS;
}

int play_find_free_slot()
{
    const game_config_t *cfg = ensure_cfg();
    if (!objects)
        return -1;

    for (int i = 0; i < cfg->limits.max_objects; ++i) {
        if (!objects[i])
            return i;
    }

    return -1;
}

void play_cleanup_objects()
{
    const game_config_t *cfg = ensure_cfg();
    if (!objects)
        return;

    for (int i = 0; i < cfg->limits.max_objects; ++i) {
        if (objects[i]) {
            free(objects[i]);
            objects[i] = NULL;
        }
    }
}

/**
 * Helper function to determine T-Rex color based on current game state
 * Considers death state and active power-ups to select appropriate colors
 * @r : Pointer to store red component (0-255)
 * @g : Pointer to store green component (0-255)
 * @b : Pointer to store blue component (0-255)
 */
static void get_trex_color(short *r, short *g, short *b)
{
    const game_config_t *cfg = ensure_cfg();

    *r = cfg->colors.trex_normal.r;
    *g = cfg->colors.trex_normal.g;
    *b = cfg->colors.trex_normal.b;

    if (is_dead) {
        *r = cfg->colors.trex_dead.r;
        *g = cfg->colors.trex_dead.g;
        *b = cfg->colors.trex_dead.b;
        return;
    }

    if (powerup_time > 0.0f) {
        if (powerup_type == OBJECT_EGG_INVINCIBLE) {
            *r = cfg->colors.trex_invincible.r;
            *g = cfg->colors.trex_invincible.g;
            *b = cfg->colors.trex_invincible.b;
        } else if (powerup_type == OBJECT_EGG_FIRE) {
            *r = cfg->colors.trex_fire.r;
            *g = cfg->colors.trex_fire.g;
            *b = cfg->colors.trex_fire.b;
        }
    }
}

/**
 * Get bounding rectangle for any object
 * @obj : Object to get bounds for
 *
 * Return bounding rectangle in world coordinates
 */
/**
 * Get object bounding rectangle with optional player adjustments
 * @obj : Object to get bounds for
 * @is_player : If true, apply player-specific duck adjustments
 *
 * Return bounding rectangle, with duck adjustments if is_player and ducking
 */
static bounding_rect_t get_bounds(const object_t *obj, bool is_player)
{
    bounding_rect_t bounds = {0};
    if (!obj)
        return bounds;

    bounds.left = obj->x + obj->bounding_box.x;
    bounds.right = bounds.left + obj->bounding_box.width;
    bounds.top = obj->y - obj->height + obj->bounding_box.y;
    bounds.bottom = bounds.top + obj->bounding_box.height;

    /* Apply player-specific duck adjustments */
    if (is_player && obj->state == STATE_DUCK) {
        bounds.top += DUCK_HITBOX_TOP_OFFSET;
        bounds.right += DUCK_HITBOX_RIGHT_EXTEND;
    }

    return bounds;
}

/**
 * Check if two bounding rectangles overlap
 * @rect1 : First bounding rectangle
 * @rect2 : Second bounding rectangle
 *
 * Return true if rectangles overlap
 */
static bool bounds_overlap(const bounding_rect_t *rect1,
                           const bounding_rect_t *rect2)
{
    return !(rect1->left >= rect2->right || rect2->left >= rect1->right ||
             rect1->top >= rect2->bottom || rect2->top >= rect1->bottom);
}

/**
 * Check if either object in collision pair is a ground hole
 * @obj1 : First collision object
 * @obj2 : Second collision object
 *
 * Return true if either object is a ground hole
 */
static bool involves_ground_hole(object_t const *obj1, object_t const *obj2)
{
    if (!obj1 || !obj2)
        return false;

    return (obj1->type == OBJECT_GROUND_HOLE ||
            obj2->type == OBJECT_GROUND_HOLE);
}

/**
 * Check if collision is between player and an enemy
 * @obj1 : First collision object
 * @obj2 : Second collision object
 *
 * Return true if one is player and other is enemy
 */
static bool is_player_enemy(object_t const *obj1, object_t const *obj2)
{
    return ((obj1 == &player && obj2->enemy) ||
            (obj2 == &player && obj1->enemy));
}

/**
 * Check if collision is between player and a powerup
 * @obj1 : First collision object
 * @obj2 : Second collision object
 *
 * Return true if one is player and other is powerup (non-enemy)
 */
static bool is_player_powerup(object_t const *obj1, object_t const *obj2)
{
    if (!obj1 || !obj2)
        return false;

    return ((obj1 == &player && !obj2->enemy) ||
            (obj2 == &player && !obj1->enemy));
}

/**
 * Check if player is currently on the ground
 *
 * Return true if player is grounded (running or ducking)
 */
static bool is_player_on_ground(void)
{
    return (player.state == STATE_RUNNING || player.state == STATE_DUCK) &&
           player.height <= 0;
}

/* Record jump key press for buffering */
static void on_keydown_jump(void)
{
    last_jump_keydown = TICKCOUNT;
}

/* Attempt to execute a jump with buffer and coyote time */
static void try_jump(void)
{
    if (is_dead || is_falling_animation)
        return;

    bool grounded_now = is_player_on_ground();
    double current_time = TICKCOUNT;

    if (grounded_now) {
        left_ground_at = 0.0;
    } else if (left_ground_at == 0.0) {
        left_ground_at = current_time;
    }

    bool buffered = (current_time - last_jump_keydown) < JUMP_BUFFER_MS;
    bool in_coyote = (left_ground_at > 0.0) &&
                     (current_time - left_ground_at) < COYOTE_TIME_MS;

    if (buffered && (grounded_now || in_coyote)) {
        player.state = STATE_JUMPING;
        player.frame = 0;
        last_jump_keydown = 0.0;
        is_fast_falling = false; /* Reset fast-fall when starting new jump */
    }
}

/* Helper function to render T-Rex object */
static void render_trex(const object_t *object)
{
    /* Get appropriate T-Rex color based on game state */
    short s_color_r, s_color_g, s_color_b;
    get_trex_color(&s_color_r, &s_color_g, &s_color_b);

    /* Select appropriate sprite based on state */
    const sprite_t *sprite =
        (object->state == STATE_DUCK) ? &sprite_trex_duck : &sprite_trex_normal;

    /* Draw T-Rex using sprite data */
    for (int i = 0; i < sprite->rows; i++) {
        for (int j = 0; j < sprite->cols; j++) {
            if (!sprite_get_pixel(sprite, i, j))
                continue;

            draw_block_color(object->x + j, object->y + i - object->height, 1,
                             1, s_color_r, s_color_g, s_color_b);
        }
    }

    /* Skip leg animation if ducking or not animated */
    if (object->state == STATE_DUCK || object->frame == 0)
        return;

    /* Leg animation data: x_offset, y_offset, width, height */
    static const int leg_frames[][5][4] = {
        [1] =
            {
                {4, 12, 2, 1},
                {10, 12, 1, 1},
                {5, 13, 3, 1},
                {10, 13, 1, 1},
                {10, 14, 2, 1},
            },
        [2] =
            {
                {4, 12, 2, 1},
                {10, 12, 1, 1},
                {4, 13, 1, 1},
                {10, 13, 3, 1},
                {4, 14, 2, 1},
            },
    };

    /* Draw animated legs */
    if (object->frame == 1 || object->frame == 2) {
        const int (*rects)[4] = leg_frames[object->frame];
        for (int i = 0; i < 5; i++) {
            draw_block_color(object->x + rects[i][0],
                             object->y + rects[i][1] - object->height,
                             rects[i][2], rects[i][3], s_color_r, s_color_g,
                             s_color_b);
        }
    }
}

/* Helper function to render sprite-based objects */
static void render_sprite_object(const object_t *object,
                                 const sprite_t *sprite,
                                 short r,
                                 short g,
                                 short b)
{
    for (int i = 0; i < object->rows; ++i) {
        for (int j = 0; j < object->cols; ++j) {
            if (!sprite_get_pixel(sprite, i, j))
                continue;

            draw_block_color(object->x + j, object->y + i - object->height, 1,
                             1, r, g, b);
        }
    }
}

/* Render ground hole */
static void render_ground_hole(const object_t *object)
{
    draw_block(object->x, object->y - object->height, object->cols,
               object->rows, TUI_COLOR_PAIR(1));
    short r = is_dead ? 178 : 182;
    short g = is_dead ? 178 : 122;
    short b = is_dead ? 178 : 87;
    draw_block_color(object->x - 2, object->y - object->height, 2, 5, r, g, b);
    draw_block_color(object->x + object->cols, object->y - object->height, 2, 5,
                     r, g, b);
}

/* Render fireball */
static void render_fireball(const object_t *object)
{
    draw_block_color(object->x, object->y - object->height, 2, 1,
                     is_dead ? 178 : 182, is_dead ? 178 : 122,
                     is_dead ? 178 : 87);
}

/* Get egg colors based on type and frame */
static void get_egg_colors(const object_t *object, short *r, short *g, short *b)
{
    const game_config_t *cfg = ensure_cfg();
    *r = cfg->colors.egg_base.r;
    *g = cfg->colors.egg_base.g;
    *b = cfg->colors.egg_base.b;

    if (object->type == OBJECT_EGG_INVINCIBLE) {
        if (object->frame == 1) {
            *r = 234;
            *g = 227;
            *b = 170;
        } else if (object->frame == 2) {
            *r = 234;
            *g = 212;
            *b = 64;
        }
    } else if (object->type == OBJECT_EGG_FIRE) {
        if (object->frame == 1) {
            *r = 255;
            *g = 170;
            *b = 80;
        } else if (object->frame == 2) {
            *r = 200;
            *g = 65;
            *b = 40;
        }
    }

    if (is_dead) {
        *r = 170;
        *g = 170;
        *b = 170;
    }
}

void play_render_object(object_t const *object)
{
    if (!object)
        return;

    const game_config_t *cfg = ensure_cfg();

    /* Handle T-Rex rendering */
    if (object->type == OBJECT_TREX) {
        render_trex(object);
        return;
    }

    /* Handle ground hole rendering */
    if (object->type == OBJECT_GROUND_HOLE) {
        render_ground_hole(object);
        return;
    }

    /* Handle fireball rendering */
    if (object->type == OBJECT_FIRE_BALL) {
        render_fireball(object);
        return;
    }

    /* Handle sprite-based objects */
    short r, g, b;
    const sprite_t *sprite = NULL;

    switch (object->type) {
    case OBJECT_CACTUS:
        r = is_dead ? 130 : cfg->colors.cactus.r;
        g = is_dead ? 130 : cfg->colors.cactus.g;
        b = is_dead ? 130 : cfg->colors.cactus.b;
        sprite = &sprite_cactus;
        break;

    case OBJECT_ROCK:
        r = cfg->colors.rock.r;
        g = cfg->colors.rock.g;
        b = cfg->colors.rock.b;
        sprite = &sprite_rock;
        break;

    case OBJECT_EGG_INVINCIBLE:
    case OBJECT_EGG_FIRE:
        get_egg_colors(object, &r, &g, &b);
        sprite = &sprite_egg;
        break;

    case OBJECT_PTERODACTYL:
        r = is_dead ? 90 : cfg->colors.pterodactyl.r;
        g = is_dead ? 90 : cfg->colors.pterodactyl.g;
        b = is_dead ? 90 : cfg->colors.pterodactyl.b;
        sprite = &sprite_pterodactyl;
        break;

    default:
        return;
    }

    /* Render the sprite if found */
    if (sprite)
        render_sprite_object(object, sprite, r, g, b);
}

/**
 * Creates and adds a new game object at the specified position
 * @x : X coordinate for the new object
 * @y : Y coordinate for the new object
 * @type : Type of object to create (must be valid object type)
 */
void play_add_object(int x, int y, object_type_t type)
{
    const game_config_t *cfg = ensure_cfg();

    /* Validate input parameters */
    if ((int) type < 0 || (int) type >= cfg->limits.object_types)
        return; /* Invalid object type */

    /* Search for a free position within the array */
    int i_index = play_find_free_slot();

    /* If the returned position was different from -1, it means there is space
     */
    if (i_index != -1) {
        object_t *object = malloc(sizeof(object_t));
        if (!object)
            return; /* Allocation failed - silently fail */

        object->x = x;
        object->y = y;
        object->type = type;
        play_init_object(object);

        /* Store the created object in the array */
        objects[i_index] = object;
    }
}

/* Object initialization data structure */
typedef struct {
    const sprite_t *sprite;
    int max_frames;
    int bbox_x, bbox_y, bbox_width, bbox_height;
    int y_adjust;
    bool enemy;
} object_init_t;

/* Object initialization lookup table with sprite references */
static const object_init_t object_init_data[] = {
    [OBJECT_TREX] = {&sprite_trex_normal, 3, 8, 0, 6, 13, 0, false},
    [OBJECT_CACTUS] = {&sprite_cactus, 1, 1, -1, 10, 10, 0, true},
    [OBJECT_ROCK] = {&sprite_rock, 1, 2, 0, 6, 3, 0, true},
    [OBJECT_EGG_INVINCIBLE] = {&sprite_egg, 3, 2, 2, 8, 3, 0, false},
    [OBJECT_EGG_FIRE] = {&sprite_egg, 3, 2, 2, 8, 3, 0, false},
    [OBJECT_PTERODACTYL] = {&sprite_pterodactyl, 1, 16, 0, 1, 12, -12, true},
    [OBJECT_GROUND_HOLE] = {NULL, 1, 14, -3, 2, 15, 5, true},
    [OBJECT_FIRE_BALL] = {NULL, 1, 0, 0, 2, 1, 0, false},
};

void play_init_object(object_t *object)
{
    if (!object || object->type < 0 || object->type > OBJECT_FIRE_BALL)
        return;

    const object_init_t *data = &object_init_data[object->type];

    /* Set sprite dimensions */
    if (data->sprite) {
        object->cols = data->sprite->cols;
        object->rows = data->sprite->rows;
    } else {
        /* Ground hole and fireball have hardcoded dimensions */
        object->cols = (object->type == OBJECT_GROUND_HOLE) ? 21 : 2;
        object->rows = (object->type == OBJECT_GROUND_HOLE) ? 5 : 1;
    }

    object->height = HEIGHT_ZERO;
    object->max_frames = data->max_frames;
    object->enemy = data->enemy;
    object->y += data->y_adjust;

    /* Set bounding box */
    object->bounding_box.x = data->bbox_x;
    object->bounding_box.y = data->bbox_y;
    object->bounding_box.width = data->bbox_width;
    object->bounding_box.height = data->bbox_height;

    /* Final y adjustment */
    object->y -= object->rows;
}

void play_kill_player()
{
    is_dead = true;
}

void play_init_world()
{
    /* Initialize configuration on first use */
    const game_config_t *cfg = ensure_cfg();

    /* Initialize random number generator once */
    static bool rng_initialized = false;
    if (!rng_initialized) {
        srand(time(NULL));
        rng_initialized = true;
    }

    /* Allocate objects array if needed */
    if (!objects)
        objects = calloc(cfg->limits.max_objects, sizeof(object_t *));

    /* Clear the array that stores the objects */
    play_cleanup_objects();

    /* Reset game settings */
    const level_config_t *level = config_get_level(current_level + 1);
    obstacle_time =
        level->spawn_min + (random() % (level->spawn_max - level->spawn_min));
    const player_spawn_t *spawn = config_get_spawn();
    player.x = spawn->x;
    player.y = RESOLUTION_ROWS - spawn->y_offset;
    player.type = OBJECT_TREX;
    player.state = STATE_JUMPING;
    player.rows = 15; /* T-Rex dimensions are fixed */
    player.cols = 22;
    player.height = 0;     /* Starting height offset */
    player.frame = 0;      /* Animation frame */
    player.max_frames = 3; /* T-Rex has 3 animation frames */

    current_level = 0;
    user_score = 0;
    distance = 0;
    is_falling_animation = false;
    is_dead = false;

    /* Reset jump buffer and coyote time state */
    last_jump_keydown = 0.0;
    left_ground_at = 0.0;

    /* Reset streak counters */
    aerial_streak = 0;
    max_streak = 0;
    was_airborne_last_frame = false;
    cleared_obstacle_while_airborne = false;

    /* Initialize the player again */
    play_init_object(&player);
}

void play_adjust_for_resize()
{
    const player_spawn_t *spawn = config_get_spawn();
    int new_player_y = RESOLUTION_ROWS - spawn->y_offset;

    /* Adjust player position to maintain relative position on screen */
    if (new_player_y > 0)
        player.y = new_player_y;

    /* Clean up objects that are now outside screen bounds */
    if (objects) {
        const game_config_t *cfg = ensure_cfg();
        for (int i = 0; i < cfg->limits.max_objects; i++) {
            if (objects[i] &&
                /* Remove objects that are now way outside the screen bounds */
                (objects[i]->y < -50 || objects[i]->y > RESOLUTION_ROWS + 50 ||
                 objects[i]->x < -100 ||
                 objects[i]->x > RESOLUTION_COLS + 100)) {
                free(objects[i]);
                objects[i] = NULL;
            }
        }
    }
}

void play_update_world(double elapsed)
{
    const game_config_t *cfg = ensure_cfg();
    if (!objects)
        return;

    static double f_time_10ms = 0.0f;
    static double f_time_150ms = 0.0f;
    static double f_time_random = 0.0f;

    f_time_10ms += elapsed;
    f_time_150ms += elapsed;
    f_time_random += elapsed;

    /* If using any powerup, decrease its total time */
    if (powerup_time > 0.0f)
        powerup_time -= elapsed;

    /* Update the game state if the player hasn't died yet */
    if (!is_dead) {
        /* Try to execute any buffered or coyote time jumps */
        try_jump();

        /* Check if fast-fall should be disabled (key timeout) */
        if (is_fast_falling &&
            (player.state == STATE_JUMPING || player.state == STATE_FALLING)) {
            /* Stop fast-falling if key hasn't been pressed recently */
            if (TICKCOUNT - last_fast_fall_time > 50) /* 50ms timeout */
                is_fast_falling = false;
        }

        /* Check if the player is still pressing the key to duck */
        if (player.state == STATE_DUCK) {
            /* If still pressing, set state as ducking, otherwise as running */
            if (TICKCOUNT - last_key_check_time < cfg->powerups.duck_timeout) {
                player.state = STATE_DUCK;
                can_throw_fireball = false;
            } else {
                player.state = STATE_RUNNING;
                can_throw_fireball = true;
            }
        }

        /* Generate obstacles randomly */
        if (f_time_random >= obstacle_time) {
            object_type_t object_type =
                play_random_object(powerup_time > 0.0f ? false : true);
            play_add_object(RESOLUTION_COLS, RESOLUTION_ROWS - 5, object_type);

            const level_config_t *level = config_get_level(current_level + 1);
            obstacle_time = level->spawn_min +
                            (random() % (level->spawn_max - level->spawn_min));
            f_time_random = 0.0f;
        }

        /* Check if 10MS have passed since the last Update */
        if (f_time_10ms >= cfg->timing.update_ms) {
            /* If it has passed, reset the variable that stores the total time
             */
            f_time_10ms = 0.0f;

            /* Update the player object according to the animation, jumping or
               falling */
            if (player.state == STATE_JUMPING) {
                player.height += 1;

                /* If reached maximum height, make him fall */
                if (player.height > cfg->physics.jump_height)
                    player.state = STATE_FALLING;
            } else if (player.state == STATE_FALLING) {
                /* Apply fast-fall multiplier if holding down */
                int fall_speed =
                    is_fast_falling ? (int) (1 * fast_fall_multiplier) : 1;
                player.height -= fall_speed;

                /* If reached the ground, change to running animation and reset
                 * variables
                 */
                if (player.height <= 0 && !is_falling_animation) {
                    player.state = STATE_RUNNING;
                    player.frame = 0;
                    player.height = 0;
                    is_fast_falling = false; /* Reset fast-fall on landing */
                } else if (is_falling_animation &&
                           player.height <
                               cfg->physics.fall_depth - player.rows)
                    play_kill_player();
            }

            if (!is_falling_animation) {
                /* Increment the distance traveled */
                distance += current_level > 7 ? 2 : 1;

                /* Clear spatial hash for this frame */
                spatial_clear();

                /* Update other game objects besides the player */
                for (int i = 0; i < cfg->limits.max_objects; ++i) {
                    object_t *object = objects[i];

                    /* Valid pointer? */
                    if (object) {
                        /* If the object is a fireball, increment its x,
                           otherwise decrement */
                        if (object->type == OBJECT_FIRE_BALL)
                            object->x += current_level > 7 ? 2 : 1;
                        else
                            object->x -= current_level > 7 ? 2 : 1;

                        /* Add object to spatial hash for collision detection */
                        spatial_add_object(object);
                    }
                }

                /* Add player to spatial hash */
                spatial_add_object(&player);

                /* Perform collision detection using spatial queries */
                for (int i = 0; i < cfg->limits.max_objects; ++i) {
                    object_t *object = objects[i];
                    if (!object)
                        continue;

                    /* Objects are already filtered by spatial_add_object */

                    if (object->type == OBJECT_FIRE_BALL) {
                        /* Fireballs seek targets using spatial hash
                         * optimization */
                        object_t *target = find_closest_target(object);
                        if (target)
                            spatial_collision_check_pair(object, target);
                    } else {
                        /* All other objects check collision with player */
                        spatial_collision_check_pair(object, &player);
                    }
                }

                /* Track if player is airborne this frame */
                bool is_airborne = (player.state == STATE_JUMPING ||
                                    player.state == STATE_FALLING);

                /* Process objects for scoring and cleanup */
                for (int i = 0; i < cfg->limits.max_objects; ++i) {
                    object_t *object = objects[i];
                    if (!object)
                        continue;

                    /* Check if enemy obstacle just cleared the player */
                    bool just_passed = object->enemy &&
                                       object->x + object->cols < player.x &&
                                       object->x + object->cols >= player.x - 2;

                    if (just_passed && is_airborne) {
                        /* Player cleared obstacle while airborne */
                        cleared_obstacle_while_airborne = true;

                        /* Award streak bonus points */
                        if (aerial_streak > 0) {
                            int multiplier = aerial_streak + 1;
                            user_score += 10 * multiplier;
                        }
                    }

                    /* Remove objects that left the screen */
                    bool off_screen = object->x + object->cols < 0 ||
                                      (object->type == OBJECT_FIRE_BALL &&
                                       object->x > RESOLUTION_COLS);

                    if (off_screen) {
                        free(object);
                        objects[i] = NULL;

                        const level_config_t *level =
                            config_get_level(current_level + 1);
                        user_score += level->level;
                    }
                }

                /* Update streak based on landing/airborne state */
                if (was_airborne_last_frame && !is_airborne) {
                    /* Just landed */
                    if (cleared_obstacle_while_airborne) {
                        /* Successfully cleared obstacle(s) while airborne */
                        aerial_streak++;
                        if (aerial_streak > max_streak)
                            max_streak = aerial_streak;
                        cleared_obstacle_while_airborne = false;
                    } else if (aerial_streak > 0) {
                        /* Landed without clearing an obstacle - reset streak */
                        aerial_streak = 0;
                    }
                }

                was_airborne_last_frame = is_airborne;
            }
        }

        /* Check if 150MS have passed since the last Update */
        if (f_time_150ms > cfg->timing.anim_ms) {
            f_time_150ms = 0.0f;

            /* Update the dinosaur animation frame (0..2) */
            player.frame = (player.frame + 1) % player.max_frames;

            /* Update other game objects besides the player */
            for (int i = 0; i < cfg->limits.max_objects; ++i) {
                if (objects[i])
                    objects[i]->frame =
                        (objects[i]->frame + 1) % objects[i]->max_frames;
            }

            /* Update the User Score */
            user_score += cfg->scoring.per_frame;

            /* Update the level if it meets the condition */
            const level_config_t *level = config_get_level(current_level + 1);
            if (user_score >= level->score_next &&
                current_level != cfg->limits.max_level - 1) {
                current_level++;
            }
        }
    }
}

void play_render_world()
{
    const game_config_t *cfg = ensure_cfg();

    /* Safety check - ensure objects array is allocated */
    if (!objects)
        return;

    /* Draw ground layers */
    const rgb_color_t *primary = is_dead ? &cfg->colors.ground_dead_primary
                                         : &cfg->colors.ground_normal_primary;
    const rgb_color_t *secondary = is_dead
                                       ? &cfg->colors.ground_dead_secondary
                                       : &cfg->colors.ground_normal_secondary;

    draw_block_color(0, RESOLUTION_ROWS - 5, RESOLUTION_COLS, 1, primary->r,
                     primary->g, primary->b);
    draw_block_color(0, RESOLUTION_ROWS - 4, RESOLUTION_COLS, 3, secondary->r,
                     secondary->g, secondary->b);
    draw_block_color(0, RESOLUTION_ROWS - 1, RESOLUTION_COLS, 1, 0, 0, 0);

    /* Draw specks */
    const rgb_color_t *speck =
        is_dead ? &cfg->colors.ground_dead_primary : &cfg->colors.ground_speck;
    for (int i = 0; i < RESOLUTION_COLS; ++i) {
        if (((distance + i) % cfg->render.speck_interval_1) == 0)
            draw_text_bg(i, RESOLUTION_ROWS - 4, "_", TUI_A_BOLD, speck->r,
                         speck->g, speck->b, secondary->r, secondary->g,
                         secondary->b);

        if (((distance + i) % cfg->render.speck_interval_2) == 0)
            draw_text_bg(i, RESOLUTION_ROWS - 3, ".", TUI_A_BOLD, speck->r,
                         speck->g, speck->b, secondary->r, secondary->g,
                         secondary->b);
    }

    /* Draw other game objects */
    for (int i = 0; i < cfg->limits.max_objects; ++i) {
        object_t const *object = objects[i];

        /* Valid pointer? */
        if (object)
            play_render_object(object);
    }

    /* Draw the player (T-Rex dinosaur) */
    play_render_object(&player);

    /* Draw screen when the player died */
    if (is_dead) {
        static const char *death_text = "Failed";
        static const int death_text_len = 9;
        draw_text_color((RESOLUTION_COLS >> 1) - (death_text_len >> 1),
                        (RESOLUTION_ROWS >> 1) - 5, (char *) death_text,
                        TUI_A_BOLD, 255, 70, 70);

        char sz_user_score[32] = {0};
        int score_len = snprintf(sz_user_score, sizeof(sz_user_score),
                                 "Final Score: %d", user_score);
        draw_text_color((RESOLUTION_COLS >> 1) - (score_len >> 1),
                        (RESOLUTION_ROWS >> 1) - 4, sz_user_score, 0, 255, 255,
                        255);

        static const char *restart_text = "Press SPACE to restart!";
        static const int restart_text_len =
            23; /* Cache strlen("Press SPACE to restart!") */
        draw_text_color((RESOLUTION_COLS >> 1) - (restart_text_len >> 1),
                        (RESOLUTION_ROWS >> 1) - 2, (char *) restart_text, 0,
                        255, 255, 255);
    } else {
        /* Draw the player's user score */
        draw_text_color(RESOLUTION_COLS - 20, 2, "User Score", 0, 255, 255,
                        255);

        char sz_text[128] = {0};
        snprintf(sz_text, sizeof(sz_text), "%d", user_score);
        draw_text_color(RESOLUTION_COLS - 8, 2, sz_text, TUI_A_BOLD, 0, 255, 0);

        /* Draw streak counter if active */
        if (aerial_streak > 0) {
            snprintf(sz_text, sizeof(sz_text), "Streak: %dx",
                     aerial_streak + 1);
            /* Gold color for streak */
            draw_text_color(RESOLUTION_COLS - 20, 4, sz_text, TUI_A_BOLD, 255,
                            215, 0);
        }

        /* Draw max streak */
        if (max_streak > 0) {
            snprintf(sz_text, sizeof(sz_text), "Max: %dx", max_streak + 1);
            /* Gray for max streak */
            draw_text_color(RESOLUTION_COLS - 20, 5, sz_text, 0, 200, 200, 200);
        }

        memset(sz_text, 0, 128);
        int level_len =
            snprintf(sz_text, sizeof(sz_text), "LEVEL %d", current_level + 1);
        draw_text_color((RESOLUTION_COLS >> 1) - (level_len >> 1), 2, sz_text,
                        TUI_A_BOLD, 255, 255, 255);
    }
}

void play_handle_input(int key_code)
{
    if (!is_dead && !is_falling_animation) {
        switch (key_code) {
        case ' ':
        case TUI_KEY_UP:
            /* Record jump input for buffering */
            on_keydown_jump();
            break;
        case TUI_KEY_DOWN:
            /* Check if the player can throw fireball */
            if (can_throw_fireball && powerup_time > 0.0f &&
                powerup_type == OBJECT_EGG_FIRE)
                play_add_object(player.x + 5, player.y + 10, OBJECT_FIRE_BALL);

            /* Handle fast-fall when airborne, or duck when grounded */
            if (player.state == STATE_JUMPING ||
                player.state == STATE_FALLING) {
                /* Enable fast-fall when down is pressed while airborne */
                is_fast_falling = true;
                last_fast_fall_time = TICKCOUNT;
                if (player.state == STATE_JUMPING) {
                    /* Immediately transition to falling if jumping */
                    player.state = STATE_FALLING;
                }
            } else {
                /* Duck when on ground */
                last_key_check_time = TICKCOUNT;
                player.state = STATE_DUCK;
            }
            break;
        default:
            break;
        }
    } else if (is_dead &&
               (key_code == ' ' || key_code == 10 || key_code == TUI_KEY_ENTER))
        play_init_world();
}
