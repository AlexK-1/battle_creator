#include <raymath.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef BOIDS_H
#define BOIDS_H

#define MAX_BOIDS_COUNT 6000
#define BOID_MAX_SPEED 1.5f
#define BOID_MIN_SPEED 0.7f
#define BOID_MAX_HEALTH 100
#define BOID_MAX_XP 30
#define BOID_MAX_FIGHTING_TIMER 60
#define BOID_HEALTH_REGEN_INTERVAL 20
#define BOID_SIZE 75
#define ORDER_LINE_MAX_POINT 64

#define BOID_BOUND_PADDING 30
#define BOID_AVOID_RADIUS 70
#define BOID_VISIBLE_RADIUS 250
#define BOID_NEAREST_ENEMY_RADIUS 1000
#define BOID_FIGHTING_RADIUS 80
#define BOID_STOP_RADIUS 500

#define BOID_FOR_RETREAT_VALUE 3 
#define BOID_FOR_RETREAT_MIN 8
#define BOID_FOR_SURRENDER_VALUE 20

#define BOID_BOUND_FACTOR 0.9f
#define BOID_AVOID_FACTOR 0.04f
#define BOID_ALIGNMENT_FACTOR 0.01f
#define BOID_COHESION_FACTOR 0.0001f
#define BOID_ATTACK_FACTOR 0.001f
#define BOID_FIGHTING_FACTOR 0.001f
#define BOID_RETREAT_FACTOR 0.001f
#define BOID_ORDER_FACTOR 0.4f
#define BOID_TARGET_FACTOR 1.0f

#define BOID_AVOID_RADIUS_SQ (BOID_AVOID_RADIUS * BOID_AVOID_RADIUS)
#define BOID_VISIBLE_RADIUS_SQ (BOID_VISIBLE_RADIUS * BOID_VISIBLE_RADIUS)
#define BOID_NEAREST_ENEMY_RADIUS_SQ (BOID_NEAREST_ENEMY_RADIUS * BOID_NEAREST_ENEMY_RADIUS)
#define BOID_FIGHTING_RADIUS_SQ (BOID_FIGHTING_RADIUS * BOID_FIGHTING_RADIUS)
#define BOID_STOP_RADIUS_SQ (BOID_STOP_RADIUS * BOID_STOP_RADIUS)

/*
ClientSomething - client structure "Something"
ServerSomething - server structure "Something"
*/


/* Boids */

typedef enum {
    TEAM_RED,
    TEAM_BLUE,
    TEAM_GREEN,
    TEAM_YELLOW
} BoidTeam;
#define TEAMS_COUNT 4

typedef enum {
    ACT_STOP,
    ACT_ATTACK,
    ACT_RETREAT,
    ACT_SURRENDER,
    ACT_FALL,
    ACT_DELETE
} BoidAction;

typedef enum {
    SPRITE_NORMAL,
    SPRITE_ANGRY,
    SPRITE_HIT_LEFT,
    SPRITE_HIT_RIGHT,
    SPRITE_OUCH,
    SPRITE_SAD,
    SPRITE_SURRENDER,
    SPRITE_FALL
} BoidSprite;
#define SPRITES_COUNT 8

typedef uint16_t BoidIndex;

typedef struct {
    Vector2 pos, velocity, direction, order_vector;
    float speed;
    int8_t health;
    uint8_t xp, fighting_timer, sprite_timer;
    uint16_t timer, order_timer;
    BoidTeam team;
    BoidAction action;
    BoidSprite sprite;
    bool is_fighting, is_selected, direction_order, point_order, is_used;
} OldBoid;

typedef struct {
    Vector2 pos, velocity;
    float speed;
    int8_t health;
    uint8_t xp, fighting_timer;
    uint16_t timer;
    BoidTeam team;
    BoidAction action;
    bool is_fighting, hit;
    BoidIndex nearest_enemy_idx;
} BaseBoid; // Default boid

typedef struct {
    uint16_t x, y;
    uint8_t vel, xp;
    int8_t health, angle, action;
} NetBoid; // Boids sync (server -> clients) every N seconds

typedef struct {
    uint16_t count;
    int8_t team;
} ClientStartNetBoids; // First placement of boids on the map (client -> server)

typedef struct {
    uint16_t x, y;
    uint8_t speed, xp, team;
} ServerStartNetBoid; // First placement of boids on the map (server -> clients)

typedef struct {
    BaseBoid b;
    Vector2 order_vector;
    uint16_t order_timer;
    bool direction_order, point_order, is_used;
} ServerBoid; // Boid on server (default boid + order fields)

typedef struct {
    BaseBoid b;
    Vector2 direction, target_pos;
    uint8_t sprite_timer;
    uint8_t sprite;
    bool is_selected, go_target;
} ClientBoid; // Boid on client (default boid + fields for visualization and selecting)

typedef enum {
    ORDER_CLEAR,
    ORDER_ACTION,
    ORDER_DIRECTION,
    ORDER_POINT,
    ORDER_LINE
} OrderType;

/* Grig and chunks */

#define CHUNK_SIZE_BOIDS 1024
#define CHUNK_SIZE_PIXELS 1050

typedef uint16_t ChunkSize;

typedef struct {
    BoidIndex boids[CHUNK_SIZE_BOIDS]; // Array of boids
    ChunkSize count; // Number of boids in the chunk
} Chunk;

typedef struct {
    Chunk *chunks;
    int screen_width, screen_height, chunk_size_pixels;
    uint16_t rows, cols; // Number of chunks by width/height
    uint32_t chunks_count;
} Grid;


void boid_bound(BaseBoid *boid, int width, int height); // Push the boid away from the bound
void boid_normal_speed(BaseBoid *boid); // Adjust boid velocity so its speed stays within [BOID_MIN_SPEED, BOID_MAX_SPEED]
void update_base_boid(void *boids, Grid *grid, BoidIndex boid_index, size_t boid_size, bool can_change_action, bool can_fall);

void clear_grid(Grid *grid); // Clear all chunks (set counts to zero)
void perform_boid_in_fill_grid(Grid *grid, BaseBoid *boid, BoidIndex i); // One iteration of fill_grid cycle

// Fill chunks with boids
#define fill_grid(grid, boids, boids_count)                                    \
    do {                                                                       \
        for (BoidIndex i = 0; i < (boids_count); i++) {                        \
            perform_boid_in_fill_grid((grid), (BaseBoid*)&(boids)[i], i);      \
        }                                                                      \
    } while (0)

// Initialize grid and fully rebuild chunks (delete and recreate all chunks)
#define init_grid(grid, boids, boids_count, width, height, chunk_size_pizels)  \
    do {                                                                       \
        (grid)->screen_width = (width);                                        \
        (grid)->screen_height = (height);                                      \
        (grid)->cols = (uint16_t)((width)/chunk_size_pizels) + 1;              \
        (grid)->rows = (uint16_t)((height)/chunk_size_pizels) + 1;             \
        (grid)->chunks_count = (grid)->rows * (grid)->cols;                    \
        (grid)->chunk_size_pixels = chunk_size_pizels;                         \
                                                                               \
        if ((grid)->chunks != NULL) {                                          \
            free((grid)->chunks);                                              \
        }                                                                      \
        (grid)->chunks = calloc((grid)->chunks_count, sizeof(Chunk));          \
                                                                               \
        fill_grid((grid), (boids), (boids_count));                             \
    } while (0)

#endif // BOIDS_H
