#include <raylib.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef BOIDS_H
#define BOIDS_H

#define MAX_BOIDS_COUNT 6000
#define BOID_MAX_SPEED 1.5f
#define BOID_MIN_SPEED 0.7f
#define BOID_MAX_HEALTH 100
#define BOID_MAX_XP 30
#define BOID_HEALTH_REGEN_INTERVAL 20
#define BOID_SIZE 75
#define ORDER_LINE_MAX_POINT 64

#define BOID_BOUND_PADDING 50
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
#define BOID_ORDER_FACTOR 0.2f

#define BOID_AVOID_RADIUS_SQ (BOID_AVOID_RADIUS * BOID_AVOID_RADIUS)
#define BOID_VISIBLE_RADIUS_SQ (BOID_VISIBLE_RADIUS * BOID_VISIBLE_RADIUS)
#define BOID_NEAREST_ENEMY_RADIUS_SQ (BOID_NEAREST_ENEMY_RADIUS * BOID_NEAREST_ENEMY_RADIUS)
#define BOID_FIGHTING_RADIUS_SQ (BOID_FIGHTING_RADIUS * BOID_FIGHTING_RADIUS)
#define BOID_STOP_RADIUS_SQ (BOID_STOP_RADIUS * BOID_STOP_RADIUS)

#define WORLD_SIZE {10024, 10024}

/*
ClientSomething - client structure "Something"
ServerSomething - server structure "Something"
*/

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
    bool is_fighting;
} Boid; // Default boids

typedef struct {
    uint16_t id, x, y;
    uint8_t angle, vel;
    uint8_t action, health, xp;
} NetBoid; // Boids sync (server -> clients) every N seconds

typedef struct {
    uint16_t x, y;
    uint8_t action, xp;
} StartNetBoid; // First placement of boids on the map (first clients -> server, then server -> clients)

typedef struct {
    Boid b;
    struct {
        Vector2 order_vector;
        uint16_t order_timer;
        bool direction_order, point_order;
    } order;
    
} ServerBoid; // Boid on server (default boid + order fields)

typedef struct {
    Boid b;
    struct {
        Vector2 direction;
        uint8_t sprite_timer;
        uint8_t sprite;
        bool is_selected, is_used;
    } visual;
} ClientBoid; // Boid on client (default boid + fields for visualization and selecting)

typedef uint16_t BoidIndex;

#endif // BOIDS_H
