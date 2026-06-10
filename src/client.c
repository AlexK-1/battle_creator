#ifndef _WIN32
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/tcp.h>
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
#include <stdarg.h>

#include <raylib.h>
#include <raymath.h>

#include "boids.h"
#include "sock.h"
#include "queue.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 450

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))


// <============================================ GRID AND CHUNKS ===========================================>

#define CHUNK_SIZE_BOIDS 1024
#define CHUNK_SIZE_PIXELS 1050

typedef uint16_t ChunkSize;

typedef struct {
    BoidIndex boids[CHUNK_SIZE_BOIDS]; // Array of boids
    ChunkSize count; // Number of boids in the chunk
} Chunk;
// TODO: Add dynamic array of areas to Chunk

typedef struct {
    Chunk *chunks;
    int screen_width, screen_height;
    uint16_t rows, cols; // Number of chunks by width/height
    uint32_t chunks_count;
} Grid;

// Clear all chunks (set counts to zero)
void clear_grid(Grid *grid) {
    for (uint32_t i = 0; i < grid->chunks_count; i++) {
        grid->chunks[i].count = 0;
    }
}

// Fill chunks with boids
void fill_grid(Grid *grid, ClientBoid *boids, BoidIndex boids_count) {
    for (BoidIndex i = 0; i < boids_count; i++) {
        ClientBoid *boid = &boids[i];
        if ((boid->b.action == ACT_FALL) || (boid->b.action == ACT_SURRENDER || (boid->b.action == ACT_DELETE))) continue;

        float x = boid->b.pos.x;
        float y = boid->b.pos.y;

        if (x < 0) x = 0;
        else if (x > grid->screen_width) x = grid->screen_width;
        if (y < 0) y = 0;
        else if (y > grid->screen_height) y = grid->screen_height;
        
        uint16_t chunk_x = x / CHUNK_SIZE_PIXELS;
        uint16_t chunk_y = y / CHUNK_SIZE_PIXELS;
        uint32_t chunk_index = chunk_x + chunk_y*grid->cols;

        Chunk *chunk = &grid->chunks[chunk_index];

        if (chunk->count < CHUNK_SIZE_BOIDS)
            chunk->boids[chunk->count++] = i;
    }
}

// Initialize grid and fully rebuild chunks (delete and recreate all chunks)
void init_grid(Grid *grid, ClientBoid *boids, BoidIndex boids_count, int width, int height) {
    grid->screen_width = width;
    grid->screen_height = height;
    grid->cols = (uint16_t)(width/CHUNK_SIZE_PIXELS) + 1;
    grid->rows = (uint16_t)(height/CHUNK_SIZE_PIXELS) + 1;
    grid->chunks_count = grid->rows * grid->cols;

    if (grid->chunks != NULL) {
        free(grid->chunks);
    }
    grid->chunks = calloc(grid->chunks_count, sizeof(Chunk));

    fill_grid(grid, boids, boids_count);
}


/* <================================================ LOGGING ===============================================> */

#define MAX_LOG_LEN 10
#define LOG_BUF_SIZE 1024

typedef struct {
    char string[LOG_BUF_SIZE];
    int lines;
} LogEntry;

typedef struct {
    LogEntry *items;
    int front, rear, size;
    size_t max_len;
} Log;

pthread_mutex_t log_mtx;

// Write a message to log
void write_log(Log *log, const char *format, ...) {
    LogEntry buf = { 0 };
    
    va_list args;
    va_start(args, format);
    vsnprintf(buf.string, LOG_BUF_SIZE, format, args);
    va_end(args);

    printf("%s", buf.string);

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

    pthread_mutex_lock(&log_mtx);
    cstack_push(*log, buf);
    pthread_mutex_unlock(&log_mtx);
}


/* <============================================ KEYBOARD INPUT ============================================> */

#define INPUT_STRING_LEN 1024

pthread_mutex_t input_mtx;
pthread_cond_t input_cond;
char input_string[INPUT_STRING_LEN];
bool get_input;

char *get_input_string() {
    get_input = true;
    pthread_mutex_lock(&input_mtx);
    while (get_input)
        pthread_cond_wait(&input_cond, &input_mtx);
    pthread_mutex_unlock(&input_mtx);

    return input_string;
}

int get_one_char() {
    int c = getchar();
    if (c != EOF && c != '\n') {
        int ch;
        do {
            ch = getchar();
        } while (ch != EOF && ch != '\n');
    }
    return c;
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

int get_player_idx(ClientPlayer *players, int id) {
    for (int i = 0; i < TEAMS_COUNT; i++) {
        if (players[i].id == id)
            return i;
    }
    return -1;
}

ClientPlayer *find_player(ClientPlayer *players, int id) {
    int i = get_player_idx(players, id);
    if (i < 0)
        return NULL;
    return &players[i];
}


/* <============================================ NETWORK THREAD ============================================> */

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

typedef enum {
    STAGE_AREAS,
    STAGE_PLACING,
    STAGE_GAME
} GameStage;

typedef struct {
    int fd, players_number, *joined_players, player_team;
    bool new_room, *running;
    uint32_t room_id;
    Point *world_size;
    BoidIndex *boids_number, *boids_count, total_boids_number;
    ClientPlayer *players;
    Log *log;
    int16_t *areas_count;
    Area *areas;
    ClientBoid *boids;
    GameMode *mode;
    GameStage *stage;
} NetThreadArgs;

pthread_mutex_t areas_mtx;
pthread_mutex_t running_mtx;
pthread_mutex_t boids_mtx;

void *net_thread_fn(void *args) {
    NetThreadArgs *nargs = args;
    
    int fd = nargs->fd;
    bool new_room = nargs->new_room;
    int players_number = nargs->players_number;
    int *joined_players = nargs->joined_players;
    int player_team = nargs->player_team;
    uint32_t room_id = nargs->room_id;
    Point *world = nargs->world_size;
    BoidIndex *boids_number = nargs->boids_number;
    BoidIndex *boids_count = nargs->boids_count;
    BoidIndex total_boids_number = nargs->total_boids_number;
    ClientPlayer *players = nargs->players;
    Log *log = nargs->log;
    bool *running = nargs->running;
    Area *areas = nargs->areas;
    int16_t *areas_count = nargs->areas_count;
    ClientBoid *boids = nargs->boids;
    GameMode *mode = nargs->mode;
    GameStage *stage = nargs->stage;

    bool ex = false;
    bool start_areas = false;
    while (1) {
        uint8_t packet_type;
        if (recv(fd, &packet_type, 1, 0) <= 0)
            break;

        switch (packet_type) {
        case SP_APPROVE_PLAYER: { // Approje/reject new player
            SPApprove other_player;
            uint32_t packet_len;
            if (recv_packet(fd, &other_player, &packet_len, 0)) {
                ex = true;
                break;
            }
            if (packet_len != sizeof(other_player)) {
                ex = true;
                break;
            }
            other_player.fd = ntohl(other_player.fd);

            write_log(log, "[?] team of new player '%s' (id=%d) (r/b/g/y or n for reject):\n", other_player.username, other_player.fd);
            
            int8_t other_team = -2;
            while (other_team == -2) {
                char other_team_char = *get_input_string();

                if (other_team_char == 'r')
                    other_team = TEAM_RED;
                else if (other_team_char == 'b')
                    other_team = TEAM_BLUE;
                else if (other_team_char == 'g')
                    other_team = TEAM_GREEN;
                else if (other_team_char == 'y')
                    other_team = TEAM_YELLOW;
                else if (other_team_char == 'n')
                    other_team = -1;
                else if (other_team_char == EOF) {
                    putchar('\n');
                    ex = true;
                    break;
                }
                
                if ((other_team != -1) && ((other_team == -2)? (other_team_char != '\n' && other_team_char != '\r') : (boids_number[other_team] == 0))) {
                    printf("enter valid team\n");
                    other_team = -2;
                } else {
                    bool team_used = false;
                    for (int i = 0; i < *joined_players; i++) {
                        if (players[i].team == other_team) {
                            team_used = true;
                            break;
                        }
                    }
                    if (team_used) {
                        printf("enter an unused team\n");
                        other_team = -2;
                    }
                }
            }

            if (other_team != -2)
                send_packet(fd, CP_APPROVE_PLAYER, &other_team, 1, 0);
            
            break;   
            }
        case SP_NEW_JOIN: {
            ClientPlayer new_player;
            uint32_t packet_len;
            if (recv_packet(fd, &new_player, &packet_len, 0)) {
                ex = true;
                break;
            }
            if (packet_len != sizeof(new_player)) {
                ex = true;
                break;
            }
            new_player.id = ntohl(new_player.id);

            players[(*joined_players)++] = new_player;
            write_log(log, "[+] new player '%s' (id=%d) - %s\n", new_player.name, new_player.id, get_team_name(new_player.team));

            if (new_room && players_number == *joined_players) {
                write_log(log, "[*] press ENTER to start placing boids\n");
            }
            
            break;
            }
        case SP_PLAYER_EXIT: {
            uint32_t exited_player, packet_len;
            if (recv_packet(fd, &exited_player, &packet_len, 0)) {
                ex = true;
                break;
            }
            if (packet_len != sizeof(exited_player)) {
                ex = true;
                break;
            }
            exited_player = ntohl(exited_player);

            int player_idx = get_player_idx(players, exited_player);
            write_log(log, "[-] disconnected player '%s' (id=%d)\n", players[player_idx].name, exited_player);
            
            // delete player from array
            memmove(players + player_idx, players + player_idx + 1,
                    sizeof(players[0]) * (*joined_players - player_idx - 1));
            (*joined_players)--;
            
            break;
            }
        case SP_START_PLACING: {
            uint32_t packet_size;
            recv_all(fd, &packet_size, sizeof(uint32_t), 0);
            packet_size = ntohl(packet_size);
            void *buf = malloc(packet_size);

            recv_all(fd, buf, packet_size, 0);
            
            int16_t new_areas_count = ntohs(*(int16_t*)buf);
            if (new_areas_count < 0 || packet_size != (sizeof(new_areas_count) + new_areas_count*sizeof(Area)))
                break;

            if (!new_room) {
                pthread_mutex_lock(&areas_mtx);
                *areas_count = new_areas_count;
                Area *a = buf + sizeof(*areas_count);
                for (int i = 0; i < *areas_count; i++) {
                    areas[i] = *a;
                    areas[i].rec.x1 = ntohs(a->rec.x1);
                    areas[i].rec.x2 = ntohs(a->rec.x2);
                    areas[i].rec.y1 = ntohs(a->rec.y1);
                    areas[i].rec.y2 = ntohs(a->rec.y2);
                    a++;
                }
                pthread_mutex_unlock(&areas_mtx);
            }
            *mode = MODE_SPAWN;
            *stage = STAGE_PLACING;

            free(buf);

            write_log(log, "[*] now you can spawn boids on your areas");
            write_log(log, "[*] press ENTER when you will ready to start the game\n");
            
            break;
            }
        case SP_START_GAME: {
            uint32_t packet_size;
            recv_all(fd, &packet_size, sizeof(uint32_t), 0);
            packet_size = ntohl(packet_size);
            void *buf = malloc(packet_size);

            recv_all(fd, buf, packet_size, 0);

            uint16_t recv_boids_count = ntohs(*(uint16_t*)buf);
            if (recv_boids_count != total_boids_number || packet_size != (sizeof(recv_boids_count) + recv_boids_count*sizeof(ServerStartNetBoid)))
                break;

            ServerStartNetBoid *recv_boids = buf + sizeof(recv_boids_count);

            pthread_mutex_lock(&boids_mtx);
            for (int i = 0; i < recv_boids_count; i++) {
                ServerStartNetBoid recv_boid = recv_boids[i];
                ClientBoid new_boid = {.b = {.pos = {ntohs(recv_boid.x), ntohs(recv_boid.y)}, .speed = recv_boid.speed, .health = BOID_MAX_HEALTH, .xp = recv_boid.xp,
                                             .team = recv_boid.team, .action = ACT_STOP}, .direction = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0}};
                boids[i] = new_boid;
            }
            *boids_count = recv_boids_count;
            pthread_mutex_unlock(&boids_mtx);

            *mode = MODE_SELECT;
            *stage = STAGE_GAME;

            free(buf);

            write_log(log, "[*] the game has started\n");
            
            break;
            }
        }

        if (ex)
            break;
    }
    
    pthread_mutex_lock(&running_mtx);
    *running = false;
    pthread_mutex_unlock(&running_mtx);
    
    return NULL;
}


/* <================================================= MAIN =================================================> */

#define MAX_AREAS_COUNT 1024

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
    if (boid->sprite == SPRITE_FALL) {
        tint.a = 20;
    } else if ((boid->sprite == SPRITE_SURRENDER) || (boid->sprite == SPRITE_FALL)) {
        tint.a = (255.0/50.0) * (50 - boid->sprite_timer);
    }
    
    DrawTexturePro(texture, sprite, destRec, (Vector2){BOID_SIZE/2.0, BOID_SIZE/2.0},
                   atan2f(boid->direction.y, boid->direction.x)*RAD2DEG, tint);
}

void draw_selection(ClientBoid *boid, Texture2D texture) {
    static Rectangle white_sprite = {0, 260*TEAMS_COUNT, 146, 149};
    Rectangle destRec = {boid->b.pos.x, boid->b.pos.y, BOID_SIZE*1.2, BOID_SIZE*1.2};
    Color tint = ORANGE;
    
    DrawTexturePro(texture, white_sprite, destRec, (Vector2){BOID_SIZE*1.2/2.0, BOID_SIZE*1.2/2.0},
                   atan2f(boid->direction.y, boid->direction.x)*RAD2DEG, tint);
}

int main(int argc, char **argv) {
    /* FORMAT
     Join to the room:
         ./client join [-s|--server] <server_ip> [--room|-r] <room_id> [--name|-n] <username>
     Create new room:
         ./client new [-s|--server] <server_ip> [--players|-p] <players_count> [--world|-w] <wold_size> <boids_count>
         <boids_count> is in the format <team>:<count>, setting <count> to the number of boids in <team>
             Example (4 teams, red and blue - 2000 boids, green - 1000 boids, yellow - 500 boids):
             ./client new 4 r:b:2000 g:1000 y:500
         <world_size> is in format <width>x<height>
        
    */

    // room/player settings, argparse
    bool new_room;
    uint32_t room_id = 0;
    int players_number = 0, player_team = TEAM_RED;
    BoidIndex boids_number[TEAMS_COUNT] = { 0 }, total_boids_number = 0;
    char *username = NULL, *server = "127.0.0.1", *player_team_name = NULL;
    Point world_size = {10050, 10050};
    short tcp_port = INPUT_PORT;;
    
    if (argc < 2) {
        fputs("missed argument: new/join\n", stderr);
        return 1;
    }
    if (strcmp(argv[1], "new") == 0 || strcmp(argv[1], "n") == 0)
        new_room = true;
    else if (strcmp(argv[1], "join") == 0 || strcmp(argv[1], "j") == 0)
        new_room = false;
    else {
        fprintf(stderr, "unexpected argument '%s'\n", argv[1]);
        return 1;
    }
    argv++; argc--;

    while (--argc) {
        char *arg = *(++argv);

        if (arg[0] == '-') {
            if (argc == 1 && !(arg[1] == 'p' && isdigit(arg[2]))) {
                fprintf(stderr, "no value for option '%s'\n", arg);
                return 1;
            }
            if (strcmp(arg, "--server") == 0 || strcmp(arg, "-s") == 0) {
                server = *(++argv);
                argc--;
            } else if (strcmp(arg, "--tcp-port") == 0 || strcmp(arg, "-t") == 0) {
                char *value_str = *(++argv);
                argc--;

                char *endp;
                tcp_port = strtoul(value_str, &endp, 10);
                if (*endp != '\0') {
                    fprintf(stderr, "illegal value '%s' for option '%s'\n", value_str, arg);
                    return 1;
                }
            } else if (strcmp(arg, "--name") == 0 || strcmp(arg, "-n") == 0) {
                username = *(++argv);
                argc--;
            } else if (new_room) {
                if (strcmp(arg, "--players") == 0 || (arg[1] == 'p' && isdigit(arg[2]))) {
                    int s = strlen(arg);

                    char *value_str;
                    if (arg[1] == 'p' && s >= 3) {
                        value_str = arg+2;
                    } else {
                        value_str = *(++argv);
                        argc--;
                    }

                    char *endp;
                    players_number = strtoul(value_str, &endp, 10);
                    if (*endp != '\0') {
                        fprintf(stderr, "illegal value '%s' for option '%s'\n", value_str, arg);
                        return 1;
                    }
                    if (players_number == 0 || players_number > 4) {
                        fprintf(stderr, "number of players must be from 1 to 4\n");
                        return 1;
                    }
                } else if (strcmp(arg, "--team") == 0 || strcmp(arg, "-t") == 0) {
                    char *value_str = *(++argv);
                    argc--;

                    if (*value_str == 'r') player_team = TEAM_RED;
                    else if (*value_str == 'b') player_team = TEAM_BLUE;
                    else if (*value_str == 'g') player_team = TEAM_GREEN;
                    else if (*value_str == 'y') player_team = TEAM_YELLOW;
                    else {
                        fprintf(stderr, "illegal value '%s' for option '%s'\n", value_str, arg);
                        return 1;
                    }
                } else if (strcmp(arg, "--world") == 0 || strcmp(arg, "-w") == 0) {
                    char *value_str = *(++argv);
                    argc--;
                    if (sscanf(value_str, "%hux%hu", &world_size.x, &world_size.y) < 2) {
                        fprintf(stderr, "illegal value '%s' for option '%s'\n", value_str, arg);
                        return 1;
                    }
                    world_size.x = ceilf((float)world_size.x / BOID_SIZE) * BOID_SIZE;
                    world_size.y = ceilf((float)world_size.y / BOID_SIZE) * BOID_SIZE;
                } else {
                    fprintf(stderr, "unexpected argument '%s'\n", arg);
                    return 1;
                }
                
            } else {
                if (strcmp(arg, "--room") == 0 || strcmp(arg, "-r") == 0) {
                    char *value_str = *(++argv);
                    argc--;
                
                    char *endp;
                    room_id = strtoul(value_str, &endp, 16);
                    if (*endp != '\0') {
                        fprintf(stderr, "illegal value '%s' for option '%s'\n", value_str, arg);
                        return 1;
                    }
                } else {
                    fprintf(stderr, "unexpected argument '%s'\n", arg);
                    return 1;
                }
            }
        } else if (new_room) {
            char teams[TEAMS_COUNT] = { 0 }, *c;
            int teams_count = 0;
            bool err = false;

            for (c = arg; *c != '\0'; c++) {
                if (isdigit(*c)) break;
                
                int team;
                if (*c == 'r') team = TEAM_RED;
                else if (*c == 'b') team = TEAM_BLUE;
                else if (*c == 'g') team = TEAM_GREEN;
                else if (*c == 'y') team = TEAM_YELLOW;
                else {
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
                fprintf(stderr, "illegal value '%s' for number of boids option\n", arg);
                return 1;
            }

            char *endp;
            BoidIndex boids = strtoul(c, &endp, 10);
            if (*endp != '\0') {
                fprintf(stderr, "illegal value '%s' for number of boids option\n", arg);
                return 1;
            }
            for (int i = 0; i < teams_count; i++)
                boids_number[teams[i]] = boids;
            
        } else {
            fprintf(stderr, "unexpected argument '%s'\n", arg);
            return 1;
        }
    }

    if (new_room) {
        player_team_name = get_team_name(player_team);
        if (boids_number[player_team] == 0) {
            fprintf(stderr, "number of boids in the player's team (%s) is not set\n", player_team_name);
            return 1;
        }
    }

    if (server == NULL)
        server = "127.0.0.1";
    if (username == NULL)
        username = "noname";
    if (new_room) {
        int teams_number = 0;
        for (int i = 0; i < TEAMS_COUNT; i++) {
            if (boids_number[i] > 0) {
                teams_number++;
                total_boids_number += boids_number[i];
            }
        }

        if (teams_number != players_number) {
            fprintf(stderr, "you have not set the number of boids for all players\n");
            return 1;
        }
        if (total_boids_number > MAX_BOIDS_COUNT) {
            fprintf(stderr, "number of boids (%u) is greater than max boids count (%u)\n", total_boids_number, MAX_BOIDS_COUNT);
            return 1;
        }
        
    }

    printf("server %s:%d\n", server, tcp_port);
    // printf("name: %s\n", username);
    // if (new_room) {
    //     printf("team: %s\n", player_team_name);
        
    //     printf("new room\n red\t: %-4d\n blue\t: %-4d\n green\t: %-4d\n yellow\t: %-4d\n",
    //            boids_number[TEAM_RED],
    //            boids_number[TEAM_BLUE],
    //            boids_number[TEAM_GREEN],
    //            boids_number[TEAM_YELLOW]);
    // } else {
    //     printf("room: %06x\n", room_id);
    // }

    #ifdef _WIN32
        WSADATA ws_data;
        int er_stat = WSAStartup(MAKEWORD(2,2), &ws_data);
        if (er_stat != 0) {
            perror("WSAStartup");
            return 1;
        }
    #endif
    
    // Create a TCP socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    #ifndef _WIN32
        int opt = 1;
    #else
        char opt = 1;
    #endif
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
        perror("setsockopt TCP_NODELAY");
        close(fd);
        #ifdef _WIN32
            WSACleanup();
        #endif
        return 1;
    }

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(tcp_port);
    socklen_t addrlen = sizeof(servaddr);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, server, &servaddr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        close(fd);
        #ifdef _WIN32
            WSACleanup();
        #endif
        return 1;
    }

    int status = connect(fd, (struct sockaddr*)&servaddr, addrlen);
    if (status < 0) {
        perror("connect");
        close(fd);
        #ifdef _WIN32
            WSACleanup();
        #endif
        return 1;
    }

    // Join/new room request
    SPJoined recv_data;
    if (new_room) {
        CPNew data = {.players_number = players_number, .player_team = player_team, .world_size = {htons(world_size.x), htons(world_size.y)}};
        for (int i = 0; i < TEAMS_COUNT; i++)
            data.boids_number[i] = htons(boids_number[i]);
        strncpy(data.creator, username, USERNAME_LEN-1);
        data.creator[USERNAME_LEN-1] = '\0';

        send_packet(fd, CP_NEW_ROOM, &data, sizeof(data), 0);

        uint8_t packet_type;
        recv(fd, &packet_type, 1, 0);
        if (packet_type != SP_JOIN_PLAYER) {
            fputs("[*] unable to create a room\n", stderr);
            return 1;
        }
        
        uint32_t data_len;
        recv_packet(fd, &recv_data, &data_len, 0);
        if (data_len != sizeof(SPJoined) || recv_data.status != JOIN_OK) {
            fputs("[*] unable to create a room\n", stderr);
            return 1;
        }
    } else {
        CPJoin data = {.room_id = htonl(room_id)};
        strncpy(data.username, username, USERNAME_LEN-1);
        data.username[USERNAME_LEN-1] = '\0';

        send_packet(fd, CP_JOIN_ROOM, &data, sizeof(data), 0);

        uint8_t packet_type;
        recv(fd, &packet_type, 1, 0);
        if (packet_type != SP_JOIN_PLAYER) {
            fputs("[*] unable to join to the room\n", stderr);
            return 1;
        }
        
        uint32_t data_len;
        recv_packet(fd, &recv_data, &data_len, 0);
        if (data_len != sizeof(SPJoined) || recv_data.status == JOIN_FAILED) {
            fputs("[*] unable to join to the room\n", stderr);
            return 1;
        }

        if (recv_data.status == JOIN_REJECTED) {
            printf("[*] admin has rejected your joining\n");
            return 1;
        }

        player_team = recv_data.player_team;
        player_team_name = get_team_name(player_team);
    }

    room_id = ntohl(recv_data.room_id);
    players_number = recv_data.players_number;
    uint8_t player_id = ntohl(recv_data.player_id);
    int joined_players = recv_data.joined_players;
    world_size.x = ntohs(recv_data.world_size.x);
    world_size.y = ntohs(recv_data.world_size.y);
    
    ClientPlayer players[TEAMS_COUNT] = { 0 };
    for (int i = 0; i < joined_players; i++) {
        memcpy(&players[i], &recv_data.players[i], sizeof(ClientPlayer));
        players[i].id = ntohl(players[i].id);
    }

    bool team_used[TEAMS_COUNT] = { 0 };

    total_boids_number = 0;
    for (int i = 0; i < TEAMS_COUNT; i++) {
        BoidIndex boids = htons(recv_data.teams[i]);
        boids_number[i] = boids;
        total_boids_number += boids;

        team_used[i] = boids > 0;
    }

    // Init log
    Log log;
    init_cstack(log, MAX_LOG_LEN);

    write_log(&log, "[*] %s\n    id: %06x\n    teams: %d\n    world: %dx%d\n    creator: %s (id=%d)\n    boids:  %-4d\n    red:    %-4d\n    blue:   %-4d\n    green:  %-4d\n    yellow: %-4d\n",
           new_room? "created a room" : "joined to the room",
           room_id, players_number, world_size.x, world_size.y, players[0].name, players[0].id, total_boids_number,
           boids_number[TEAM_RED],
           boids_number[TEAM_BLUE],
           boids_number[TEAM_GREEN],
           boids_number[TEAM_YELLOW]);

    // Init Raylib
    SetTraceLogLevel(LOG_WARNING);
    #ifdef _WIN32
        SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_BORDERLESS_WINDOWED_MODE);
        InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Battle creator");
        ToggleFullscreen();

        int monitor = GetCurrentMonitor();
        SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
    #else
        SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_FULLSCREEN_MODE);
        InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Battle creator");
    #endif
    SetTargetFPS(60);

    // Control
    GameMode mode = new_room ? MODE_AREAS : MODE_WAIT;
    GameStage stage = STAGE_AREAS;
    bool show_log = true, show_grid = false, delete_boids = false;
    int brush_size = 1;
    
    // Selection
    int selecting_team = TEAM_RED;
    bool selecting = false, select_mode = false, selecting_shift_pressed = false;
    Vector2 selection_start = { 0 };
    
    // Camera
    Camera2D camera = { 0 };
    camera.zoom = 1.0f;
    camera.target = (Vector2){world_size.x/2.0 - GetScreenWidth()/2.0, world_size.y/2.0 - GetScreenHeight()/2.0};

    // Keyboard string input
    bool typing_string_input = false;
    int input_len = 0;

    // Areas selecting
    Point area_start_selecting, area_end_selecting;
    Area areas[MAX_AREAS_COUNT] = { 0 };
    int areas_size[TEAMS_COUNT] = { 0 };
    int16_t areas_count = 0;

    // Boids
    ClientBoid *boids = calloc(total_boids_number, sizeof(*boids));
    BoidIndex boids_count = 0; // Number of placed boids

    // Grid of chunks
    Grid grid = { 0 };
    init_grid(&grid, boids, boids_count, world_size.x, world_size.y);

    // Textures
    Texture2D texture = LoadTexture("../resources/texture.png");
    GenTextureMipmaps(&texture);
    // SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    // Start a thread to recive messages from the server
    bool running = true;
    pthread_mutex_init(&log_mtx, NULL);
    pthread_mutex_init(&input_mtx, NULL);
    pthread_cond_init(&input_cond, NULL);
    pthread_mutex_init(&boids_mtx, NULL);
    NetThreadArgs thread_args = {.fd = fd, .players_number = players_number, .joined_players = &joined_players, .player_team = player_team, .new_room = new_room,
                                 .running=&running, .room_id = room_id, .world_size = &world_size, .boids_number = boids_number, .boids_count = &boids_count,
                                 .total_boids_number = total_boids_number, .players = players, .log = &log, .areas_count = &areas_count,
                                 .areas = areas, boids = boids, .mode = &mode, .stage = &stage};
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, net_thread_fn, &thread_args);
    
    if (!new_room) {
        write_log(&log, "[*] players:\n");
        for (int i = 0; i < joined_players; i++) {
            ClientPlayer *op = &players[i];

            char *team = get_team_name(op->team);

            write_log(&log, "    %s (id=%d) - %s\n", op->name, op->id, team);
        }
    }
    
    while (!WindowShouldClose()) {
        pthread_mutex_lock(&running_mtx);
        if (!running) break;
        pthread_mutex_unlock(&running_mtx);
        
        // Keys
        if (!show_log || !get_input) {
            if (IsKeyPressed(KEY_L)) show_log = !show_log;
            if (IsKeyPressed(KEY_K)) show_grid = !show_grid;
        
            if (stage == STAGE_PLACING) {
                if (IsKeyPressed(KEY_A)) mode = MODE_SPAWN, select_mode = false;
                if (IsKeyPressed(KEY_S)) mode = MODE_SELECT, select_mode = true, selecting = false, selecting_shift_pressed = false;
                if (IsKeyPressed(KEY_D)) mode = MODE_DELETE, select_mode = false;
                if (IsKeyPressed(KEY_X)) delete_boids = true;
            }

            if (IsKeyPressed(KEY_P)) brush_size += MAX(1, log10f(brush_size));
            if (IsKeyPressed(KEY_O) && brush_size > 1) brush_size -= MAX(1, log10f(brush_size));
        }

        Vector2 mouse_position = GetScreenToWorld2D(GetMousePosition(), camera);
        int screen_width = GetScreenWidth();
        int screen_height = GetScreenHeight();

        // Camera
        float wheel = GetMouseWheelMove();
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) wheel = -1.0;
        if (IsKeyPressed(KEY_KP_ADD) || (IsKeyPressed(KEY_EQUAL) && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))) wheel = 1.0;
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

        // Get string input from keyboard
        if (show_log && get_input) {
            if (!typing_string_input) {
                // Input starts with SPACE
                if (IsKeyPressed(KEY_SPACE)) {
                    typing_string_input = true;
                    input_len = 0;
                }
            } else {
                // Input ends with ENTER
                if (IsKeyPressed(KEY_ENTER)) {
                    typing_string_input = false;
                    write_log(&log, input_string);
                    pthread_mutex_lock(&input_mtx);
                    get_input = false;
                    pthread_cond_signal(&input_cond);
                    pthread_mutex_unlock(&input_mtx);
                } else {
                    int key = GetCharPressed();
                    while (key > 0) {
                        // Only allow keys in range [32..125]
                        if ((key >= 32) && (key <= 125) && (input_len < INPUT_STRING_LEN)) {
                            input_string[input_len++] = (char)key;
                            input_string[input_len] = '\0';
                        }
                        key = GetCharPressed();
                    }
                    if (IsKeyPressed(KEY_BACKSPACE)) {
                        input_len--;
                        if (input_len < 0) input_len = 0;
                        input_string[input_len] = '\0';
                    }
                }
            }
        }
        
        // Areas mode
        if (mode == MODE_AREAS) {
            // Selecting
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && areas_count < MAX_AREAS_COUNT - 3) {
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
            } else if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
                // Create new area
                
                selecting = false;
                Rec area = {MIN(area_start_selecting.x, area_end_selecting.x), MIN(area_start_selecting.y, area_end_selecting.y),
                            MAX(area_start_selecting.x, area_end_selecting.x), MAX(area_start_selecting.y, area_end_selecting.y)};

                // If new area is not aempty
                if (area.x1 != area.x2 && area.y1 != area.y2) {
                    pthread_mutex_lock(&areas_mtx);
                    bool insert_area = true;
                    int orig_areas_count = areas_count;
                    // Compare a new area with exeisting ones
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

                            // Divide the old area into nonoverlapping new ones
                            // TODO: optimize it
                            if (intersection.y1 == r.y1 || intersection.y2 == r.y2) {
                                if (intersection.x1 != r.x1) areas[areas_count++] = (Area){.rec = {r.x1, r.y1, intersection.x1, r.y2}, .team = rt};
                                if (intersection.y2 != r.y2 && intersection.y1 == r.y1) areas[areas_count++] = (Area){.rec = {intersection.x1, intersection.y2, intersection.x2, r.y2}, .team = rt};
                                else if (intersection.y1 != r.y1 && intersection.y2 == r.y2) areas[areas_count++] = (Area){.rec = {intersection.x1, r.y1, intersection.x2, intersection.y1}, .team = rt};
                                if (intersection.x2 != r.x2) areas[areas_count++] = (Area){.rec = {intersection.x2, r.y1, r.x2, r.y2}, .team = rt};
                            } else if (intersection.x1 == r.x1 || intersection.x2 == r.x2) {
                                areas[areas_count++] = (Area){.rec = {r.x1, r.y1, r.x2, intersection.y1}, .team = rt};
                                if (intersection.x1 == r.x1 && intersection.x2 != r.x2) areas[areas_count++] = (Area){.rec = {intersection.x2, intersection.y1, r.x2, intersection.y2}, .team = rt};
                                else if (intersection.x1 != r.x1 && intersection.x2 == r.x2) areas[areas_count++] = (Area){.rec = {r.x1, intersection.y1, intersection.x1, intersection.y2}, .team = rt};
                                areas[areas_count++] = (Area){.rec = {r.x1, intersection.y2, r.x2, r.y2}, .team = rt};
                            } else {
                                areas[areas_count++] = (Area){.rec = {r.x1, r.y1, r.x2, intersection.y1}, .team = rt};
                                areas[areas_count++] = (Area){.rec = {r.x1, intersection.y1, intersection.x1, intersection.y2}, .team = rt};
                                areas[areas_count++] = (Area){.rec = {intersection.x2, intersection.y1, r.x2, intersection.y2}, .team = rt};
                                areas[areas_count++] = (Area){.rec = {r.x1, intersection.y2, r.x2, r.y2}, .team = rt};
                            }

                            // Subtract the intersecting part from total areas size
                            areas_size[rt] -= (intersection.x2 - intersection.x1) * (intersection.y2 - intersection.y1);
                        }
                    }
                    // Add new area
                    if (selecting_team >= 0 && insert_area) {
                        areas[areas_count++] = (Area){.rec = area, .team = selecting_team};
                        areas_size[selecting_team] += (area.x2 - area.x1) * (area.y2 - area.y1);
                    }
                    pthread_mutex_unlock(&areas_mtx);
                }
            }

            if (!show_log || !get_input) {
                if (IsKeyPressed(KEY_Q)) selecting_team = TEAM_RED;
                if (IsKeyPressed(KEY_W)) selecting_team = TEAM_BLUE;
                if (IsKeyPressed(KEY_E)) selecting_team = TEAM_GREEN;
                if (IsKeyPressed(KEY_R)) selecting_team = TEAM_YELLOW;
                if (IsKeyPressed(KEY_Z)) selecting_team = -1;
            }

            // Start placing
            if (IsKeyPressed(KEY_ENTER) && new_room && players_number == joined_players) {
                bool ok = true;
                for (int team = 0; team < TEAMS_COUNT; team++) {
                    if (team_used[team] && boids_number[team] > areas_size[team]) {
                        ok = false;
                        break;
                    }
                }

                // Send data to the server only if size of areas of all teams is greater than or equal to the number of boids
                if (ok) {
                    uint32_t buf_size = areas_count * sizeof(*areas) + sizeof(areas_count);
                    void *buf = malloc(buf_size);

                    pthread_mutex_lock(&areas_mtx);
                    *(uint16_t*)buf = htons(areas_count);
                    Area *a = buf + sizeof(areas_count);
                    for (int i = 0; i < areas_count; i++) {
                        *a = areas[i];
                        a->rec.x1 = htons(a->rec.x1);
                        a->rec.x2 = htons(a->rec.x2);
                        a->rec.y1 = htons(a->rec.y1);
                        a->rec.y2 = htons(a->rec.y2);
                        a++;
                    }
                    pthread_mutex_unlock(&areas_mtx);
                
                    send_packet(fd, CP_START_PLACING, buf, buf_size, 0);

                    free(buf);
                } else {
                    write_log(&log, "[!] you cannot atart placing if size of areas of all teams is less than the number of boids\n");
                }
            }
        }

        // Spawn boids
        BoidIndex deleted_boids_count = 0;
        static SPoint spawn_prev_pos = {-1, -1};
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
                        CELL_UNCHEKED,
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
                            if (area != NULL && area->team == player_team &&
                                cell_x >= area->rec.x1 && cell_x < area->rec.x2 && cell_y >= area->rec.y1 && cell_y < area->rec.y2) {
                                can_place = true;
                            } else {
                                for (int i = 0; i < areas_count; i++) {
                                    Area *area = &areas[i];
                                    if (area->team == player_team &&
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
                                } else if (cell_status == CELL_UNCHEKED) {
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
                                            if (cells[check_cell_idx] == CELL_UNCHEKED)
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
                }
            }

            // Delete boids
            static SPoint delete_prev_pos = {-1, -1};
            if (mode == MODE_DELETE && IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && boids_count > 0 &&
                mouse_position.x + (int)(brush_size - brush_size/2) * BOID_SIZE >= 0 &&
                mouse_position.x - (int)(brush_size/2) * BOID_SIZE <= world_size.x &&
                mouse_position.y + (int)(brush_size - brush_size/2) * BOID_SIZE >= 0 &&
                mouse_position.y - (int)(brush_size/2) * BOID_SIZE <= world_size.y) {
                
                SPoint pos = {(int)mouse_position.x/BOID_SIZE, (int)mouse_position.y/BOID_SIZE};
                
                if (pos.x != spawn_prev_pos.x || pos.y != spawn_prev_pos.y) {
                    SRec brush_rec = {pos.x - brush_size/2, pos.y - brush_size/2,
                                     pos.x - brush_size/2 + brush_size, pos.y - brush_size/2 + brush_size};

                    int *used_chunks = calloc(pow(ceilf((float)brush_size*BOID_SIZE/CHUNK_SIZE_PIXELS) + 1, 2), sizeof(*used_chunks));
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

                    delete_prev_pos = pos;
                }
            }

            
            // Send boids and a message that the player is ready to start the game
            if (IsKeyPressed(KEY_ENTER)) {
                if (boids_count == boids_number[player_team]) {
                    ClientStartNetBoids *data = calloc(boids_count + 1, sizeof(*data));
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
                                data[++index] = (ClientStartNetBoids){.team = find ? player_team : -1, .count = 1};
                        }
                    }

                    uint16_t count = index + 1;

                    for (int i = 0; i < count; i++) {
                        data[i].count = htons(data[i].count);
                    }
                    
                    uint32_t bufsize = sizeof(count) + count * sizeof(*data);
                    void *buf = malloc(bufsize);

                    uint16_t ncount = htons(count);
                    memcpy(buf, &ncount, sizeof(count));
                    memcpy(buf+sizeof(count), data, count * sizeof(*data));

                    send_packet(fd, CP_SEND_BOIDS, buf, bufsize, 0);
                    write_log(&log, "[*] wait until other players are ready to start the game\n");

                    free(data);
                    free(buf);
                } else {
                    write_log(&log, "[!] place all your boids before you start the game\n");
                }
            }
        }

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
                if (boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL && boid->b.action != ACT_DELETE) {
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
                        }
                    }
                    
                    selected_boids_count += boid->is_selected;
                }
            }

            pthread_mutex_unlock(&boids_mtx);
            
            delete_boids = false;
        }
        
        // Remove deleted boids from array
        pthread_mutex_lock(&boids_mtx);
        if (deleted_boids_count > 0) {
            int offset = 0;
            for (int i = 0; i < boids_count; i++) {
                boids[i - offset] = boids[i];
                ClientBoid *boid = &boids[i];
                if (boid->b.action == ACT_DELETE)
                    offset++;
            }
            boids_count -= offset;
        }
        pthread_mutex_unlock(&boids_mtx);
        
        // Update boids
        /*for (BoidIndex i = 0; i < boids_count; i++) {
            ClientBoid *boid = &boids[i];

            if (boid->b.action == ACT_DELETE) continue;

            // update the boid and change its ccordinates
        }*/

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
            if (mode == MODE_AREAS || stage == STAGE_PLACING) {
                pthread_mutex_lock(&areas_mtx);
                for (int i = 0; i < areas_count; i++) {
                    Area area = areas[i];
                    Color color = RAYWHITE;
                    switch (area.team) {
                        case TEAM_RED: color = (Color){RED.r, RED.g, RED.b, (stage == STAGE_AREAS)? 255: 20}; break;
                        case TEAM_BLUE: color = (Color){BLUE.r, BLUE.g, BLUE.b, (stage == STAGE_AREAS)? 255: 20}; break;
                        case TEAM_GREEN: color = (Color){GREEN.r, GREEN.g, GREEN.b, (stage == STAGE_AREAS)? 255: 20}; break;
                        case TEAM_YELLOW: color = (Color){ORANGE.r, ORANGE.g, ORANGE.b, (stage == STAGE_AREAS)? 255: 20}; break;
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

            // Draw selection
            for (BoidIndex i = 0; i < boids_count; i++) {
                ClientBoid *boid = &boids[i];
                if ((boid->sprite != SPRITE_FALL) && (boid->is_selected)) {
                    draw_selection(boid, texture);
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
                    //     DrawText(TextFormat("%d", boid->b.health), boid->b.pos.x- 5 - ((int)log10(boid->b.health) * 7), boid->b.pos.y - 50, 20, BLACK);
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

            // Drawing boids slection
            if ((mode == MODE_SELECT) && selecting) {
                float rectangleX = fmin(selection_start.x, mouse_position.x);
                float rectangleY = fmin(selection_start.y, mouse_position.y);
                DrawText(TextFormat("%d", selected_boids_count), rectangleX, rectangleY-(20/camera.zoom), 20/camera.zoom, BLACK);
                DrawRectangleLinesEx((Rectangle){rectangleX, rectangleY,
                                     fabs(mouse_position.x - selection_start.x), fabs(mouse_position.y - selection_start.y)},
                                 thick, BLACK);
            }

            // Draw brush
            if (mode == MODE_SPAWN || mode == MODE_DELETE) {
                SPoint pos = {(int)mouse_position.x/BOID_SIZE, (int)mouse_position.y/BOID_SIZE};
                DrawRectangle(pos.x*BOID_SIZE - brush_size/2*BOID_SIZE, pos.y*BOID_SIZE - brush_size/2*BOID_SIZE,
                              brush_size*BOID_SIZE, brush_size*BOID_SIZE, (Color){20, 20, 20, 20});
            }
            
            
            EndMode2D();

            // Draw "Paused" and "Mode" labels
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
            // if (pause)
            //     DrawText("Paused", screen_width - 85, 40, 20, BLACK);
            

            // Draw log
            if (show_log) {
                int line = typing_string_input, idx = log.front;
                pthread_mutex_lock(&log_mtx);
                for (int i = 0; i < log.size; i++) {
                    line += log.items[idx].lines;
                    DrawText(log.items[idx].string, 10, screen_height - line*22 - 5, 20, BLACK);
                    idx = (idx > 0)? (idx - 1) : log.max_len-1;
                }
                pthread_mutex_unlock(&log_mtx);
            }

            // Draw keyboard input
            if (typing_string_input) {
                DrawText(input_string, 10, screen_height - 22 - 5, 20, BLACK);
            }

            // Draw boids number
            int l = 0; // line
            for (int team = 0; team < TEAMS_COUNT; team++) {
                if (!team_used[team]) continue;
                
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

                char *str;
                if (stage == STAGE_AREAS) {
                    // <player_name>: <target_boids_number> (<areas_size><! if boids_count less than areas_size>)
                    str = (char*)TextFormat("%s: %d%c(%d%s", player_joined ? players[player_idx].name : "-", boids_number[team], new_room ? ' ' : '\0',
                                            areas_size[team], (boids_number[team] < areas_size[team])? ")" : "!)");
                } else if (stage == STAGE_PLACING) {
                    if (team == player_team) {
                        str = (char*)TextFormat("%s: %d (%d left)", players[player_idx].name, boids_count, boids_number[team]-boids_count);
                    } else {
                        str = (char*)TextFormat("%s: -", players[player_idx].name);
                    }
                } else if (stage == STAGE_GAME) {
                    str = (char*)TextFormat("%s: %d", players[player_idx].name, boids_number[team]);
                }
                DrawText(str, 10, 40 + (l++)*20, 20, team_color);
            }

            DrawFPS(10, 10);
        EndDrawing();
    }

    // Close input
    if (get_input) {
        pthread_mutex_lock(&input_mtx);
        get_input = false;
        pthread_cond_signal(&input_cond);
        pthread_mutex_unlock(&input_mtx);
        
    }
    
    // Close the second (network) thread
    pthread_cancel(net_thread);
    pthread_join(net_thread, NULL);
    
    // Close soketd
    close(fd);
    #ifdef _WIN32
        WSACleanup();
    #endif

    // Destroy all mutexes and conditions
    pthread_mutex_destroy(&input_mtx);
    pthread_cond_destroy(&input_cond);
    pthread_mutex_destroy(&log_mtx);
    pthread_mutex_destroy(&areas_mtx);
    pthread_mutex_destroy(&running_mtx);
    pthread_mutex_destroy(&boids_mtx);

    // Free allocated memory
    free(log.items);
    free(grid.chunks);
    free(boids);

    // Close Raylib
    UnloadTexture(texture);
    CloseWindow();

    return 0;
}
