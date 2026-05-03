#include <math.h>
#include <raylib.h>
#include <raymath.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// <======================================== MACROS AND DEFINITIONS ========================================>

#define MAX_BOIDS_COUNT 10000
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 450

#define BOID_MAX_SPEED 1.0f
#define BOID_MIN_SPEED 0.4f
#define BOID_MAX_HEALTH 100
#define BOID_MAX_XP 30
#define BOID_HEALTH_REGEN_INTERVAL 30
#define BOID_SIZE 75

#define BOID_BOUND_PADDING 50
#define BOID_AVOID_RADIUS 70
#define BOID_VISIBLE_RADIUS 250
#define BOID_NEAREST_ENEMY_RADIUS 1000
#define BOID_FIGHTING_RADIUS 80
#define BOID_STOP_RADIUS 500

// #define BOID_MAX_SPEED 0.1f
// #define BOID_MIN_SPEED 0.04f
// #define BOID_MAX_HEALTH 100
// #define BOID_MAX_XP 30
// #define BOID_HEALTH_REGEN_INTERVAL 30
// #define BOID_SIZE 7

// #define BOID_BOUND_PADDING 5
// #define BOID_AVOID_RADIUS 7
// #define BOID_VISIBLE_RADIUS 25
// #define BOID_NEAREST_ENEMY_RADIUS 100
// #define BOID_FIGHTING_RADIUS 8
// #define BOID_STOP_RADIUS 50

#define BOID_FOR_RETREAT_VALUE 3 
#define BOID_FOR_RETREAT_MIN 8
#define BOID_FOR_SURRENDER_VALUE 20

#define BOID_BOUND_FACTOR 0.9f
#define BOID_AVOID_FACTOR 0.04f
#define BOID_ALIGNMENT_FACTOR 0.01f
#define BOID_COHESION_FACTOR 0.0001f
#define BOID_ATTACK_FACTOR 0.0005f
#define BOID_FIGHTING_FACTOR 0.001f
#define BOID_RETREAT_FACTOR 0.02f

#define BOID_AVOID_RADIUS_SQ (BOID_AVOID_RADIUS * BOID_AVOID_RADIUS)
#define BOID_VISIBLE_RADIUS_SQ (BOID_VISIBLE_RADIUS * BOID_VISIBLE_RADIUS)
#define BOID_NEAREST_ENEMY_RADIUS_SQ (BOID_NEAREST_ENEMY_RADIUS * BOID_NEAREST_ENEMY_RADIUS)
#define BOID_FIGHTING_RADIUS_SQ (BOID_FIGHTING_RADIUS * BOID_FIGHTING_RADIUS)
#define BOID_STOP_RADIUS_SQ (BOID_STOP_RADIUS * BOID_STOP_RADIUS)

// <======================================== STRUCTURES AND TYPEDEFS =======================================>

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
    ACT_FALL
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
    Vector2 pos, velocity, direction;
    float speed;
    int8_t health;
    uint8_t xp, fightingTimer, spriteTimer;
    uint16_t timer;
    BoidTeam team;
    BoidAction action;
    BoidSprite sprite;
    bool isFighting;
} Boid;

typedef uint16_t BoidIndex;

// <================================================= BOIDS ================================================>

// Push the boid away from the bound
void BoidBound(Boid *boid, int screenWidth, int screenHeight) {
    if (boid->pos.x < BOID_BOUND_PADDING)
        boid->velocity.x += BOID_BOUND_FACTOR;
    else if (boid->pos.x > screenWidth-BOID_BOUND_PADDING)
        boid->velocity.x -= BOID_BOUND_FACTOR;
    if (boid->pos.y < BOID_BOUND_PADDING)
        boid->velocity.y += BOID_BOUND_FACTOR;
    else if (boid->pos.y > screenHeight-BOID_BOUND_PADDING)
        boid->velocity.y -= BOID_BOUND_FACTOR;
}

// Speed up or slow down the boid so that its speed is between BOID_MIN_SPEED and BOID_MAX_SPEED
void BoidNormalSpeed(Boid *boid) {
    float speed = Vector2Length(boid->velocity);
    // if (speed == 0) // Randomize boid's speed, if it stops
    //     boid->velocity = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};
    if (speed > BOID_MAX_SPEED)
        boid->velocity = Vector2Scale(boid->velocity, BOID_MAX_SPEED/speed);
    else if (speed < BOID_MIN_SPEED && boid->action != ACT_STOP)
        boid->velocity = Vector2Scale(boid->velocity, BOID_MIN_SPEED/speed);
}

void UpdateBoid(Boid *boids, BoidIndex boidsCount, BoidIndex boidIndex) {
    Boid *boid = &boids[boidIndex];

    // Skip if surrending or falled
    if (boid->action == ACT_FALL) {
        boid->velocity = (Vector2){ 0 };
        boid->sprite = SPRITE_FALL;
        if (boid->spriteTimer < 45)
            boid->spriteTimer++;
        // printf("%d\n", boid->spriteTimer);
        return;
    }
    if (boid->action == ACT_SURRENDER) {
        boid->velocity = (Vector2){ 0 };
        boid->sprite = SPRITE_SURRENDER;
        if (boid->spriteTimer < 50)
            boid->spriteTimer++;
        return;
    }

    // Determine a fall
    if (boid->health <= 0) {
        boid->action = ACT_FALL;
        boid->sprite = SPRITE_FALL;
        boid->velocity = (Vector2){ 0 };
        boid->spriteTimer = 0;
        return;
    }
    
    if (boid->action == ACT_STOP) {
        boid->velocity = (Vector2){ 0 };
    }
    
    // if (boid->action == ACT_ATTACK) {
    //     boid->sprite = SPRITE_ANGRY;
    // }
    // if (boid->action == ACT_RETREAT) {
    //     boid->sprite = SPRITE_SAD;
    // }

    Vector2 close = { 0 }; // Avoid vector
    BoidIndex teamsBoidsCount[TEAMS_COUNT] = { 0 }, teamsCloseBoidsCount[TEAMS_COUNT] = { 0 }; // Boids count by teams
    Vector2 neighborsVelocity = { 0 }; // Alignment velocity vector
    Vector2 neighborsPos = { 0 }; // Cohesion position vector

    Vector2 nearestEnemyPos = { 0 };
    Vector2 closeEnemiesPos = { 0 }; // Sum of vectors of enemies positions for retreat
    float nearestEnemyDistanceSqr = INFINITY;
    Boid *nearestEnemy = NULL;

    for (size_t i = 0; i < boidsCount; i++) {
        if (i == boidIndex) continue;

        Boid *otherBoid = &boids[i];

        // Skip surrending and falled boids
        if (otherBoid->action == ACT_SURRENDER || otherBoid->action == ACT_FALL) continue;
        
        Vector2 distanceV = Vector2Subtract(boid->pos, otherBoid->pos);
        float distanceSqr = Vector2LengthSqr(distanceV);
        
        // Calculate avoid
        if (distanceSqr < BOID_AVOID_RADIUS_SQ)
            close = Vector2Add(close, distanceV);

        // Count only not retreating boids
        // if (otherBoid->action != ACT_RETREAT) {
            teamsBoidsCount[otherBoid->team]++;
            if (distanceSqr < BOID_VISIBLE_RADIUS_SQ) {
                teamsCloseBoidsCount[otherBoid->team]++;
                if (boid->team == otherBoid->team && distanceSqr > BOID_AVOID_RADIUS_SQ) { // Cohesion and alignment only with teammates
                    neighborsVelocity = Vector2Add(neighborsVelocity, otherBoid->velocity); // Calculate alignment
                    neighborsPos = Vector2Add(neighborsPos, otherBoid->pos); // Calculate cohesion
                }
            }
        // }

        // Find nearest enemy
        if ((otherBoid->team != boid->team) && (distanceSqr < BOID_NEAREST_ENEMY_RADIUS_SQ) && (distanceSqr < nearestEnemyDistanceSqr)) {
            nearestEnemyPos = distanceV;
            nearestEnemyDistanceSqr = distanceSqr;
            nearestEnemy = otherBoid;
        }

        // Calculate vector for retreat
        if ((boid->action == ACT_RETREAT) && (otherBoid->team != boid->team) && (distanceSqr < BOID_VISIBLE_RADIUS_SQ)) {
            closeEnemiesPos = Vector2Add(closeEnemiesPos, distanceV);
        }
    }

    BoidIndex allEnemiesCount = 0, closeEnemiesCount = 0; // Get the enemy team with max boid's count
    BoidIndex allTeammatesCount = 0, closeTeammatesCount = 0;
    for (uint8_t teamIndex = 0; teamIndex < TEAMS_COUNT; teamIndex++) {
        if (teamIndex != boid->team) { // Count enemies
            if (teamsBoidsCount[teamIndex] > allEnemiesCount) allEnemiesCount = teamsBoidsCount[teamIndex];
            if (teamsCloseBoidsCount[teamIndex] > closeEnemiesCount) closeEnemiesCount = teamsCloseBoidsCount[teamIndex];
        } else { // Count teammates
            if (teamsBoidsCount[teamIndex] > allTeammatesCount) allTeammatesCount = teamsBoidsCount[teamIndex];
            if (teamsCloseBoidsCount[teamIndex] > closeTeammatesCount) closeTeammatesCount = teamsCloseBoidsCount[teamIndex];
        }
    }
    
    // Do not include yourself
    // allTeammatesCount--;
    // closeTeammatesCount--;

    if (boid->action != ACT_STOP) {
        // Alignment
        if (closeTeammatesCount > 0) {
            neighborsVelocity = Vector2Scale(neighborsVelocity, 1.0f / closeTeammatesCount);
            boid->velocity = Vector2Add(boid->velocity, Vector2Scale(neighborsVelocity, BOID_ALIGNMENT_FACTOR));
        }
    
        // Cohesion
        if (closeTeammatesCount > 0) {
            neighborsPos = Vector2Scale(neighborsPos, 1.0f / closeTeammatesCount);
            neighborsPos = Vector2Subtract(neighborsPos, boid->pos);
            boid->velocity = Vector2Add(boid->velocity, Vector2Scale(neighborsPos, BOID_COHESION_FACTOR));
        }
    }

    // Determine attack
    if ((boid->action == ACT_STOP) && (nearestEnemyDistanceSqr < BOID_STOP_RADIUS_SQ)) {
        boid->action = ACT_ATTACK;
        boid->sprite = SPRITE_ANGRY;
    }

    // Attack
    nearestEnemyPos = Vector2Scale(nearestEnemyPos, -1);
    if ((boid->action == ACT_ATTACK) && (nearestEnemy != NULL)) {
        boid->velocity = Vector2Add(boid->velocity, Vector2Scale(nearestEnemyPos, BOID_ATTACK_FACTOR));
    }

    // Fighting
    if (boid->fightingTimer > 0) boid->fightingTimer--;
    if (boid->spriteTimer > 0) boid->spriteTimer--;
    if ((nearestEnemy != NULL) && (boid->fightingTimer == 0) && (nearestEnemyDistanceSqr < BOID_FIGHTING_RADIUS_SQ)) {
        boid->fightingTimer = GetRandomValue(30, 100);
        nearestEnemy->spriteTimer = 5;
        boid->spriteTimer = 5;
        if (boid->xp < BOID_MAX_XP) boid->xp++;
        nearestEnemy->health -= 5 + ((boid->action == ACT_RETREAT)? 0 : 10*((float)boid->xp / BOID_MAX_XP));
        boid->sprite = (rand()%2)? SPRITE_HIT_LEFT : SPRITE_HIT_RIGHT;
        nearestEnemy->sprite = SPRITE_OUCH;
        boid->isFighting = true;
        nearestEnemy->isFighting = true;
    }
    if ((nearestEnemy == NULL) || (nearestEnemyDistanceSqr > BOID_FIGHTING_RADIUS_SQ*2)) {
        boid->isFighting = false;
    }
    if ((boid->sprite == SPRITE_HIT_LEFT || boid->sprite == SPRITE_HIT_RIGHT || boid->sprite == SPRITE_OUCH) && boid->spriteTimer == 0) {
        if (boid->action == ACT_ATTACK) boid->sprite = SPRITE_ANGRY;
        if (boid->action == ACT_RETREAT) boid->sprite = SPRITE_SAD;
        if (boid->action == ACT_STOP) boid->sprite = SPRITE_NORMAL;
    }
    if (boid->isFighting) {
        boid->direction = Vector2Add(boid->direction, Vector2Scale(nearestEnemyPos, BOID_FIGHTING_FACTOR));
    }

    // Retreat
    if (boid->action == ACT_RETREAT) {
        boid->velocity = Vector2Add(boid->velocity, Vector2Scale(closeEnemiesPos, BOID_RETREAT_FACTOR));
        if (!boid->isFighting)
            boid->sprite = SPRITE_SAD;
    }

    // Health regen
    if (boid->action == ACT_STOP) {
        boid->timer++;
        if (boid->timer > BOID_HEALTH_REGEN_INTERVAL) {
            boid->timer = 0;
            if (boid->health < BOID_MAX_HEALTH) boid->health++;
        }
    }
    
    // Include yourself
    allTeammatesCount++;
    closeTeammatesCount++;

    // Determine retreat
    if ((closeEnemiesCount >= BOID_FOR_RETREAT_MIN) &&
        ((float)closeEnemiesCount / closeTeammatesCount >= BOID_FOR_RETREAT_VALUE)) {
        boid->action = ACT_RETREAT;
        boid->sprite = SPRITE_SAD;
    }

    // Determine stop
    if (boid->action == ACT_RETREAT) {
        boid->timer++;
        if ((closeEnemiesCount == 0) && (boid->timer > 360)) { // 6 seconds
            boid->timer = 0;
            boid->action = ACT_STOP;
            boid->sprite = SPRITE_NORMAL;
        } else if (closeEnemiesCount > 0) {
            boid->timer = 0;
        }
    } else if (boid->action == ACT_ATTACK) {
        boid->timer++;
        if ((nearestEnemy != NULL) && (nearestEnemyDistanceSqr > BOID_STOP_RADIUS_SQ) && (boid->timer > 1200)) { // 20 seconds
            boid->timer = 0;
            boid->action = ACT_STOP;
            boid->sprite = SPRITE_NORMAL;
        } else if (closeEnemiesCount > 0) {
            boid->timer = 0;
        }
        
    }

    // Determine surrender
    // if ((float)allEnemiesCount / allTeammatesCount >= BOID_FOR_SURRENDER_VALUE) {
    //     boid->action = ACT_SURRENDER;
    //     boid->sprite = SPRITE_SURRENDER;
    //     boid->timer = 0;
    // }

    // Avoid
    boid->velocity = Vector2Add(boid->velocity, Vector2Scale(close, BOID_AVOID_FACTOR));
}

void DrawBoid(Boid *boid, Texture2D texture) {
    static Rectangle sprites[SPRITES_COUNT] = {
         [SPRITE_NORMAL] = {0, 0, 146, 152},
         [SPRITE_ANGRY] = {160, 0, 146, 152},
         [SPRITE_HIT_LEFT] = {320, 0, 146, 152},
         [SPRITE_HIT_RIGHT] = {480, 0, 146, 152},
         [SPRITE_OUCH] = {640, 0, 146, 152},
         [SPRITE_SAD] = {800, 0, 146, 152},
         [SPRITE_SURRENDER] = {960, 0, 144, 252},
         [SPRITE_FALL] = {1120, 0, 265, 203},
    };
    Rectangle sprite = sprites[boid->sprite];
    sprite.y += 260 * boid->team;
    
    Rectangle destRec = {boid->pos.x, boid->pos.y, BOID_SIZE, BOID_SIZE};
    if (boid->sprite == SPRITE_SURRENDER) {
        destRec.height = BOID_SIZE * (128/75.0);
    } else if (boid->sprite == SPRITE_FALL) {
        destRec.width = BOID_SIZE * (132/75.0);
        destRec.height = BOID_SIZE * (100/75.0);
    }

    Color tint = WHITE;
    if (boid->sprite == SPRITE_FALL)
        tint.a = 20;
    if ((boid->sprite == SPRITE_SURRENDER) || (boid->sprite == SPRITE_FALL)) {
        tint.a = (255.0/50.0) * (50 - boid->spriteTimer);
    }
    
    DrawTexturePro(texture, sprite, destRec, (Vector2){BOID_SIZE/2.0, BOID_SIZE/2.0},
                   atan2f(boid->direction.y, boid->direction.x)*RAD2DEG, tint);
}

// <================================================= MAIN =================================================>

int main(int argc, char *argv[]) {
    printf("%zu\n", sizeof(Boid));
    
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Battle creator");
    SetTargetFPS(60);

    // Boids
    Boid boids[MAX_BOIDS_COUNT];
    BoidIndex boidsCount = 0;

    // Camera
    Camera2D camera = { 0 };
    camera.zoom = 1.0f;

    // Control
    bool pause = false;
    BoidAction action = 0;
    BoidTeam team = TEAM_RED;

    // Textures
    Texture2D texture = LoadTexture("resources/texture.png");
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    // for (BoidIndex i = 0; i < 10000; i++) {
    //     Boid newBoid = { 0 };
    //     newBoid.pos = (Vector2){GetRandomValue(0, GetScreenWidth()), GetRandomValue(0, GetScreenHeight())};
    //     newBoid.velocity = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};
    //     newBoid.direction = newBoid.velocity;
    //     newBoid.speed = GetRandomValue(90, 150)/100.0;
    //     newBoid.health = BOID_MAX_HEALTH;
    //     newBoid.xp = GetRandomValue(0, 5);
    //     newBoid.action = ACT_ATTACK;
    //     newBoid.team = GetRandomValue(0, 3);

    //     boids[boidsCount++] = newBoid;
    // }
    
    while (!WindowShouldClose()) {
        // Keys
        if (IsKeyPressed(KEY_SPACE)) pause = !pause;
        if (IsKeyPressed(KEY_ZERO)) action = 0;
        if (IsKeyPressed(KEY_ONE)) action = 1;
        if (IsKeyPressed(KEY_TWO)) action = 2;
        if (IsKeyPressed(KEY_THREE)) action = 3;
        if (IsKeyPressed(KEY_FOUR)) action = 4;
        if (IsKeyPressed(KEY_FIVE)) action = 5;
        if (IsKeyPressed(KEY_Q)) team = 0;
        if (IsKeyPressed(KEY_W)) team = 1;
        if (IsKeyPressed(KEY_E)) team = 2;
        if (IsKeyPressed(KEY_R)) team = 3;
        if (IsKeyPressed(KEY_C)) boidsCount = 0;

        // Camera
        float wheel = GetMouseWheelMove();
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) wheel = -1.0;
        if (IsKeyPressed(KEY_KP_ADD) || (IsKeyPressed(KEY_EQUAL) && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))) wheel = 1.0;
        if (wheel != 0) {
            Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), camera);
            camera.offset = GetMousePosition();
            camera.target = mouseWorldPos;
            float scale = 0.2f*wheel;
            camera.zoom = Clamp(expf(logf(camera.zoom)+scale), 0.125f, 64.0f);
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 delta = GetMouseDelta();
            delta = Vector2Scale(delta, -1.0f/camera.zoom);
            camera.target = Vector2Add(camera.target, delta);
        }

        Vector2 mousePosition = GetScreenToWorld2D(GetMousePosition(), camera);
        int screenWidth = GetScreenWidth();
        int screenHeight = GetScreenHeight();

        // Spawn boids
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON) && boidsCount < MAX_BOIDS_COUNT) {
            Boid newBoid = { 0 };
            newBoid.pos = mousePosition;
            if (action == ACT_STOP) {
                newBoid.direction = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};
            } else {
                newBoid.velocity = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};
                newBoid.direction = newBoid.velocity;
            }
            newBoid.speed = GetRandomValue(90, 150)/100.0;
            newBoid.health = BOID_MAX_HEALTH;
            newBoid.xp = GetRandomValue(0, 5);
            newBoid.action = action;
            newBoid.team = team;

            boids[boidsCount++] = newBoid;
        }

        // Update boids
        if (!pause) {
            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
            
                UpdateBoid(boids, boidsCount, i);

                if (boid->action != ACT_SURRENDER && boid->action != ACT_FALL) {
                    BoidNormalSpeed(boid);
                    BoidBound(boid, screenWidth, screenHeight);

                    boid->direction.x = boid->direction.x*0.97f + boid->velocity.x*0.03f;
                    boid->direction.y = boid->direction.y*0.97f + boid->velocity.y*0.03f;
                }

                boid->pos = Vector2Add(boid->pos, Vector2Scale(boid->velocity, boid->speed));
            }
        }

        // Drawing
        BeginDrawing();
            ClearBackground(RAYWHITE);
            BeginMode2D(camera);

            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
                if (boid->sprite == SPRITE_FALL)
                    DrawBoid(boid, texture);
            }
            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
                if (boid->sprite != SPRITE_FALL) {
                    DrawBoid(boid, texture);
                    // DrawText(TextFormat("%d", boid->health), boid->pos.x, boid->pos.y + 40, 20, BLACK);
                }
            }

            EndMode2D();

            // Draw "Paused" label, if the simulation is paused
            if (pause)
                DrawText("Paused", screenWidth - 85, 10, 20, BLACK);

            DrawFPS(10, 10);

            BoidIndex teamsBoidsCount[TEAMS_COUNT] = { 0 };
            BoidIndex allBoidsCount = 0;
            for (size_t i = 0; i < boidsCount; i++) {
                Boid *otherBoid = &boids[i];

                // Skip surrending and falled boids
                if (otherBoid->action == ACT_SURRENDER || otherBoid->action == ACT_FALL) continue;

                teamsBoidsCount[otherBoid->team]++;
                allBoidsCount++;
            }

            DrawText(TextFormat("ALL: %2d", allBoidsCount), 10, 40, 20, BLACK);
            DrawText(TextFormat("RED: %2d", teamsBoidsCount[TEAM_RED]), 10, 70, 20, RED);
            DrawText(TextFormat("BLUE: %2d", teamsBoidsCount[TEAM_BLUE]), 10, 90, 20, BLUE);
            DrawText(TextFormat("GREEN: %2d", teamsBoidsCount[TEAM_GREEN]), 10, 110, 20, GREEN);
            DrawText(TextFormat("YELLOW: %2d", teamsBoidsCount[TEAM_YELLOW]), 10, 130, 20, ORANGE);
            

        EndDrawing();
    }

    UnloadTexture(texture);
    CloseWindow();

    return 0;
}
