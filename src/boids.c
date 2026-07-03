#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define RAYMATH_STATIC_INLINE
#include <raymath.h>

#include "boids.h"

// <============================================ GRID AND CHUNKS ===========================================>

// Clear all chunks (set counts to zero)
void clear_grid(Grid *grid) {
    for (uint32_t i = 0; i < grid->chunks_count; i++) {
        grid->chunks[i].count = 0;
    }
}

// One iteration of fill_grid cycle
void perform_boid_in_fill_grid(Grid *grid, BaseBoid *boid, BoidIndex i) {
    if ((boid->action == ACT_FALL) || (boid->action == ACT_SURRENDER || (boid->action == ACT_DELETE))) return;

    float x = boid->pos.x;
    float y = boid->pos.y;

    if (x < 0) x = 0;
    else if (x > grid->screen_width) x = grid->screen_width;
    if (y < 0) y = 0;
    else if (y > grid->screen_height) y = grid->screen_height;
    
    uint16_t chunk_x = x / grid->chunk_size_pixels;
    uint16_t chunk_y = y / grid->chunk_size_pixels;
    uint32_t chunk_index = chunk_x + chunk_y*grid->cols;

    Chunk *chunk = &grid->chunks[chunk_index];

    if (chunk->count < CHUNK_SIZE_BOIDS)
        chunk->boids[chunk->count++] = i;
}

// <================================================= BOIDS ================================================>

// Push the boid away from the bound
void boid_bound(BaseBoid *boid, int width, int height) {
    if (boid->pos.x < BOID_SIZE/2.0)
        boid->pos.x = BOID_SIZE/2.0;
    else if (boid->pos.x > width-BOID_SIZE/2.0)
        boid->pos.x = width-BOID_SIZE/2.0;
    if (boid->pos.y < BOID_SIZE/2.0)
        boid->pos.y = BOID_SIZE/2.0;
    else if (boid->pos.y > height-BOID_SIZE/2.0)
        boid->pos.y = width-BOID_SIZE/2.0;
    
    // if (boid->pos.x < BOID_BOUND_PADDING)
    //     boid->velocity.x += BOID_BOUND_FACTOR;
    // else if (boid->pos.x > width-BOID_BOUND_PADDING)
    //     boid->velocity.x -= BOID_BOUND_FACTOR;
    // if (boid->pos.y < BOID_BOUND_PADDING)
    //     boid->velocity.y += BOID_BOUND_FACTOR;
    // else if (boid->pos.y > height-BOID_BOUND_PADDING)
    //     boid->velocity.y -= BOID_BOUND_FACTOR;
}

// Adjust boid velocity so its speed stays within [BOID_MIN_SPEED, BOID_MAX_SPEED]
void boid_normal_speed(BaseBoid *boid) {
    float speed = Vector2Length(boid->velocity);
    if (speed > BOID_MAX_SPEED)
        boid->velocity = Vector2Scale(boid->velocity, BOID_MAX_SPEED/speed);
    else if (speed < BOID_MIN_SPEED && boid->action != ACT_STOP)
        boid->velocity = Vector2Scale(boid->velocity, BOID_MIN_SPEED/speed);
}

// boids_count - total boids count
// boid_size - sizeof(original_boid)
void update_base_boid(void *boids, Grid *grid, BoidIndex boid_index, size_t boid_size, bool can_change_action, bool can_fall) {
    BaseBoid *boid = (BaseBoid*)((char*)boids + boid_index*boid_size);

    // Skip if surrendering or fallen
    if (boid->action == ACT_FALL) {
        boid->velocity = (Vector2){ 0 };
        return;
    }
    if (boid->action == ACT_SURRENDER) {
        boid->velocity = (Vector2){ 0 };
        return;
    }

    // Determine if the boid should fall
    if (can_fall && boid->health <= 0) {
        boid->action = ACT_FALL;
        boid->velocity = (Vector2){ 0 };
        return;
    }

    if (boid->action == ACT_STOP) {
        boid->velocity = (Vector2){ 0 };
    }
    
    Vector2 close = { 0 }; // Avoid vector
    BoidIndex teams_boids_count[TEAMS_COUNT] = { 0 }, teams_close_boids_count[TEAMS_COUNT] = { 0 }; // Boids counts per teams
    Vector2 neighbors_velocity = { 0 }; // Alignment velocity vector
    Vector2 neighbors_pos = { 0 }; // Cohesion position vector

    Vector2 nearest_enemy_pos = { 0 };
    Vector2 close_enemies_pos = { 0 }; // Sum of enemy direction vectors for retreat
    float nearest_enemy_distance_sqr = INFINITY;
    BaseBoid *nearest_enemy = NULL;
    BoidIndex nearest_enemy_idx = 0;

    // Clamp position within bounds
    Vector2 pos = boid->pos;
    if (pos.x < 0) pos.x = 0;
    else if (pos.x > grid->screen_width) pos.x = grid->screen_width;
    if (pos.y < 0) pos.y = 0;
    else if (pos.y > grid->screen_height) pos.y = grid->screen_height;

    // Get boid's chunk
    int32_t chunk_x = pos.x / grid->chunk_size_pixels;
    int32_t chunk_y = pos.y / grid->chunk_size_pixels;

    // 8 neighboring chunks + current chunk
    for (int32_t x = chunk_x-1; x <= chunk_x+1; x++) {
        for (int32_t y = chunk_y-1; y <= chunk_y+1; y++) {
            if ((x < 0) || (x >= grid->cols) || (y < 0) || y >= grid->rows) continue;

            uint32_t chunk_index = x + y*grid->cols;
            Chunk *chunk = &grid->chunks[chunk_index];

            // Process boids from neighboring chunks
            for (size_t i = 0; i < chunk->count; i++) {
                BoidIndex other_boid_index = chunk->boids[i];
                if (other_boid_index == boid_index) continue;

                BaseBoid *other_boid = (BaseBoid*)((char*)boids + other_boid_index*boid_size);

                // Skip surrendering and fallen boids
                if (other_boid->action == ACT_SURRENDER || other_boid->action == ACT_FALL) continue;
        
                Vector2 distance_v = Vector2Subtract(boid->pos, other_boid->pos);
                float distance_sqr = Vector2LengthSqr(distance_v);
        
                // Calculate avoid
                if (distance_sqr < BOID_AVOID_RADIUS_SQ)
                    close = Vector2Add(close, distance_v);

                // ~Count only not retreating boids~
                // if (other_boid->b.action != ACT_RETREAT) {
                teams_boids_count[other_boid->team]++;
                if (distance_sqr < BOID_VISIBLE_RADIUS_SQ) {
                    teams_close_boids_count[other_boid->team]++;
                    if (boid->team == other_boid->team && distance_sqr > BOID_AVOID_RADIUS_SQ) { // Cohesion and alignment only with teammates
                        neighbors_velocity = Vector2Add(neighbors_velocity, other_boid->velocity); // Calculate alignment
                        neighbors_pos = Vector2Add(neighbors_pos, other_boid->pos); // Calculate cohesion
                    }
                }
                // }

                // Find nearest enemy
                if ((other_boid->team != boid->team) && (distance_sqr < BOID_NEAREST_ENEMY_RADIUS_SQ) && (distance_sqr < nearest_enemy_distance_sqr)) {
                    nearest_enemy_pos = distance_v;
                    nearest_enemy_distance_sqr = distance_sqr;
                    nearest_enemy = other_boid;
                    nearest_enemy_idx = other_boid_index;
                }

                // Calculate vector for retreat
                if ((boid->action == ACT_RETREAT) && (other_boid->team != boid->team) && (distance_sqr < BOID_VISIBLE_RADIUS_SQ)) {
                    close_enemies_pos = Vector2Add(close_enemies_pos, distance_v);
                }
            }
        }
    }

    if (nearest_enemy != NULL)
        boid->nearest_enemy_idx = nearest_enemy_idx;

    BoidIndex all_enemies_count = 0, close_enemies_count = 0; // Get the enemy team with max boid's count
    BoidIndex all_teammates_count = 0, close_teammates_count = 0;
    for (uint8_t team_index = 0; team_index < TEAMS_COUNT; team_index++) {
        if (team_index != boid->team) { // Count enemies
            if (teams_boids_count[team_index] > all_enemies_count) all_enemies_count = teams_boids_count[team_index];
            if (teams_close_boids_count[team_index] > close_enemies_count) close_enemies_count = teams_close_boids_count[team_index];
        } else { // Count teammates
            if (teams_boids_count[team_index] > all_teammates_count) all_teammates_count = teams_boids_count[team_index];
            if (teams_close_boids_count[team_index] > close_teammates_count) close_teammates_count = teams_close_boids_count[team_index];
        }
    }
    
    if (boid->action != ACT_STOP) {
        // Alignment
        if (close_teammates_count > 0) {
            neighbors_velocity = Vector2Scale(neighbors_velocity, 1.0f / close_teammates_count);
            boid->velocity = Vector2Add(boid->velocity, Vector2Scale(neighbors_velocity, BOID_ALIGNMENT_FACTOR));
        }
    
        // Cohesion
        if (close_teammates_count > 0) {
            neighbors_pos = Vector2Scale(neighbors_pos, 1.0f / close_teammates_count);
            neighbors_pos = Vector2Subtract(neighbors_pos, boid->pos);
            boid->velocity = Vector2Add(boid->velocity, Vector2Scale(neighbors_pos, BOID_COHESION_FACTOR));
        }
    }

    // Determine attack
    if ((boid->action == ACT_STOP) && (nearest_enemy_distance_sqr < BOID_STOP_RADIUS_SQ)) {
        boid->action = ACT_ATTACK;
    }

    // Attack
    nearest_enemy_pos = Vector2Scale(nearest_enemy_pos, -1);
    if ((boid->action == ACT_ATTACK) && (nearest_enemy != NULL)) {
        boid->velocity = Vector2Add(boid->velocity, Vector2Scale(nearest_enemy_pos, BOID_ATTACK_FACTOR));
    }

    // Fighting
    if (boid->fighting_timer > 0) boid->fighting_timer--;
    if ((nearest_enemy != NULL) && (boid->fighting_timer == 0) && (nearest_enemy_distance_sqr < BOID_FIGHTING_RADIUS_SQ)) {
        boid->is_fighting = true;
        boid->fighting_timer = BOID_MAX_FIGHTING_TIMER;
        if (boid->xp < BOID_MAX_XP) boid->xp++;
        
        nearest_enemy->health -= 5 + ((boid->action == ACT_RETREAT)? 0 : 10*((float)boid->xp / BOID_MAX_XP));
        nearest_enemy->is_fighting = true;
        nearest_enemy->hit = true;
        
    }
    if ((nearest_enemy == NULL) || (nearest_enemy_distance_sqr > BOID_FIGHTING_RADIUS_SQ*2)) {
        boid->is_fighting = false;
    }

    // Retreat
    if (boid->action == ACT_RETREAT) {
        boid->velocity = Vector2Add(boid->velocity, Vector2Scale(close_enemies_pos, BOID_RETREAT_FACTOR));
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
    all_teammates_count++;
    close_teammates_count++;

    if (can_change_action) {
        // Determine retreat
        if ((close_enemies_count >= BOID_FOR_RETREAT_MIN) &&
            ((float)close_enemies_count / close_teammates_count >= BOID_FOR_RETREAT_VALUE)) {
            boid->action = ACT_RETREAT;
        }

        // Determine stop
        if (boid->action == ACT_RETREAT) {
            boid->timer++;
            if ((close_enemies_count == 0) && (boid->timer > 6*60)) { // 6 seconds
                boid->timer = 0;
                boid->action = ACT_STOP;
            } else if (close_enemies_count > 0) {
                boid->timer = 0;
            }
        } else if (boid->action == ACT_ATTACK) {
            boid->timer++;
            if (((nearest_enemy == NULL) || (nearest_enemy_distance_sqr < BOID_STOP_RADIUS_SQ)) && (boid->timer > 20*60)) { // 20 seconds
                boid->timer = 0;
                boid->action = ACT_STOP;
            } else if (close_enemies_count > 0) {
                boid->timer = 0;
            }
        }

        // Determine surrender
        // if ((float)all_enemies_count / all_teammates_count >= BOID_FOR_SURRENDER_VALUE) {
        //     boid->action = ACT_SURRENDER;
        //     boid->sprite = SPRITE_SURRENDER;
        //     boid->timer = 0;
        // }
    }

    // Avoid
    boid->velocity = Vector2Add(boid->velocity, Vector2Scale(close, BOID_AVOID_FACTOR * (1/boid->speed)));
}
