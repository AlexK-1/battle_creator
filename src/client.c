#ifndef _WIN32
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/tcp.h>
    #include <fcntl.h>
#else
    #include "winsupport.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>

#include <raylib.h>
#include <raymath.h>

#include "boids.h"
#include "network.h"
#include "logging.h"
#include "queue.h"

#undef ORANGE
#define ORANGE (Color){ 240, 146, 0, 255 }
#define DARKRED (Color){ 200, 11, 25, 255 }

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 450

#define DEFAULT_PLAYERS_COUNT 2
#define DEFAULT_WORLD_SIZE_X 10050
#define DEFAULT_WORLD_SIZE_Y 10050
#define DEFAULT_USERNAME "noname"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

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


/* <================================================ LOGGING ===============================================> */

#define MAX_LOG_LEN 10
#define LOG_BUF_SIZE 1024

typedef struct {
    char string[LOG_BUF_SIZE];
    int lines;
    LogType type;
} LogEntry;

typedef struct {
    LogEntry *items;
    int front, rear, size, max_len;
} Log;

pthread_mutex_t log_mtx;

// Write a message to log (console, game window, maybe file)
void log_message(Log *log, LogType type, const char *format, ...) {
    LogEntry buf = { 0 };
    buf.type = type;
    
    va_list args;
    va_start(args, format);
    vsnprintf(buf.string, LOG_BUF_SIZE, format, args);
    va_end(args);

    // Write to console/file
    write_log(type, "%s", buf.string);

    if (log->items != NULL && type >= log_conf.stdout_log_level) { // log_conf from logging.h
        // Count lines in message
        char *c = buf.string;
        while (*c != '\0') {
            if (*c == '\n') buf.lines++;
            c++;
        }
        if (buf.lines == 0 || *(c-1) != '\n') {
            putchar('\n');
            buf.lines++;
        }

        // Push the message to the log displayed in the game window
        pthread_mutex_lock(&log_mtx);
        cstack_push(*log, buf);
        pthread_mutex_unlock(&log_mtx);
    }
}


/* <=============================================== SOMETHING ==============================================> */

char *get_team_name(int team) {
    switch (team) {
        case TEAM_RED: return "red";
        case TEAM_BLUE: return "blue";
        case TEAM_GREEN: return "green";
        case TEAM_YELLOW: return "yellow";
        default: return NULL;
    }
}

int get_team_id(const char *team_name) {
    switch (team_name[0]) {
        case 'r': return TEAM_RED;
        case 'b': return TEAM_BLUE;
        case 'g': return TEAM_GREEN;
        case 'y': return TEAM_YELLOW;
        default: return -1;
    }
}

int64_t get_player_idx(ClientPlayer *players, uint32_t id) {
    for (int i = 0; i < TEAMS_COUNT; i++) {
        if (players[i].id == id)
            return i;
    }
    return -1;
}

ClientPlayer *find_player(ClientPlayer *players, uint32_t id) {
    int64_t i = get_player_idx(players, id);
    if (i < 0)
        return NULL;
    return &players[i];
}


/* <============================================ NETWORK THREAD ============================================> */

#define INPUT_STRING_LEN 1024

char input_string[INPUT_STRING_LEN];
bool get_input = false, input_received = false, typing_keyboard_input = false, running = true;

int tcp_fd, udp_fd;
struct sockaddr_in udp_servaddr = { 0 };
bool udp_opened = false;

int players_number = DEFAULT_PLAYERS_COUNT, joined_players = 0;
ClientPlayer players[TEAMS_COUNT] = { 0 };
BoidIndex boids_number[TEAMS_COUNT] = { 0 }, total_boids_number = 0;
bool teams_used[TEAMS_COUNT] = { 0 };
uint32_t room_id = 0, player_id = 0;
Point world_size = {DEFAULT_WORLD_SIZE_X, DEFAULT_WORLD_SIZE_Y};
int chunk_size = DEFAULT_CLIENT_CHUNK_SIZE_PIXELS;
int server_target_tps = 0;

typedef enum {
    MODE_WAIT,
    MODE_AREAS,
    MODE_SPAWN,
    MODE_DELETE,
    MODE_SELECT,
    MODE_DIRECTION,
    MODE_POINT,
    MODE_LINE
} GameMode;

RoomStage stage = STAGE_AREAS;
GameMode mode = MODE_WAIT;

typedef struct {
    bool new_room, *select_mode;
    BoidTeam *player_team;
    BoidIndex *boids_count;
    uint8_t *server_tps;
    Log *log;
    Grid *grid;
    uint16_t *areas_count;
    Area *areas;
    ClientBoid *boids;
} NetThreadArgs;

pthread_mutex_t areas_mtx;
pthread_mutex_t boids_mtx;
pthread_mutex_t running_mtx;
pthread_mutex_t input_mtx;
pthread_mutex_t players_mtx;

#define CHECK_LEAST_SIZE(size)                                                                                     \
    if (packet_size < (size)) {                                                                                    \
        log_message(log, L_ERROR, "invalid SP#%d packet length (expected at least %u bytes, %u bytes received)\n", \
                  packet_type, (uint32_t)(size), packet_size);                                                               \
        ex = true;                                                                                                 \
        break;                                                                                                     \
    }

#define CHECK_SIZE(size)                                                                                           \
    if (packet_size != (size)) {                                                                                   \
        log_message(log, L_ERROR, "invalid SP#%d packet length (expected %u bytes, %u bytes received)\n",          \
                  packet_type, (uint32_t)(size), packet_size);                                                               \
        ex = true;                                                                                                 \
        break;                                                                                                     \
    }

void *net_thread_fn(void *args) {
    NetThreadArgs *nargs = args;
    
    bool new_room = nargs->new_room;
    bool *select_mode = nargs->select_mode;
    BoidTeam *player_team = nargs->player_team;
    BoidIndex *boids_count = nargs->boids_count;
    Log *log = nargs->log;
    Area *areas = nargs->areas;
    uint16_t *areas_count = nargs->areas_count;
    uint8_t *server_tps = nargs->server_tps;
    Grid *grid = nargs->grid;
    ClientBoid *boids = nargs->boids;

    uint32_t approved_player_id = 0;
    char approved_player_username[USERNAME_LEN];
    
    enum {
        SYNC_PROTO_NONE = 0,
        SYNC_PROTO_TCP,
        SYNC_PROTO_UDP,
    } sync_proto = SYNC_PROTO_NONE;
    
    bool ex = false; // exit from loop
    while (1) {
        pthread_mutex_lock(&running_mtx);
        if (!running) {
            pthread_mutex_unlock(&running_mtx);
            break;
        }
        pthread_mutex_unlock(&running_mtx);
        
        pthread_mutex_lock(&input_mtx);
        if (input_received) {
            if (approved_player_id != 0) { // 0 is an invalid player id
                bool ok = true;
                int8_t team = -1;
                char team_char = *input_string;
            
                if (team_char == 'r')
                    team = TEAM_RED;
                else if (team_char == 'b')
                    team = TEAM_BLUE;
                else if (team_char == 'g')
                    team = TEAM_GREEN;
                else if (team_char == 'y')
                    team = TEAM_YELLOW;
                else if (team_char == 'n')
                    team = -1;
                else {
                    log_message(log, L_WARNING, "enter valid team\n");
                    get_input = true;
                    ok = false;
                }

                if (team >= 0) {
                    if (boids_number[team] == 0) {
                        log_message(log, L_WARNING, "enter valid team\n");
                        get_input = true;
                        ok = false;
                    }

                    bool team_used = false;
                    pthread_mutex_lock(&players_mtx);
                    for (int i = 0; i < joined_players; i++) {
                        if (players[i].team == team) {
                            team_used = true;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&players_mtx);
                    if (team_used) {
                        log_message(log, L_WARNING, "enter an unused team\n");
                        get_input = true;
                        ok = false;
                    }
                }

                if (ok) {
                    CPApprove send_data = {.id = htonl(approved_player_id), .team = team};
                    send_packet(tcp_fd, CP_APPROVE_PLAYER, &send_data, sizeof(send_data), 0);
                }
            }
            
            input_received = false;
        }
        pthread_mutex_unlock(&input_mtx);
        
        uint8_t packet_type;
        bool recv_udp = false, recv_tcp = false;

        // Try to receive a packet by TCP
        int r = recv(tcp_fd, &packet_type, 1, 0);
        if (r == 0) {
            log_message(log, L_DEBUG, "TCP connection closed\n");
            break;
        } else if (r < 0) {
            bool err_wouldblock;
            #ifdef _WIN32
                int err = WSAGetLastError();
                err_wouldblock = (err == WSAEWOULDBLOCK);
            #else
                err_wouldblock = (errno == EWOULDBLOCK);
            #endif
            if (err_wouldblock) { // No data in TCP
                recv_udp = true;
            } else {
                log_message(log, L_ERROR, "failed to receive a packet size by TCP\n");
                perror("recv");
                break;
            }
        } else {
            recv_tcp = true;
        }

        uint32_t packet_size;
        char recv_buf_stack[32 * 1024], *recv_buf;

        // Receive data by TCP
        if (recv_tcp) {
            recv_all(tcp_fd, &packet_size, sizeof(uint32_t), 0);
            packet_size = ntohl(packet_size);
            recv_buf = malloc(packet_size);

            if (recv_all(tcp_fd, recv_buf, packet_size, 0)) {
                log_message(log, L_ERROR, "error receiving SP#%d packet\n", packet_type);
                perror("recv_all");
                free(recv_buf);
                break;
            }
        }

        // Try to receive a packet by UDP
        if (recv_udp) {
            socklen_t addrlen = sizeof(udp_servaddr);
            ssize_t n = recvfrom(udp_fd, recv_buf_stack, sizeof(recv_buf_stack), 0, (struct sockaddr*)&udp_servaddr, &addrlen);
            if (n == 0) {
                log_message(log, L_DEBUG, "UDP connection closed\n");
                break;
            } else if (n < 0) {
                bool err_wouldblock;
                #ifdef _WIN32
                    int err = WSAGetLastError();
                    err_wouldblock = (err == WSAEWOULDBLOCK);
                #else
                    err_wouldblock = (errno == EWOULDBLOCK);
                #endif
                if (err_wouldblock) {
                    recv_udp = false; // No data
                } else {
                    log_message(log, L_ERROR, "failed to receive data by UDP\n");
                    perror("recv");
                    break;
                }
            } else {
                if ((uint32_t)n >= (1 + sizeof(packet_size))) {
                    // Prepare data to processing
                    recv_buf = recv_buf_stack;
                    packet_type = *(uint8_t*)recv_buf;
                    packet_size = ntohl(*(uint32_t*)(recv_buf + 1));
                    recv_buf += 1 + sizeof(packet_size); // Skip header

                    recv_udp = (uint32_t)n == (1 + sizeof(packet_size) + packet_size);
                } else {
                    recv_udp = false; // Invalid packet
                }
            }
        }

        if (!recv_tcp && !recv_udp) {
            // No data at all (neither TCP nor UDP)
            WaitTime(0.03);
        } else {
            // Process received data
            
            switch (packet_type) {
            case SP_APPROVE_PLAYER: { // Approve/reject new player
                /* PACKET FORMAT
                [SPApprove player]
                */

                CHECK_SIZE( sizeof(SPApprove) );
                
                SPApprove *other_player = (SPApprove*)recv_buf;

                approved_player_id = ntohl(other_player->id);
                strcpy(approved_player_username, other_player->username);

                pthread_mutex_lock(&input_mtx);
                get_input = true;
                pthread_mutex_unlock(&input_mtx);

                log_message(log, L_QUESTION, "team of new player '%s' (r/b/g/y or n for reject):\n", other_player->username);

                break;   
                }
            case SP_NEW_JOIN: {
                /* PACKET FORMAT
                [ClientPlayer new_player]
                */
            
                CHECK_SIZE( sizeof(ClientPlayer) );
                
                ClientPlayer *new_player = (ClientPlayer*)recv_buf;
                new_player->id = ntohl(new_player->id);

                pthread_mutex_lock(&players_mtx);
                players[joined_players++] = *new_player;
                pthread_mutex_unlock(&players_mtx);
                log_message(log, L_JOIN, "new player '%s' - %s\n", new_player->name, get_team_name(new_player->team));

                if (new_room && players_number == joined_players && stage == STAGE_AREAS) {
                    log_message(log, L_INFO, "press ENTER to start placing boids\n");
                }
                approved_player_id = 0;
            
                break;
                }
            case SP_PLAYER_EXIT: {
                /* PACKET FORMAT
                [uin32_t player_id]
                */
            
                CHECK_SIZE( sizeof(uint32_t) );

                uint32_t exited_player = ntohl(*(uint32_t*)recv_buf);

                if ((stage == STAGE_AREAS || stage == STAGE_PLACING) && exited_player == approved_player_id) {
                    log_message(log, L_DISCONNECT, "player '%s' disconnected\n", approved_player_username);
                    approved_player_id = 0;

                    pthread_mutex_lock(&input_mtx);
                    if (get_input)
                        typing_keyboard_input = false;
                    get_input = false;
                    pthread_mutex_unlock(&input_mtx);
                } else {
                    pthread_mutex_lock(&players_mtx);
                    int player_idx = get_player_idx(players, exited_player);
                    if (player_idx < 0 || player_idx >= joined_players) {
                        pthread_mutex_unlock(&players_mtx);
                        break;
                    }

                    log_message(log, L_DISCONNECT, "player '%s' disconnected\n", players[player_idx].name);
            
                    // delete player from array
                    memmove(players + player_idx, players + player_idx + 1,
                            sizeof(players[0]) * (joined_players - player_idx - 1));
                    joined_players--;
                    pthread_mutex_unlock(&players_mtx);
                }
            
                break;
                }
            case SP_START_PLACING:
            case SP_SEND_AREAS: {
                /* PACKET FORMAT
                [uint16 areas_count] [Area[areas_count] areas]
                */

                CHECK_LEAST_SIZE( /*areas_count*/ sizeof(int16_t) );
                
                uint16_t new_areas_count = ntohs(*(int16_t*)recv_buf);
                CHECK_SIZE( sizeof(new_areas_count) + new_areas_count*sizeof(Area) );
                
                if (!new_room) {
                    pthread_mutex_lock(&areas_mtx);
                    *areas_count = new_areas_count;
                    Area *a = (Area*)(recv_buf + sizeof(new_areas_count));
                    for (int i = 0; i < *areas_count; i++) {
                        areas[i].team = a->team;
                        areas[i].rec.x1 = ntohs(a->rec.x1);
                        areas[i].rec.x2 = ntohs(a->rec.x2);
                        areas[i].rec.y1 = ntohs(a->rec.y1);
                        areas[i].rec.y2 = ntohs(a->rec.y2);
                        a++;
                    }
                    pthread_mutex_unlock(&areas_mtx);
                }

                if (packet_type == SP_START_PLACING) {
                    mode = MODE_SPAWN;
                    stage = STAGE_PLACING;

                    log_message(log, L_INFO, "now you can spawn boids on your areas");
                    log_message(log, L_INFO, "press ENTER when you will ready to start the game\n");
                }
            
                break;
                }
            case SP_PLAYER_READY: {
                /* PACKET FORMAT
                [uint32 player_id]
                */

                CHECK_SIZE( sizeof(uint32_t) );

                pthread_mutex_lock(&players_mtx);
                uint32_t id = ntohl(*(uint32_t*)recv_buf);
                int idx = get_player_idx(players, id);
                players[players[idx].team].ready = true;
                log_message(log, L_INFO, "player '%s' is ready\n", players[idx].name);
                pthread_mutex_unlock(&players_mtx);
            
                break;
                }
            case SP_START_GAME: {
                /* PACKET FORMAT
                [uint16 boids_count] [ServerStartNetBoid[boids_count] boids]
                */

                CHECK_LEAST_SIZE( /*boids_count*/ sizeof(uint16_t) );
            
                uint16_t recv_boids_count = ntohs(*(uint16_t*)recv_buf);
                CHECK_SIZE( sizeof(recv_boids_count) + recv_boids_count*sizeof(ServerStartNetBoid) );
                
                if (recv_boids_count != total_boids_number) {
                    log_message(log, L_ERROR, "invalid number of boids in SP#%d packet (expected %d boids, %d boids received)\n",
                              packet_type, total_boids_number, recv_boids_count);
                    ex = true;
                    break;
                }

                ServerStartNetBoid *recv_boids = (ServerStartNetBoid*)(recv_buf + sizeof(recv_boids_count));

                pthread_mutex_lock(&boids_mtx);
                for (int i = 0; i < recv_boids_count; i++) {
                    ServerStartNetBoid recv_boid = recv_boids[i];
                    ClientBoid new_boid = {.b = {.pos = {ntohs(recv_boid.x), ntohs(recv_boid.y)}, .speed = recv_boid.speed/100.0f, .health = BOID_MAX_HEALTH, .xp = recv_boid.xp,
                                                 .team = recv_boid.team, .action = ACT_STOP},
                                           .direction = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0}};
                    boids[i] = new_boid;
                }
                *boids_count = recv_boids_count;

                // Reinit grid
                init_grid(grid, boids, *boids_count, world_size.x, world_size.y, chunk_size);

                pthread_mutex_unlock(&boids_mtx);

                mode = MODE_SELECT;
                stage = STAGE_GAME;
                *select_mode = true;

                log_message(log, L_INFO, "the game has started\n");
            
                break;
                }
            case SP_BOIDS_SYNC: {
                /* PACKET FORMAT
                [uint8 current_server_tps] [uint16 boids_count] [uint16 first_boid_index] [NetBoid[boids_count] boids]
                */

                CHECK_LEAST_SIZE( /*current_server_tps*/ 1 + /*boids_count*/ sizeof(BoidIndex) + /*first_boid_index*/ sizeof(BoidIndex) );

                BoidIndex recv_boids_count = ntohs(*(BoidIndex*)(recv_buf+1));
                CHECK_SIZE( 1 + sizeof(BoidIndex) + sizeof(BoidIndex) + recv_boids_count*sizeof(NetBoid) );
                
                if (sync_proto == SYNC_PROTO_NONE) {
                    if (recv_tcp) {
                        sync_proto = SYNC_PROTO_TCP;
                        log_message(log, L_DEBUG, "Using TCP as a protocol for boids sync\n");
                    } else if (recv_udp) {
                        sync_proto = SYNC_PROTO_UDP;
                        log_message(log, L_DEBUG, "Using UDP as a protocol for boids sync\n");
                    }
                }

                *server_tps = *(uint8_t*)recv_buf;
                BoidIndex boids_first_index = ntohs(*(BoidIndex*)(recv_buf+1+sizeof(BoidIndex)));

                NetBoid *recv_boids = (NetBoid*)(recv_buf + 1 + sizeof(recv_boids_count)*2);

                pthread_mutex_lock(&boids_mtx);
                for (int i = 0; i < recv_boids_count; i++) {
                    NetBoid *recv_boid = &recv_boids[i];
                    ClientBoid *boid = &boids[boids_first_index + i];
                    boid->b.health = recv_boid->health;
                    boid->b.xp = recv_boid->xp;
                    if (recv_boid->action == ACT_FALL || recv_boid->action == ACT_SURRENDER) {
                        boid->is_selected = false;
                        if ((recv_boid->action == ACT_FALL && boid->b.action != ACT_FALL) ||
                            (recv_boid->action == ACT_SURRENDER && boid->b.action != ACT_SURRENDER))
                            boid->sprite_timer = 0;
                        boid->b.action = recv_boid->action;
                        continue;
                    }
                    boid->b.action = recv_boid->action;
                    boid->target_pos = (Vector2){ntohs(recv_boid->x), ntohs(recv_boid->y)};
                    boid->b.velocity.x = recv_boid->vel/255.0*BOID_MAX_SPEED * cos(recv_boid->angle/127.0*PI);
                    boid->b.velocity.y = recv_boid->vel/255.0*BOID_MAX_SPEED * sin(recv_boid->angle/127.0*PI);
                    // if (recv_boid->vel > 0 && !boid->b.is_fighting)
                    //     boid->direction = boid->b.velocity;
                    boid->go_target = true;
                }

                // Update grid
                clear_grid(grid);
                fill_grid(grid, boids, *boids_count);

                pthread_mutex_unlock(&boids_mtx);

                break;
                }
            case SP_ROOM_CLOSED: {
                /* PACKET FORMAT
                [uint8 auto_close]
                */

                CHECK_SIZE( 1 );

                uint8_t auto_close = *(uint8_t*)recv_buf;
                if (auto_close) // room is closed by server
                    log_message(log, L_INFO, "room closed\n");
                else // room is closed player (admin)
                    log_message(log, L_INFO, "room closed by player '%s'\n", players[0].name);
                ex = true;
            
                break;
                }
            case SP_PLAYER_KICKED: {
                /* PACKET FORMAT
                [uint32 player_id]
                */

                CHECK_SIZE( sizeof(uint32_t) );

                uint32_t kicked_player = ntohl(*(uint32_t*)recv_buf);

                pthread_mutex_lock(&players_mtx);
                int player_idx = get_player_idx(players, kicked_player);
                if (player_idx < 0 || player_idx >= joined_players) {
                    pthread_mutex_unlock(&players_mtx);
                    break;
                }
                
                log_message(log, L_DISCONNECT, "player '%s' was kicked out of the room by player '%s'\n", players[player_idx].name, players[0].name);
        
                // delete player from array
                memmove(players + player_idx, players + player_idx + 1,
                        sizeof(players[0]) * (joined_players - player_idx - 1));
                joined_players--;
                pthread_mutex_unlock(&players_mtx);

                break;
                }
            case SP_CHANGE_TEAM: {
                /* PACKET FORMAT
                [uint32 player1_id] [uint32 player2_id]
                */

                CHECK_SIZE( sizeof(uint32_t) + 1 );

                uint32_t pid = ntohl(*(uint32_t*)(recv_buf)); // player ID

                pthread_mutex_lock(&players_mtx);
                int pidx = get_player_idx(players, pid); // player idx
                if (pidx < 0 || pidx >= joined_players) {
                    pthread_mutex_unlock(&players_mtx);
                    break;
                }

                int8_t team = *(int8_t*)(recv_buf+sizeof(uint32_t));
                if (team < 0 || team >= TEAMS_COUNT) {
                    pthread_mutex_unlock(&players_mtx);
                    break;
                }
                
                players[pidx].team = team;
                *player_team = players[get_player_idx(players, player_id)].team;
                
                log_message(log, L_INFO, "player '%s' changed team of player '%s' to %s\n",
                            players[0].name, players[pidx].name, get_team_name(team));

                pthread_mutex_unlock(&players_mtx);
                break;
                }
            case SP_SWAP_TEAMS: {
                /* PACKET FORMAT
                [uint32 player1_id] [uint32 player2_id]
                */

                CHECK_SIZE( sizeof(uint32_t)*2 );
                
                uint32_t pid1 = ntohl(*(uint32_t*)(recv_buf));
                uint32_t pid2 = ntohl(*(uint32_t*)(recv_buf+sizeof(uint32_t)));
                
                pthread_mutex_lock(&players_mtx);

                int pidx1 = get_player_idx(players, pid1);
                if (pidx1 < 0 || pidx1 >= joined_players) {
                    pthread_mutex_unlock(&players_mtx);
                    break;
                }
                int pidx2 = get_player_idx(players, pid2);
                if (pidx2 < 0 || pidx2 >= joined_players) {
                    pthread_mutex_unlock(&players_mtx);
                    break;
                }

                int team_tmp = players[pidx1].team;
                players[pidx1].team = players[pidx2].team;
                players[pidx2].team = team_tmp;
                *player_team = players[get_player_idx(players, player_id)].team;
                
                log_message(log, L_INFO, "player '%s' changed team of player '%s' to %s and team of player '%s' to %s\n",
                            players[0].name, players[pidx1].name, get_team_name(players[pidx1].team),
                            players[pidx2].name, get_team_name(players[pidx2].team));

                pthread_mutex_unlock(&players_mtx);
                break;
                }
            case SP_CHAT_MSG: {
                /* PACKET FORMAT
                [uint32 sender_id] [uint16 msg_len] [uint8[msg_len] msg]
                */

                CHECK_LEAST_SIZE( sizeof(uint32_t) + sizeof(uint16_t) );

                uint32_t sender_id = ntohl(*(uint32_t*)(recv_buf));
                uint32_t msg_len = ntohs(*(uint16_t*)(recv_buf+sizeof(uint32_t)));
                
                CHECK_SIZE( sizeof(uint32_t) + sizeof(uint16_t) + msg_len );

                int sender_idx = get_player_idx(players, sender_id);
                if (sender_idx < 0)
                    break;
                
                char *msg = recv_buf+sizeof(uint32_t)+sizeof(uint16_t);

                log_message(log, L_CHAT, "@%s: %.*s", players[sender_idx].name, msg_len, msg);;
                
                break;
                }
            }
        }

        if (recv_tcp)
            free(recv_buf);

        if (ex)
            break;
    }
    
    pthread_mutex_lock(&running_mtx);
    running = false;
    pthread_mutex_unlock(&running_mtx);
    
    return NULL;
}


/* <============================================ BOIDS AND MAIN ============================================> */

void update_boid_sprite(ClientBoid *boids, BoidIndex boid_index) {
    ClientBoid *boid = &boids[boid_index];
    
    if (boid->b.action == ACT_FALL) {
        boid->sprite = SPRITE_FALL;
        if (boid->sprite_timer < 45)
            boid->sprite_timer++;
        return;
    }
    if (boid->b.action == ACT_SURRENDER) {
        boid->sprite = SPRITE_SURRENDER;
        if (boid->sprite_timer < 50)
            boid->sprite_timer++;
        return;
    }

    // Determine if the boid should fall
    if (boid->sprite_timer > 0) boid->sprite_timer--;

    if (boid->b.is_fighting && boid->sprite_timer == 0 && boid->b.fighting_timer == BOID_MAX_FIGHTING_TIMER) {
        boid->sprite_timer = 5;
        if (boid->b.hit) {
            boid->sprite = SPRITE_OUCH;
        } else {
            boid->sprite = (rand()%2)? SPRITE_HIT_LEFT : SPRITE_HIT_RIGHT;
        }
    }

    if (((boid->sprite == SPRITE_HIT_LEFT || boid->sprite == SPRITE_HIT_RIGHT || boid->sprite == SPRITE_OUCH) && boid->sprite_timer == 0) || !boid->b.is_fighting) {
        if (boid->b.action == ACT_ATTACK) boid->sprite = SPRITE_ANGRY;
        if (boid->b.action == ACT_RETREAT) boid->sprite = SPRITE_SAD;
        if (boid->b.action == ACT_STOP) boid->sprite = SPRITE_NORMAL;
        boid->b.hit = false;
    }
    if (boid->b.is_fighting) {
        boid->direction = Vector2Add(boid->direction, Vector2Scale(Vector2Subtract(boids[boid->b.nearest_enemy_idx].b.pos, boid->b.pos), BOID_FIGHTING_FACTOR));
    }
}

void draw_boid(ClientBoid *boid, Texture2D texture) {
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
    sprite.y += 260 * boid->b.team;
    
    Rectangle destRec = {boid->b.pos.x, boid->b.pos.y, BOID_SIZE, BOID_SIZE};
    if (boid->sprite == SPRITE_SURRENDER) {
        destRec.height = BOID_SIZE * (128/75.0);
    } else if (boid->sprite == SPRITE_FALL) {
        destRec.width = BOID_SIZE * (132/75.0);
        destRec.height = BOID_SIZE * (100/75.0);
    }

    Color tint = WHITE;
    if ((boid->sprite == SPRITE_SURRENDER) || (boid->sprite == SPRITE_FALL)) {
        tint.a = (255.0/50.0) * (50 - boid->sprite_timer);
    }
    
    DrawTexturePro(texture, sprite, destRec, (Vector2){BOID_SIZE/2.0, BOID_SIZE/2.0},
                   atan2f(boid->direction.y, boid->direction.x)*RAD2DEG, tint);
}

void draw_selection(ClientBoid *boid, Texture2D texture, float scale, Color color) {
    static Rectangle white_sprite = {0, 260*TEAMS_COUNT, 146, 149};
    Rectangle dest_rec = {boid->b.pos.x, boid->b.pos.y, BOID_SIZE*scale, BOID_SIZE*scale};
    
    DrawTexturePro(texture, white_sprite, dest_rec, (Vector2){BOID_SIZE*1.2/2.0, BOID_SIZE*1.2/2.0},
                   atan2f(boid->direction.y, boid->direction.x)*RAD2DEG, color);
}

// Original function by @G-5ars
void cleanup_areas(Area *areas, uint16_t *areas_count) {
    // x pass
    for (int i = 0; i < *areas_count; i++) {
        Area *area = &areas[i];
        if (area->team == -1)
            continue;
        for (int j = i+1; j < *areas_count; j++) {
            Area *other = &areas[j];
            if (i == j ||
                area->team != other->team ||
                other->team == -1)
                continue;

            if (area->rec.x1 == other->rec.x1 && area->rec.x2 == other->rec.x2 && (area->rec.y1 == other->rec.y2 || area->rec.y2 == other->rec.y1)) {
                area->rec.y1 = MIN(area->rec.y1, other->rec.y1);
                area->rec.y2 = MAX(area->rec.y2, other->rec.y2);
                other->team = -1; // Mark areas as deleted
            }
        }
    }

    // y pass
    for (int i = 0; i < *areas_count; i++) {
        Area *area = &areas[i];
        if (area->team == -1)
            continue;
        for (int j = i+1; j < *areas_count; j++) {
            Area *other = &areas[j];
            if (i == j ||
                area->team != other->team ||
                other->team == -1)
                continue;

            if (area->rec.y1 == other->rec.y1 && area->rec.y2 == other->rec.y2 && (area->rec.x1 == other->rec.x2 || area->rec.x2 == other->rec.x1)) {
                area->rec.x1 = MIN(area->rec.x1, other->rec.x1);
                area->rec.x2 = MAX(area->rec.x2, other->rec.x2);
                other->team = -1; // Mark area as deleted
            }
        }
    }

    // Remove deleted areas from array
    int offset = 0;
    for (int i = 0; i < *areas_count; i++) {
        areas[i - offset] = areas[i];
        Area *area = &areas[i];
        if (area->team == -1)
            offset++;
    }
    *areas_count -= offset;
}

int send_areas(int fd, uint8_t package_type, Area *areas, uint16_t areas_count) {
    uint32_t buf_size = areas_count * sizeof(*areas) + sizeof(areas_count);
    char *buf = malloc(buf_size);

    *(uint16_t*)buf = htons(areas_count);
    Area *a = (Area*)(buf + sizeof(areas_count));
    for (int i = 0; i < areas_count; i++) {
        *a = areas[i];
        a->rec.x1 = htons(a->rec.x1);
        a->rec.x2 = htons(a->rec.x2);
        a->rec.y1 = htons(a->rec.y1);
        a->rec.y2 = htons(a->rec.y2);
        a++;
    }

    int r = send_packet(fd, package_type, buf, buf_size, 0);

    free(buf);
    return r;
}

typedef enum {
    CMD_HELP,
    CMD_LOG_WARN,
    CMD_LOG_INFO,
    CMD_LOG_CLEAR,
    CMD_EXIT,
    CMD_PLAYERS_LIST,
    CMD_ROOM_INFO,
    CMD_ROOM_ID,
    CMD_COPY_ROOM_ID,
    CMD_CLOSE_ROOM,
    CMD_KICK_PLAYER,
    CMD_CHANGE_TEAM,
    CMD_SWAP_TEAMS
} Command;

typedef struct {
    char *full_cmd;
    char *short_cmd;
    char *description;
} CommandInfo;

CommandInfo commands_list[] = {
    [CMD_HELP]         = {"/help",       "/h",   "[CMD] - Show help message"},
    [CMD_LOG_WARN]     = {"/logwarn",    "/lw",  "<MSG> - Write a warning message to the log"},
    [CMD_LOG_INFO]     = {"/loginfo",    "/li",  "<MSG> - Write an info message to the log"},
    [CMD_LOG_CLEAR]    = {"/logclear",   "/lc",  "- Clear the log"},
    [CMD_EXIT]         = {"/exit",       "/e",   "- Exit from the game"},
    [CMD_PLAYERS_LIST] = {"/players",    "/p",   "- Write a list of players connected to the room"},
    [CMD_ROOM_INFO]    = {"/room",       "/r",   "- Write information about the room"},
    [CMD_ROOM_ID]      = {"/roomid",     "/ri",  "- Print ID of the room"},
    [CMD_COPY_ROOM_ID] = {"/copyroomid", "/cri", "- Copy ID of the room to the clipboard"},
    [CMD_CLOSE_ROOM]   = {"/closeroom",  "/cr",  "- Close the room and disconnect all players"},
    [CMD_KICK_PLAYER]  = {"/kick",       "/k",   "@<NAME> - Kick the player out of the room"},
    [CMD_CHANGE_TEAM]  = {"/changeteam", "/ct",  "@<NAME> <TEAM> - Change player's team"},
    [CMD_SWAP_TEAMS]   = {"/swapteams",  "/st",  "@<NAME1> @<NAME2> - Swap teams of two players"}
};
const int commands_count = sizeof(commands_list)/sizeof(commands_list[0]);

char *autocomple_word(char *word) {
    static char new_word[LOG_BUF_SIZE];
    strcpy(new_word, word);

    size_t len = strlen(word);

    if (word[0] == '/') {
        int matches_count = 0;
        for (int i = 0; i < commands_count; i++) {
            if (strncmp(word, commands_list[i].full_cmd, len) == 0) {
                const char* command = commands_list[i].full_cmd;
                
                char *w = new_word;
                const char *c = command;
                while (*w == *c && *w != '\0') {
                    w++;
                    c++;
                }

                if (matches_count == 0) strcpy(new_word, command);
                else *w = '\0';

                matches_count++;
            }
        }
    } else if (word[0] == '@') {
        int matches_count = 0;
        pthread_mutex_lock(&players_mtx);
        for (int i = 0; i < joined_players; i++) {
            if (strncmp(word+1, players[i].name, len-1) == 0) {
                const char* name = players[i].name;
                
                char *w = new_word+1;
                const char *c = name;
                while (*w == *c && *w != '\0') {
                    w++;
                    c++;
                }

                if (matches_count == 0) strcpy(new_word+1, name);
                else *w = '\0';

                matches_count++;
            }
        }
        pthread_mutex_unlock(&players_mtx);
    }

    return new_word;
}

#define CHECK_ARGS(args_number)                                                               \
    if (argc < (args_number) + 1) {                                                           \
        log_message(log, L_WARNING, "invalid number of arguments for %s command\n", argv[0]); \
        return 1;                                                                             \
    }

#define CHECK_ADMIN()                                                                         \
    if (get_player_idx(players, player_id) != 0) {                                            \
        log_message(log, L_WARNING, "command %s is not aviable\n", argv[0]);                  \
        return 1;                                                                             \
    }

#define GET_TEAM(team_id, arg)                                                                \
    do {                                                                                      \
        (team_id) = get_team_id(arg);                                                         \
        if ((team_id) == -1) {                                                                \
            log_message(log, L_WARNING, "invalid team '%s'\n", (arg));                        \
            return 1;                                                                         \
        }                                                                                     \
    } while (0)

#define GET_USERNAME(username, arg)                                                           \
    do {                                                                                      \
        (username) = (arg);                                                                   \
        if ((username)[0] != '@') {                                                           \
            log_message(log, L_WARNING, "username should start with '@'\n");                  \
            return 1;                                                                         \
        }                                                                                     \
        (username)++; /* skip @ before the username*/                                         \
    } while (0)

#define GET_PLAYER_ID(pid, username)                                                          \
    do {                                                                                      \
        pthread_mutex_lock(&players_mtx);                                                     \
        (pid) = 0;                                                                            \
        for (int i = 0; i < joined_players; i++) {                                            \
            if (strcmp((username), players[i].name) == 0) {                                   \
                (pid) = players[i].id;                                                        \
                break;                                                                        \
            }                                                                                 \
        }                                                                                     \
        pthread_mutex_unlock(&players_mtx);                                                   \
                                                                                              \
        if ((pid) == 0) {                                                                     \
            log_message(log, L_WARNING, "there is no player with name '%s'\n", (username));   \
            return 1;                                                                         \
        }                                                                                     \
    } while (0)

int get_command(char *cmd_string) {
    for (int i = 0; i < commands_count; i++) {
        if (strcmp(cmd_string, commands_list[i].full_cmd) == 0 || strcmp(cmd_string, commands_list[i].short_cmd) == 0)
            return i;
    }

    return -1;
}

int process_command(Log *log, char *command) {
    if (command[0] != '/') {
        log_message(log, L_WARNING, "command should start with '/'\n");
        return 1;
    }

    int argc;
    char **argv = TextSplit(command, ' ', &argc); // Split the command string by spaces

    char delimiter[] = " ";
    char message[LOG_BUF_SIZE];
    char *username;
    uint32_t pid = 0; // player ID
    
    Command cmd = get_command(argv[0]);
    switch (cmd) {
        case CMD_HELP:
            if (argc > 1) {
                // Help message for one command
                int help_cmd = get_command(argv[1]);
                if (help_cmd == -1) {
                    log_message(log, L_WARNING, "unknown command %s\n", argv[1]);
                    return 1;
                }
                CommandInfo command_data = commands_list[help_cmd];

                strcat(message, command_data.full_cmd);
                if (command_data.short_cmd != NULL)
                    sprintf(message+strlen(message), ", %s", command_data.short_cmd);
                if (command_data.description != NULL)
                    sprintf(message+strlen(message), " %s", command_data.description);
                strcat(message, "\n");
            } else {
                // Global help message
                strcpy(message, "list of commands:\n");
                for (int i = 0; i < commands_count; i++) {
                    CommandInfo command_data = commands_list[i];
                    sprintf(message+strlen(message), "     %s", command_data.full_cmd);
                    if (command_data.short_cmd != NULL)
                        sprintf(message+strlen(message), ", %s", command_data.short_cmd);
                    if (command_data.description != NULL)
                        sprintf(message+strlen(message), " %s", command_data.description);
                    strcat(message, "\n");
                }
            }

            log_message(log, L_INFO, "%s", message);
            break;
        case CMD_LOG_WARN:
            CHECK_ARGS(1);
            
            log_message(log, L_WARNING, "%s", TextJoin(argv+1, argc-1, delimiter));
            break;
        case CMD_LOG_INFO:
            CHECK_ARGS(1);
        
            log_message(log, L_INFO, "%s", TextJoin(argv+1, argc-1, delimiter));
            break;
        case CMD_LOG_CLEAR:
            pthread_mutex_lock(&log_mtx);
            log->size = 0;
            pthread_mutex_unlock(&log_mtx);
            break;
        case CMD_EXIT:
            pthread_mutex_lock(&running_mtx);
            running = false;
            pthread_mutex_unlock(&running_mtx);
            break;
        case CMD_PLAYERS_LIST:
            strcpy(message, "players:\n");
            pthread_mutex_lock(&players_mtx);
            for (int i = 0; i < joined_players; i++) {
                ClientPlayer *op = &players[i];

                char *team = get_team_name(op->team);

                snprintf(message+strlen(message), sizeof(message), "  %s - %s\n", op->name, team);
            }
            pthread_mutex_unlock(&players_mtx);
            log_message(log, L_INFO, "%s", message);
            break;
        case CMD_ROOM_INFO:
            log_message(log, L_INFO, "room info\n     id: %06x\n     teams: %d\n     world: %dx%d\n     chunk: %d\n     server tps: %d\n     creator: %s\n"
                            "     boids:  %-4d\n     red:    %-4d\n     blue:   %-4d\n     green:  %-4d\n     yellow: %-4d\n",
                   room_id, players_number, world_size.x, world_size.y, chunk_size, server_target_tps, players[0].name, total_boids_number,
                   boids_number[TEAM_RED],
                   boids_number[TEAM_BLUE],
                   boids_number[TEAM_GREEN],
                   boids_number[TEAM_YELLOW]);
            break;
        case CMD_ROOM_ID:
            log_message(log, L_INFO, "%06x\n", room_id);
            break;
        case CMD_COPY_ROOM_ID:
            SetClipboardText(TextFormat("%06x", room_id));
            break;
        case CMD_CLOSE_ROOM:
            CHECK_ADMIN();

            send_packet(tcp_fd, CP_CLOSE_ROOM, NULL, 0, 0);
            break;
        case CMD_KICK_PLAYER:
            CHECK_ARGS(1);
            CHECK_ADMIN();
            
            GET_USERNAME(username, argv[1]);
            GET_PLAYER_ID(pid, username);
            
            pid = htonl(pid);
            send_packet(tcp_fd, CP_KICK_PLAYER, &pid, sizeof(pid), 0);
            
            break;
        case CMD_CHANGE_TEAM:
            CHECK_ARGS(2);
            CHECK_ADMIN();

            if (stage != STAGE_AREAS) {
                log_message(log, L_WARNING, "you can use %s command only during areas stage\n", argv[0]);
                return 1;
            }

            GET_USERNAME(username, argv[1]);
            GET_PLAYER_ID(pid, username);

            int8_t team;
            GET_TEAM(team, argv[2]);

            if (!teams_used[team]) {
                log_message(log, L_WARNING, "there are no boids for %s team\n", get_team_name(team));
                return 1;
            }

            for (int i = 0; i < joined_players; i++) {
                if (players[i].team == team) {
                    log_message(log, L_WARNING, "%s team already used\n", get_team_name(team));
                    return 1;
                }
            }
            
            char ct_buf[sizeof(pid) + sizeof(team)];
            *(uint32_t*)(ct_buf) = htonl(pid);
            *(int8_t*)(ct_buf+sizeof(pid)) = team;
            send_packet(tcp_fd, CP_CHANGE_TEAM, ct_buf, sizeof(ct_buf), 0);

            break;
        case CMD_SWAP_TEAMS:
            CHECK_ARGS(2);
            CHECK_ADMIN();

            if (stage != STAGE_AREAS) {
                log_message(log, L_WARNING, "you can use %s command only during areas stage\n", argv[0]);
                return 1;
            }

            char *username1, *username2;
            int pid1, pid2;

            GET_USERNAME(username1, argv[1]);
            GET_PLAYER_ID(pid1, username1);

            GET_USERNAME(username2, argv[2]);
            GET_PLAYER_ID(pid2, username2);

            char st_buf[sizeof(pid)*2];
            *(uint32_t*)(st_buf) = htonl(pid1);
            *(uint32_t*)(st_buf+sizeof(pid)) = htonl(pid2);
            send_packet(tcp_fd, CP_SWAP_TEAMS, st_buf, sizeof(st_buf), 0);

            break;
        default:
            log_message(log, L_WARNING, "unknown command %s\n", argv[0]);
            return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    // room/player settings, argparse
    bool new_room = false, hide_areas = false, show_global_help = false, show_command_help = false;
    BoidTeam player_team = TEAM_RED;
    char *prog = argv[0], username[USERNAME_LEN] = DEFAULT_USERNAME , server[INET_ADDRSTRLEN] = DEFAULT_SERVER;
    unsigned short tcp_port = TCP_PORT, udp_port = UDP_PORT;
    
    if (argc < 2) {
        ERR("missed argument: new/join\n");
    }
    if (strcmp(argv[1], "new") == 0 || strcmp(argv[1], "n") == 0)
        new_room = true;
    else if (strcmp(argv[1], "join") == 0 || strcmp(argv[1], "j") == 0)
        new_room = false;
    else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        show_global_help = true;
    } else {
        ERRF("unexpected argument '%s'\n", argv[1]);
    }
    argv++; argc--;

    while (--argc && !show_global_help) {
        char *arg = *(++argv);

        if (arg[0] == '-') {
            int old_argc = argc;
            bool ext = false; // exit

            do {
                if (strcmp(arg, "--help") == 0 || strncmp(arg, "-h", 2) == 0) {
                    show_command_help = true;
                    ext = true;
                    break;
                } else if (strcmp(arg, "--server") == 0 || strcmp(arg, "-s") == 0) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    strcpy(server, *(++argv));
                    argc--;
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
                } else if (strcmp(arg, "--name") == 0 || strcmp(arg, "-n") == 0) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    strcpy(username, *(++argv));
                    argc--;
                } else if (new_room) {
                    if (strcmp(arg, "--players") == 0 || strcmp(arg, "-p") == 0 || (arg[1] == 'p' && isdigit(arg[2]))) {
                        if (argc == 1 && !isdigit(arg[2])) ERRF("no value for option '%s'\n", arg);

                        int s = strlen(arg);

                        char *value_str;
                        if (arg[1] == 'p' && s >= 3) {
                            value_str = arg+2;
                            arg += 1;
                        } else {
                            value_str = *(++argv);
                            argc--;
                        }

                        char *endp;
                        players_number = strtoul(value_str, &endp, 10);
                        if (*endp != '\0') {
                            ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                        }
                        if (players_number == 0 || players_number > 4) {
                            ERR("number of players must be from 1 to 4\n");
                        }
                    } else if (strcmp(arg, "--team") == 0 || strcmp(arg, "-t") == 0) {
                        if (argc == 1) ERRF("no value for option '%s'\n", arg);

                        char *value_str = *(++argv);
                        argc--;

                        int team = get_team_id(value_str);
                        if (team == -1) {
                            ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                        }
                        player_team = team;
                    } else if (strcmp(arg, "--world") == 0 || strcmp(arg, "-w") == 0) {
                        if (argc == 1) ERRF("no value for option '%s'\n", arg);

                        char *value_str = *(++argv);
                        argc--;
                        if (sscanf(value_str, "%hux%hu", &world_size.x, &world_size.y) < 2) {
                            ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                        }
                        world_size.x = ceilf((float)world_size.x / BOID_SIZE) * BOID_SIZE;
                        world_size.y = ceilf((float)world_size.y / BOID_SIZE) * BOID_SIZE;
                    } else if (strcmp(arg, "--hide-areas") == 0 || strncmp(arg, "-a", 2) == 0) {
                        hide_areas = true;
                    } else {
                        ERRF("unexpected argument '%s'\n", arg);
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
        } else if (new_room) {
            char *c;
            int teams[TEAMS_COUNT] = { 0 }, teams_count = 0;
            bool err = false;

            for (c = arg; *c != '\0'; c++) {
                if (isdigit(*c)) break;
                
                int team = get_team_id(c);
                if (team == -1) {
                    err = true;
                    break;
                }
                teams[teams_count++] = team;

                if (*(++c) != ':') {
                    err = true;
                    break;
                }
            }

            if (err || teams_count == 0) {
                ERRF("illegal value '%s' for number of boids option\n", arg);
            }

            char *endp;
            BoidIndex boids = strtoul(c, &endp, 10);
            if (*endp != '\0') {
                ERRF("illegal value '%s' for number of boids option\n", arg);
            }
            for (int i = 0; i < teams_count; i++)
                boids_number[teams[i]] = boids;
        } else if (!new_room) {
            char *endp;
            room_id = strtoul(arg, &endp, 16);
            if (*endp != '\0') {
                ERRF("illegal value '%s' for option 'room'\n", arg);
            }
        } else {
            ERRF("unexpected argument '%s'\n", arg);
        }
    }

    if (show_global_help) {
        printf(
            "Usage: %s COMMAND [OPTIONS]\n"
            "\n"
            "A game's client app.\n"
            "\n"
            "Commands:\n"
            "  new - create a new game room\n"
            "  join - join to an existing room\n"
            "\n"
            "Options:\n"
            "  -h, --help\n"
            "    Show this message and exit\n"
            "\n"
            "Run '%s COMMAND --help' for command-specific help.\n",
            prog, prog);
        exit(0);
    }

    if (show_command_help) {
        if (new_room) { // new
            printf(
                "Usage: %s new [OPTIONS] BOIDS...\n"
                "\n"
                "Create a new game room.\n"
                "\n"
                "Arguments:\n"
                "  <BOIDS>...\n"
                "    Format: [<team>...]:<count>\n"
                "    - <team> is one of: r, b, g, ot y\n"
                "    - Teams can be stacked, for example \"r:b:100\" means teams r and b \n"
                "      share count 100\n"
                "\n"
                "Options:\n"
                "  -h, --help\n"
                "    Show this message and exit\n"
                "  -s, --server <IP>\n"
                "    Server's IP address (default: %s)\n"
                "  -T, --tcp-port <NUM>\n"
                "    TCP port of the game server (default: %d)\n"
                "  -U, --udp-port <NUM>\n"
                "    UDP port of the game server (default: %d)\n"
                "  -n, --name <STR>\n"
                "    Username (default: %s)\n"
                "  -c, --chunk <NUM>\n"
                "    Size of chunk in pixels, rounded down to the nearest multiple of %d\n"
                "    (default: %d)\n"
                "  -p, --players <NUM>\n"
                "    Number of player in new room (default: %d)\n"
                "  -t, --team\n"
                "    A letter indicating the player's team: r, g, b or y (default: r)\n"
                "  -w, --world <NUM>x<NUM>\n"
                "    Size of the game world in pixels in the <width>x<height> format,\n"
                "    rounded up to the nearest number divisible by %d\n"
                "    (default: %dx%d)\n"
                "  -a, --hide-areas\n"
                "    Do not show areas to otrher players while admin player draws them\n"
                "\n"
                "Examples:\n"
                "  %s new --name bebob --world 1050x1050 -p2 r:30 b:40\n"
                "  %s new -s 192.168.0.1 --chunk 525 -p4 r:b:1500 g:y:1000\n",
                prog, DEFAULT_SERVER, TCP_PORT, UDP_PORT, DEFAULT_USERNAME, BOID_SIZE, DEFAULT_CLIENT_CHUNK_SIZE_PIXELS, DEFAULT_PLAYERS_COUNT, BOID_SIZE,
                DEFAULT_WORLD_SIZE_X, DEFAULT_WORLD_SIZE_Y, prog, prog);
        } else { // join
            printf(
                "Usage: %s new [OPTIONS] ROOM\n"
                "\n"
                "Join to an existing room.\n"
                "\n"
                "Arguments:\n"
                "  ROOM\n"
                "    6-digit hex number of room id\n"
                "\n"
                "Options:\n"
                "  -h, --help\n"
                "    Show this message and exit\n"
                "  -s, --server <IP>\n"
                "    Server's IP address (default: %s)\n"
                "  -T, --tcp-port <NUM>\n"
                "    TCP port of the game server (default: %d)\n"
                "  -U, --udp-port <NUM>\n"
                "    UDP port of the game server (default: %d)\n"
                "  -n, --name <STR>\n"
                "    Username (default: %s)\n"
                "  -c, --chunk <NUM>\n"
                "    Size of chunk in pixels, rounded down to the nearest multiple of %d\n"
                "    (default: %d)\n"
                "\n"
                "Examples:\n"
                "  %s join --name glug -c 525 015dc2\n"
                "  %s join -s 192.168.0.1 -T 1234 1012c4\n",
                prog, DEFAULT_SERVER, TCP_PORT, UDP_PORT, DEFAULT_USERNAME, BOID_SIZE, DEFAULT_CLIENT_CHUNK_SIZE_PIXELS, prog, prog);
        }

        exit(0);
    }
    
    if (new_room) {
        int teams_number = 0;
        for (int i = 0; i < TEAMS_COUNT; i++) {
            if (boids_number[i] > 0) {
                teams_number++;
                total_boids_number += boids_number[i];
            }
        }

        if (teams_number != players_number) {
            ERR("you have not set the number of boids for all players\n");
        }
        if (total_boids_number > (world_size.x/BOID_SIZE)*(world_size.y/BOID_SIZE))
            ERRF("you won't be able to place %d boids in a %dx%d world\n", total_boids_number, world_size.x, world_size.y);
        if (total_boids_number > MAX_BOIDS_COUNT) {
            ERRF("number of boids (%u) is greater than max boids count (%u)\n", total_boids_number, MAX_BOIDS_COUNT);
        }
    }

    printf("server %s:%d\n", server, tcp_port);

    #ifdef DEBUG
        set_log_config(NULL, /*print_time=*/ false, /*stdout*/ L_DEBUG, /*file*/ L_DEBUG);
    #else
        set_log_config(NULL, /*print_time=*/ false, /*stdout*/ L_INFO, /*file*/ L_DEBUG);
    #endif

    #ifdef _WIN32
        WSADATA ws_data;
        int er_stat = WSAStartup(MAKEWORD(2,2), &ws_data);
        if (er_stat != 0) {
            perror("WSAStartup");
            return 1;
        }
    #endif
    
    // Create a TCP socket
    bool tcp_opened = false;
    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        perror("socket SOCK_STREAM");
        return 1;
    }

    #ifndef _WIN32
        int opt = 1;
    #else
        char opt = 1;
    #endif
    if (setsockopt(tcp_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
        perror("setsockopt TCP_NODELAY");
        goto tcp_fail;
    }

    struct sockaddr_in tcp_servaddr = { 0 };
    tcp_servaddr.sin_family = AF_INET;
    tcp_servaddr.sin_port = htons(tcp_port);
    socklen_t tcp_addrlen = sizeof(tcp_servaddr);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, server, &tcp_servaddr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        goto tcp_fail;
    }

    if (connect(tcp_fd, (struct sockaddr*)&tcp_servaddr, tcp_addrlen) < 0) {
        perror("connect");
        goto tcp_fail;
    }

    write_log(L_DEBUG, "opened a TCP socket to %s:%d\n", server, tcp_port);
    tcp_opened = true;

    tcp_fail: {
        if (!tcp_opened) {
            write_log(L_ERROR, "failed to open a TCP socket to %s:%d\n", server, tcp_port);
            close(tcp_fd);
            #ifdef _WIN32
                WSACleanup();
            #endif
            return 1;
        }
    }

    // Create a UDP socket
    udp_opened = false;
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("socket SOCK_DGRAM");
        goto udp_fail;
   }

    udp_servaddr.sin_family = AF_INET;
    udp_servaddr.sin_port = htons(udp_port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, server, &udp_servaddr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        goto udp_fail;
    }

    write_log(L_DEBUG, "opened a UDP socket to %s:%d\n", server, udp_port);
    udp_opened = true;

    udp_fail: {
        if (!udp_opened) {
            write_log(L_WARNING, "failed to open a UDP socket to %s:%d\n", server, udp_port);
            close(udp_fd);
        }
    }

    // Join/new room request
    SPJoined recv_data;
    if (new_room) {
        CPNew data = {.players_number = players_number, .player_team = player_team,
                      .world_size = {htons(world_size.x), htons(world_size.y)}, .hide_areas = hide_areas};
        for (int i = 0; i < TEAMS_COUNT; i++)
            data.boids_number[i] = htons(boids_number[i]);
        strncpy(data.creator, username, USERNAME_LEN);
        data.creator[USERNAME_LEN-1] = '\0';

        send_packet(tcp_fd, CP_NEW_ROOM, &data, sizeof(data), 0);

        uint8_t packet_type;
        recv(tcp_fd, &packet_type, 1, 0);
        if (packet_type != SP_JOIN_PLAYER) {
            write_log(L_INFO, "unable to create a room\n");
            return 1;
        }
        
        uint32_t data_len;
        recv_packet(tcp_fd, &recv_data, &data_len, 0);
        if (data_len != sizeof(SPJoined) || recv_data.status != JOIN_OK) {
            write_log(L_INFO, "unable to create a room\n");
            return 1;
        }
    } else {
        CPJoin data = {.room_id = htonl(room_id)};
        strncpy(data.username, username, USERNAME_LEN);
        data.username[USERNAME_LEN-1] = '\0';

        send_packet(tcp_fd, CP_JOIN_ROOM, &data, sizeof(data), 0);

        uint8_t packet_type;
        recv(tcp_fd, &packet_type, 1, 0);
        if (packet_type != SP_JOIN_PLAYER) {
            write_log(L_INFO, "unable to join to the room\n");
            return 1;
        }
        
        uint32_t data_len;
        recv_packet(tcp_fd, &recv_data, &data_len, 0);
        if (data_len != sizeof(SPJoined) || recv_data.status == JOIN_FAILED) {
            write_log(L_INFO, "unable to join to the room\n");
            return 1;
        }

        if (recv_data.status == JOIN_REJECTED) {
            write_log(L_INFO, "admin has rejected your joining\n");
            return 1;
        }

        player_team = recv_data.player_team;
    }

    room_id = ntohl(recv_data.room_id);
    players_number = recv_data.players_number;
    joined_players = recv_data.joined_players;
    world_size.x = ntohs(recv_data.world_size.x);
    world_size.y = ntohs(recv_data.world_size.y);
    server_target_tps = recv_data.server_target_tps;
    
    stage = recv_data.room_stage;
    if (stage == STAGE_AREAS)
        mode = new_room ? MODE_AREAS : MODE_WAIT;
    else
        mode = MODE_SPAWN;
    
    memcpy(&players, &recv_data.players, sizeof(players));
    for (int i = 0; i < joined_players; i++) {
        if (players[i].id == recv_data.player_id)
            strncpy(username, players[i].name, sizeof(username)-1);
        players[i].id = ntohl(players[i].id);
    }
    player_id = ntohl(recv_data.player_id);

    total_boids_number = 0;
    for (int i = 0; i < TEAMS_COUNT; i++) {
        BoidIndex boids = htons(recv_data.teams[i]);
        boids_number[i] = boids;
        total_boids_number += boids;

        teams_used[i] = boids > 0;
    }

    if (udp_opened) {
        /* PACKET FORMAT
        [uint32 player_id] [int32_t player_tcp_fd]
        */
        char buf[sizeof(uint32_t) + sizeof(int32_t)];
        *(uint32_t*)(buf) = recv_data.player_id;
        *(int32_t*)(buf+sizeof(uint32_t)) = recv_data.player_tcp_fd;

        sendto_packet(udp_fd, CP_UDP_HELLO, buf, sizeof(buf), 0, (struct sockaddr*)&udp_servaddr, sizeof(udp_servaddr));
    }

    // Make sockets nonblocking
    #ifdef _WIN32
        u_long flag = 1;
        if (ioctlsocket(tcp_fd, FIONBIO, &flag) != 0) {
            write_log(L_ERROR, "failed to make a TCP socket nonblocking\n");
            perror("ioctlsocket FIONBIO");
            close(tcp_fd);
            if (udp_opened)
                close(udp_fd);
            WSACleanup();
            return 1;
        }
        if (udp_opened && ioctlsocket(udp_fd, FIONBIO, &flag) != 0) {
            write_log(L_WARNING, "failed to make a UDP socket nonblocking\n");
            perror("ioctlsocket FIONBIO");
            close(udp_fd);
            udp_opened = false;
        }
    #else
        int flags = fcntl(tcp_fd, F_GETFL, 0);
        if (fcntl(tcp_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            write_log(L_ERROR, "failed to make a TCP socket nonblocking\n");
            perror("fcntl O_NONBLOCK");
            close(tcp_fd);
            if (udp_opened)
                close(udp_fd);
            return 1;
        }
        if (udp_opened) {
            int flags = fcntl(udp_fd, F_GETFL, 0);
            if (fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                write_log(L_ERROR, "failed to make a UDP socket nonblocking\n");
                perror("fcntl O_NONBLOCK");
                close(udp_fd);
                udp_opened = false;
            }
        }
    #endif

    // Init log
    Log log;
    init_cstack(log, MAX_LOG_LEN);
    
    log_message(&log, L_INFO, "%s\n     id: %06x\n     teams: %d\n     world: %dx%d\n     chunk: %d\n     server tps: %d\n     creator: %s\n"
                    "     boids:  %-4d\n     red:    %-4d\n     blue:   %-4d\n     green:  %-4d\n     yellow: %-4d\n",
           new_room? "created a room" : "joined to the room",
           room_id, players_number, world_size.x, world_size.y, chunk_size, server_target_tps, players[0].name, total_boids_number,
           boids_number[TEAM_RED],
           boids_number[TEAM_BLUE],
           boids_number[TEAM_GREEN],
           boids_number[TEAM_YELLOW]);

    if (!new_room) {
        char players_info[LOG_BUF_SIZE] = "players:\n";
        for (int i = 0; i < joined_players; i++) {
            ClientPlayer *op = &players[i];

            char *team = get_team_name(op->team);

            snprintf(players_info+strlen(players_info), sizeof(players_info), "  %s - %s\n", op->name, team);
        }
        log_message(&log, L_INFO, "%s", players_info);
    }

    log_message(&log, L_INFO, "your username is '%s' and your team is %s\n", username, TextToUpper(get_team_name(player_team)));

    // Init Raylib
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_FULLSCREEN_MODE);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Battle creator");
    SetTargetFPS(60);

    // Control
    bool show_log = true, show_grid = false, show_health = false, delete_boids = false;
    int input_cursor = 0;
    int brush_size = 1;
    BoidAction action = ACT_STOP;

    bool show_arrow = false, change_boids_direction = false; // Delete a variable change_boids_direction?
    Vector2 arrow_start = { 0 };

    bool show_line = false;
    Vector2 line_points[ORDER_LINE_MAX_POINT];
    int line_points_count = 0;
    
    // Selection
    int selecting_team = TEAM_RED;
    bool selecting = false, select_mode = false, selecting_shift_pressed = false,
         clear_order = false, change_boids_action = false;
    Vector2 selection_start = { 0 };

    // Camera
    Camera2D camera = { 0 };
    camera.zoom = 1.0f;
    camera.target = (Vector2){world_size.x/2.0 - GetScreenWidth()/2.0, world_size.y/2.0 - GetScreenHeight()/2.0};

    // Keyboard string input
    // bool typing_string_input = false;
    int input_len = 0;

    // Areas selecting
    Point area_start_selecting = { 0 }, area_end_selecting = { 0 };
    Area areas[MAX_AREAS_COUNT] = { 0 };
    int areas_size[TEAMS_COUNT] = { 0 };
    uint16_t areas_count = 0;

    // Boids
    ClientBoid *boids = calloc(total_boids_number, sizeof(*boids));
    BoidIndex boids_count = 0; // Number of placed boids

    // Grid of chunks
    Grid grid = { 0 };
    init_grid(&grid, boids, boids_count, world_size.x, world_size.y, CHUNK_SIZE_PIXELS);

    // Textures
    Texture2D texture = LoadTexture("resources/texture.png");
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    // Server TPS
    enum {
        TPS_NUM,
        TPS_PERCENT,
        TPS_HIDE
    } tps_display_type = TPS_NUM;
    const int tps_display_types_count = 3;
    uint8_t server_tps = 0;

    // Start a thread to receive messages from the server
    pthread_mutex_init(&log_mtx, NULL);
    pthread_mutex_init(&input_mtx, NULL);
    pthread_mutex_init(&areas_mtx, NULL);
    pthread_mutex_init(&boids_mtx, NULL);
    pthread_mutex_init(&players_mtx, NULL);
    NetThreadArgs thread_args = {.new_room = new_room, .select_mode = &select_mode, .player_team = &player_team, .boids_count = &boids_count,
                                 .server_tps = &server_tps, .log = &log, .grid = &grid, .areas_count = &areas_count, .areas = areas, .boids = boids};
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, net_thread_fn, &thread_args);
    
    while (!WindowShouldClose()) {
        pthread_mutex_lock(&running_mtx);
        if (!running) {
            pthread_mutex_unlock(&running_mtx);
            break;
        }
        pthread_mutex_unlock(&running_mtx);
        
        // Keys
        if (!typing_keyboard_input) {
            if (IsKeyPressed(KEY_L)) show_log = !show_log;
            if (IsKeyPressed(KEY_K)) show_grid = !show_grid;
            if (IsKeyPressed(KEY_M)) tps_display_type++, tps_display_type %= tps_display_types_count;

            if (mode == MODE_SELECT)
                selecting_shift_pressed |= IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        
            if (mode == MODE_AREAS) {
                if (!typing_keyboard_input) {
                    if (IsKeyPressed(KEY_Q)) selecting_team = TEAM_RED;
                    if (IsKeyPressed(KEY_W)) selecting_team = TEAM_BLUE;
                    if (IsKeyPressed(KEY_E)) selecting_team = TEAM_GREEN;
                    if (IsKeyPressed(KEY_R)) selecting_team = TEAM_YELLOW;
                    if (IsKeyPressed(KEY_Z)) selecting_team = -1;
                }
            }
            if (stage == STAGE_PLACING) {
                if (IsKeyPressed(KEY_A)) mode = MODE_SPAWN, select_mode = false;
                if (IsKeyPressed(KEY_S)) mode = MODE_SELECT, select_mode = true, selecting = false, selecting_shift_pressed = false;
                if (IsKeyPressed(KEY_D)) mode = MODE_DELETE, select_mode = false;
                
                if (IsKeyPressed(KEY_X)) delete_boids = true;
            } else if (stage == STAGE_GAME) {
                if (IsKeyPressed(KEY_S)) mode = MODE_SELECT, select_mode = true, selecting = false, selecting_shift_pressed = false;
                if (IsKeyPressed(KEY_D)) mode = MODE_DIRECTION, select_mode = true;
                if (IsKeyPressed(KEY_F)) mode = MODE_POINT, select_mode = true;
                if (IsKeyPressed(KEY_G)) mode = MODE_LINE, select_mode = true, show_line = false;
                
                if (IsKeyPressed(KEY_ONE)) action = ACT_STOP, change_boids_action = select_mode;
                else if (IsKeyPressed(KEY_TWO)) action = ACT_ATTACK, change_boids_action = select_mode;
                else if (IsKeyPressed(KEY_THREE)) action = ACT_RETREAT, change_boids_action = select_mode;
                
                if (IsKeyPressed(KEY_H)) show_health = !show_health;
                if (IsKeyPressed(KEY_Z) && select_mode) clear_order = true;
                if (IsKeyPressed(KEY_T) && (mode == MODE_LINE)) change_boids_direction = true, show_line = false;
            }

            if (IsKeyPressed(KEY_P) || IsKeyPressedRepeat(KEY_P)) brush_size += MAX(1, log10f(brush_size));
            if ((IsKeyPressed(KEY_O) || IsKeyPressedRepeat(KEY_O)) && brush_size > 1) brush_size -= MAX(1, log10f(brush_size));
        }

        Vector2 mouse_position = GetScreenToWorld2D(GetMousePosition(), camera);
        int screen_width = GetScreenWidth();
        int screen_height = GetScreenHeight();

        // Camera
        float wheel = GetMouseWheelMove();
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT) || IsKeyPressedRepeat(KEY_MINUS) || IsKeyPressedRepeat(KEY_KP_SUBTRACT)) wheel = -1.0;
        if (IsKeyPressed(KEY_KP_ADD) || IsKeyPressedRepeat(KEY_KP_ADD) || ((IsKeyPressed(KEY_EQUAL) || IsKeyPressedRepeat(KEY_EQUAL)) && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))) wheel = 1.0;
        if (wheel != 0) {
            if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
                // Change size of the brush
                brush_size += wheel * MAX(1, roundf(logf(brush_size)));
                if (brush_size < 1) brush_size = 1;
            } else {
                // Zoom
                camera.offset = GetMousePosition();
                camera.target = mouse_position;
                float scale = 0.2f*wheel;
                camera.zoom = Clamp(expf(logf(camera.zoom)+scale), 1/16.0f, 64.0f);
            }
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 delta = GetMouseDelta();
            delta = Vector2Scale(delta, -1.0f/camera.zoom);
            camera.target = Vector2Add(camera.target, delta);
        }

        // Areas mode
        if (mode == MODE_AREAS) {
            // Selecting
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && areas_count < MAX_AREAS_COUNT && (selecting_team == -1 || teams_used[selecting_team])) {
                int x = roundf(mouse_position.x/BOID_SIZE);
                if (x < 0) x = 0;
                if (x > world_size.x/BOID_SIZE) x = world_size.x/BOID_SIZE;

                int y = roundf(mouse_position.y/BOID_SIZE);
                if (y < 0) y = 0;
                if (y > world_size.y/BOID_SIZE) y = world_size.y/BOID_SIZE;

                area_end_selecting = (Point){.x = x, .y = y};
                if (!selecting) {
                    area_start_selecting = area_end_selecting;
                    selecting = true;
                }
            } else if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) && (selecting_team == -1 || teams_used[selecting_team])) {
                // Create new area
                
                selecting = false;
                Rec area = {MIN(area_start_selecting.x, area_end_selecting.x), MIN(area_start_selecting.y, area_end_selecting.y),
                            MAX(area_start_selecting.x, area_end_selecting.x), MAX(area_start_selecting.y, area_end_selecting.y)};

                // If new area is not aempty
                if (area.x1 != area.x2 && area.y1 != area.y2) {
                    pthread_mutex_lock(&areas_mtx);
                    bool insert_area = true;
                    int orig_areas_count = areas_count;
                    // Compare a new area with existing ones
                    for (int i = 0; i < orig_areas_count; i++) {
                        int rt = areas[i].team; // Team of the area
                        Rec r = areas[i].rec; // Rectangle of the area

                        // If new area smaller than one of the list and belongs to the same team, do not create this area
                        if ((rt == selecting_team) && (r.x1 <= area.x1) && (r.y1 <= area.y1) && (r.x2 >= area.x2) && (r.y2 >= area.y2)) {
                            insert_area = false;
                            break;
                        }

                        // If the areas overlap
                        Rec intersection = {MAX(area.x1, r.x1), MAX(area.y1, r.y1), MIN(area.x2, r.x2), MIN(area.y2, r.y2)};
                        if (intersection.x1 < intersection.x2 && intersection.y1 < intersection.y2) {
                            // Delete area from the list
                            memmove(&areas[i], &areas[i+1], sizeof(*areas) * (areas_count - i - 1));
                            areas_count--;
                            orig_areas_count--;
                            i--;

                            // Divide the old area into non-overlapping new ones
                            if (intersection.y1 != r.y1) areas[areas_count++] = (Area){.rec = {r.x1, r.y1, r.x2, intersection.y1}, .team = rt};
                            if (intersection.x1 != r.x1) areas[areas_count++] = (Area) {.rec = {r.x1, intersection.y1, intersection.x1, intersection.y2}, .team = rt};
                            if (intersection.x2 != r.x2) areas[areas_count++] = (Area){.rec = {intersection.x2, intersection.y1, r.x2, intersection.y2}, .team = rt};
                            if (intersection.y2 != r.y2) areas[areas_count++] = (Area){.rec = {r.x1, intersection.y2, r.x2, r.y2}, .team = rt};

                            // Subtract the intersecting part from total areas size
                            areas_size[rt] -= (intersection.x2 - intersection.x1) * (intersection.y2 - intersection.y1);
                        }
                    }
                    // Add new area
                    if (selecting_team >= 0 && insert_area) {
                        areas[areas_count++] = (Area){.rec = area, .team = selecting_team};
                        areas_size[selecting_team] += (area.x2 - area.x1) * (area.y2 - area.y1);
                    }

                    cleanup_areas(areas, &areas_count);

                    if (!hide_areas)
                        send_areas(tcp_fd, CP_SEND_AREAS, areas, areas_count);

                    pthread_mutex_unlock(&areas_mtx);
                }
            }

            // Start placing
            pthread_mutex_lock(&players_mtx);
            if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !typing_keyboard_input && new_room && players_number == joined_players) {
                bool ok = true;
                for (int team = 0; team < TEAMS_COUNT; team++) {
                    if (teams_used[team] && boids_number[team] > areas_size[team]) {
                        ok = false;
                        break;
                    }
                }

                // Send data to the server only if size of areas of all teams is greater than or equal to the number of boids
                if (ok) {
                    pthread_mutex_lock(&areas_mtx);
                    send_areas(tcp_fd, CP_START_PLACING, areas, areas_count);
                    pthread_mutex_unlock(&areas_mtx);
                    
                } else {
                    log_message(&log, L_WARNING, "you cannot start placing if size of areas of all teams is less than the number of boids\n");
                }
            }
            pthread_mutex_unlock(&players_mtx);
        }

        // Spawn boids
        BoidIndex deleted_boids_count = 0;
        static SPoint spawn_prev_pos = {-1, -1}, delete_prev_pos = {-1, -1};
        if (stage == STAGE_PLACING) {
            if (mode == MODE_SPAWN && IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && boids_count < boids_number[player_team] && \
                mouse_position.x + (int)(brush_size - brush_size/2) * BOID_SIZE >= 0 &&
                mouse_position.x - (int)(brush_size/2) * BOID_SIZE <= world_size.x &&
                mouse_position.y + (int)(brush_size - brush_size/2) * BOID_SIZE >= 0 &&
                mouse_position.y - (int)(brush_size/2) * BOID_SIZE <= world_size.y) {

                SPoint pos = {(int)mouse_position.x/BOID_SIZE, (int)mouse_position.y/BOID_SIZE};
                
                if (pos.x != spawn_prev_pos.x || pos.y != spawn_prev_pos.y) {
                    SRec brush_rec = {pos.x - brush_size/2, pos.y - brush_size/2,
                                     pos.x - brush_size/2 + brush_size, pos.y - brush_size/2 + brush_size};

                    enum {
                        CELL_UNCHECKED,
                        CELL_USED,
                        CELL_FREE
                    } *cells = calloc(brush_size*brush_size, sizeof(*cells));
                    
                    Area *area = NULL;
                    for (int cell_x = brush_rec.x1; cell_x < brush_rec.x2; cell_x++) {
                        if (cell_x < 0 || cell_x >= world_size.x / BOID_SIZE)
                            continue;
                        if (boids_count >= boids_number[player_team])
                            break;

                        for (int cell_y = brush_rec.y1; cell_y < brush_rec.y2; cell_y++) {
                            if (cell_y < 0 || cell_y >= world_size.y / BOID_SIZE)
                                continue;
                            if (boids_count >= boids_number[player_team])
                                break;
                            
                            bool can_place = false;

                            // Check if new boids is in his team's area
                            if (area != NULL && area->team == (int)player_team &&
                                cell_x >= area->rec.x1 && cell_x < area->rec.x2 && cell_y >= area->rec.y1 && cell_y < area->rec.y2) {
                                can_place = true;
                            } else {
                                for (int i = 0; i < areas_count; i++) {
                                    Area *area = &areas[i];
                                    if (area->team == (int)player_team &&
                                        cell_x >= area->rec.x1 && cell_x < area->rec.x2 && cell_y >= area->rec.y1 && cell_y < area->rec.y2) {
                                        can_place = true;
                                        break;
                                    }
                                }
                            }

                            if (can_place) {
                                float x = cell_x * BOID_SIZE + BOID_SIZE/2.0;
                                float y = cell_y * BOID_SIZE + BOID_SIZE/2.0;

                                uint16_t chunk_x = x / CHUNK_SIZE_PIXELS;
                                uint16_t chunk_y = y / CHUNK_SIZE_PIXELS;
                                uint32_t chunk_index = chunk_x + chunk_y*grid.cols;

                                Chunk *chunk = &grid.chunks[chunk_index];

                                // Check if there are no another boid on the same place
                                int cell_idx = (cell_x - brush_rec.x1) + (cell_y - brush_rec.y1)*brush_size;
                                int cell_status = cells[cell_idx];
                                if (cell_status == CELL_FREE) {
                                    can_place = true;
                                    // UNCHECKED
                                } else if (cell_status == CELL_UNCHECKED) {
                                    for (int i = 0; i < chunk->count; i++) {
                                        ClientBoid *boid = &boids[chunk->boids[i]];
                                        Point boid_pos = {(int)boid->b.pos.x/BOID_SIZE, (int)boid->b.pos.y/BOID_SIZE};
                                        int cell_idx = (boid_pos.x - brush_rec.x1) + (boid_pos.y - brush_rec.y1)*brush_size;
                                        if (boid_pos.x >= brush_rec.x1 && boid_pos.x < brush_rec.x2 && boid_pos.y >= brush_rec.y1 && boid_pos.y < brush_rec.y2)
                                            cells[cell_idx] = CELL_USED;
                                    }

                                    Rec check_border = {MAX(brush_rec.x1, chunk_x*CHUNK_SIZE_PIXELS/BOID_SIZE),
                                                        MAX(brush_rec.y1, chunk_y*CHUNK_SIZE_PIXELS/BOID_SIZE),
                                                        MIN(brush_rec.x2, (chunk_x+1)*CHUNK_SIZE_PIXELS/BOID_SIZE),
                                                        MIN(brush_rec.y2, (chunk_y+1)*CHUNK_SIZE_PIXELS/BOID_SIZE)};
                                    for (int check_cell_x = check_border.x1; check_cell_x < check_border.x2; check_cell_x++) {
                                        for (int check_cell_y = check_border.y1; check_cell_y < check_border.y2; check_cell_y++) {
                                            int check_cell_idx = (check_cell_x - brush_rec.x1) + (check_cell_y - brush_rec.y1)*brush_size;
                                            if (cells[check_cell_idx] == CELL_UNCHECKED)
                                                cells[check_cell_idx] = CELL_FREE;
                                        }
                                    }
                                } else {
                                    can_place = false;
                                }

                                if (can_place && cells[cell_idx] == CELL_FREE) {
                                // If all checks are passed, create a new boid
                                    ClientBoid new_boid = {.b = {.pos = {x, y}, .team = player_team, .action = ACT_STOP},
                                                           .direction = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0}};
                                    boids[boids_count++] = new_boid;
                                    cells[cell_idx] = CELL_USED;
                                }
                            }
                        }
                    }

                    free(cells);

                    spawn_prev_pos = pos;
                    delete_prev_pos = (SPoint){-1, -1};
                }
            }

            // Delete boids (place stage)
            if (mode == MODE_DELETE && IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && boids_count > 0 &&
                mouse_position.x + (int)(brush_size - brush_size/2) * BOID_SIZE >= 0 &&
                mouse_position.x - (int)(brush_size/2) * BOID_SIZE <= world_size.x &&
                mouse_position.y + (int)(brush_size - brush_size/2) * BOID_SIZE >= 0 &&
                mouse_position.y - (int)(brush_size/2) * BOID_SIZE <= world_size.y) {
                
                SPoint pos = {(int)mouse_position.x/BOID_SIZE, (int)mouse_position.y/BOID_SIZE};
                
                if (pos.x != delete_prev_pos.x || pos.y != delete_prev_pos.y) {
                    SRec brush_rec = {pos.x - brush_size/2, pos.y - brush_size/2,
                                     pos.x - brush_size/2 + brush_size, pos.y - brush_size/2 + brush_size};

                    uint32_t *used_chunks = calloc(pow(ceilf((float)brush_size*BOID_SIZE/CHUNK_SIZE_PIXELS) + 1, 2), sizeof(*used_chunks));
                    int used_chunks_count = 0;
                    
                    for (int cell_x = brush_rec.x1; cell_x < brush_rec.x2; cell_x++) {
                        if (cell_x < 0 || cell_x >= world_size.x / BOID_SIZE)
                            continue;

                        for (int cell_y = brush_rec.y1; cell_y < brush_rec.y2; cell_y++) {
                            if (cell_y < 0 || cell_y >= world_size.y / BOID_SIZE)
                                continue;

                            float x = cell_x * BOID_SIZE + BOID_SIZE/2.0;
                            float y = cell_y * BOID_SIZE + BOID_SIZE/2.0;

                            uint16_t chunk_x = x / CHUNK_SIZE_PIXELS;
                            uint16_t chunk_y = y / CHUNK_SIZE_PIXELS;
                            uint32_t chunk_index = chunk_x + chunk_y*grid.cols;

                            Chunk *chunk = &grid.chunks[chunk_index];

                            bool chunk_used = false;
                            for (int i = 0; i < used_chunks_count; i++) {
                                if (chunk_index == used_chunks[i]) {
                                    chunk_used = true;
                                    break;
                                }
                            }

                            if (!chunk_used) {
                                for (int i = 0; i < chunk->count; i++) {
                                    ClientBoid *boid = &boids[chunk->boids[i]];
                                    if (boid->b.pos.x >= brush_rec.x1*BOID_SIZE && boid->b.pos.x < brush_rec.x2*BOID_SIZE &&
                                        boid->b.pos.y >= brush_rec.y1*BOID_SIZE && boid->b.pos.y < brush_rec.y2*BOID_SIZE) {
                                        boid->b.action = ACT_DELETE;
                                        deleted_boids_count++;
                                    }
                                }
                                used_chunks[used_chunks_count++] = chunk_index;
                            }
                        }
                    }

                    free(used_chunks);

                    spawn_prev_pos = (SPoint){-1, -1};
                    delete_prev_pos = pos;
                }
            }

            
            // Send boids and a message that the player is ready to start the game
            if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !typing_keyboard_input) {
                if (boids_count == boids_number[player_team]) {
                    ClientStartNetBoids *data = calloc((boids_count + 1)*2, sizeof(*data));
                    data[0].team = -1;
                    int index = 0;

                    for (int y = 0; y < world_size.y / BOID_SIZE; y++) {
                        for (int x = 0; x < world_size.x / BOID_SIZE; x++) {
                            uint16_t chunk_x = x * BOID_SIZE / CHUNK_SIZE_PIXELS;
                            uint16_t chunk_y = y * BOID_SIZE / CHUNK_SIZE_PIXELS;
                            uint32_t chunk_index = chunk_x + chunk_y*grid.cols;

                            Chunk *chunk = &grid.chunks[chunk_index];

                            bool find = false;
                            for (int i = 0; i < chunk->count; i++) {
                                ClientBoid *boid = &boids[chunk->boids[i]];
                                Point boid_pos = {(int)boid->b.pos.x/BOID_SIZE, (int)boid->b.pos.y/BOID_SIZE};
                                if (boid_pos.x == x && boid_pos.y == y) {
                                    find = true;
                                    break;
                                }
                            }

                            if (find == (data[index].team >= 0))
                                data[index].count++;
                            else
                                data[++index] = (ClientStartNetBoids){.team = find ? (signed)player_team : -1, .count = 1};
                        }
                    }

                    uint16_t count = index + 1;

                    for (int i = 0; i < count; i++) {
                        data[i].count = htons(data[i].count);
                    }
                    
                    uint32_t bufsize = sizeof(count) + count * sizeof(*data);
                    char *buf = malloc(bufsize);

                    uint16_t ncount = htons(count);
                    memcpy(buf, &ncount, sizeof(count));
                    memcpy(buf+sizeof(count), data, count * sizeof(*data));

                    send_packet(tcp_fd, CP_SEND_BOIDS, buf, bufsize, 0);
                    log_message(&log, L_INFO, "wait until other players are ready to start the game\n");

                    free(data);
                    free(buf);
                } else {
                    log_message(&log, L_WARNING, "place all your boids before you start the game\n");
                }
            }
        }

        // Get string input from keyboard
        bool input_key_pressed = false;
        pthread_mutex_lock(&input_mtx);
        if (!typing_keyboard_input) {
            bool command = IsKeyPressed(KEY_SLASH) || IsKeyPressed(KEY_KP_DIVIDE);
            
            if (IsKeyPressed(KEY_SPACE) || (!get_input && command)) {
                typing_keyboard_input = true;
                if (!command) {
                    input_len = 0;
                } else {
                    input_len = 1;
                    input_string[0] = '/'; // Command starts with '/'
                }
                input_string[input_len] = '\0';
                input_cursor = input_len;
            }
        } else {
            // Input ends with ENTER
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                typing_keyboard_input = false;

                if (get_input) {
                    get_input = false;
                    input_received = true;
                    log_message(&log, L_INPUT, "%s", input_string);
                } else if (input_string[0] == '/') { // command
                    log_message(&log, L_INPUT, "%s", input_string);
                    process_command(&log, input_string);
                } else { // message for chat
                    uint32_t packet_size = sizeof(uint16_t) + input_len+1;
                    uint8_t *buf = malloc(packet_size);

                    *(uint16_t*)buf = htons(input_len+1);
                    memcpy(buf+sizeof(uint16_t), input_string, input_len+1);
                    
                    send_packet(tcp_fd, CP_CHAT_MSG, buf, packet_size, 0);
                    free(buf);
                }
            } else {
                const int min_len = get_input ? 0 : 1;

                char pressed_keys[128];
                int pressed_keys_count = 0;

                // Receive characters
                int key = GetCharPressed();
                while (key > 0) {
                    // Only allow keys in range [32..125]
                    if ((key >= 32) && (key <= 125) && (input_len < INPUT_STRING_LEN)) {
                        pressed_keys[pressed_keys_count++] = (char)key;
                    }
                    key = GetCharPressed();
                    input_key_pressed = true;
                }

                // Insert characters
                if (pressed_keys_count > 0 && input_len+pressed_keys_count < LOG_BUF_SIZE-1) {
                    memmove(input_string + input_cursor + pressed_keys_count, input_string + input_cursor, input_len - input_cursor);
                    memcpy(input_string + input_cursor, pressed_keys, pressed_keys_count);
                    input_len += pressed_keys_count;
                    input_cursor += pressed_keys_count;
                    input_string[input_len] = '\0';
                }
                
                if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
                    input_key_pressed = true;
                    if (input_cursor > min_len) {
                        int del_start;
                        int del_end;
                        if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) { // Delete a word
                            del_start = input_cursor;
                            del_end = input_cursor;
                            
                            while (del_start >= min_len && !isalnum(input_string[--del_start]))
                                ;
                            while (del_start >= min_len && isalnum(input_string[--del_start]))
                                ;
                            del_start++;

                        } else {
                            // Delete one character
                            del_start = input_cursor - 1;
                            del_end = input_cursor;
                        }
                        memmove(input_string + del_start, input_string + del_end, input_len - del_end);
                        input_cursor = del_start;
                        input_len -= del_end - del_start;
                        input_string[input_len] = '\0';
                    } else {
                        typing_keyboard_input = false;
                    }
                }

                if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
                    input_key_pressed = true;
                    if (input_cursor < input_len) {
                        int del_start;
                        int del_end;
                        if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) { // Delete a word
                            del_start = input_cursor;
                            del_end = input_cursor;
                            while (del_end < input_len && !isalnum(input_string[++del_end]))
                                ;
                            while (del_end < input_len && isalnum(input_string[++del_end]))
                                ;
                        } else {
                            // Delete one character
                            del_start = input_cursor;
                            del_end = input_cursor + 1;
                        }
                        memmove(input_string + del_start, input_string + del_end, input_len - del_end);
                        input_len -= del_end - del_start;
                        input_string[input_len] = '\0';
                    }
                }

                if ((IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) && input_cursor < input_len) {
                    input_key_pressed = true;
                    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) { // Move through the words
                        while (input_cursor < input_len && !isalnum(input_string[++input_cursor]))
                            ;
                        while (input_cursor < input_len && isalnum(input_string[++input_cursor]))
                            ;
                    } else {
                        // Move through the characters
                        input_cursor++;
                    }
                }
                if ((IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) && input_cursor > min_len) {
                    input_key_pressed = true;
                    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) { // Move through the words
                        while (input_cursor >= min_len && !isalnum(input_string[--input_cursor]))
                            ;
                        while (input_cursor >= min_len && isalnum(input_string[--input_cursor]))
                            ;
                        input_cursor++;
                    } else {
                        // Move through the characters
                        input_cursor--;
                    }
                }

                // Autocomplete last word
                if (IsKeyPressed(KEY_TAB)) {
                    // Get the word before the cursor
                    char word[LOG_BUF_SIZE];
                    char *word_ptr = input_string + input_cursor;
                    char *cursor_ptr = word_ptr;
                    while (word_ptr > input_string && !isspace(*(word_ptr-1)))
                        word_ptr--;
                    int word_len = cursor_ptr - word_ptr;
                    memcpy(word, word_ptr, word_len);
                    word[word_len] = '\0';

                    // Get new word
                    char *new_word = autocomple_word(word);
                    int new_word_len = strlen(new_word);
                    
                    // Insert new word
                    memmove(cursor_ptr + (new_word_len - word_len), cursor_ptr, input_len - input_cursor + (new_word_len - word_len));
                    memmove(word_ptr, new_word, new_word_len);
                    input_cursor += new_word_len - word_len;
                    input_len += new_word_len - word_len;
                }

                if (IsKeyPressed(KEY_HOME)) input_cursor = min_len;
                if (IsKeyPressed(KEY_END)) input_cursor = input_len;
            }
        }
        pthread_mutex_unlock(&input_mtx);

        // Select boids
        if (mode == MODE_SELECT) {
            if (IsMouseButtonDown(MOUSE_RIGHT_BUTTON)) {
                if (!selecting) {
                    selecting = true;
                    selection_start = mouse_position;
                }
            } else if (IsMouseButtonReleased(MOUSE_RIGHT_BUTTON)) {
                selecting = false;
                selecting_shift_pressed = false;
            }
        }
        
        // Update boids in selection
        BoidIndex selected_boids_count = 0;
        if (select_mode) {
            pthread_mutex_lock(&boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
               ClientBoid *boid = &boids[i];
                if (boid->b.team == player_team && boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL && boid->b.action != ACT_DELETE) {
                    if (selecting) {
                        boid->is_selected = (selecting_shift_pressed && boid->is_selected) || (
                                           (boid->b.pos.x > fmin(selection_start.x, mouse_position.x)) &&
                                           (boid->b.pos.x < fmax(selection_start.x, mouse_position.x)) &&
                                           (boid->b.pos.y > fmin(selection_start.y, mouse_position.y)) &&
                                           (boid->b.pos.y < fmax(selection_start.y, mouse_position.y)));
                    }

                    if (boid->is_selected) {
                        // Delete boid
                        if (delete_boids) {
                            boid->b.action = ACT_DELETE;
                            boid->is_selected = false;
                            deleted_boids_count++;
                            continue;
                        } else {
                            /*if (change_boids_action) {
                                if ((boid->b.action == ACT_STOP) && (action != ACT_STOP)) // Randomize boid's speed, if it stops
                                    boid->b.velocity = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};

                                boid->b.action = action;
                                if (action == ACT_STOP) boid->sprite = SPRITE_NORMAL;
                                if (action == ACT_ATTACK) boid->sprite = SPRITE_ANGRY;
                                if (action == ACT_RETREAT) boid->sprite = SPRITE_SAD;
                            }*/
                        }
                    }
                    
                    selected_boids_count += boid->is_selected;
                }
            }

            pthread_mutex_unlock(&boids_mtx);
            
            delete_boids = false;
        }

        // Remove deleted boids from array
        if (deleted_boids_count > 0) {
            pthread_mutex_lock(&boids_mtx);
            int offset = 0;
            for (int i = 0; i < boids_count; i++) {
                boids[i - offset] = boids[i];
                ClientBoid *boid = &boids[i];
                if (boid->b.action == ACT_DELETE)
                    offset++;
            }
            boids_count -= offset;
            pthread_mutex_unlock(&boids_mtx);
        }

        // Direction mode
        Vector2 arrow_vector = Vector2Subtract(mouse_position, arrow_start);
        Vector2 arrow_vector_norm = Vector2Normalize(arrow_vector);
        if (mode == MODE_DIRECTION) {
            show_arrow = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
            if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
                show_arrow = false;
                change_boids_direction = Vector2LengthSqr(arrow_vector) >= 40*40;
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
                arrow_start = mouse_position;
        }

        // Point mode
        if (mode == MODE_POINT) {
            if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
                change_boids_direction= true;
        }

        // Line mode
        if (mode == MODE_LINE) {
            // Building a line from segments
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                float x = mouse_position.x;
                if (x < 0) x = 0;
                else if (x > world_size.x) x = world_size.x;

                float y = mouse_position.y;
                if (y < 0) y = 0;
                else if (y > world_size.y) y = world_size.y;

                Vector2 point_pos = {x, y};

                if (show_line && (line_points_count < ORDER_LINE_MAX_POINT)) {
                    line_points[line_points_count++] = point_pos;
                } else if (!show_line) { // First point
                    show_line = true;
                    line_points[0] = point_pos;
                    line_points_count = 1;
                }
            }
        }

        if (stage == STAGE_GAME && (change_boids_action || change_boids_direction || clear_order)) {
            BoidIndex selected_boids[MAX_BOIDS_COUNT] = { 0 };
            BoidIndex selected_boids_count = 0;
            for (BoidIndex i = 0; i < boids_count; i++) {
                ClientBoid *boid = &boids[i];
                if (boid->b.team == player_team && boid->is_selected)
                    selected_boids[selected_boids_count++] = htons(i);
            }

            /* PACKET FORMAT
            ORDER_CLEAR - [int8 order_type] [uint16 boids_count] [uint16[boids_count] boids]
            ORDER_ACTION - [int8 order_type] [int8 new_action] [uint16 boids_count] [uint16[boids_count] boids]
            ORDER_DIRECTION - [int8 order_type] [int32 vector.x*65535] [int32 vector.y*65535] [uint16 boids_count] [uint16[boids_count] boids]
            ORDER_POINT - [int8 order_type] [uint16 point.x] [uint16 point.y] [uint16 boids_count] [uint16[boids_count] boids]
            ORDER_LINE - [int8 order_type] [uint8 points_count] [{uint16 x, y}[points_count] points] [uint16 boids_count] [uint16[boids_count] boids]
            */

            uint32_t base_packet_size = 1 + sizeof(BoidIndex) + sizeof(BoidIndex)*selected_boids_count; // order_type + boids_count + boids
            uint32_t packet_size = base_packet_size;
            char *data = NULL;

            if (clear_order) { // ORDER_CLEAR
                packet_size += 0;
                data = malloc(packet_size);

                *(int8_t*)(data) = ORDER_CLEAR;
            } else if (change_boids_action) { // ORDER_ACTION
                packet_size += 1;
                data = malloc(packet_size);

                *(int8_t*)(data) = ORDER_ACTION;
                *(int8_t*)(data+1) = action;
            } else if (mode == MODE_DIRECTION) { // ORDER_DIRECTION
                packet_size += 4 + 4;
                data = malloc(packet_size);

                *(int8_t*)(data) = ORDER_DIRECTION;
                *(int32_t*)(data+1) = htonl(arrow_vector_norm.x*65535);
                *(int32_t*)(data+1+4) = htonl(arrow_vector_norm.y*65535);
            } else if (mode == MODE_POINT) { // ORDER_POINT
                packet_size += 2 + 2;
                data = malloc(packet_size);

                int x = mouse_position.x;
                if (x < 0) x = 0;
                else if (x > world_size.x) x = world_size.x;

                int y = mouse_position.y;
                if (y < 0) y = 0;
                else if (y > world_size.y) y = world_size.y;

                *(int8_t*)(data) = ORDER_POINT;
                *(uint16_t*)(data+1) = htons(x);
                *(uint16_t*)(data+1+2) = htons(y);
            } else if (mode == MODE_LINE) { // ORDER_LINE
                packet_size += 1 + sizeof(Point)*line_points_count;
                data = malloc(packet_size);

                *(int8_t*)(data) = ORDER_LINE;
                *(uint8_t*)(data+1) = line_points_count;

                Point *points = (Point*)(data+1+1);
                for (int i = 0; i < line_points_count; i++) {
                    points[i].x = htons(line_points[i].x);
                    points[i].y = htons(line_points[i].y);
                }
            }

            if (data != NULL) {
                *(BoidIndex*)(data + (packet_size - base_packet_size + 1)) = htons(selected_boids_count);
                memcpy(data + (packet_size - base_packet_size + 1 + sizeof(BoidIndex)), selected_boids, sizeof(BoidIndex)*selected_boids_count);

                send_packet(tcp_fd, CP_ORDER, data, packet_size, 0);
                free(data);
            }

            change_boids_action = false;
            change_boids_direction = false;
            clear_order = false;
        }

        // Update boids
        if (stage == STAGE_GAME) {
            memset(boids_number, 0, sizeof(boids_number)); // Clear boids_number

            pthread_mutex_lock(&boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                ClientBoid *boid = &boids[i];

                if (boid->b.action == ACT_DELETE) continue;

                update_base_boid(boids, &grid, i, sizeof(*boids), /*can_change_action*/ true, /*can_fall*/ false);
                update_boid_sprite(boids, i);

                if (boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL) {
                    if (boid->go_target) {
                        Vector2 target_dir = Vector2Subtract(boid->target_pos, boid->b.pos);
                        if (Vector2LengthSqr(target_dir) >= BOID_SIZE*BOID_SIZE) {
                            target_dir = Vector2Normalize(target_dir);
                            boid->b.velocity = Vector2Add(boid->b.velocity, Vector2Scale(target_dir, BOID_TARGET_FACTOR));
                        } else {
                            boid->go_target = false;
                        }
                    }
                    
                    boid_normal_speed(&boid->b);
                    boid_bound(&boid->b, world_size.x, world_size.y);

                    boid->direction.x = boid->direction.x*0.97f + boid->b.velocity.x*0.03f;
                    boid->direction.y = boid->direction.y*0.97f + boid->b.velocity.y*0.03f;

                    boids_number[boid->b.team]++;
                } else {
                    boid->is_selected = false;
                }

                boid->b.pos = Vector2Add(boid->b.pos, Vector2Scale(boid->b.velocity, boid->b.speed));
            }
            pthread_mutex_unlock(&boids_mtx);
        }

        // Update grid
        pthread_mutex_lock(&boids_mtx);
        clear_grid(&grid);
        fill_grid(&grid, boids, boids_count);
        pthread_mutex_unlock(&boids_mtx);

        // Drawing
        BeginDrawing();
            // ClearBackground((Color){255, 235, 206, 255});
            ClearBackground(RAYWHITE);

            BeginMode2D(camera);
            
            // Word border
            DrawRectangleLines(0, 0, world_size.x, world_size.y, BLACK);

            // Draw areas
            if (stage == STAGE_AREAS || stage == STAGE_PLACING) {
                pthread_mutex_lock(&areas_mtx);
                for (int i = 0; i < areas_count; i++) {
                    Area area = areas[i];
                    
                    Color color = RAYWHITE;
                    switch (area.team) {
                        case TEAM_RED: color = (Color){RED.r, RED.g, RED.b, (mode == MODE_AREAS)? 50: 20}; break;
                        case TEAM_BLUE: color = (Color){BLUE.r, BLUE.g, BLUE.b, (mode == MODE_AREAS)? 50: 20}; break;
                        case TEAM_GREEN: color = (Color){GREEN.r, GREEN.g, GREEN.b, (mode == MODE_AREAS)? 50: 20}; break;
                        case TEAM_YELLOW: color = (Color){ORANGE.r, ORANGE.g, ORANGE.b, (mode == MODE_AREAS)? 50: 20}; break;
                    }
                    DrawRectangle(area.rec.x1 * BOID_SIZE, area.rec.y1 * BOID_SIZE,
                                  (area.rec.x2 - area.rec.x1) * BOID_SIZE, (area.rec.y2 - area.rec.y1) * BOID_SIZE, color);
                }
                pthread_mutex_unlock(&areas_mtx);
            }

            // Draw grid
            if (show_grid) {
                for (int i = 1; i < world_size.x / BOID_SIZE; i++) {
                    DrawLine(i*BOID_SIZE, 0, i*BOID_SIZE, world_size.y, BLACK);
                }

                for (int i = 1; i < world_size.x / BOID_SIZE; i++) {
                    DrawLine(0, i*BOID_SIZE, world_size.x, i*BOID_SIZE, BLACK);
                }
            }

            // Draw fallen boids
            for (BoidIndex i = 0; i < boids_count; i++) {
                ClientBoid *boid = &boids[i];
                if (boid->sprite == SPRITE_FALL)
                    draw_boid(boid, texture);
            }

            // Draw selection
            for (BoidIndex i = 0; i < boids_count; i++) {
                ClientBoid *boid = &boids[i];
                if ((boid->sprite != SPRITE_FALL) && (boid->is_selected)) {
                    draw_selection(boid, texture, 1.2, ORANGE);
                    // if (boid->pointOrder)
                        // DrawCircle(boid->orderVector.x, boid->orderVector.y, 20, (Color){0, 0, 0, 50});
                }
            }
            
            // Draw boids
            pthread_mutex_lock(&boids_mtx);
            for (BoidIndex i = 0; i < boids_count; i++) {
                ClientBoid *boid = &boids[i];
                if ((boid->sprite != SPRITE_FALL) && (boid->b.action != ACT_DELETE)) {
                    draw_boid(boid, texture);
                    // if (show_health)
                    //     DrawText(TextFormat("%d", boid->b.health), boid->b.pos.x - 5 - ((int)log10(boid->b.health) * 7), boid->b.pos.y - 50, 20, BLACK);
                }
            }
            pthread_mutex_unlock(&boids_mtx);

            float thick = 4/camera.zoom;
            
            // Draw area selecting
            if (mode == MODE_AREAS && selecting) {
                float rectangleX = fmin(area_start_selecting.x * BOID_SIZE, area_end_selecting.x * BOID_SIZE);
                float rectangleY = fmin(area_start_selecting.y * BOID_SIZE, area_end_selecting.y * BOID_SIZE);
                DrawRectangleLinesEx((Rectangle){rectangleX, rectangleY,
                                     abs(area_start_selecting.x * BOID_SIZE - area_end_selecting.x * BOID_SIZE),
                                     abs(area_start_selecting.y * BOID_SIZE - area_end_selecting.y * BOID_SIZE)},
                                     thick, BLACK);
            }

            // Drawing boids selection box
            if ((mode == MODE_SELECT) && selecting) {
                float rectangleX = fmin(selection_start.x, mouse_position.x);
                float rectangleY = fmin(selection_start.y, mouse_position.y);
                DrawText(TextFormat("%d", selected_boids_count), rectangleX, rectangleY-(20/camera.zoom), 20/camera.zoom, BLACK);
                DrawRectangleLinesEx((Rectangle){rectangleX, rectangleY,
                                     fabs(mouse_position.x - selection_start.x), fabs(mouse_position.y - selection_start.y)},
                                 thick, BLACK);
            }

            // Draw arrow (in direction mode)
            if ((mode == MODE_DIRECTION) && show_arrow && (Vector2LengthSqr(arrow_vector) >= powf(40/camera.zoom, 2))) {
                DrawLineEx(arrow_start, mouse_position, thick, BLACK);
                DrawLineEx(mouse_position, Vector2Add(mouse_position, Vector2Scale(Vector2Rotate(arrow_vector_norm,  160*DEG2RAD), 40/camera.zoom)), thick, BLACK);
                DrawLineEx(mouse_position, Vector2Add(mouse_position, Vector2Scale(Vector2Rotate(arrow_vector_norm, -160*DEG2RAD), 40/camera.zoom)), thick, BLACK);
            }
            
            // Draw point (in point mode)
            if (mode == MODE_POINT) {
                DrawCircle(mouse_position.x, mouse_position.y, 20/camera.zoom, (Color){0, 0, 0, 50});
            }

            // Draw lines (in line mode)
            if (mode == MODE_LINE) {
                if (show_line) {
                    DrawCircle(line_points[0].x, line_points[0].y, 10/camera.zoom, (Color){0, 0, 0, 50});
                    for (uint8_t i = 1; i < line_points_count; i++) {
                        DrawCircle(line_points[i].x, line_points[i].y, 10/camera.zoom, (Color){0, 0, 0, 50});
                        DrawLineEx(line_points[i-1], line_points[i], 10/camera.zoom, (Color){0, 0, 0, 50});
                    }
                    if (line_points_count < ORDER_LINE_MAX_POINT)
                        DrawLineEx(line_points[line_points_count-1], mouse_position, 10/camera.zoom, (Color){0, 0, 0, 10});
                }
                DrawCircle(mouse_position.x, mouse_position.y, 10/camera.zoom, (Color){0, 0, 0, 50});
            }
            
            // Draw brush
            if (mode == MODE_SPAWN || mode == MODE_DELETE) {
                SPoint pos = {(int)mouse_position.x/BOID_SIZE, (int)mouse_position.y/BOID_SIZE};
                DrawRectangle(pos.x*BOID_SIZE - brush_size/2*BOID_SIZE, pos.y*BOID_SIZE - brush_size/2*BOID_SIZE,
                              brush_size*BOID_SIZE, brush_size*BOID_SIZE, (Color){20, 20, 20, 20});
            }
            
            
            EndMode2D();

            // Draw "Mode" label
            switch (mode) {
            case MODE_WAIT:
                DrawText("Mode: Wait", screen_width - 115, 10, 20, BLACK); break;
            case MODE_AREAS:
                DrawText("Mode: Areas", screen_width - 140, 10, 20, BLACK); break;
            case MODE_SPAWN:
                DrawText("Mode: Spawn", screen_width - 140, 10, 20, BLACK); break;
            case MODE_DELETE:
                DrawText("Mode: Delete", screen_width - 140, 10, 20, BLACK); break;
            case MODE_SELECT:
                DrawText("Mode: Select", screen_width - 140, 10, 20, BLACK); break;
            case MODE_DIRECTION:
                DrawText("Mode: Direction", screen_width - 165, 10, 20, BLACK); break;
            case MODE_POINT:
                DrawText("Mode: Point", screen_width - 125, 10, 20, BLACK); break;
            case MODE_LINE:
                DrawText("Mode: Line", screen_width - 115, 10, 20, BLACK); break;
            }

            if (stage == STAGE_GAME) {
                switch (tps_display_type) {
                    case TPS_NUM: DrawText(TextFormat("Server TPS: %2d/%2d", server_tps, server_target_tps), screen_width - 212, 40, 20, BLACK); break;
                    case TPS_PERCENT: DrawText(TextFormat("Server TPS: %3.0f%%", (float)server_tps/server_target_tps * 100), screen_width - 195, 40, 20, BLACK); break;
                    case TPS_HIDE: break;
                }
            }

            Color log_colors[] = {
                [L_DEBUG] = GRAY,
                [L_JOIN] = LIME,
                [L_DISCONNECT] = ORANGE,
                [L_CHAT] = DARKBLUE,
                [L_QUESTION] = BLUE,
                [L_INPUT] = DARKPURPLE,
                [L_INFO] = BLACK,
                [L_WARNING] = RED,
                [L_ERROR] = DARKRED,
                [L_NONE] = BLANK
            };
            
            // Draw log
            if (show_log) {
                int line = 1, idx = log.front;
                pthread_mutex_lock(&log_mtx);
                for (int i = 0; i < log.size; i++) {
                    line += log.items[idx].lines;
                    LogEntry *entry = &log.items[idx];
                    
                    // DrawText(log_prefixes[entry->type], 10, screen_height - line*22 - 5, 20, log_colors[entry->type]);
                    // DrawText(entry->string, 40, screen_height - line*22 - 5, 20, BLACK);
                    DrawText(TextFormat("%s%s", log_prefixes[entry->type], entry->string), 10, screen_height - line*22 - 5, 20, log_colors[entry->type]);

                    idx = (idx > 0)? (idx - 1) : log.max_len-1;
                }
                pthread_mutex_unlock(&log_mtx);
            }

            // Draw keyboard input
            pthread_mutex_lock(&input_mtx);
            if (typing_keyboard_input) {
                static int timer = 0;
                timer++;
                if (timer > 80 || input_key_pressed) timer = 0;
                
                const char *text = TextFormat("%s%s", log_prefixes[L_INPUT], input_string);
                DrawText(text, 10, screen_height - 22 - 5, 20, log_colors[L_INPUT]);

                if (timer < 40) {
                    char text_copy[LOG_BUF_SIZE];
                    const int len = input_cursor + strlen(log_prefixes[L_INPUT]);
                    strncpy(text_copy, text, len);
                    text_copy[len] = '\0';

                    float width = MeasureText(text_copy, 20);
                    DrawText("|", 10 + width, screen_height - 22 - 5, 20, BLACK);
                }
                // DrawText(TextFormat("%d\n%d", input_len, input_cursor), 10, 100, 20, BLACK);
            } else {
                DrawText(log_prefixes[L_INPUT], 10, screen_height - 22 - 5, 20, log_colors[L_INPUT]);
                DrawText("Press '/' or SPACE to start typing . . .", 10 + 22, screen_height - 22 - 5, 20, GRAY);
                
            }
            pthread_mutex_unlock(&input_mtx);

            // Draw boids number
            int l = 0; // line
            pthread_mutex_lock(&players_mtx);
            for (int team = 0; team < TEAMS_COUNT; team++) {
                if (!teams_used[team]) continue;
                
                bool player_joined = false;
                int player_idx = 0;
                for (; player_idx < joined_players; player_idx++) {
                    if (players[player_idx].team == team) {
                        player_joined = true;
                        break;
                    }
                }
                
                Color team_color = BLACK;
                switch (team) {
                    case TEAM_RED: team_color = RED; break;
                    case TEAM_BLUE: team_color = BLUE; break;
                    case TEAM_GREEN: team_color = GREEN; break;
                    case TEAM_YELLOW: team_color = ORANGE; break;
                }

                const char *str = NULL;
                if (stage == STAGE_AREAS) {
                    // <player_name>: <target_boids_number> (<areas_size><! if boids_count less than areas_size>)
                    str = TextFormat("%s: %d%c(%d%s", player_joined ? players[player_idx].name : "-", boids_number[team], new_room ? ' ' : '\0',
                                            areas_size[team], (boids_number[team] < areas_size[team])? ")" : "!)");
                } else if (stage == STAGE_PLACING) {
                    if (team == (signed)player_team) {
                        str = TextFormat("%s: %d (%d left) %s", players[player_idx].name, boids_count, boids_number[team]-boids_count,
                                                players[team].ready? "ready" : "");
                    } else {
                        if (player_joined)
                            str = TextFormat("%s: - %s", players[player_idx].name, players[team].ready? "(ready)" : "");
                        else
                            str = "- : -";
                    }
                } else if (stage == STAGE_GAME) {
                    str = TextFormat("%s: %d", players[player_idx].name, boids_number[team]);
                }

                if (str != NULL)
                    DrawText(str, 10, 40 + (l++)*22, 20, team_color);
            }
            pthread_mutex_unlock(&players_mtx);

            DrawFPS(10, 10);
        EndDrawing();
    }

    // Close socket
    close(tcp_fd);
    if (udp_opened)
        close(udp_fd);
    #ifdef _WIN32
        WSACleanup();
    #endif

    // Close the second (network) thread
    // pthread_cancel(net_thread);
    pthread_mutex_lock(&running_mtx);
    running = false;
    pthread_mutex_unlock(&running_mtx);
    pthread_join(net_thread, NULL);

    log_message(&log, L_INFO, "exit\n");
    
    // Destroy all mutexes and conditions
    pthread_mutex_destroy(&input_mtx);
    pthread_mutex_destroy(&log_mtx);
    pthread_mutex_destroy(&areas_mtx);
    pthread_mutex_destroy(&running_mtx);
    pthread_mutex_destroy(&boids_mtx);

    // Free allocated memory
    free(log.items);
    log.items = NULL;
    free(grid.chunks);
    free(boids);

    // Close Raylib
    UnloadTexture(texture);
    CloseWindow();

    return 0;
}
