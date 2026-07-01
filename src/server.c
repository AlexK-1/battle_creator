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

#include <raylib.h>

#include "boids.h"
#include "network.h"
#include "queue.h"
#include "kdtree.h"

#define MAX_ROOMS 256
#define MAX_EVENTS 1024
#define MAX_APPROVING_QUEUE_LEN 10
#define MAX_PACKET_SIZE (1024*64)
#define TPS 15

#define ERR(str)                                                              \
    do {                                                                      \
        fprintf(stderr, "%s: " str, prog);                                    \
        exit(EXIT_FAILURE);                                                   \
    } while (0)

#define ERRF(format, ...)                                                     \
    do {                                                                      \
        fprintf(stderr, "%s: " format, prog, __VA_ARGS__);                    \
        exit(EXIT_FAILURE);                                                   \
    } while (0)


/*
join -> room
connect -> server
*/

typedef struct Player {
    int fd;
    uint32_t id;
    char name[USERNAME_LEN];
    uint8_t team;
    bool joined, ready;
    ClientStartNetBoids start_boids[MAX_BOIDS_COUNT*2];
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
    } net;
} Player;

typedef struct Room {
    Player *players[TEAMS_COUNT];
    BoidIndex teams[TEAMS_COUNT], total_boids_number;
    uint8_t players_number, joined_players;
    uint32_t id;
    Point world;
    pthread_t thread;
    bool thread_run, sync_boids;
    ServerBoid *boids;
    pthread_mutex_t boids_mtx;
    enum {
        ROOM_AREAS,
        ROOM_PLACING,
        ROOM_GAME
    } status;
} Room;

/* V global variables V */
Player **players = NULL;
uint32_t last_player_id = 0; // 0 must be an invalid player id
Room *rooms[MAX_ROOMS] = { 0 };
int32_t last_room_idx = -1;
int chunk_size = CHUNK_SIZE_PIXELS;
/* ^ global variables ^ */

int get_player_idx(Player **players, int id) {
    for (int i = 0; i < TEAMS_COUNT; i++) {
        if (players[i]->fd == id)
            return i;
    }
    return -1;
}

Player *find_player(Player **players, int id) {
    int i = get_player_idx(players, id);
    if (i < 0)
        return NULL;
    return players[i];
}

static inline int random_value(int min, int max) {
    return rand() % (max-min) + min;
}

void close_client(int, int);

void close_room(int epfd, Room *room) {
    if (room->status == ROOM_GAME && room->thread_run) {
        room->thread_run = false;
        pthread_join(room->thread, NULL);
    }
    
    printf("[*] room %06x closed\n", room->id);

    while (room->players[0]->approving_queue.size > 0) {
        Player *op;
        dequeue(room->players[0]->approving_queue, op);
        op->joined = false;
        close_client(epfd, op->fd);
    }

    for (int i = 0; i < room->joined_players; i++) {
        Player *op = room->players[i]; // other_player
        op->joined = false;
        send_packet(op->fd, SP_ROOM_CLOSED, NULL, 0, 0);
        close_client(epfd, op->fd);
    }

    if (room->boids != NULL) free(room->boids);
    free(room);
    rooms[last_room_idx] = NULL;
}

void close_client(int epfd, int fd) {
    Player *p = players[fd];

    // room
    bool room_closed = false;
    if (p->joined && p->room != NULL) {
        Room *room = p->room;

        int player_idx = get_player_idx(room->players, p->fd);

        last_room_idx = ((room->id & 0xffff0000) >> 16);
        if ((room->status == ROOM_AREAS && player_idx == 0) || // room's creator disconnected
            (room->status == ROOM_PLACING) ||
            (room->status == ROOM_GAME && room->joined_players == 1)) {
            close_room(epfd, room); // close entire room
            room_closed = true;
        } else {
            // delete player from array
            memmove(room->players + player_idx, room->players + player_idx + 1,
                    sizeof(room->players[0]) * (room->joined_players - player_idx - 1));
            room->joined_players--;

            // send a message to all players in the room that the player has disconnected
            uint32_t nfd = htonl(p->id);
            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i];
                send_packet(op->fd, SP_PLAYER_EXIT, &nfd, sizeof(op->id), 0);
            }
        }

        last_room_idx--;
    }

    if (!room_closed) {
        // close files
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);

        if (p->approving_queue.max_len != 0) free(p->approving_queue.items);
        if (!p->joined && p->in_queue != NULL && p->in_queue->size > 0) {
            int find_idx = -1;
            for (int i = 0; i < p->in_queue->size; i++) {
                int idx = (p->in_queue->front + i) % p->in_queue->max_len;
                if (p->in_queue->items[idx]->fd == p->fd) {
                    find_idx = idx;
                    break;
                }
            }

            if (find_idx != -1) {
                Player **q = p->in_queue->items;
                if (find_idx < p->in_queue->max_len-1)
                    memmove(q + p->in_queue->front + 1, q + find_idx + 1, sizeof(*q) * (p->in_queue->max_len-1 - find_idx));
                if (p->in_queue->rear < p->in_queue->front) {
                    q[p->in_queue->max_len-1] = q[0];
                    memmove(q, q + 1, sizeof(*q) * (p->in_queue->rear));
                }
            }
            p->in_queue->size--;
            p->in_queue->rear--;

            if (find_idx == 0) {
                uint32_t nid = htonl(p->id);
                send_packet(p->approving_player->fd, SP_PLAYER_EXIT, &nid, sizeof(nid), 0);
            }
        }

        printf("[-] fd=%d id=%d hung up\n", fd, p->id);
        
        free(p);
        players[fd] = NULL;
    }
}

typedef struct {
    Room *room;
    int epfd;
} RoomThreadArgs;

void *room_thread_fn(void *args) {
    RoomThreadArgs *nargs = args;
    
    Room *room = nargs->room;
    int epfd = nargs->epfd;

    free(args);
    
    ServerBoid *boids = room->boids = calloc(room->total_boids_number, sizeof(*room->boids));
    if (boids == NULL) {
        room->thread_run = false;
        close_room(epfd, room);
        return NULL;
    }
    BoidIndex boids_count = 0;
    
    for (int player_idx = 0; player_idx < room->joined_players; player_idx++) {
        Player *player = room->players[player_idx];
        
        int cell = 0, cell_x = 0, cell_y = 0;
        for (int i = 0; i < player->start_boids_len; i++) {
            ClientStartNetBoids b = player->start_boids[i];
            if (b.team < 0) {
                cell += b.count;
                cell_x = cell % (room->world.x / BOID_SIZE);
                cell_y = cell / (room->world.y / BOID_SIZE);
            } else {
                for (int i = 0; i < b.count; i++) {
                    if (boids_count >= room->total_boids_number)
                        break;
                    
                    ServerBoid new_boid = {.b = {.pos = {cell_x*BOID_SIZE + (int)(BOID_SIZE/2.0), cell_y*BOID_SIZE + (int)(BOID_SIZE/2.0)}, .velocity = { 0 },
                                                 .speed = random_value(80, 130)/100.0, .health = BOID_MAX_HEALTH, .xp = random_value(0, 5),
                                                 .team = player->team, .action = ACT_STOP}};
                    room->boids[boids_count++] = new_boid;
                
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

    if (boids_count > room->total_boids_number) {
        close_room(epfd, room);
        return NULL;
    }

    uint32_t packet_size = sizeof(boids_count) + boids_count*sizeof(ServerStartNetBoid);
    char *data = malloc(packet_size);

    BoidIndex net_boids_count = htons(boids_count);
    memcpy(data, &net_boids_count, sizeof(boids_count));

    ServerStartNetBoid *boids_data = (ServerStartNetBoid*)(data + sizeof(boids_count));

    for (int i = 0; i < boids_count; i++) {
        ServerBoid *orig_boid = &room->boids[i];
        ServerStartNetBoid boid = {.x = htons(orig_boid->b.pos.x), .y = htons(orig_boid->b.pos.y),
                                   .speed = orig_boid->b.speed*100.0, .xp = orig_boid->b.xp, .team = orig_boid->b.team};
        boids_data[i] = boid;
    }

    for (int i = 0; i < room->joined_players; i++) {
        send_packet(room->players[i]->fd, SP_START_GAME, data, packet_size, 0);
    }

    free(data);

    // Grid of chunks
    Grid grid = { 0 };
    init_grid(&grid, (BaseBoid*)boids, boids_count, room->world.x, room->world.y, chunk_size);

    double target_delay = 1.0/(double)TPS;
    int timer = 0;

    while (room->thread_run && boids_count > 0) {
        clock_t prev_time = clock();

        // Update boids
        for (BoidIndex i = 0; i < boids_count; i++) {
            ServerBoid *boid = &boids[i];

            if (boid->b.action == ACT_DELETE) continue;

            if (boid->order_timer > 0) {
                boid->order_timer--;

                if (boid->point_order && Vector2Distance(boid->order_vector, boid->b.pos) < BOID_SIZE) {
                    boid->order_timer = 0;
                    boid->b.action = ACT_STOP;
                }

                // Change direction by order
                Vector2 direction = { 0 };
                if (boid->direction_order)
                    direction = boid->order_vector;
                else if (boid->point_order)
                    direction = Vector2Normalize(Vector2Subtract(boid->order_vector, boid->b.pos));
                boid->b.velocity = Vector2Add(boid->b.velocity, Vector2Scale(direction, BOID_ORDER_FACTOR));
            }
            update_base_boid(boids, &grid, i, sizeof(*boids), /*can_change_action*/ boid->order_timer == 0, /*can_fall*/ true);

            if (boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL) {
                boid_normal_speed((BaseBoid*)boid);
                boid_bound((BaseBoid*)boid, room->world.x, room->world.y);
            }

            boid->b.pos = Vector2Add(boid->b.pos, Vector2Scale(boid->b.velocity, boid->b.speed * (60.0f/TPS)));
        }

        pthread_mutex_lock(&room->boids_mtx);
        /*if (room->sync_boids) {
            // timer = tps*2;
            // room->sync_boids = false;
            
            printf("ordr %ld\n", time(NULL) - prevt);
            prevt = time(NULL);
        }*/
        if (timer == 0 || room->sync_boids) {
            // Send boids data to clients (boids sync)

            /* PACKET FORMAT
            [uint16 boids_count] [uint16 first_boid_index] [NetBoid[boids_count] boids]
            */
            uint32_t packet_size = sizeof(BoidIndex)*2 + boids_count*sizeof(NetBoid);
            char *data = malloc(packet_size);

            *(BoidIndex*)(data) = htons(boids_count);
            *(BoidIndex*)(data+sizeof(BoidIndex)) = htons(0);

            NetBoid *boids_data = (NetBoid*)(data + sizeof(BoidIndex)*2);

            for (int i = 0; i < boids_count; i++) {
                BaseBoid *b = &boids[i].b;
                NetBoid send_boid = {.x = htons(b->pos.x), .y = htons(b->pos.y), .angle = atan2f(b->velocity.y, b->velocity.x)/PI*127,
                                     .vel = Vector2Length(b->velocity)/BOID_MAX_SPEED*255, .health = b->health, .xp = b->xp, .action = b->action};
                boids_data[i] = send_boid;
            }

            for (int i = 0; i < room->joined_players; i++) {
                send_packet(room->players[i]->fd, SP_BOIDS_SYNC, data, packet_size, 0);
            }

            free(data);

            room->sync_boids = false;
            timer = TPS; // 1 second
        }
        timer--;
        
        clear_grid(&grid);
        fill_grid(&grid, boids, boids_count);
        pthread_mutex_unlock(&room->boids_mtx);

        
        clock_t current_time = clock();
        double tick_seconds = ((double)(current_time - prev_time)) / CLOCKS_PER_SEC;

        if (tick_seconds < target_delay) {
            double wait_seconds = target_delay - tick_seconds;
            time_t sec = wait_seconds;
            double nsec = (wait_seconds - sec)*1000000000L;
            struct timespec delay = {.tv_sec = sec, .tv_nsec = nsec};
            while (nanosleep(&delay, &delay) == -1) continue;
        }
    }

    free(grid.chunks);

    pthread_mutex_destroy(&room->boids_mtx);

    room->thread_run = false;
    return NULL;
}

void process_data(Player *p, int epfd) {
    switch (p->net.type) {
    case CP_NEW_ROOM: {
        CPNew data;
        if (p->net.data_len != sizeof(data))
            break;
        memcpy(&data, p->net.data_buf, sizeof(data));

        bool free_rooms =  false;
        for (int j = 1; j < MAX_ROOMS; j++) {
            if (rooms[last_room_idx + j] == NULL) {
                last_room_idx = (last_room_idx + j) % MAX_ROOMS;
                free_rooms = true;
                break;
            }
        }
        
        if (!free_rooms) {
            printf("[!] no free rooms\n");

            SPJoined send_data = {.status = JOIN_FAILED};
            send_packet(p->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
            
            break;
        }
        
        strcpy(p->name, data.creator);
        p->team = data.player_team;
        p->joined = true;
        if (p->approving_queue.max_len == 0)
            init_queue(p->approving_queue, MAX_APPROVING_QUEUE_LEN);

        Room *room = malloc(sizeof(Room));
        room->players_number = data.players_number;
        room->joined_players = 1;
        room->players[0] = p;
        room->world = (Point){ntohs(data.world_size.x), ntohs(data.world_size.y)};
        room->boids = NULL;
        room->thread_run = room->sync_boids = false;
        for (int j = 0; j < TEAMS_COUNT; j++)
            room->teams[j] = ntohs(data.boids_number[j]);
        room->status = ROOM_AREAS;
        rooms[last_room_idx] = room;
        p->room = room;

        BoidIndex total_boids_number = 0;
        for (int team_idx = 0; team_idx < TEAMS_COUNT; team_idx++)
            total_boids_number += room->teams[team_idx];
        room->total_boids_number = total_boids_number;

        /* ROOM ID FORMAT
        [ byte 1 ][ byte 2 ][ byte 3 ][ byte 4 ]
        [aaaaaaaa][aaaaaaaa][cccccccc][cccccctt]
        a - room index in array
        c - boids number
        t - teams (players) number
        byte 1 is hidden
        */

        room->id = (last_room_idx << 16) | (total_boids_number << 2) | data.players_number;
        rooms[last_room_idx] = room;
        printf("[*] new room\n    id: %06x\n    teams: %d\n    world: %dx%d\n    creator: %s (id=%d)\n    boids:  %-4d\n    red:    %-4d\n    blue:   %-4d\n    green:  %-4d\n    yellow: %-4d\n",
               room->id, data.players_number, room->world.x, room->world.y, data.creator, p->id, total_boids_number,
               room->teams[TEAM_RED],
               room->teams[TEAM_BLUE],
               room->teams[TEAM_GREEN],
               room->teams[TEAM_YELLOW]);

        SPJoined send_data = {.room_id = htonl(room->id), .player_id = htonl(p->id), .players_number = room->players_number,
                              .joined_players = room->joined_players, .player_team = p->team, .world_size = data.world_size, .status = JOIN_OK};

        for (int i = 0; i < room->joined_players; i++) {
            Player *op = room->players[i]; // other_player
            send_data.players[i] = (ClientPlayer){.id = htonl(op->id), .team = op->team};
            strcpy(send_data.players[i].name, op->name);
        }

        for (int i = 0; i < TEAMS_COUNT; i++) {
            send_data.teams[i] = htons(room->teams[i]);
        }

        send_packet(p->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
        
        break;
        }
    case CP_JOIN_ROOM: {
        CPJoin data;
        if (p->net.data_len != sizeof(data))
            break;
        memcpy(&data, p->net.data_buf, sizeof(data));
        data.room_id = ntohl(data.room_id);

        uint16_t room_idx = data.room_id & 0xffff0000;
        Room *room = rooms[room_idx];
        if ((room != NULL) && (room->id == data.room_id) && (room->joined_players < room->players_number) && (room->status == ROOM_AREAS)) {
            strcpy(p->name, data.username);

            Player *room_owner = room->players[0];
            if (is_queue_full(room_owner->approving_queue)) {
                SPJoined send_data = {.status = JOIN_FAILED};
                send_packet(p->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
                break;
            }
            
            enqueue(room_owner->approving_queue, p);
            p->approving_player = room_owner;
            p->in_queue = &room_owner->approving_queue;
            
            if (room_owner->approving_queue.size == 1) {
                SPApprove send_data = {.id = htonl(p->id), .username = { 0 }};
                strcpy(send_data.username, data.username);

                send_packet(room_owner->fd, SP_APPROVE_PLAYER, &send_data, sizeof(send_data), 0);
            }
        } else {
            SPJoined send_data = {.status = JOIN_FAILED};
            send_packet(p->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
        }
        
        break;
        }
    case CP_APPROVE_PLAYER: {
        CPApprove data;
        if (p->net.data_len != sizeof(data))
            break;
        memcpy(&data, p->net.data_buf, sizeof(data));
        data.id = ntohl(data.id);

        if (p->approving_queue.size == 0)
            break;
        
        Player *approved_player = queue_front(p->approving_queue);
        if (approved_player->id != data.id)
            break;
        
        dequeue(p->approving_queue, approved_player);

        if (approved_player->joined)
            break;
        
        if (data.team == -1) {
            approved_player->joined = false;
            SPJoined send_data = {.status = JOIN_REJECTED};
            send_packet(p->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
        } else {
            approved_player->joined = true;
            approved_player->ready = false;
            approved_player->team = data.team;
            approved_player->room = p->room;
            approved_player->in_queue = NULL;

            Room *room = p->room;
            room->players[room->joined_players++] = approved_player;

            SPJoined send_data = {.room_id = htonl(room->id), .player_id = htonl(approved_player->id), .players_number = room->players_number,
                                  .joined_players = room->joined_players, .player_team = approved_player->team,
                                  .world_size = {htons(room->world.x), htons(room->world.y)}, .status = JOIN_OK};

            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i]; // other_player
                send_data.players[i] = (ClientPlayer){.id = htonl(op->id), .team = op->team};
                strcpy(send_data.players[i].name, op->name);
            }

            for (int i = 0; i < TEAMS_COUNT; i++) {
                send_data.teams[i] = htons(room->teams[i]);
            }

            send_packet(approved_player->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);

            // send a message to all players in the room that the player has joined
            ClientPlayer player_data = {.id = htonl(approved_player->id), .team = approved_player->team};
            strcpy(player_data.name, approved_player->name);
            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i];
                if (op != approved_player)
                    send_packet(op->fd, SP_NEW_JOIN, &player_data, sizeof(player_data), 0);
            }

            if (room->joined_players == room->players_number) {
                while (!is_queue_empty(p->approving_queue)) {
                    Player *op;
                    dequeue(p->approving_queue, op);

                    SPJoined send_data = {.status = JOIN_FAILED};
                    send_packet(op->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
                    break;
                }
            }
        }

        if (p->approving_queue.size > 0) {
            approved_player = queue_front(p->approving_queue);
            
            SPApprove send_data = {.id = htonl(approved_player->id), .username = { 0 }};
            strcpy(send_data.username, approved_player->name);

            send_packet(p->fd, SP_APPROVE_PLAYER, &send_data, sizeof(send_data), 0);
        }

        break;
        }
    case CP_START_PLACING: {
        Room *room = p->room;
        
        if (p->net.data_len < sizeof(int16_t) || room->players[0]->fd != p->fd ||
            room->joined_players != room->players_number || room->status != ROOM_AREAS)
            break;

        int16_t areas_count = ntohs(*(int16_t*)p->net.data_buf);
        if (areas_count <= 0 || p->net.data_len != (areas_count * sizeof(Area) + sizeof(areas_count)))
            break;
        
        // send a message to all players that admin starts placing boids
        for (int i = 1; i < room->joined_players; i++) {
            Player *op = room->players[i];
            send_packet(op->fd, SP_START_PLACING, p->net.data_buf, p->net.data_len, 0);
        }

        // send a message to admin of the room
        areas_count = 0;
        send_packet(p->fd, SP_START_PLACING, &areas_count, sizeof(areas_count), 0);
        
        room->status = ROOM_PLACING;
        printf("[*] room %06x started placing\n", room->id);

        break;
        }
    case CP_SEND_BOIDS: {
        Room *room = p->room;
        
        if (p->net.data_len < sizeof(uint16_t) ||
            room->joined_players != room->players_number || room->status != ROOM_PLACING)
            break;

        uint16_t count = ntohs(*(uint16_t*)p->net.data_buf);
        if (p->net.data_len != (count*sizeof(ClientStartNetBoids) + sizeof(count)))
            break;

        ClientStartNetBoids *data = (ClientStartNetBoids*)(p->net.data_buf + sizeof(count));

        for (int i = 0; i < count; i++) {
            p->start_boids[i] = data[i];
            p->start_boids[i].count = ntohs(p->start_boids[i].count);
        }

        p->start_boids_len = count;
        p->ready = true;

        bool all_ready = true;
        for (int i = 0; i < room->joined_players; i++) {
            Player *op = room->players[i];
            if (!op->ready) {
                all_ready = false;
                break;
            }
        }

        if (all_ready) {
            room->status = ROOM_GAME;
            printf("[*] room %06x started the game\n", room->id);

            room->thread_run = true;
            pthread_mutex_init(&room->boids_mtx, NULL);

            RoomThreadArgs *args = malloc(sizeof(*args));
            *args = (RoomThreadArgs){.room = room, .epfd = epfd};
            pthread_create(&room->thread, NULL, room_thread_fn, args);
        }
        
        break;
        }
    case CP_ORDER: {
        /* PACKET FORMAT
        ORDER_CLEAR - [int8 order_type] [uint16 boids_count] [uint16[boids_count] boids]
        ORDER_ACTION - [int8 order_type] [int8 new_action] [uint16 boids_count] [uint16[boids_count] boids]
        ORDER_DIRECTION - [int8 order_type] [int32 vector.x*65535] [int32 vector.y*65535] [uint16 boids_count] [uint16[boids_count] boids]
        ORDER_POINT - [int8 order_type] [uint16 point.x] [uint16 point.y] [uint16 boids_count] [uint16[boids_count] boids]
        ORDER_LINE - [int8 order_type] [uint8 points_count] [{uint16 x, y}[points_count] points] [uint16 boids_count] [uint16[boids_count] boids]
        */
        Room *room = p->room;

        if (room->status != ROOM_GAME)
            break;

        uint8_t order_type = *p->net.data_buf;
        if (order_type == ORDER_CLEAR) {
            if (p->net.data_len < 1+sizeof(BoidIndex))
                break;

            BoidIndex boids_count = ntohs(*(BoidIndex*)(p->net.data_buf + 1));
            BoidIndex *boids = (BoidIndex*)(p->net.data_buf + 1+sizeof(BoidIndex));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(boids[i]);
                ServerBoid *boid = &room->boids[idx];

                boid->direction_order = false;
                boid->point_order = false;                
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_ACTION) {
            if (p->net.data_len < 1+1+sizeof(BoidIndex))
                break;
            
            BoidAction action = *(p->net.data_buf + 1);
            BoidIndex boids_count = ntohs(*(BoidIndex*)(p->net.data_buf + 1+1));
            BoidIndex *boids = (BoidIndex*)(p->net.data_buf + 1+1+sizeof(BoidIndex));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(boids[i]);
                ServerBoid *boid = &room->boids[idx];
                
                if ((boid->b.action == ACT_STOP) && (action != ACT_STOP)) // Randomize boid's speed, if it stops
                    boid->b.velocity = (Vector2){random_value(-10, 10)/10.0, random_value(-10, 10)/10.0};

                boid->b.action = action;
                boid->order_timer = random_value(20, 30)*TPS; // 20-30 seconds
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_DIRECTION) {
            if (p->net.data_len < 1+4+4+sizeof(BoidIndex))
                break;
            
            Vector2 direction = {.x = (int32_t)ntohl(*(int32_t*)(p->net.data_buf + 1))/65535.0,
                                 .y = (int32_t)ntohl(*(int32_t*)(p->net.data_buf + 1+4))/65535.0};
            BoidIndex boids_count = ntohs(*(BoidIndex*)(p->net.data_buf + 1+4+4));
            BoidIndex *boids = (BoidIndex*)(p->net.data_buf + 1+4+4+sizeof(BoidIndex));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(boids[i]);
                ServerBoid *boid = &room->boids[idx];
                
                boid->order_vector = direction;
                boid->direction_order = true;
                boid->point_order = false;
                boid->order_timer = random_value(30, 45)*TPS; // 30-45 seconds
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_POINT) {
            if (p->net.data_len < 1+2+2+sizeof(BoidIndex))
                break;
            
            Vector2 point = {.x = ntohs(*(uint16_t*)(p->net.data_buf + 1)),
                             .y = ntohs(*(uint16_t*)(p->net.data_buf + 1+2))};
            BoidIndex boids_count = ntohs(*(BoidIndex*)(p->net.data_buf + 1+2+2));
            BoidIndex *boids = (BoidIndex*)(p->net.data_buf + 1+2+2+sizeof(BoidIndex));

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(boids[i]);
                ServerBoid *boid = &room->boids[idx];
                
                boid->order_vector = point;
                boid->direction_order = false;
                boid->point_order = true;
                boid->order_timer = random_value(30, 45)*TPS; // 30-45 seconds
            }
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        } else if (order_type == ORDER_LINE) {
            if (p->net.data_len < 1+1+sizeof(Point)+sizeof(BoidIndex))
                break;

            uint8_t line_points_count = *(uint8_t*)(p->net.data_buf + 1);
            Point *points = (Point*)(p->net.data_buf + 1+1); // Received points

            BoidIndex boids_count = ntohs(*(BoidIndex*)(p->net.data_buf + 1+1+line_points_count*sizeof(Point)));
            BoidIndex *boids = (BoidIndex*)(p->net.data_buf + 1+1+line_points_count*sizeof(Point)+sizeof(BoidIndex));

            ServerBoid **b = malloc(boids_count * sizeof(*b)); // Array of selected boids
            BoidIndex bc = 0;

            pthread_mutex_lock(&room->boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                BoidIndex idx = ntohs(boids[i]);
                ServerBoid *boid = &room->boids[idx];

                boid->is_used = false;
                b[bc++] = boid;
            }

            KDNode *tree = create_kdtree(b, bc, 16);

            Vector2 *line_points = calloc(line_points_count, sizeof(*line_points));
            float line_len = 0;

            // First point
            line_points[0] = (Vector2){ntohs(points[0].x), ntohs(points[0].y)};
            
            // Convert all points to Vector2 and calc line_len
            for (int i = 1; i < line_points_count; i++) {
                line_points[i] = (Vector2){ntohs(points[i].x), ntohs(points[i].y)};
                line_len += Vector2Distance(line_points[i-1], line_points[i]);
            }

            float interval = line_len / (bc - 1), remains = 0;
            BoidIndex point_idx = 0;
            Rectangle rec = {0, 0, room->world.x, room->world.y};
            
            // Cycle for each segment
            for (uint8_t segment_idx = 1; segment_idx < line_points_count; segment_idx++) {
                Vector2 segment_start = line_points[segment_idx-1];
                Vector2 segment_end = line_points[segment_idx];
                Vector2 segment_dir = Vector2Normalize(Vector2Subtract(segment_end, segment_start));
                segment_start = Vector2Add(segment_start, Vector2Scale(segment_dir, remains)); // Shift of the beginning of segment to the
                                                                                               // remainder of previous segment

                float segment_len = Vector2Distance(segment_end, segment_start);
                BoidIndex segment_points_count = floorf(segment_len / interval) + ((segment_len > interval) || (segment_idx == 0));

                Vector2 point = segment_start;
                float path_len = 0;
                // Placing boids on segment
                for (BoidIndex segment_point_idx = 0; segment_point_idx < segment_points_count; segment_point_idx++, point_idx++) {
                    ServerBoid *nearest_boid = find_nearest_in_kdtree_approx(tree, point, rec);
                    if (nearest_boid == NULL) break;
                    
                    nearest_boid->order_vector = point;
                    nearest_boid->order_timer = Vector2Distance(nearest_boid->b.pos, point) / BOID_MIN_SPEED / (60.0/TPS);
                    nearest_boid->is_used = true;
                    nearest_boid->point_order = true;
                    nearest_boid->direction_order = false;
                    
                    path_len += interval;
                    point = Vector2Add(point, Vector2Scale(segment_dir, interval));
                }
                remains = path_len - segment_len;
            }

            // printf("boids: %d | used: %d | remains: %d\n", bc, point_idx, bc-point_idx);

            clear_kdtree(tree);
            free(line_points);
            free(b);
            
            room->sync_boids = true;
            pthread_mutex_unlock(&room->boids_mtx);
        }
        
        break;
        }
    }
}

int client_recv(Player *p, int epfd) {
    while (1) {
        // Receive data
        int n = recv(p->fd, p->net.recv_buf, sizeof(p->net.recv_buf), 0);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                return -1; // Error
            
        } else if (n == 0) {
            return 1; // Close client
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
                uint32_t copy = (remaining < need)? remaining : need;

                if (copy > 0) {
                    memcpy((uint8_t*)&p->net.data_len + (sizeof(p->net.data_len) - p->net.bytes_remaining), ptr, copy);
                    p->net.bytes_remaining -= copy;
                    remaining -= copy;
                    ptr += copy;
                }
                if (p->net.bytes_remaining == 0) {
                    p->net.data_len = ntohl(p->net.data_len);
                    if (p->net.data_len > MAX_PACKET_SIZE)
                        return 1;

                    p->net.data_buf = malloc(p->net.data_len);
                    if (p->net.data_buf == NULL)
                        return 1;
                    p->net.state = PARSE_DATA;
                    p->net.bytes_remaining = p->net.data_len;
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
                    process_data(p, epfd);
                    
                    free(p->net.data_buf);
                    p->net.data_buf = NULL;
                    p->net.state = PARSE_TYPE;
                }
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    /* FORMAT
      ./server [-T|--tcp-port] <port> [-c|--chunk] <chunk_size>
    */
    
    unsigned short tcp_port = TCP_PORT;
    bool show_help = false;
    char *prog = argv[0];

    while (--argc) {
        char *arg = *(++argv);

        if (arg[0] == '-') {
            if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
                show_help = true;
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
            } else {
                ERRF("unexpected argument '%s'\n", arg);
            }
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
            "  -c, --chunk <NUM>\n"
            "    Size of chunk in pixels, rounded down to the nearest multiple of %d\n"
            "    (default: %d)\n",
            prog, TCP_PORT, BOID_SIZE, DEFAULT_SERVER_CHUNK_SIZE_PIXELS);

        exit(0);
    }

    printf("chunk: %d pixels\n", chunk_size);
    
    const long max_fd = sysconf(_SC_OPEN_MAX);
    players = calloc(max_fd, sizeof(Player*));
    
    // Create a TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        return 1;
    }

    opt = 1;
    if (setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
        perror("setsockopt TCP_NODELAY");
        close(server_fd);
        return 1;
    }
    
    struct sockaddr_in servaddr = { 0 };
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(tcp_port);
    servaddr.sin_family = AF_INET;

    // Forcefully attaching socket to the port
    if (bind(server_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    // Create epoll instance
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return 1;
    }

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN; // EPOLLIN | EPOLLOUT
    event.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &event)) {
        perror("epoll_ctl");
        close(server_fd);
        close(epfd);
        return 1;
    }

    // Make stdin nonblocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    // Add stdin to epoll
    event.events = EPOLLIN;
    event.data.fd = STDIN_FILENO;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &event)) {
        perror("epoll_ctl stdin");
        close(server_fd);
        close(epfd);
        return 1;
    }

    printf("epoll server on 0.0.0.0:%d\n", tcp_port);

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
            if (fd == server_fd) {
                // Accept all pending connections
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                    if (client_fd > max_fd) {
                        close(client_fd);
                        continue;
                    }

                    Player *player = malloc(sizeof(Player));
                    *player = (Player){.id = (++last_player_id), .fd = client_fd, .joined = false, .ready = false, .approving_queue = { 0 }, .in_queue = NULL};
                    players[client_fd] = player;

                    char ip[INET_ADDRSTRLEN];
                    if (!inet_ntop(servaddr.sin_family, &client_addr.sin_addr, ip, sizeof(ip)))
                        strcpy("?", ip);
                    printf("[+] %s:%d (fd=%d id=%d)\n", ip, ntohs(client_addr.sin_port), client_fd, last_player_id);

                    event.events = EPOLLIN | EPOLLRDHUP;
                    event.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &event)) {
                        perror("epoll_ctl");
                        free(players[client_fd]);
                        close(client_fd);
                    }
                }
            } else if (events[i].data.fd == STDIN_FILENO) {
                // Process standard input
                char buf[256];
                ssize_t r = read(STDIN_FILENO, buf, sizeof(buf) - 1);
                if (r > 0) {
                    buf[r] = '\0';
                    if (strcmp(buf, "quit\n") == 0 || strcmp(buf, "q\n") == 0) {
                        printf("[!] shutting down server\n");
                        running = false;
                        break;
                    }
                } else {
                    running = false;
                    break;
                }
            } else if (events[i].events & EPOLLERR) {
                // Disconnect client
                close_client(epfd, fd);
            } else {
                Player *player = players[fd];
                if (client_recv(player, epfd)) {
                    close_client(epfd, fd);
                }
            }
        }
    }

    for (long i = 0; i < max_fd; i++) {
        if (players[i] != NULL) {
            players[i]->joined = false;
            close_client(epfd, i);
        };
    }
    free(players);

    close(epfd);
    close(server_fd);
}
