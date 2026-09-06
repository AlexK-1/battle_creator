#if defined(__linux__) && (_POSIX_C_SOURCE < 199309L)
    #undef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 199309L
#endif

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

#define RAYMATH_STATIC_INLINE
#include <raylib.h>
#include <raymath.h>

#include "boids.h"
#include "network.h"
#include "logging.h"
#include "queue.h"
#include "kdtree.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#define MAX_ROOMS 256
#define MAX_EVENTS 1024
#define MAX_APPROVING_QUEUE_LEN 10
#define MAX_PACKET_SIZE (1024*64)
#define DEFAULT_TPS 15

#define ERR(str)                                                                                     \
    do {                                                                                             \
        fprintf(stderr, "%s: " str, prog);                                                           \
        exit(EXIT_FAILURE);                                                                          \
    } while (0)

#define ERRF(format, ...)                                                                            \
    do {                                                                                             \
        fprintf(stderr, "%s: " format, prog, __VA_ARGS__);                                           \
        exit(EXIT_FAILURE);                                                                          \
    } while (0)

#define PUSH_DATA(pointer, type, data) (*(type*)(pointer) = (type)(data), (pointer) += sizeof(type))
#define PUSH_MEM(pointer, data, len)                                                                 \
    do {                                                                                             \
        memcpy((pointer), (data), (len));                                                            \
        (pointer) += (len);                                                                          \
    } while (0)
#define PUSH_STRING(pointer, string)                                                                 \
    do {                                                                                             \
        char *s = string;                                                                            \
        while (*s) *((pointer)++) = *(s++);                                                          \
    } while (0)

#define POP_DATA(pointer, type) ((pointer) += sizeof(type), *(type*)((pointer) - sizeof(type)))
#define POP_MEM(pointer, data, len)                                                                  \
    do {                                                                                             \
        memcpy((data), (pointer), (len));                                                            \
        (pointer) += (len);                                                                          \
    } while (0)
#define POP_STRING(pointer, string, max_len)                                                         \
    do {                                                                                             \
        char *s = string;                                                                            \
        int len = 0;                                                                                 \
        while (*(pointer) && (len++) < (max_len)) *((pointer)++) = *(s++);                           \
    } while (0)


/*
join -> room
connect -> server
*/

#define MAX_VIOLATINS_PER_SEC 5

typedef struct Player {
    int tcp_fd;
    struct sockaddr_in tcp_addr, udp_addr;
    uint32_t id;
    char name[USERNAME_LEN];
    uint8_t team;
    bool joined, ready, udp_enabled;
    StartBoids start_boids[MAX_BOIDS_COUNT*2];
    int start_boids_len;
    struct Player *approving_player;
    struct Room *room;
    struct {
        struct Player **items;
        int front, rear, size, max_len;
    } approving_queue, *in_queue;
    struct {
        enum {
            PARSE_TYPE,
            PARSE_LEN,
            PARSE_DATA
        } state;
        uint8_t type;
        uint32_t data_len, bytes_remaining;
        unsigned char *data_buf, recv_buf[1024];
        struct timespec violation_timestamps[MAX_VIOLATINS_PER_SEC];
        int violations_count;
    } net;
} Player;

typedef struct Room {
    Player *players[TEAMS_COUNT];
    BoidIndex teams[TEAMS_COUNT], total_boids_number;
    uint8_t players_number, joined_players;
    uint32_t id;
    Point world;
    pthread_t thread;
    bool thread_run, sync_boids, hide_areas;
    ServerBoid *boids;
    pthread_mutex_t boids_mtx, players_mtx;
    Area areas[MAX_AREAS_COUNT];
    uint16_t areas_count;
    RoomStage stage;
} Room;

/* V global variables V */
Player **players = NULL;
uint32_t last_player_id = 0; // 0 must be an invalid player id
Room *rooms[MAX_ROOMS] = { 0 };
int32_t last_room_idx = -1;
int chunk_size = CHUNK_SIZE_PIXELS, epfd, tcp_fd, udp_fd;
bool udp_opened = false;
long max_fd;
int tps = DEFAULT_TPS;
/* ^ global variables ^ */

// Search by id or fd among the ROOM players
int get_player_idx(Player **players, int fd, uint32_t id) {
    for (int i = 0; i < TEAMS_COUNT; i++) {
        if (fd > 0 && players[i] != NULL && players[i]->tcp_fd == fd) // Search by fd
            return i;
        if (id > 0 && players[i] != NULL && players[i]->id == id) // Search by ID
            return i;
    }
    return -1;
}

Player *find_player(Player **players, int players_count, uint32_t id) {
    for (int i = 0; i < players_count; i++) {
        if (players[i] != NULL && players[i]->id == id) // Search by ID
            return players[i];
    }
    return NULL;
}

static inline int random_value(int min, int max) {
    return rand() % (max-min+1) + min;
}

void close_client(int fd, DisconnectionReason reason, bool cl_room);

void close_room(Room *room, DisconnectionReason reason) {
    if (room->stage == STAGE_GAME && room->thread_run) {
        room->thread_run = false;
        pthread_join(room->thread, NULL);
    }
    
    if (reason == DISCONNECT_ADMIN_CLOSED_ROOM)
        write_log(L_INFO, "room %06x closed py player id=%u\n", room->id, room->players[0]->id);
    else
        write_log(L_INFO, "room %06x closed\n", room->id);

    while (room->players[0]->approving_queue.size > 0) {
        Player *op;
        dequeue(room->players[0]->approving_queue, op);
        op->joined = false;
        close_client(op->tcp_fd, reason, false);
    }

    pthread_mutex_lock(&room->players_mtx);
    for (int i = 0; i < room->joined_players; i++) {
        Player *p = room->players[i];
        if (p->joined) {
            /* SP_DISCONNECT_PLAYER PACKET FORMAT
            (uint8 reason)
            */
            uint8_t buf = reason;
            send_packet(p->tcp_fd, SP_DISCONNECT_PLAYER, &buf, sizeof(buf), MSG_NOSIGNAL);

            p->joined = false;
            close_client(p->tcp_fd, reason, false);
        }
    }
    pthread_mutex_unlock(&room->players_mtx);

    if (room->boids != NULL) free(room->boids);
    free(room);
    rooms[last_room_idx] = NULL;
    last_room_idx--;
}

void close_client(int fd, DisconnectionReason reason, bool cl_room) {
    //cl_room - close the room if admin is disconnected
    
    Player *p = players[fd];

    // Room
    bool room_closed = false;
    if (p->joined && p->room != NULL) {
        Room *room = p->room;

        int player_idx = get_player_idx(room->players, p->tcp_fd, 0);

        // Send a message to all players in the room that the player has disconnected
        uint32_t nid = htonl(p->id);
        pthread_mutex_lock(&room->players_mtx);
        for (int i = 0; i < room->joined_players; i++) {
            Player *op = room->players[i];
            if (op->id != room->players[player_idx]->id)
                send_packet(op->tcp_fd, SP_PLAYER_EXIT, &nid, sizeof(op->id), MSG_NOSIGNAL);
        }
        pthread_mutex_unlock(&room->players_mtx);

        /* SP_DISCONNECT_PLAYER PACKET FORMAT
        (uint8 reason)
        */
        uint8_t buf = reason;
        send_packet(fd, SP_DISCONNECT_PLAYER, &buf, sizeof(buf), MSG_NOSIGNAL);

        last_room_idx = ((room->id & 0xffff0000) >> 16);
        if ((room->stage == STAGE_AREAS   && player_idx == 0 && cl_room) || // Room's creator disconnected
            (room->stage == STAGE_PLACING && player_idx == 0 && cl_room) || // Room's creator disconnected
             room->joined_players == 1) { // Last player disconnected
            p->joined = false;
            close_room(room, DISCONNECT_ADMIN_EXITED); // Close entire room
            room_closed = true;
        } else {
            // Delete player from array
            pthread_mutex_lock(&room->players_mtx);
            memmove(room->players + player_idx, room->players + player_idx + 1,
                    sizeof(room->players[0]) * (room->joined_players - player_idx - 1));
            room->joined_players--;
            pthread_mutex_unlock(&room->players_mtx);
        }

    }

    if (!room_closed) {
        // Close files
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);

        if (p->approving_queue.max_len != 0) free(p->approving_queue.items);
        if (!p->joined && p->in_queue != NULL && p->in_queue->size > 0) {
            int find_idx = -1;
            for (int i = 0; i < p->in_queue->size; i++) {
                int idx = (p->in_queue->front + i) % p->in_queue->max_len;
                if (p->in_queue->items[idx] == p) {
                    find_idx = idx;
                    break;
                }
            }

            if (find_idx != -1) {
                Player **q = p->in_queue->items;
                if (find_idx < p->in_queue->max_len-1)
                    memmove(q + find_idx, q + find_idx + 1, sizeof(*q) * (p->in_queue->max_len-1 - find_idx));
                if (p->in_queue->rear < p->in_queue->front) {
                    q[p->in_queue->max_len-1] = q[0];
                    memmove(q, q + 1, sizeof(*q) * (p->in_queue->rear));
                }
            }

            p->in_queue->size--;
            p->in_queue->rear--;
            
            if (find_idx == p->in_queue->front) {
                uint32_t nid = htonl(p->id);
                send_packet(p->approving_player->tcp_fd, SP_PLAYER_EXIT, &nid, sizeof(nid), 0);

                if (p->in_queue->size > 0) {
                    Player *new_approved_player = queue_front(*p->in_queue);
            
                    /* SP_APPROVE_PLAYER PACKET FORMAT
                    (uint32 player_id) (uint8[USERNAME_LEN] username)
                    */
                    
                    const uint32_t packet_size = /*id*/ sizeof(uint32_t) + /*username*/ USERNAME_LEN;
                    char data[packet_size];
                    char *d = data;

                    PUSH_DATA(d, uint16_t, htonl(new_approved_player->id));
                    strcpy(d, new_approved_player->name);
                    
                    send_packet(p->approving_player->tcp_fd, SP_APPROVE_PLAYER, data, packet_size, 0);
                }
            }
        }

        write_log(L_DISCONNECT, "fd=%d id=%d hung up\n", fd, p->id);
        
        if (p->net.data_buf != NULL)
            free(p->net.data_buf);
        free(p);
        players[fd] = NULL;
    }
}

void *room_thread_fn(void *args) {
    Room *room = args;
    
    ServerBoid *boids = room->boids = calloc(room->total_boids_number, sizeof(*room->boids));
    if (boids == NULL) {
        room->thread_run = false;
        close_room(room, DISCONNECT_SERVER_ERROR);
        return NULL;
    }
    BoidIndex boids_count = 0;
    
    // Place boids
    pthread_mutex_lock(&room->players_mtx);
    for (int player_idx = 0; player_idx < room->joined_players; player_idx++) {
        Player *player = room->players[player_idx];
        
        int cell = 0, cell_x = 0, cell_y = 0;
        for (int i = 0; i < player->start_boids_len; i++) {
            StartBoids b = player->start_boids[i];
            if (b.team < 0) {
                cell += b.count;
                cell_x = cell % (room->world.x / BOID_SIZE);
                cell_y = cell / (room->world.y / BOID_SIZE);
            } else {
                for (int i = 0; i < b.count; i++) {
                    if (boids_count >= room->total_boids_number)
                        break;

                    bool in_area = false;
                    for (int i = 0; i < room->areas_count; i++) {
                        Area *a = &room->areas[i];
                        if (a->rec.x1 <= cell_x && a->rec.x2 > cell_x &&
                            a->rec.y1 <= cell_y && a->rec.y2 > cell_y) {
                            in_area = (a->team == b.team);
                            break;
                        }
                    }

                    if (in_area) {
                        ServerBoid new_boid = {.b = {.pos = {cell_x*BOID_SIZE + (int)(BOID_SIZE/2.0), cell_y*BOID_SIZE + (int)(BOID_SIZE/2.0)},
                                                     .velocity = { 0 }, .speed = random_value(80, 130)/100.0,
                                                     .max_health = random_value(BOID_MIN_HEALTH, BOID_MAX_HEALTH),
                                                     .xp = random_value(0, 5), .team = player->team, .action = ACT_STOP, .boid_idx = boids_count}};
                        new_boid.b.health = new_boid.b.max_health;
                        room->boids[boids_count++] = new_boid;
                    }
                
                    cell++;
                    cell_x++;
                    if (cell_x >= room->world.x / BOID_SIZE) {
                        cell_x = 0;
                        cell_y++;
                    }
                }
            }
        }

        if (boids_count > room->total_boids_number)
            break;
    }
    pthread_mutex_unlock(&room->players_mtx);

    if (boids_count > room->total_boids_number) {
        close_room(room, DISCONNECT_SERVER_ERROR);
        return NULL;
    }

    /* SP_START_GAME PACKET FORMAT
    (uint16 boids_count) ( {(uint16 x) (uint16 y) (uint8 speed) (uint8 xp) (uint8 team) (uint8 max_health) }[boids_count] boids)
    */
    
    uint32_t packet_size = 2 + (2 + 2 + 1 + 1 + 1 + 1)*boids_count;
    char *data = malloc(packet_size);
    char *d = data;

    PUSH_DATA(d, BoidIndex, htons(boids_count));

    for (int i = 0; i < boids_count; i++) {
        BaseBoid *boid = &room->boids[i].b;

        PUSH_DATA(d, uint16_t, htons(boid->pos.x));
        PUSH_DATA(d, uint16_t, htons(boid->pos.y));
        PUSH_DATA(d, uint8_t,  boid->speed*100.0);
        PUSH_DATA(d, uint8_t,  boid->xp);
        PUSH_DATA(d, uint8_t,  boid->team);
        PUSH_DATA(d, uint8_t,  boid->max_health);
    }

    pthread_mutex_lock(&room->players_mtx);
    for (int i = 0; i < room->joined_players; i++) {
        send_packet(room->players[i]->tcp_fd, SP_START_GAME, data, packet_size, 0);
    }
    pthread_mutex_unlock(&room->players_mtx);

    free(data);

    // Grid of chunks
    Grid grid = { 0 };
    INIT_GRID(&grid, (BaseBoid*)boids, boids_count, room->world.x, room->world.y, chunk_size);

    double target_delay = 1.0 / (double)tps;
    double delay = 0.0;
    int timer = 0;

    while (room->thread_run && boids_count > 0) {
        clock_t prev_time = clock();

        // Update boids
        for (BoidIndex i = 0; i < boids_count; i++) {
            ServerBoid *boid = &boids[i];

            if (boid->b.action == ACT_DELETE) continue;

            if (boid->o.order_timer > 0) {
                boid->o.order_timer--;

                if (boid->o.point_order && Vector2Distance(boid->o.order_vector, boid->b.pos) < BOID_SIZE) {
                    boid->o.order_timer = 0;
                    boid->b.action = ACT_STOP;
                }

                // Change direction by order
                Vector2 direction = { 0 };
                if (boid->o.direction_order)
                    direction = boid->o.order_vector;
                else if (boid->o.point_order)
                    direction = Vector2Normalize(Vector2Subtract(boid->o.order_vector, boid->b.pos));
                boid->b.velocity = Vector2Add(boid->b.velocity, Vector2Scale(direction, BOID_ORDER_FACTOR));
            }
            update_base_boid(boids, &grid, i, sizeof(*boids), /*can_change_action=*/ boid->o.order_timer == 0, /*can_fall=*/ true);

            if (boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL) {
                boid_normal_speed((BaseBoid*)boid);
                boid_bound((BaseBoid*)boid, room->world.x, room->world.y);
            }

            boid->b.pos = Vector2Add(boid->b.pos, Vector2Scale(boid->b.velocity, boid->b.speed * (60.0f/tps)));
        }

        pthread_mutex_lock(&room->boids_mtx);
        if (timer == 0 || room->sync_boids) {
            // Send boids data to clients (boids sync)

            /* SP_BOIDS_SYNC PACKET FORMAT
            (uint8 current_server_tps) (uint16 boids_count) (uint16 first_boid_index)
            ({
              (uint16 x) (uint16 y) (int8 health) (uint8 xp) (int8 action) (uint8 angle) (int8 vel)
            }[boids_count] boids)
            */

            uint32_t boid_size = 2 + 2 + 1 + 1 + 1 + 1 + 1;
            uint32_t packet_size = 1 + 2 + 2 + boid_size*boids_count;
            char *data = malloc(packet_size);
            memset(data, 0, packet_size);
            char *d = data;

            PUSH_DATA(d, uint8_t,   (tps - timer) / (float)delay);
            PUSH_DATA(d, BoidIndex, htons(boids_count));
            PUSH_DATA(d, BoidIndex, htons(0));

            for (int i = 0; i < boids_count; i++) {
                BaseBoid *b = &boids[i].b;

                PUSH_DATA(d, uint16_t, htons(b->pos.x)); // x
                PUSH_DATA(d, uint16_t, htons(b->pos.y)); // y
                PUSH_DATA(d, int8_t,   b->health); // health
                PUSH_DATA(d, uint8_t,  b->xp); // xp
                PUSH_DATA(d, int8_t,   b->action); // action
                PUSH_DATA(d, uint8_t,  atan2f(b->velocity.y, b->velocity.x)/PI*127); // angle
                PUSH_DATA(d, int8_t,   Vector2Length(b->velocity)/BOID_MAX_SPEED*255); // vel
            }

            pthread_mutex_lock(&room->players_mtx);
            
            bool send_udp = false;
            for (int i = 0; i < room->joined_players; i++) {
                Player *p = room->players[i];

                if (udp_opened && p->udp_enabled) {
                    send_udp = true;
                } else {
                    // Send by TCP
                    send_packet(p->tcp_fd, SP_BOIDS_SYNC, data, packet_size, 0);
                }
            }

            // At least one client has UDP enabled
            if (send_udp) {
                const int boids_in_packet = 1400 / boid_size;
                BoidIndex boids_sent = 0;
                char *packet = data;

                // Divide all data to small packets and send by UDP
                while (boids_sent < boids_count) {
                    char *p = packet;
                    
                    BoidIndex send_boids_count = MIN(boids_in_packet, boids_count - boids_sent);
                    
                    PUSH_DATA(p, uint8_t,   (tps - timer) / (float)delay);
                    PUSH_DATA(p, BoidIndex, htons(send_boids_count));
                    PUSH_DATA(p, BoidIndex, htons(boids_sent));
                    
                    uint32_t packet_size = 1 + 2 + 2 + boid_size*send_boids_count;

                    for (int i = 0; i < room->joined_players; i++) {
                        Player *p = room->players[i];
                        if (p->udp_enabled)
                            sendto_packet(udp_fd, SP_BOIDS_SYNC, packet, packet_size, 0, (struct sockaddr*)&p->udp_addr, sizeof(p->udp_addr));
                    }

                    packet += boid_size*send_boids_count;
                    boids_sent += send_boids_count;
                }
            }
            
            pthread_mutex_unlock(&room->players_mtx);

            free(data);

            room->sync_boids = false;
            timer = tps; // 1 second
            delay = 0;
        }
        timer--;
        
        clear_grid(&grid);
        FILL_GRID(&grid, boids, boids_count);
        pthread_mutex_unlock(&room->boids_mtx);

        
        clock_t current_time = clock();
        double tick_seconds = ((double)(current_time - prev_time)) / CLOCKS_PER_SEC;

        if (tick_seconds < target_delay) {
            delay += target_delay;
            double wait_seconds = target_delay - tick_seconds;
            time_t sec = wait_seconds;
            double nsec = (wait_seconds - sec)*1000000000L;
            struct timespec delay = {.tv_sec = sec, .tv_nsec = nsec};
            while (nanosleep(&delay, &delay) == -1) continue;
        } else {
            delay += tick_seconds;
        }
    }

    free_grid(&grid);

    pthread_mutex_destroy(&room->boids_mtx);
    pthread_mutex_destroy(&room->players_mtx);

    room->thread_run = false;
    return NULL;
}

// Copy SRC to DST, removing unnecessary characters
char *copy_correct_username(char *dst, const char *src) {
    char *d = dst;
    while (*src != '\0') {
        if (isalnum(*src) || *src == '-' || *src == '_')
            *(d++) = *src;
        src++;
    }
    *d = '\0';

    if (strlen(dst) == 0)
        strcpy(dst, "noname");

    return dst;
}

#define INVALID_PACKET()                                                  \
    do {                                                                  \
        if (p->net.violations_count < MAX_VIOLATINS_PER_SEC) {            \
            struct timespec now;                                          \
            clock_gettime(CLOCK_MONOTONIC, &now);                         \
            p->net.violation_timestamps[p->net.violations_count++] = now; \
        }                                                                 \
        invalid_packet = true;                                            \
        goto switch_exit;                                                 \
    } while (0)

#define CHECK_PACKET(expr)                                                \
    do {                                                                  \
        if (!(expr)) {                                                    \
            INVALID_PACKET();                                             \
        }                                                                 \
    } while (0)

void process_data(Player *p) {
    int package_type = p->net.type;
    uint32_t packet_size = p->net.data_len;
    char *packet_data = (char*)p->net.data_buf;
    char *d = packet_data;

    bool critical_packet = false;
    bool invalid_packet = false;
    
    switch (package_type) {
    case CP_NEW_ROOM: {
        /* CP_NEW_ROOM
        (uint8 player_team) (uint8 players_number) (uint8 hide_areas) (uint16 world_size_x) (uint16 world_size_y)
        (uint16[TEAMS_COUNT] boids_number) (uint8[USERNALE_LEN] creator)
        */

        /* SP_JOIN_PLAYER PACKET FORMAT
        (uint8 status)
        (
          if status == JOIN_OK {
            (uint32 room_id) (uint32 player_id) (int32 player_tcp_fd) (uint8 players_number) (uint8 joined_players) (uint8 player_team)
            (uint8 server_target_tps) (uint8 room_stage) (uint16 world_size_x) (uint16 world_size_y) (uint16[TEAMS_COUNT] teams)
            ({
              (uint32 id) (uint8 team) (uint8 ready) (uint8[USERNAME_LEN] username)
            }[joined_players] players)
          }
        )
        */

        critical_packet = true;

        CHECK_PACKET(packet_size == (uint32_t)(1 + 1 + 1 + 2 + 2 + 2*TEAMS_COUNT + USERNAME_LEN));

        bool free_rooms =  false;
        for (int j = 1; j < MAX_ROOMS; j++) {
            if (rooms[(last_room_idx + j) % MAX_ROOMS] == NULL) {
                last_room_idx = (last_room_idx + j) % MAX_ROOMS;
                free_rooms = true;
                break;
            }
        }
        
        if (!free_rooms) {
            write_log(L_WARNING, "no free rooms\n");

            uint8_t send_data = JOIN_FAILED;
            send_packet(p->tcp_fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
            close_client(p->tcp_fd, DISCONNECT_SERVER_ERROR, true);
            
            return;
        }

        uint8_t team = POP_DATA(d, uint8_t);
        CHECK_PACKET(team < TEAMS_COUNT);

        uint8_t players_number = POP_DATA(d, uint8_t);
        CHECK_PACKET(players_number <= TEAMS_COUNT);

        bool hide_areas = POP_DATA(d, uint8_t);

        Point world_size;
        world_size.x = ntohs(POP_DATA(d, uint16_t));
        world_size.y = ntohs(POP_DATA(d, uint16_t));
        
        BoidIndex boids_number[TEAMS_COUNT];
        BoidIndex total_boids_number = 0;
        for (int i = 0; i < TEAMS_COUNT; i++) {
            boids_number[i] = ntohs(POP_DATA(d, uint16_t));
            total_boids_number += boids_number[i];
        }
        CHECK_PACKET(total_boids_number <= MAX_BOIDS_COUNT);

        CHECK_PACKET(boids_number[team] > 0);
        
        char username[USERNAME_LEN];
        POP_MEM(d, username, USERNAME_LEN);
        username[USERNAME_LEN-1] = '\0';
        
        p->team = team;

        p->joined = true;
        if (p->approving_queue.max_len == 0)
            init_queue(p->approving_queue, MAX_APPROVING_QUEUE_LEN);
        
        Room *room = malloc(sizeof(Room));

        room->players_number = players_number;
        room->joined_players = 1;
        room->players[0] = p;
        room->hide_areas = hide_areas;
        room->world.x = world_size.x;
        room->world.y = world_size.y;
        room->boids = NULL;
        room->thread_run = room->sync_boids = false;
        room->stage = STAGE_AREAS;
        for (int i = 0; i < TEAMS_COUNT; i++)
            room->teams[i] = boids_number[i];

        rooms[last_room_idx] = room;
        p->room = room;

        copy_correct_username(p->name, username);
        
        room->total_boids_number = total_boids_number;

        /* ROOM ID FORMAT
        [ byte 1 ][ byte 2 ][ byte 3 ][ byte 4 ]
        [aaaaaaaa][aaaaaaaa][cccccccc][cccccctt]
        a - room index in array
        c - boids number
        t - teams (players) number
        byte 1 is hidden
        */

        room->id = (last_room_idx << 16) | (total_boids_number << 2) | room->players_number;
        rooms[last_room_idx] = room;
        write_log(L_INFO, "new room\n    id: %06x\n    teams: %d\n    world: %dx%d\n    creator: %s (id=%d)\n    boids:  %-4d\n    red:    %-4d\n    blue:   %-4d\n    green:  %-4d\n    yellow: %-4d\n",
               room->id, room->players_number, room->world.x, room->world.y, p->name, p->id, total_boids_number,
               room->teams[TEAM_RED],
               room->teams[TEAM_BLUE],
               room->teams[TEAM_GREEN],
               room->teams[TEAM_YELLOW]);

        // TODO: Do it normally
        const uint32_t size = 1 + 4 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2*TEAMS_COUNT + (4 + 1 + 1 + USERNAME_LEN)*room->joined_players;
        char *send_data = malloc(size);
        char *d = send_data;
        
        PUSH_DATA(d, uint8_t, JOIN_OK);
        
        PUSH_DATA(d, uint32_t, htonl(room->id));
        PUSH_DATA(d, uint32_t, htonl(p->id));
        PUSH_DATA(d, int32_t,  htonl(p->tcp_fd));
        PUSH_DATA(d, uint8_t,  room->players_number);
        PUSH_DATA(d, uint8_t,  room->joined_players);
        PUSH_DATA(d, uint8_t,  p->team);
        PUSH_DATA(d, uint8_t,  tps);
        PUSH_DATA(d, uint8_t,  STAGE_AREAS);
        PUSH_DATA(d, uint16_t, htons(room->world.x));
        PUSH_DATA(d, uint16_t, htons(room->world.y));
        
        for (int i = 0; i < TEAMS_COUNT; i++) {
            PUSH_DATA(d, uint16_t, htons(room->teams[i]));
        }
        
        for (int i = 0; i < room->joined_players; i++) {
            Player *op = room->players[i]; // other_player
            PUSH_DATA(d, uint32_t, htonl(op->id));
            PUSH_DATA(d, uint8_t, op->team);
            PUSH_DATA(d, uint8_t, op->ready);
            PUSH_MEM(d, op->name, USERNAME_LEN);
        }
                
        send_packet(p->tcp_fd, SP_JOIN_PLAYER, send_data, size, 0);
        free(send_data);
        
        break;
        }
    case CP_JOIN_ROOM: {
        /* CP_JOIN_ROOM PACKET FORMAT
        (uint32 room_id) (uint8[USERNAME_LEN] username)
        */

        critical_packet = true;

        CHECK_PACKET(packet_size == (uint32_t)(4 + USERNAME_LEN));

        uint32_t room_id = ntohl(POP_DATA(d, uint32_t));

        char username[USERNAME_LEN];
        POP_MEM(d, username, USERNAME_LEN);
        username[USERNAME_LEN-1] = '\0';
        
        uint16_t room_idx = (room_id & 0xffff0000) >> 16;
        if (room_idx > MAX_ROOMS-1) {
            uint8_t send_data = JOIN_FAILED;
            send_packet(p->tcp_fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
            close_client(p->tcp_fd, DISCONNECT_SERVER_ERROR, false);
            return;
        }
        
        Room *room = rooms[room_idx];
        if ((room != NULL) && (room->id == room_id) && (room->joined_players < room->players_number) &&
            (room->stage == STAGE_AREAS || room->stage == STAGE_PLACING)) {
            copy_correct_username(p->name, username);

            bool unique_username;
            do {
                unique_username = true;
                // If the username of the new player matches the username of another player, add a numbers to it at the end
                for (int i = 0; i < room->joined_players; i++) {
                    if (strcmp(p->name, room->players[i]->name) == 0) {
                        char suffix[5];
                        snprintf(suffix, sizeof(suffix), "_%1d", room->joined_players);
                        strcat(p->name, suffix);
                        unique_username = false;
                    }
                }
            } while (!unique_username);

            Player *room_owner = room->players[0];
            if (is_queue_full(room_owner->approving_queue)) {
                uint8_t send_data = JOIN_FAILED;
                send_packet(p->tcp_fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
                close_client(p->tcp_fd, DISCONNECT_SERVER_ERROR, false);
                return;
            }
            
            enqueue(room_owner->approving_queue, p);
            p->approving_player = room_owner;
            p->in_queue = &room_owner->approving_queue;
            
            if  (room_owner->approving_queue.size == 1) {
                /* SP_APPROVE_PLAYER PACKET FORMAT
                (uint32 player_id) (int8[USERNAME_LEN] username)
                */

                const uint32_t size = /*id*/ sizeof(uint32_t) + /*username*/ USERNAME_LEN;
                char data[size];
                char *d = data;

                PUSH_DATA(d, uint32_t, htonl(p->id));
                strcpy(d, p->name);
                
                send_packet(room_owner->tcp_fd, SP_APPROVE_PLAYER, data, size, 0);
            }
        } else {
            uint8_t send_data = JOIN_FAILED;
            send_packet(p->tcp_fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
            close_client(p->tcp_fd, DISCONNECT_SERVER_ERROR, false);
            return;
        }
        
        break;
        }
    case CP_APPROVE_PLAYER: {
        /* CP_APPROVE_PLAYER PACKET FORMAT
        (uint32 player_id) (int8 team)
        */

        /* SP_JOIN_PLAYER PACKET FORMAT
        (uint8 status)
        (
          if status == JOIN_OK {
            (uint32 room_id) (uint32 player_id) (int32 player_tcp_fd) (uint8 players_number) (uint8 joined_players) (uint8 player_team)
            (uint8 server_target_tps) (uint8 room_stage) (uint16 world_size_x) (uint16 world_size_y) (uint16[TEAMS_COUNT] teams)
            ({
              (uint32 id) (uint8 team) (uint8 ready) (uint8[USERNAME_LEN] username)
            }[joined_players] players)
          }
        )
        */

        CHECK_PACKET(packet_size == (uint32_t)(4 + 1));

        CHECK_PACKET(p->room != NULL && p->approving_queue.size > 0);
        
        uint32_t approved_player_id = ntohl(POP_DATA(d, uint32_t));
        CHECK_PACKET(approved_player_id > 0);
        
        int8_t approved_player_team = POP_DATA(d, int8_t);
        CHECK_PACKET(approved_player_team < TEAMS_COUNT);
        
        Player *approved_player = queue_front(p->approving_queue);
        if (approved_player->id != approved_player_id)
            break;
        
        dequeue(p->approving_queue, approved_player);
        approved_player->in_queue = NULL;

        if (approved_player->joined)
            break;
        
        if (approved_player_team == -1) {
            approved_player->joined = false;
            uint8_t send_data = JOIN_REJECTED;
            send_packet(approved_player->tcp_fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
            close_client(approved_player->tcp_fd, DISCONNECT_KICKED, false);
            return;
        } else {
            approved_player->joined = true;
            approved_player->ready = false;
            approved_player->team = approved_player_team;
            approved_player->room = p->room;

            Room *room = p->room;
            room->players[room->joined_players++] = approved_player;

            // TODO: Do it normally
            const uint32_t size = 1 + 4 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2*TEAMS_COUNT + (4 + 1 + 1 + USERNAME_LEN)*room->joined_players;
            char *send_data = malloc(size);
            char *d = send_data;
        
            PUSH_DATA(d, uint8_t, JOIN_OK);
        
            PUSH_DATA(d, uint32_t, htonl(room->id));
            PUSH_DATA(d, uint32_t, htonl(approved_player->id));
            PUSH_DATA(d, int32_t,  htonl(approved_player->tcp_fd));
            PUSH_DATA(d, uint8_t,  room->players_number);
            PUSH_DATA(d, uint8_t,  room->joined_players);
            PUSH_DATA(d, uint8_t,  approved_player->team);
            PUSH_DATA(d, uint8_t,  tps);
            PUSH_DATA(d, uint8_t,  room->stage);
            PUSH_DATA(d, uint16_t, htons(room->world.x));
            PUSH_DATA(d, uint16_t, htons(room->world.y));
        
            for (int i = 0; i < TEAMS_COUNT; i++) {
                PUSH_DATA(d, uint16_t, htons(room->teams[i]));
            }
        
            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i]; // other_player
                PUSH_DATA(d, uint32_t, htonl(op->id));
                PUSH_DATA(d, uint8_t, op->team);
                PUSH_DATA(d, uint8_t, op->ready);
                PUSH_MEM(d, op->name, USERNAME_LEN);
            }
            
            send_packet(approved_player->tcp_fd, SP_JOIN_PLAYER, send_data, size, 0);
            free(send_data);

            if (!room->hide_areas) {
                /* SP_SEND_AREAS PACKET FORMAT
                (uint16 areas_count) ( { (uint16 x1) (uint16 y1) (uint16 x2) (uint16 y2) (uint8 team) }[areas_count] areas)
                */
                
                const uint32_t size = 2 + (2 + 2 + 2 + 2 + 1)*room->areas_count;
                char *data = malloc(size);
                char *d = data;

                PUSH_DATA(d, uint16_t, htons(room->areas_count));
                for (int i = 0; i < room->areas_count; i++) {
                    Area *a = &room->areas[i];
                    PUSH_DATA(d, uint16_t, htons(a->rec.x1));
                    PUSH_DATA(d, uint16_t, htons(a->rec.y1));
                    PUSH_DATA(d, uint16_t, htons(a->rec.x2));
                    PUSH_DATA(d, uint16_t, htons(a->rec.y2));
                    PUSH_DATA(d, uint8_t, a->team);
                }

                send_packet(approved_player->tcp_fd, SP_SEND_AREAS, data, size, 0);

                free(data);
            }
            
            // Send a message to all players in the room that the player has joined
            
            /* SP_NEW_JOIN PACKET FORMAT
            (uint32 id) (uint8 team) (uint8 ready) (uint8[USERNAME_LEN] username)
            */
            const uint32_t player_packet_size = 4 + 1 + 1 + USERNAME_LEN;
            char player_packet[player_packet_size];
            char *b = player_packet;
            
            PUSH_DATA(b, uint32_t, htonl(approved_player->id));
            PUSH_DATA(b, uint8_t,  approved_player->team);
            PUSH_DATA(b, uint8_t,  false); // ready
            PUSH_MEM(b, approved_player->name, USERNAME_LEN);
            
            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i];
                if (op != approved_player)
                    send_packet(op->tcp_fd, SP_NEW_JOIN, player_packet, player_packet_size, 0);
            }

            if (room->joined_players == room->players_number) {
                while (!is_queue_empty(p->approving_queue)) {
                    Player *op;
                    dequeue(p->approving_queue, op);

                    uint8_t send_data = JOIN_FAILED;
                    send_packet(op->tcp_fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
                    close_client(op->tcp_fd, DISCONNECT_SERVER_ERROR, false);
                }
            }
        }

        if (p->approving_queue.size > 0) {
            approved_player = queue_front(p->approving_queue);
            
            /* SP_APPROVE_PLAYER PACKET FORMAT
            (uint32 player_id) (int8[USERNAME_LEN] username)
            */

            const uint32_t size = /*id*/ sizeof(uint32_t) + /*username*/ USERNAME_LEN;
            char data[size];
            char *d = data;

            PUSH_DATA(d, uint32_t, htonl(approved_player->id));
            strcpy(d, approved_player->name);
            
            send_packet(p->tcp_fd, SP_APPROVE_PLAYER, data, size, 0);
        }

        break;
        }
    case CP_START_PLACING:
    case CP_SEND_AREAS: {
        /* CP_START_PLACING|CP_SEND_AREAS PACKET FORMAT
        (uint16 areas_count) ( { (uint16 x1) (uint16 y1) (uint16 x2) (uint16 y2) (uint8 team) }[areas_count] areas)
        */

        Room *room = p->room;
        CHECK_PACKET(room != NULL);
        
        if (packet_size < sizeof(int16_t) || room->players[0]->tcp_fd != p->tcp_fd ||
            (room->joined_players != room->players_number && package_type == CP_START_PLACING) ||
            room->stage != STAGE_AREAS)
            break;

        uint16_t areas_count = ntohs(POP_DATA(d, uint16_t));
        CHECK_PACKET(areas_count < MAX_AREAS_COUNT);

        CHECK_PACKET(packet_size == (uint32_t)(2 + (2 + 2 + 2 + 2 + 1)*areas_count));

        char *areas_d = d;
        
        for (int i = 0; i < areas_count; i++) {
            d += 2 + 2 + 2 + 2;
            uint8_t team = POP_DATA(d, uint8_t);
            CHECK_PACKET(team < TEAMS_COUNT);
        }
        
        // Resend this message to other players (skip room admin)
        for (int i = 1; i < room->joined_players; i++) {
            Player *op = room->players[i];
            send_packet(op->tcp_fd, (package_type == CP_START_PLACING)? SP_START_PLACING : SP_SEND_AREAS, packet_data, packet_size, 0);
        }

        d = areas_d;

        room->areas_count = areas_count;
        for (int i = 0; i < areas_count; i++) {
            room->areas[i].rec.x1 = ntohs(POP_DATA(d, uint16_t));
            room->areas[i].rec.y1 = ntohs(POP_DATA(d, uint16_t));
            room->areas[i].rec.x2 = ntohs(POP_DATA(d, uint16_t));
            room->areas[i].rec.y2 = ntohs(POP_DATA(d, uint16_t));
            room->areas[i].team = POP_DATA(d, uint8_t);
        }

        if (package_type == CP_START_PLACING) {
            // Send a message to admin of the room
            areas_count = 0;
            send_packet(p->tcp_fd, SP_START_PLACING, &areas_count, sizeof(areas_count), 0);
        
            room->stage = STAGE_PLACING;
            write_log(L_INFO, "room %06x started placing\n", room->id);
        }

        break;
        }
    case CP_SEND_BOIDS: {
        /* CP_SEND_BOIDS PACKET FORMAT
        (uint16 count) ({ (uint16 boids_count) (int8 team) }[count] boids)
        */

        Room *room = p->room;
        CHECK_PACKET(room != NULL);
        
        CHECK_PACKET(packet_size >= sizeof(uint16_t) && room->stage == STAGE_PLACING);

        uint16_t count = ntohs(POP_DATA(d, uint16_t));
        CHECK_PACKET(packet_size == (uint32_t)(2 + (2 + 1)*count));

        char *boids_d = d; // Save pointer

        BoidIndex boids_sum = 0;
        for (int i = 0; i < count; i++) {
            BoidIndex boids = ntohs(POP_DATA(d, uint16_t));
            int8_t team = POP_DATA(d, int8_t);
            CHECK_PACKET(team == -1 || team == p->team);
            if (team != -1)
                boids_sum += boids;
        }
        CHECK_PACKET(boids_sum <= room->teams[p->team]);
        if (boids_sum < room->teams[p->team]) break;

        d = boids_d;
        
        for (int i = 0; i < count; i++) {
            p->start_boids[i].count = ntohs(POP_DATA(d, uint16_t));
            p->start_boids[i].team = POP_DATA(d, int8_t);
        }

        p->start_boids_len = count;
        p->ready = true;

        // Send a message, that the player is ready
        uint32_t nid = htonl(p->id);
        for (int i = 0; i < room->joined_players; i++) {
            Player *op = room->players[i];
            send_packet(op->tcp_fd, SP_PLAYER_READY, &nid, sizeof(nid), 0);
        }

        int ready_count = 0;
        for (int i = 0; i < room->joined_players; i++) {
            Player *op = room->players[i];
            if (op->ready)
                ready_count++;
        }

        if (ready_count == room->players_number) {
            room->stage = STAGE_GAME;
            write_log(L_INFO, "room %06x started the game\n", room->id);

            room->thread_run = true;
            pthread_mutex_init(&room->boids_mtx, NULL);
            pthread_mutex_init(&room->players_mtx, NULL);

            pthread_create(&room->thread, NULL, room_thread_fn, room);
        }
        
        break;
        }
    case CP_ORDER: {
        /* CP_ORDER PACKET FORMAT
        ORDER_CLEAR - (int8 order_type) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_ACTION - (int8 order_type) (int8 new_action) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_DIRECTION - (int8 order_type) (int32 vector.x*65535) (int32 vector.y*65535) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_POINT - (int8 order_type) (uint16 point.x) (uint16 point.y) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_LINE - (int8 order_type) (uint8 points_count) ({ (uint16 x) (uint16 y) }[points_count] points) (uint16 boids_count) (uint16[boids_count] boids)
        */

        Room *room = p->room;
        if (room == NULL)
            break;

        if (room->stage != STAGE_GAME)
            break;

        uint8_t order_type = POP_DATA(d, int8_t);
        BoidIndex max_boids = room->teams[p->team];

        if (order_type == ORDER_CLEAR) {
            CHECK_PACKET(packet_size >= (uint32_t)(1 + 2));

            BoidIndex boids_count = ntohs(POP_DATA(d, uint16_t));
            CHECK_PACKET(boids_count <= max_boids);
            CHECK_PACKET(packet_size == (uint32_t)(1 + 2 + boids_count*2));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(POP_DATA(d, uint16_t));
                if (idx >= room->total_boids_number)
                    continue;

                ServerBoid *boid = &room->boids[idx];
                if (boid->b.team != p->team)
                    continue;

                boid->o.direction_order = false;
                boid->o.point_order = false;                
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_ACTION) {
            CHECK_PACKET(packet_size >= (uint32_t)(1 + 1 + 2));
            
            BoidAction action = POP_DATA(d, int8_t);
            CHECK_PACKET(action < ACT_COUNT);

            BoidIndex boids_count = ntohs(POP_DATA(d, uint16_t));
            CHECK_PACKET(boids_count <= max_boids);
            CHECK_PACKET(packet_size == (uint32_t)(1 + 1 + 2 + boids_count*2));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(POP_DATA(d, uint16_t));
                if (idx >= room->total_boids_number)
                    continue;

                ServerBoid *boid = &room->boids[idx];
                if (boid->b.team != p->team)
                    continue;
                
                if ((boid->b.action == ACT_STOP) && (action != ACT_STOP)) // Randomize boid's speed, if it stops
                    boid->b.velocity = (Vector2){random_value(-10, 10)/10.0, random_value(-10, 10)/10.0};

                boid->b.action = action;
                boid->o.order_timer = random_value(20, 30)*tps; // 20-30 seconds
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_DIRECTION) {
            CHECK_PACKET(packet_size >= (uint32_t)(1 + 4 + 4 + 2));
            
            Vector2 direction;
            direction.x = (int32_t)ntohl(POP_DATA(d, int32_t)) / 65535.0;
            direction.y = (int32_t)ntohl(POP_DATA(d, int32_t)) / 65535.0;

            BoidIndex boids_count = ntohs(POP_DATA(d, uint16_t));
            CHECK_PACKET(boids_count <= max_boids);
            CHECK_PACKET(packet_size == (uint32_t)(1 + 4 + 4 + 2 + boids_count*2));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(POP_DATA(d, uint16_t));
                if (idx >= room->total_boids_number)
                    continue;
                
                ServerBoid *boid = &room->boids[idx];
                if (boid->b.team != p->team)
                    continue;
                
                boid->o.order_vector = direction;
                boid->o.direction_order = true;
                boid->o.point_order = false;
                boid->o.order_timer = random_value(30, 45)*tps; // 30-45 seconds
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_POINT) {
            CHECK_PACKET(packet_size >= (uint32_t)(1 + 2 + 2 + 2));
            
            Vector2 point;
            point.x = ntohs(POP_DATA(d, uint16_t));
            point.y = ntohs(POP_DATA(d, uint16_t));

            BoidIndex boids_count = ntohs(POP_DATA(d, uint16_t));
            CHECK_PACKET(boids_count <= max_boids);
            CHECK_PACKET(packet_size == (uint32_t)(1 + 2 + 2 + 2 + boids_count*2));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(POP_DATA(d, uint16_t));
                if (idx >= room->total_boids_number)
                    continue;

                ServerBoid *boid = &room->boids[idx];
                if (boid->b.team != p->team)
                    continue;
                
                boid->o.order_vector = point;
                boid->o.direction_order = false;
                boid->o.point_order = true;
                boid->o.order_timer = random_value(30, 45)*tps; // 30-45 seconds
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_LINE) {
            CHECK_PACKET(packet_size >= (uint32_t)(1 + 1 + 2));

            uint8_t line_points_count = POP_DATA(d, uint8_t);
            CHECK_PACKET(packet_size >= (uint32_t)(1 + 1 + line_points_count*(2 + 2) + 2));

            char *points_d = d; // Store_pointer
            d += (2 + 2)*line_points_count; // Skip array of points

            BoidIndex boids_count = ntohs(POP_DATA(d, uint16_t));
            CHECK_PACKET(boids_count <= max_boids);
            CHECK_PACKET(packet_size == (uint32_t)(1 + 1 + line_points_count*(2 + 2) + 2 + boids_count*2));

            BaseBoid **b = malloc(boids_count * sizeof(*b)); // Array of selected boids
            BoidIndex bc = 0;

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(POP_DATA(d, uint16_t));
                if (idx >= room->total_boids_number)
                    continue;

                ServerBoid *boid = &room->boids[idx];
                if (boid->b.team != p->team)
                    continue;

                boid->b.kdtree_is_used = false;
                b[bc++] = &boid->b;
            }

            KDNode *tree = CREATE_KDTREE(b, bc, 16);

            Vector2 *line_points = calloc(line_points_count, sizeof(*line_points));
            float line_len = 0;

            // First point
            line_points[0].x = ntohs(POP_DATA(points_d, uint16_t));
            line_points[0].y = ntohs(POP_DATA(points_d, uint16_t));
            
            // Convert all points to Vector2 and calc line_len
            for (int i = 1; i < line_points_count; i++) {
                line_points[i].x = ntohs(POP_DATA(points_d, uint16_t));
                line_points[i].y = ntohs(POP_DATA(points_d, uint16_t));
                line_len += Vector2Distance(line_points[i-1], line_points[i]);
            }

            float interval = line_len / (bc - 1);
            float remains = 0;
            int point_idx = 0;
            Rectangle rec = {0, 0, room->world.x, room->world.y};
            
            // Cycle for each segment
            for (int segment_idx = 1; segment_idx < line_points_count; segment_idx++) {
                Vector2 segment_start = line_points[segment_idx-1];
                Vector2 segment_end = line_points[segment_idx];
                Vector2 segment_dir = Vector2Normalize(Vector2Subtract(segment_end, segment_start));
                segment_start = Vector2Add(segment_start, Vector2Scale(segment_dir, remains)); // Shift of the beginning of segment to the
                                                                                               // remainder of previous segment

                float segment_len = Vector2Distance(segment_end, segment_start);
                int segment_points_count = floorf(segment_len / interval) + (int)((segment_len > interval) || (segment_idx == 0));

                Vector2 point = segment_start;
                float path_len = 0;
                // Placing boids on segment
                for (BoidIndex segment_point_idx = 0; segment_point_idx < segment_points_count; segment_point_idx++, point_idx++) {
                    BaseBoid *nearest_base_boid = find_nearest_in_kdtree_approx(tree, point, rec);
                    if (nearest_base_boid == NULL) break;
                    ServerBoid *nearest_boid = &room->boids[nearest_base_boid->boid_idx];
                    
                    nearest_boid->o.order_vector = point;
                    nearest_boid->o.order_timer = Vector2Distance(nearest_boid->b.pos, point) / BOID_MIN_SPEED / (60.0/tps);
                    nearest_boid->o.point_order = true;
                    nearest_boid->o.direction_order = false;
                    nearest_boid->b.kdtree_is_used = true;
                    
                    path_len += interval;
                    point = Vector2Add(point, Vector2Scale(segment_dir, interval));
                }
                remains = path_len - segment_len;
            }

            if (bc - point_idx > 0) {
                for (BoidIndex i = 0; i < bc; i++) {
                    BaseBoid *base_boid = b[i];
                    if (!base_boid->kdtree_is_used) {
                        ServerBoid *boid = &room->boids[base_boid->boid_idx];
                        Vector2 point = line_points[line_points_count-1];
                        
                        boid->o.order_vector = point;
                        boid->o.order_timer = Vector2Distance(boid->b.pos, point) / BOID_MIN_SPEED / (60.0/tps);
                        boid->o.point_order = true;
                        boid->o.direction_order = false;
                        boid->b.kdtree_is_used = true;
                    }
                }
            }

            clear_kdtree(tree);
            free(line_points);
            free(b);
            
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else {
            INVALID_PACKET();
        }
        
        break;
        }
    case CP_CLOSE_ROOM: {
        Room *room = p->room;
        CHECK_PACKET(room != NULL);

        if (room->players[0] != p)
            break;

        close_room(room, DISCONNECT_ADMIN_CLOSED_ROOM);
        break;
        }
    case CP_KICK_PLAYER: {
        /* CP_KICK_PLAYER PACKET FORMAT
        (uint32 player_id)
        */

        CHECK_PACKET(packet_size == 4);
        Room *room = p->room;
        if (room->players[0] != p)
            break;

        uint32_t kicked_player_id = ntohl(*(uint32_t*)packet_data);
        int kicked_player_idx = get_player_idx(room->players, 0, kicked_player_id);

        if (kicked_player_idx != -1) {
            uint32_t nid = *(uint32_t*)packet_data; // BE

            pthread_mutex_lock(&room->players_mtx);
            for (int i = 0; i < room->joined_players; i++) {
                send_packet(room->players[i]->tcp_fd, SP_PLAYER_KICKED, &nid, sizeof(nid), 0);
            }
            pthread_mutex_unlock(&room->players_mtx);

            close_client(room->players[kicked_player_idx]->tcp_fd, DISCONNECT_KICKED, true);
        }

        break;
        }
    case CP_CHANGE_TEAM: {
        /* CP_CHANGE_TEAM PACKET FORMAT
        (uint32 player_id) (int8 new_team)
        */

        CHECK_PACKET(packet_size == 4+1);

        Room *room = p->room;
        CHECK_PACKET(room != NULL);

        if (room->players[0] != p)
            break;

        uint32_t player_id = ntohl(POP_DATA(d, uint32_t));
        CHECK_PACKET(player_id > 0);

        int8_t new_team = POP_DATA(d, int8_t);
        CHECK_PACKET(new_team < TEAMS_COUNT);

        bool can_use_team = true;
        for (int i = 0; i < room->joined_players; i++) {
            if (room->players[i]->team == new_team) {
                can_use_team = false;
                break;
            }
        }
        CHECK_PACKET(can_use_team);

        CHECK_PACKET(room->teams[new_team] != 0);
        
        pthread_mutex_lock(&room->players_mtx);
        int player_idx = get_player_idx(room->players, 0, player_id);

        if (player_idx != -1) {
            room->players[player_idx]->team = new_team;
            for (int i = 0; i < room->joined_players; i++) {
                send_packet(room->players[i]->tcp_fd, SP_CHANGE_TEAM, packet_data, packet_size, 0);
            }
        }
        pthread_mutex_unlock(&room->players_mtx);
        
        break;
        }
    case CP_SWAP_TEAMS: {
        /* CP_SWAP_TEAMS PACKET FORMAT
        (uint32 player1_id) (uint32 player2_id)
        */

        CHECK_PACKET(packet_size == 4 + 4);

        Room *room = p->room;
        CHECK_PACKET(room != NULL);

        if (room->players[0] != p)
            break;

        uint32_t player1_id = ntohl(POP_DATA(d, uint32_t));
        CHECK_PACKET(player1_id > 0);
        uint32_t player2_id = ntohl(POP_DATA(d, uint32_t));
        CHECK_PACKET(player2_id > 0);
        
        pthread_mutex_lock(&room->players_mtx);
        int player1_idx = get_player_idx(room->players, 0, player1_id);
        int player2_idx = get_player_idx(room->players, 0, player2_id);
        
        if (player1_idx != -1 && player2_idx != -1) {
            int team_tmp = room->players[player1_idx]->team;
            room->players[player1_idx]->team = room->players[player2_idx]->team;;
            room->players[player2_idx]->team = team_tmp;

            for (int i = 0; i < room->joined_players; i++) {
                send_packet(room->players[i]->tcp_fd, SP_SWAP_TEAMS, packet_data, packet_size, 0);
            }
        }
        pthread_mutex_unlock(&room->players_mtx);

        break;
        }
    case CP_CHAT_MSG: {
        /* CP_CHAT_MSG PACKET FORMAT
        (uint16 msg_len) (uint8[msg_len] msg)
        */

        CHECK_PACKET(packet_size >= 2);

        Room *room = p->room;
        CHECK_PACKET(room != NULL);

        uint16_t msg_len = ntohs(*(uint16_t*)packet_data);
        CHECK_PACKET(packet_size == (uint32_t)(2 + 1*msg_len));

        /* SP_CHAT_MSG PACKET FORMAT
        (uint32 sender_id) (uint16 msg_len) (uint8[msg_len] msg)
        */
        
        uint32_t size = 4 + 2 + 1*msg_len;;
        char *buf = malloc(size);

        *(uint32_t*)buf = htonl(p->id);
        memcpy(buf+sizeof(uint32_t), packet_data, packet_size);
        buf[size-1] = '\0'; // \0 at the end of message string
        
        pthread_mutex_lock(&room->players_mtx);
        for (int i = 0; i < room->joined_players; i++) {
            send_packet(room->players[i]->tcp_fd, SP_CHAT_MSG, buf, size, 0);
        }
        pthread_mutex_unlock(&room->players_mtx);
        
        free(buf);
        break;
        }
    default:
        critical_packet = true;
        INVALID_PACKET();
    }
    
    switch_exit:
    if (invalid_packet) {
        if (critical_packet) {
            send_packet(p->tcp_fd, SP_INVALID_PACKET, NULL, 0, 0);
            close_client(p->tcp_fd, DISCONNECT_PACKET_VIOLATIONS, true);
        } else {
            // Kick the client if he fas sent MAX_VIOLATINS_PER_SEC invalid packets in 1 second
            
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            int offset = 0;
            for (int i = 0; i < p->net.violations_count; i++) {
                p->net.violation_timestamps[i - offset] = p->net.violation_timestamps[i];
                
                struct timespec t = p->net.violation_timestamps[i];
                
                int diff_sec = 0;
                if (now.tv_nsec < t.tv_nsec)
                    diff_sec = now.tv_sec - t.tv_sec - 1;
                else
                    diff_sec = now.tv_sec - t.tv_sec;

                // Remove the timestamp if it was created more than 1 second ago
                if (diff_sec >= 1)
                    offset++;
            }

            p->net.violations_count -= offset;

            send_packet(p->tcp_fd, SP_INVALID_PACKET, NULL, 0, 0);

            #ifdef DEBUG
                write_log(L_DEBUG, "id=%d %d violation%s\n", p->id, p->net.violations_count, (p->net.violations_count == 1) ? "" : "s");
            #endif
            
            if (p->net.violations_count >= MAX_VIOLATINS_PER_SEC)
                close_client(p->tcp_fd, DISCONNECT_PACKET_VIOLATIONS, true);
        }
    }
}

#undef INVALID_PACKET
#undef CHECK_PACKET

typedef enum {
    RECV_OK,
    RECV_ERROR,
    RECV_INVALID_PACKET,
    RECV_CLIENT_CLOSE
} PacketRecvCode;

PacketRecvCode client_recv(Player *p) {
    while (1) {
        // Receive data
        int n = recv(p->tcp_fd, p->net.recv_buf, sizeof(p->net.recv_buf), 0);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                return RECV_ERROR;
            
        } else if (n == 0) {
            return RECV_CLIENT_CLOSE;
        }

        // Parse data
        uint8_t *ptr = p->net.recv_buf;
        uint32_t remaining = n;
        while (remaining > 0) {
            if (p->net.state == PARSE_TYPE) {
                if (remaining >= 1) {
                    p->net.type = *(ptr++);
                    remaining--;
                    p->net.bytes_remaining = sizeof(p->net.data_len);
                    p->net.state = PARSE_LEN;
                } else break;
            } else if (p->net.state == PARSE_LEN) {
                uint32_t need = p->net.bytes_remaining;
                uint32_t copy = MIN(remaining, need);

                if (copy > 0) {
                    memcpy((uint8_t*)&p->net.data_len + (sizeof(p->net.data_len) - p->net.bytes_remaining), ptr, copy);
                    p->net.bytes_remaining -= copy;
                    remaining -= copy;
                    ptr += copy;
                }
                if (p->net.bytes_remaining == 0) {
                    p->net.data_len = ntohl(p->net.data_len);
                    if (p->net.data_len > MAX_PACKET_SIZE)
                        return RECV_INVALID_PACKET;

                    if (p->net.data_len > 0) {
                        p->net.data_buf = malloc(p->net.data_len);
                        if (p->net.data_buf == NULL)
                            return RECV_ERROR;
                        p->net.state = PARSE_DATA;
                        p->net.bytes_remaining = p->net.data_len;
                    } else { // Empty packet body
                        int fd = p->tcp_fd;
                        process_data(p);

                        // Check that player not disconnected
                        if (players[fd] == NULL) {
                            return RECV_OK;
                        } else {
                            p->net.data_buf = NULL;
                            p->net.state = PARSE_TYPE;
                        }
                    }
                }
            } else if (p->net.state == PARSE_DATA) {
                uint32_t need = p->net.bytes_remaining;
                uint32_t copy = (remaining < need)? remaining : need;

                if (copy > 0) {
                    memcpy(p->net.data_buf + (p->net.data_len - p->net.bytes_remaining), ptr, copy);
                    p->net.bytes_remaining -= copy;
                    remaining -= copy;
                    ptr += copy;
                }
                if (p->net.bytes_remaining == 0) {
                    int fd = p->tcp_fd;
                    process_data(p);

                    // Check that player not disconnected
                    if (players[fd] == NULL) {
                        return RECV_OK;
                    } else {
                        free(p->net.data_buf);
                        p->net.data_buf = NULL;
                        p->net.state = PARSE_TYPE;
                    }
                }
            }
        }
    }

    return RECV_OK;
}

void quit(int sig) {
    write_log(L_WARNING, "shutting down server\n");

    for (long i = 0; i < max_fd; i++) {
        if (players[i] != NULL) {
            // players[i]->joined = false;
            close_client(i, DISCONNECT_SERVER_DOWN, false);
        };
    }
    free(players);

    close(epfd);
    close(tcp_fd);
    if (udp_opened)
        close(udp_fd);

    exit(sig);
}

int main(int argc, char **argv) {
    unsigned short tcp_port = TCP_PORT, udp_port = UDP_PORT;
    bool show_help = false;
    char *prog = argv[0];

    while (--argc) {
        char *arg = *(++argv);
        bool ext = false; // exit

        if (arg[0] == '-') {
            int old_argc = argc;
            do {
                if (strcmp(arg, "--help") == 0 || strncmp(arg, "-h", 2) == 0) {
                    show_help = true;
                    ext = true;
                    break;
                } else if (strcmp(arg, "--tcp-port") == 0 || strcmp(arg, "-T") == 0) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);
                
                    char *value_str = *(++argv);
                    argc--;

                    char *endp;
                    tcp_port = strtoul(value_str, &endp, 10);
                    if (*endp != '\0') {
                        ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                    }
                } else if (strcmp(arg, "--udp-port") == 0 || strcmp(arg, "-U") == 0) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);
                
                    char *value_str = *(++argv);
                    argc--;

                    char *endp;
                    udp_port = strtoul(value_str, &endp, 10);
                    if (*endp != '\0') {
                        ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                    }
                } else if (strcmp(arg, "--chunk") == 0 || strcmp(arg, "-c") == 0) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    char *value_str = *(++argv);
                    argc--;

                    char *endp;
                    chunk_size = strtoul(value_str, &endp, 10);
                    if (*endp != '\0') {
                        ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                    }

                    if (chunk_size < BOID_SIZE) {
                        ERRF("size of chunk must be greater than or equal to %d\n", BOID_SIZE);
                    } else if (chunk_size > CHUNK_SIZE_PIXELS) {
                        ERRF("size of chunk must be less than or equal to %d\n", CHUNK_SIZE_PIXELS);
                    }
                    chunk_size = (chunk_size / BOID_SIZE) * BOID_SIZE;
                } else if (strcmp(arg, "--tps") == 0 || strcmp(arg, "-t") == 0) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    char *value_str = *(++argv);
                    argc--;

                    char *endp;
                    tps = strtoul(value_str, &endp, 10);
                    if (*endp != '\0') {
                        ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                    }

                    if (chunk_size < BOID_SIZE) {
                        ERR("tps must be greater than or equal to 1\n");
                    }
                } else {
                    ERRF("unexpected argument '%s'\n", arg);
                }

                if (argc == old_argc && arg[1] != '-' && arg[2] != '\0') {
                    // Move the arg pointer to flags like -abcd, which will be interpreted as -a -b -c -d
                    arg++;
                    *arg = '-';
                } else {
                    break;
                }
            } while (1);

            if (ext)
                break;
        } else {
            ERRF("unexpected argument '%s'\n", arg);
        }
    }

    if (show_help) {
        printf(
            "Usage: %s [OPTIONS]\n"
            "\n"
            "A game's server program.\n"
            "\n"
            "Options:\n"
            "  -h, --help\n"
            "    Show this message and exit\n"
            "  -T, --tcp-port\n"
            "    TCP port of the game server (default: %d)\n"
            "  -U, --udp-port\n"
            "    UDP port of the game server (default: %d)\n"
            "  -c, --chunk <NUM>\n"
            "    Size of chunk in pixels, rounded down to the nearest multiple of %d\n"
            "    (default: %d)\n"
            "  -t, --tps <NUM>\n"
            "    Simulation's target tps (default: %d)\n",
            prog, TCP_PORT, UDP_PORT, BOID_SIZE, DEFAULT_SERVER_CHUNK_SIZE_PIXELS, DEFAULT_TPS);

        exit(0);
    }

    #ifdef DEBUG
        set_log_config(NULL, /*print_time=*/ true, /*stdout*/ L_DEBUG, /*file*/ L_DEBUG);
    #else
        set_log_config(NULL, /*print_time=*/ true, /*stdout*/ L_INFO, /*file*/ L_DEBUG);
    #endif

    printf("chunk: %d pixels\n", chunk_size);
    printf("target tps: %d\n", tps);
    
    max_fd = sysconf(_SC_OPEN_MAX);
    players = calloc(max_fd, sizeof(Player*));
    
    // Create a TCP socket
    bool tcp_opened = false;
    tcp_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (tcp_fd < 0) {
        perror("socket");
        goto tcp_fail;
    }

    int opt = 1;
    if (setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        goto tcp_fail;
    }

    opt = 1;
    if (setsockopt(tcp_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
        perror("setsockopt TCP_NODELAY");
        goto tcp_fail;
    }
    
    struct sockaddr_in tcp_servaddr = { 0 };
    tcp_servaddr.sin_addr.s_addr = INADDR_ANY;
    tcp_servaddr.sin_port = htons(tcp_port);
    tcp_servaddr.sin_family = AF_INET;

    // Forcefully attaching socket to the port
    if (bind(tcp_fd, (struct sockaddr*)&tcp_servaddr, sizeof(tcp_servaddr)) < 0) {
        perror("bind");
        goto tcp_fail;
    }

    if (listen(tcp_fd, SOMAXCONN) < 0) {
        perror("listen");
        goto tcp_fail;
    }

    write_log(L_INFO, "opened a TCP socket on port %d\n", tcp_port);
    tcp_opened = true;

    tcp_fail: {
        if (!tcp_opened) {
            write_log(L_WARNING, "failed to open a TCP socket on port %d\n", tcp_port);
            close(tcp_fd);
            return 1;
        }
    }

    // Create a UDP socket
    udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (udp_fd < 0) {
        perror("socket");
        goto udp_fail;
    }

    opt = 1;
    if (setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        close(udp_fd);
        goto udp_fail;
    }

    struct sockaddr_in udp_servaddr = { 0 };
    udp_servaddr.sin_addr.s_addr = INADDR_ANY;
    udp_servaddr.sin_port = htons(udp_port);
    udp_servaddr.sin_family = AF_INET;

    // Forcefully attaching socket to the port
    if (bind(udp_fd, (struct sockaddr*)&udp_servaddr, sizeof(udp_servaddr)) < 0) {
        perror("bind");
        close(udp_fd);
        goto udp_fail;
    }

    write_log(L_INFO, "opened a UDP socket on port %d\n", udp_port);
    udp_opened = true;

    udp_fail: {
        if (!udp_opened)
            write_log(L_WARNING, "failed to open a UDP socket on port %d\n", udp_port);
    }

    // Create epoll instance
    struct epoll_event event, events[MAX_EVENTS];
    epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(tcp_fd);
        return 1;
    }

    event.events = EPOLLIN;
    event.data.fd = tcp_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tcp_fd, &event)) {
        perror("epoll_ctl");
        close(tcp_fd);
        close(epfd);
        return 1;
    }

    if (udp_opened) {
        event.events = EPOLLIN;
        event.data.fd = udp_fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, udp_fd, &event)) {
            write_log(L_ERROR, "failed to add a UDP socket to epoll");
            perror("epoll_ctl");
            close(udp_fd);
        }
    }

    if (fcntl(STDIN_FILENO, F_GETFD) != -1) {
        // Make stdin nonblocking
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
        // Add stdin to epoll
        event.events = EPOLLIN;
        event.data.fd = STDIN_FILENO;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &event))
            write_log(L_WARNING, "stdin is invalid\n");
    } else {
        write_log(L_WARNING, "stdin is closed\n");
    }
    
    signal(SIGINT, quit);
    
    bool running = true;
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == tcp_fd) {
                // Accept all pending connections
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(tcp_fd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                    if (client_fd >= max_fd) {
                        close(client_fd);
                        continue;
                    }

                    Player *player = malloc(sizeof(Player));
                    *player = (Player){.id = (++last_player_id), .tcp_fd = client_fd, .tcp_addr = client_addr,
                                       .joined = false, .ready = false, .approving_queue = { 0 }, .in_queue = NULL};
                    players[client_fd] = player;

                    char ip[INET_ADDRSTRLEN];
                    if (!inet_ntop(tcp_servaddr.sin_family, &client_addr.sin_addr, ip, sizeof(ip)))
                        strcpy("?", ip);
                    write_log(L_JOIN, "%s:%d (fd=%d id=%d)\n", ip, ntohs(client_addr.sin_port), client_fd, last_player_id);

                    event.events = EPOLLIN | EPOLLRDHUP;
                    event.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &event)) {
                        perror("epoll_ctl");
                        free(players[client_fd]);
                        close(client_fd);
                    }
                }
            } else if (fd == udp_fd && udp_opened) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                char buffer[1024];
                ssize_t n = recvfrom(udp_fd, &buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &client_len);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    perror("accept");
                    break;
                }

                uint8_t packet_type = *(uint8_t*)buffer;
                uint32_t packet_size = ntohl(*(uint32_t*)(buffer + 1));
                if ((uint32_t)n != (1 + sizeof(packet_size) + packet_size))
                    continue;
                char *data = buffer + 1 + sizeof(packet_size);

                if (packet_type == CP_UDP_HELLO) {
                    /* CP_UDP_HELLO PACKET FORMAT
                    (uint32 player_id) (int32_t player_tcp_fd)
                    */

                    if (packet_size != sizeof(uint32_t)*2)
                        continue;

                    char *d = data;

                    uint32_t player_id = ntohl(POP_DATA(d, uint32_t));
                    int32_t player_tcp_fd = ntohl(POP_DATA(d, int32_t));

                    if (player_tcp_fd >= max_fd)
                        continue;

                    Player *p = players[player_tcp_fd];

                    if (p != NULL && p->id == player_id && !p->udp_enabled) {
                        p->udp_addr = client_addr;
                        p->udp_enabled = true;
                        write_log(L_INFO, "id=%d enabled UDP sync\n", p->id);
                    }
                }
            } else if (events[i].data.fd == STDIN_FILENO) {
                // Process standard input
                char buf[256];
                ssize_t r = read(STDIN_FILENO, buf, sizeof(buf) - 1);
                if (r > 0) {
                    buf[r] = '\0';
                    if (strcmp(buf, "quit\n") == 0 || strcmp(buf, "q\n") == 0) {
                        write_log(L_INPUT, "%s", buf);
                        running = false;
                        break;
                    }
                } else {
                    running = false;
                    break;
                }
            } else if (events[i].events & EPOLLERR) {
                // Disconnect client
                close_client(fd, DISCONNECT_SERVER_ERROR, true);
            } else {
                Player *player = players[fd];
                int fd = player->tcp_fd;

                PacketRecvCode r = client_recv(player);
                if (players[fd] != NULL) { // Check that player not disconnected
                    if (r == RECV_ERROR) {
                        close_client(fd, DISCONNECT_SERVER_ERROR, true);
                    } else if (r == RECV_INVALID_PACKET) {
                        send_packet(player->tcp_fd, SP_INVALID_PACKET, NULL, 0, MSG_NOSIGNAL);
                        close_client(fd, DISCONNECT_PACKET_VIOLATIONS, true);
                    } else if (r == RECV_CLIENT_CLOSE) {
                        close_client(fd, DISCONNECT_PLAYER_EXITED, true);
                    }
                }
            }
        }
    }

    quit(0);
}
