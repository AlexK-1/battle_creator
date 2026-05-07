// compile: cc main.c -lraylib -lm -O2

#include <assert.h>
#include <math.h>
#include <raylib.h>
#include <raymath.h>
#include <stdbool.h>
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
#define BOID_AVOID_FACTOR 0.03f
#define BOID_ALIGNMENT_FACTOR 0.01f
#define BOID_COHESION_FACTOR 0.0001f
#define BOID_ATTACK_FACTOR 0.001f
#define BOID_FIGHTING_FACTOR 0.001f
#define BOID_RETREAT_FACTOR 0.01f
#define BOID_ORDER_FACTOR 0.2f

#define BOID_AVOID_RADIUS_SQ (BOID_AVOID_RADIUS * BOID_AVOID_RADIUS)
#define BOID_VISIBLE_RADIUS_SQ (BOID_VISIBLE_RADIUS * BOID_VISIBLE_RADIUS)
#define BOID_NEAREST_ENEMY_RADIUS_SQ (BOID_NEAREST_ENEMY_RADIUS * BOID_NEAREST_ENEMY_RADIUS)
#define BOID_FIGHTING_RADIUS_SQ (BOID_FIGHTING_RADIUS * BOID_FIGHTING_RADIUS)
#define BOID_STOP_RADIUS_SQ (BOID_STOP_RADIUS * BOID_STOP_RADIUS)

#define CHUNK_SIZE_BOIDS 1024
#define CHUNK_SIZE_PIXELS BOID_NEAREST_ENEMY_RADIUS
#define WORLD_SIZE {10024, 10024}

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
    bool isFighting, isSelected, isExecutingOrder;
} Boid;

typedef uint16_t BoidIndex;
typedef uint16_t ChunkSize;

typedef struct {
    BoidIndex boids[CHUNK_SIZE_BOIDS]; // Array of boids
    ChunkSize count; // Number of boids in the chunk
} Chunk;

typedef struct {
    Chunk *chunks;
    int screenWidth, screenHeight;
    uint16_t rows, cols; // Number of chunks by width/height
    uint32_t chunksCount;
} Grid;

// <============================================ GRID AND CHUNKS ===========================================>

// Clear all chunks (set counts to zero)
void ClearGrid(Grid *grid) {
    for (uint32_t i = 0; i < grid->chunksCount; i++) {
        grid->chunks[i].count = 0;
    }
}

// Fill chunks with boids
void FillGrid(Grid *grid, Boid *boids, BoidIndex boidsCount) {
    for (BoidIndex i = 0; i < boidsCount; i++) {
        Boid *boid = &boids[i];
        if ((boid->action == ACT_FALL) || (boid->action == ACT_SURRENDER || (boid->action == ACT_DELETE))) continue;

        float x = boid->pos.x;
        float y = boid->pos.y;

        if (x < 0) x = 0;
        else if (x > grid->screenWidth) x = grid->screenWidth;
        if (y < 0) y = 0;
        else if (y > grid->screenHeight) y = grid->screenHeight;
        
        uint16_t chunkX = x / CHUNK_SIZE_PIXELS;
        uint16_t chunkY = y / CHUNK_SIZE_PIXELS;
        uint32_t chunkIndex = chunkX + chunkY*grid->cols;

        Chunk *chunk = &grid->chunks[chunkIndex];

        if (chunk->count < CHUNK_SIZE_BOIDS)
            chunk->boids[chunk->count++] = i;
    }
}

// Initialize grid and fully rebuild chunks (delete and recreate all chunks)
void InitGrid(Grid *grid, Boid *boids, BoidIndex boidsCount, int width, int height) {
    grid->screenWidth = width;
    grid->screenHeight = height;
    grid->cols = (uint16_t)(width/CHUNK_SIZE_PIXELS) + 1;
    grid->rows = (uint16_t)(height/CHUNK_SIZE_PIXELS) + 1;
    grid->chunksCount = grid->rows * grid->cols;

    if (grid->chunks != NULL) {
        free(grid->chunks);
    }
    grid->chunks = RL_CALLOC(grid->chunksCount, sizeof(Chunk));

    FillGrid(grid, boids, boidsCount);
}

// <================================================= BOIDS ================================================>

// Push the boid away from the bound
void BoidBound(Boid *boid, int width, int height) {
    if (boid->pos.x < BOID_BOUND_PADDING)
        boid->velocity.x += BOID_BOUND_FACTOR;
    else if (boid->pos.x > width-BOID_BOUND_PADDING)
        boid->velocity.x -= BOID_BOUND_FACTOR;
    if (boid->pos.y < BOID_BOUND_PADDING)
        boid->velocity.y += BOID_BOUND_FACTOR;
    else if (boid->pos.y > height-BOID_BOUND_PADDING)
        boid->velocity.y -= BOID_BOUND_FACTOR;
}

// Adjust boid velocity so its speed stays within [BOID_MIN_SPEED, BOID_MAX_SPEED]
void BoidNormalSpeed(Boid *boid) {
    float speed = Vector2Length(boid->velocity);
    if (speed > BOID_MAX_SPEED)
        boid->velocity = Vector2Scale(boid->velocity, BOID_MAX_SPEED/speed);
    else if (speed < BOID_MIN_SPEED && boid->action != ACT_STOP)
        boid->velocity = Vector2Scale(boid->velocity, BOID_MIN_SPEED/speed);
}

void UpdateBoid(Boid *boids, Grid *grid, BoidIndex boidsCount, BoidIndex boidIndex) {
    Boid *boid = &boids[boidIndex];

    if (boid->action == ACT_DELETE)
        return;

    // Skip if surrendering or fallen
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

    // Determine if the boid should fall
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
    BoidIndex teamsBoidsCount[TEAMS_COUNT] = { 0 }, teamsCloseBoidsCount[TEAMS_COUNT] = { 0 }; // Boids counts per teams
    Vector2 neighborsVelocity = { 0 }; // Alignment velocity vector
    Vector2 neighborsPos = { 0 }; // Cohesion position vector

    Vector2 nearestEnemyPos = { 0 };
    Vector2 closeEnemiesPos = { 0 }; // Sum of enemy direction vectors for retreat
    float nearestEnemyDistanceSqr = INFINITY;
    Boid *nearestEnemy = NULL;

    // Clamp position within bounds
    Vector2 pos = boid->pos;
    if (pos.x < 0) pos.x = 0;
    else if (pos.x > grid->screenWidth) pos.x = grid->screenWidth;
    if (pos.y < 0) pos.y = 0;
    else if (pos.y > grid->screenHeight) pos.y = grid->screenHeight;

    // Get boid's chunk
    int32_t chunkX = boid->pos.x / CHUNK_SIZE_PIXELS;
    int32_t chunkY = boid->pos.y / CHUNK_SIZE_PIXELS;

    // 8 neighboring chunks + current chunk
    for (int32_t x = chunkX-1; x <= chunkX+1; x++) {
        for (int32_t y = chunkY-1; y <= chunkY+1; y++) {
            if ((x < 0) || (x >= grid->cols) || (y < 0) || y >= grid->rows) continue;

            uint32_t chunkIndex = x + y*grid->cols;
            Chunk *chunk = &grid->chunks[chunkIndex];

            // Process boids from neighboring chunks
            for (size_t i = 0; i < chunk->count; i++) {
                BoidIndex otherBoidIndex = chunk->boids[i];
                if (otherBoidIndex == boidIndex) continue;

                Boid *otherBoid = &boids[otherBoidIndex];

                // Skip surrendering and fallen boids
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
    if ((boid->action == ACT_STOP) && (nearestEnemyDistanceSqr < ((boid->orderTimer == 0)? BOID_STOP_RADIUS_SQ : BOID_VISIBLE_RADIUS_SQ))) {
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
    
    // Include self in teammate counts
    allTeammatesCount++;
    closeTeammatesCount++;

    if (boid->orderTimer > 0) {
        boid->orderTimer--;

        // Change direction by order
        if (boid->isExecutingOrder) {
            boid->velocity = Vector2Add(boid->velocity, Vector2Scale(boid->orderVector, BOID_ORDER_FACTOR));
        }
    }
    if (boid->orderTimer == 0) { // Behavior of the boid may change some time after last order.
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
            if ((nearestEnemy != NULL) && (nearestEnemyDistanceSqr > BOID_STOP_RADIUS_SQ) && (boid->timer > 20*60)) { // 20 seconds
                boid->timer = 0;
                boid->action = ACT_STOP;
                boid->sprite = SPRITE_NORMAL;
            } else if (closeEnemiesCount > 0) {
                boid->timer = 0;
            }
        
        }
    }
    
    // Determine surrender
    // if ((float)allEnemiesCount / allTeammatesCount >= BOID_FOR_SURRENDER_VALUE) {
    //     boid->action = ACT_SURRENDER;
    //     boid->sprite = SPRITE_SURRENDER;
    //     boid->timer = 0;
    // }

    // Avoid
    boid->velocity = Vector2Add(boid->velocity, Vector2Scale(close, BOID_AVOID_FACTOR * (1/boid->speed)));
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

    if (boid->action == ACT_DELETE)
        return;
    
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
    if (boid->sprite == SPRITE_FALL) {
        tint.a = 20;
    } else if ((boid->sprite == SPRITE_SURRENDER) || (boid->sprite == SPRITE_FALL)) {
        tint.a = (255.0/50.0) * (50 - boid->spriteTimer);
    }
    
    DrawTexturePro(texture, sprite, destRec, (Vector2){BOID_SIZE/2.0, BOID_SIZE/2.0},
                   atan2f(boid->direction.y, boid->direction.x)*RAD2DEG, tint);
}

void DrawSelection(Boid *boid, Texture2D texture) {
    static Rectangle white_sprite = {0, 260*TEAMS_COUNT, 146, 149};
    Rectangle destRec = {boid->pos.x, boid->pos.y, BOID_SIZE*1.2, BOID_SIZE*1.2};
    Color tint = ORANGE;
    
    DrawTexturePro(texture, white_sprite, destRec, (Vector2){BOID_SIZE*1.2/2.0, BOID_SIZE*1.2/2.0},
                   atan2f(boid->direction.y, boid->direction.x)*RAD2DEG, tint);
}

// <================================================= MAIN =================================================>

int main(int argc, char *argv[]) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Battle creator");
    SetTargetFPS(60);

    // Boids
    Boid boids[MAX_BOIDS_COUNT];
    BoidIndex boidsCount = 0;
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

    Grid grid = { 0 }; // Grid of chunks
    struct {
        int x;
        int y;
    } world_size = WORLD_SIZE;
    bool is_dragging_border = false;

    InitGrid(&grid, boids, boidsCount, world_size.x, world_size.y);
    printf("Chunks: %dx%d\n", grid.cols, grid.rows);

    // Camera
    Camera2D camera = { 0 };
    camera.zoom = 1.0f;

    // Control
    bool pause = false;
    BoidAction action = ACT_STOP;
    BoidTeam team = TEAM_RED;

    enum {
        MODE_SPAWN,
        MODE_SELECT,
        MODE_DIRECTION,
        MODE_POINT,
    } mode = MODE_SPAWN;

    bool selectMode = false, selecting = false,
         changeBoidAction = false, changeSelectionTeam = false,
         deleteBoid = false, selectingShiftPressed = false;
    Vector2 selectionStart = { 0 };

    bool showArrow = false, changeBoidDirection = false;
    Vector2 arrowStart = { 0 };

    // Textures
    Texture2D texture = LoadTexture("resources/texture.png");
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    Vector2 prev_mousePosition = {0};

    while (!WindowShouldClose()) {
        // Keys
        if (IsKeyPressed(KEY_SPACE)) pause = !pause;
        if (IsKeyPressed(KEY_A)) mode = MODE_SPAWN, selectMode = false;
        if (IsKeyPressed(KEY_S)) mode = MODE_SELECT, selectMode = true, selecting = false, selectingShiftPressed = false;
        if (IsKeyPressed(KEY_D)) mode = MODE_DIRECTION, selectMode = true;
        if (IsKeyPressed(KEY_F)) mode = MODE_POINT, selectMode = true;
        if (IsKeyPressed(KEY_Q)) team = 0, changeSelectionTeam = selectMode;
        else if (IsKeyPressed(KEY_W)) team = 1, changeSelectionTeam = selectMode;
        else if (IsKeyPressed(KEY_E)) team = 2, changeSelectionTeam = selectMode;
        else if (IsKeyPressed(KEY_R)) team = 3, changeSelectionTeam = selectMode;
        if (IsKeyPressed(KEY_C)) boidsCount = 0;
        if (IsKeyPressed(KEY_ONE)) action = ACT_STOP, changeBoidAction = selectMode;
        else if (IsKeyPressed(KEY_TWO)) action = ACT_ATTACK, changeBoidAction = selectMode;
        else if (IsKeyPressed(KEY_THREE)) action = ACT_RETREAT, changeBoidAction = selectMode;
        if (IsKeyPressed(KEY_X) && (mode == MODE_SELECT)) deleteBoid = true;
        if (mode == MODE_SELECT) {
            selectingShiftPressed |= IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        }

        Vector2 mousePosition = GetScreenToWorld2D(GetMousePosition(), camera);
        int screenWidth = GetScreenWidth();
        int screenHeight = GetScreenHeight();

        // Camera
        float wheel = GetMouseWheelMove();
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) wheel = -1.0;
        if (IsKeyPressed(KEY_KP_ADD) || (IsKeyPressed(KEY_EQUAL) && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))) wheel = 1.0;
        if (wheel != 0) {
            camera.offset = GetMousePosition();
            camera.target = mousePosition;
            float scale = 0.2f*wheel;
            camera.zoom = Clamp(expf(logf(camera.zoom)+scale), 0.125f, 64.0f);
        }
        if (!is_dragging_border && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 delta = GetMouseDelta();
            delta = Vector2Scale(delta, -1.0f/camera.zoom);
            camera.target = Vector2Add(camera.target, delta);
        }

        // move border
        if (is_dragging_border) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                world_size.x = (int)mousePosition.x;
                world_size.y = (int)mousePosition.y;
                world_size.x = world_size.x > 100? world_size.x : 100;
                world_size.y = world_size.y > 100? world_size.y : 100;
            } else {
                is_dragging_border = false;
            }
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && Vector2Distance(mousePosition, (Vector2){world_size.x, world_size.y}) <= 50) {
            is_dragging_border = true;
        }

        // Spawn boids
        static Vector2 prev_pos = {INFINITY, INFINITY};
        if ((mode == MODE_SPAWN) && IsMouseButtonDown(MOUSE_RIGHT_BUTTON) && (boidsCount < MAX_BOIDS_COUNT)) {
            const int lerp_amt = 10;
            for (int i = 0; i < lerp_amt; i++) {
                Vector2 pos = Vector2Lerp(prev_mousePosition, mousePosition, (float)i/lerp_amt);
                if (pos.x < 0 || pos.y < 0 || pos.x > world_size.x || pos.y > world_size.y)
                    continue;
                if (Vector2Distance(prev_pos, pos) >= 100) {
                    prev_pos = pos;
                } else continue;

                Boid newBoid = { 0 };
                newBoid.pos = pos;
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
        }

        if (IsMouseButtonReleased(MOUSE_RIGHT_BUTTON))
            prev_pos = (Vector2){INFINITY, INFINITY};

        // Select boids
        if (mode == MODE_SELECT) {
            if (IsMouseButtonDown(MOUSE_RIGHT_BUTTON)) {
                if (!selecting) {
                    selecting = true;
                    selectionStart = mousePosition;
                }
            } else if (IsMouseButtonReleased(MOUSE_RIGHT_BUTTON)) {
                selecting = false;
                selectingShiftPressed = false;
            }
        }

        // Direction mode
        if (mode == MODE_DIRECTION) {
            showArrow = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
            if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
                // directionMode = false;
                showArrow = false;
                changeBoidDirection = true;
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
                arrowStart = mousePosition;
        }

        if (mode == MODE_POINT) {
            if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
                changeBoidDirection= true;
        }

        // Update boids
        if (!pause) {
            if ((grid.screenWidth != screenWidth) || (grid.screenHeight != screenHeight))
                InitGrid(&grid, boids, boidsCount, world_size.x, world_size.y);

            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
            
                UpdateBoid(boids, &grid, boidsCount, i);

                if (boid->action != ACT_SURRENDER && boid->action != ACT_FALL && boid->action != ACT_DELETE) {
                    BoidNormalSpeed(boid);
                    BoidBound(boid, world_size.x, world_size.y);

                    boid->direction.x = boid->direction.x*0.97f + boid->velocity.x*0.03f;
                    boid->direction.y = boid->direction.y*0.97f + boid->velocity.y*0.03f;
                } else {
                    boid->isSelected = false;
                }

                boid->pos = Vector2Add(boid->pos, Vector2Scale(boid->velocity, boid->speed));
            }

            ClearGrid(&grid);
            FillGrid(&grid, boids, boidsCount);
        }

        // Update boids selection
        Vector2 arrowVector = Vector2Subtract(mousePosition, arrowStart);
        Vector2 arrowVectorNorm = Vector2Normalize(arrowVector);
        BoidIndex selectedBoidsCount = 0;
        if (selectMode) {
            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
                if (boid->action != ACT_SURRENDER && boid->action != ACT_FALL && boid->action != ACT_DELETE) {
                    if (selecting) {
                        boid->isSelected = (selectingShiftPressed && boid->isSelected) || (
                                           (boid->pos.x > fmin(selectionStart.x, mousePosition.x)) &&
                                           (boid->pos.x < fmax(selectionStart.x, mousePosition.x)) &&
                                           (boid->pos.y > fmin(selectionStart.y, mousePosition.y)) &&
                                           (boid->pos.y < fmax(selectionStart.y, mousePosition.y)));
                    }

                    if (boid->isSelected) {
                        if (deleteBoid) {
                            boid->action = ACT_DELETE;
                            boid->isSelected = false;
                            continue;
                        }
                        if (changeBoidAction) {
                            if ((boid->action == ACT_STOP) && (action != ACT_STOP)) // Randomize boid's speed, if it stops
                                boid->velocity = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};

                            boid->action = action;
                            boid->orderTimer = GetRandomValue(5, 15)*60; // 5-15 seconds
                            if (action == ACT_STOP) boid->sprite = SPRITE_NORMAL;
                            if (action == ACT_ATTACK) boid->sprite = SPRITE_ANGRY;
                            if (action == ACT_RETREAT) boid->sprite = SPRITE_SAD;
                        }
                        if (changeSelectionTeam) {
                            boid->isSelected = (boid->team == team);
                        }
                        if (changeBoidDirection) {
                            if (mode == MODE_DIRECTION)
                                boid->orderVector = (Vector2LengthSqr(arrowVector) >= 40*40)? arrowVectorNorm : (Vector2){ 0 };
                            else if (mode == MODE_POINT)
                                boid->orderVector = Vector2Normalize(Vector2Subtract(mousePosition, boid->pos));
                            boid->orderTimer = GetRandomValue(15, 30)*60; // 15-30 seconds
                            boid->isExecutingOrder = true;
                        }
                    }
                    selectedBoidsCount += boid->isSelected;
                }
            }
            changeBoidAction = false;
            changeSelectionTeam = false;
            changeBoidDirection = false;
            deleteBoid = false;
        }

        // Drawing
        BeginDrawing();
            // ClearBackground((Color){255, 235, 206, 255});
            ClearBackground(RAYWHITE);
            BeginMode2D(camera);

            // Draw fallen boids
            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
                if (boid->sprite == SPRITE_FALL)
                    DrawBoid(boid, texture);
            }

            DrawRectangleLines(0, 0, world_size.x, world_size.y, BLACK);
            DrawCircle(world_size.x, world_size.y, 50 + 10*is_dragging_border, BLACK);

            // Draw selection
            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
                if ((boid->sprite != SPRITE_FALL) && (boid->isSelected)) {
                    DrawSelection(boid, texture);
                }
            }

            // Draw boids
            for (BoidIndex i = 0; i < boidsCount; i++) {
                Boid *boid = &boids[i];
                if (boid->sprite != SPRITE_FALL) {
                    DrawBoid(boid, texture);
                    // DrawText(TextFormat("%d", boid->health), boid->pos.x, boid->pos.y + 40, 20, BLACK);
                }
            }

            float thick = 5/camera.zoom;
            
            // Drawing slection
            if (selecting) {
                float rectangleX = fmin(selectionStart.x, mousePosition.x);
                float rectangleY = fmin(selectionStart.y, mousePosition.y);
                DrawText(TextFormat("%d", selectedBoidsCount), rectangleX, rectangleY-(20/camera.zoom), 20/camera.zoom, BLACK);
                DrawRectangleLinesEx((Rectangle){rectangleX, rectangleY,
                                     fabs(mousePosition.x - selectionStart.x), fabs(mousePosition.y - selectionStart.y)},
                                 thick, BLACK);
            }

            // Draw arrow
            if (showArrow && (Vector2LengthSqr(arrowVector) >= powf(40/camera.zoom, 2))) {
                DrawLineEx(arrowStart, mousePosition, thick, BLACK);
                DrawLineEx(mousePosition, Vector2Add(mousePosition, Vector2Scale(Vector2Rotate(arrowVectorNorm,  160*DEG2RAD), 40/camera.zoom)), thick, BLACK);
                DrawLineEx(mousePosition, Vector2Add(mousePosition, Vector2Scale(Vector2Rotate(arrowVectorNorm, -160*DEG2RAD), 40/camera.zoom)), thick, BLACK);
            }

            // Draw point
            if (mode == MODE_POINT) {
                DrawCircle(mousePosition.x, mousePosition.y, 20/camera.zoom, (Color){0, 0, 0, 50});
            }

            EndMode2D();

            // Draw "Paused" and "Mode" labels
            switch (mode) {
            case MODE_SPAWN:
                DrawText("Mode: Spawn", screenWidth - 140, 10, 20, BLACK);
                break;
            case MODE_SELECT:
                DrawText("Mode: Select", screenWidth - 140, 10, 20, BLACK);
                break;
            case MODE_DIRECTION:
                DrawText("Mode: Direction", screenWidth - 165, 10, 20, BLACK);
                break;
            case MODE_POINT:
                DrawText("Mode: Point", screenWidth - 130, 10, 20, BLACK);
                break;
            }
            if (pause)
                DrawText("Paused", screenWidth - 85, 40, 20, BLACK);

            DrawFPS(10, 10);

            BoidIndex teamsBoidsCount[TEAMS_COUNT] = { 0 };
            BoidIndex allBoidsCount = 0;
            for (size_t i = 0; i < boidsCount; i++) {
                Boid *otherBoid = &boids[i];

                // Skip surrendering and fallen boids
                if (otherBoid->action == ACT_SURRENDER || otherBoid->action == ACT_FALL || otherBoid->action == ACT_DELETE)
                    continue;

                teamsBoidsCount[otherBoid->team]++;
                allBoidsCount++;
            }

            DrawText(TextFormat("ALL: %2d", allBoidsCount), 10, 40, 20, BLACK);
            DrawText(TextFormat("RED: %2d", teamsBoidsCount[TEAM_RED]), 10, 70, 20, RED);
            DrawText(TextFormat("BLUE: %2d", teamsBoidsCount[TEAM_BLUE]), 10, 90, 20, BLUE);
            DrawText(TextFormat("GREEN: %2d", teamsBoidsCount[TEAM_GREEN]), 10, 110, 20, GREEN);
            DrawText(TextFormat("YELLOW: %2d", teamsBoidsCount[TEAM_YELLOW]), 10, 130, 20, ORANGE);
            

        EndDrawing();
        prev_mousePosition = mousePosition;
    }

    UnloadTexture(texture);
    CloseWindow();

    return 0;
}
