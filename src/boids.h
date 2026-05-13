#include <raylib.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef BOIDS_H
#define BOIDS_H

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
    Vector2 pos, velocity, direction, orderVector;
    float speed;
    int8_t health;
    uint8_t xp, fightingTimer, spriteTimer;
    uint16_t timer, orderTimer;
    BoidTeam team;
    BoidAction action;
    BoidSprite sprite;
    bool isFighting, isSelected, directionOrder, pointOrder, isUsed;
} Boid;

typedef uint16_t BoidIndex;

#endif // BOIDS_H
