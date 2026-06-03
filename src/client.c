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

#define MIN(x, y) ((x) < (y)) ? (x) : (y)
#define MAX(x, y) ((x) > (y)) ? (x) : (y)


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
    MODE_SELECT,
    MODE_DIRECTION,
    MODE_POINT,
    MODE_LINE
} GameMode;

typedef struct {
    int fd, players_number, *joined_players, player_team;
    bool new_room, *ext;
    uint32_t room_id;
    Point *world_size;
    BoidIndex *boids_number;
    ClientPlayer *players;
    Log *log;
    int16_t *areas_count;
    Area *areas;
    GameMode *mode;
} NetThreadArgs;

pthread_mutex_t areas_mtx;

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
    ClientPlayer *players = nargs->players;
    Log *log = nargs->log;
    bool *ext = nargs->ext;
    Area *areas = nargs->areas;
    int16_t *areas_count = nargs->areas_count;
    GameMode *mode = nargs->mode;

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
            if (new_areas_count < 0) break;

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

            write_log(log, "[*] now you can spawn boids on your areas");
            
            break;
            }
        }

        if (ex)
            break;
    }
    
    *ext = true;
    return NULL;
}


/* <================================================= MAIN =================================================> */

#define MAX_AREAS_COUNT 1024

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
            if (argc == 1) {
                fprintf(stderr, "no value for option '%s'\n", arg);
                return 1;
            }
            if (strcmp(arg, "--server") == 0 || strcmp(arg, "-s") == 0) {
                server = *(++argv);
                argc--;
            } else if (strcmp(arg, "--name") == 0 || strcmp(arg, "-n") == 0) {
                username = *(++argv);
                argc--;
            } else if (new_room) {
                if (strcmp(arg, "--players") == 0 || strcmp(arg, "-p") == 0) {
                    char *value_str = *(++argv);
                    argc--;
                
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
                    fprintf(stderr, "unexpected argument '%s'\n", argv[1]);
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
                    fprintf(stderr, "unexpected argument '%s'\n", argv[1]);
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

            if (err) {
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
            fprintf(stderr, "unexpected argument '%s'\n", argv[1]);
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
            fprintf(stderr, "number of players (%d) is not equal to number of teams (%d)\n", players_number, teams_number);
            return 1;
        }
        if (total_boids_number > MAX_BOIDS_COUNT) {
            fprintf(stderr, "number of boids (%u) is greater than max boids count (%u)\n", total_boids_number, MAX_BOIDS_COUNT);
            return 1;
        }
        
    }

    printf("server: %s\n", server);
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
    servaddr.sin_port = htons(INPUT_PORT);
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
    bool pause = false;
    
    // Camera
    Camera2D camera = { 0 };
    camera.zoom = 1.0f;
    camera.target = (Vector2){world_size.x/2.0, world_size.y/2.0};

    // Keyboard string input
    bool typing_string_input = false;
    int input_len = 0;

    // Areas selecting
    int selecting_team = TEAM_RED;
    bool selecting = false;
    Point area_start_selecting, area_end_selecting;
    Area areas[MAX_AREAS_COUNT] = { 0 };
    int areas_size[TEAMS_COUNT] = { 0 };
    int16_t areas_count = 0;

    // Start a thread to recive messages from the server
    bool ext = false; // Exit game
    pthread_mutex_init(&log_mtx, NULL);
    pthread_mutex_init(&input_mtx, NULL);
    pthread_cond_init(&input_cond, NULL);
    NetThreadArgs thread_args = {.fd = fd, .players_number = players_number, .joined_players = &joined_players, .player_team = player_team, .new_room = new_room,
                                 .ext=&ext, .room_id = room_id, .world_size = &world_size, .boids_number = boids_number, .players = players, .log = &log,
                                 .areas_count = &areas_count, .areas = areas, .mode = &mode};
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, net_thread_fn, &thread_args);
    pthread_detach(net_thread);
    
    if (!new_room) {
        write_log(&log, "[*] players:\n");
        for (int i = 0; i < joined_players; i++) {
            ClientPlayer *op = &players[i];

            char *team = get_team_name(op->team);

            write_log(&log, "    %s (id=%d) - %s\n", op->name, op->id, team);
        }
    }
    
    while (!WindowShouldClose() && !ext) {
        // Keys
        if (IsKeyPressed(KEY_P)) pause = !pause;

        Vector2 mouse_position = GetScreenToWorld2D(GetMousePosition(), camera);
        int screen_width = GetScreenWidth();
        int screen_height = GetScreenHeight();

        // Camera
        float wheel = GetMouseWheelMove();
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) wheel = -1.0;
        if (IsKeyPressed(KEY_KP_ADD) || (IsKeyPressed(KEY_EQUAL) && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))) wheel = 1.0;
        if (wheel != 0) {
            camera.offset = GetMousePosition();
            camera.target = mouse_position;
            float scale = 0.2f*wheel;
            camera.zoom = Clamp(expf(logf(camera.zoom)+scale), 0.125f, 64.0f);
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 delta = GetMouseDelta();
            delta = Vector2Scale(delta, -1.0f/camera.zoom);
            camera.target = Vector2Add(camera.target, delta);
        }

        // Get string input from keyboard
        if (get_input) {
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

            if (IsKeyPressed(KEY_Q)) selecting_team = TEAM_RED;
            if (IsKeyPressed(KEY_W)) selecting_team = TEAM_BLUE;
            if (IsKeyPressed(KEY_E)) selecting_team = TEAM_GREEN;
            if (IsKeyPressed(KEY_R)) selecting_team = TEAM_YELLOW;
            if (IsKeyPressed(KEY_Z)) selecting_team = -1;

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
                } else {
                    write_log(&log, "[!] you cannot atart placing if size of areas of all teams is less than the number of boids\n");
                }
            }
        }
        
        // Drawing
        BeginDrawing();
            // ClearBackground((Color){255, 235, 206, 255});
            ClearBackground(RAYWHITE);

            BeginMode2D(camera);
            
            // Word border
            DrawRectangleLines(0, 0, world_size.x, world_size.y, BLACK);

            float thick = 4/camera.zoom;
            
            // Draw areas
            if (mode == MODE_AREAS || mode == MODE_SPAWN) {
                pthread_mutex_lock(&areas_mtx);
                for (int i = 0; i < areas_count; i++) {
                    Area area = areas[i];
                    Color color = RAYWHITE;
                    switch (area.team) {
                        case TEAM_RED: color = RED; break;
                        case TEAM_BLUE: color = BLUE; break;
                        case TEAM_GREEN: color = GREEN; break;
                        case TEAM_YELLOW: color = ORANGE; break;
                    }
                    DrawRectangle(area.rec.x1 * BOID_SIZE, area.rec.y1 * BOID_SIZE, (area.rec.x2 - area.rec.x1) * BOID_SIZE, (area.rec.y2 - area.rec.y1) * BOID_SIZE, color);
                    DrawRectangleLines(area.rec.x1 * BOID_SIZE, area.rec.y1 * BOID_SIZE, (area.rec.x2 - area.rec.x1) * BOID_SIZE, (area.rec.y2 - area.rec.y1) * BOID_SIZE, BLACK);
                }
                pthread_mutex_unlock(&areas_mtx);
            }

            // Draw area selecting
            if (mode == MODE_AREAS && selecting) {
                float rectangleX = fmin(area_start_selecting.x * BOID_SIZE, area_end_selecting.x * BOID_SIZE);
                float rectangleY = fmin(area_start_selecting.y * BOID_SIZE, area_end_selecting.y * BOID_SIZE);
                DrawRectangleLinesEx((Rectangle){rectangleX, rectangleY,
                                     abs(area_start_selecting.x * BOID_SIZE - area_end_selecting.x * BOID_SIZE),
                                     abs(area_start_selecting.y * BOID_SIZE - area_end_selecting.y * BOID_SIZE)},
                                     thick, BLACK);
            }
            
            EndMode2D();

            // Draw "Paused" and "Mode" labels
            switch (mode) {
            case MODE_WAIT:
                DrawText("Mode: Wait", screen_width - 115, 10, 20, BLACK);
                break;
            case MODE_AREAS:
                DrawText("Mode: Areas", screen_width - 140, 10, 20, BLACK);
                break;
            case MODE_SPAWN:
                DrawText("Mode: Spawn", screen_width - 140, 10, 20, BLACK);
                break;
            case MODE_SELECT:
                DrawText("Mode: Select", screen_width - 140, 10, 20, BLACK);
                break;
            case MODE_DIRECTION:
                DrawText("Mode: Direction", screen_width - 165, 10, 20, BLACK);
                break;
            case MODE_POINT:
                DrawText("Mode: Point", screen_width - 125, 10, 20, BLACK);
                break;
            case MODE_LINE:
                DrawText("Mode: Line", screen_width - 115, 10, 20, BLACK);
            }
            if (pause)
                DrawText("Paused", screen_width - 85, 40, 20, BLACK);
            

            // Draw log
            int line = typing_string_input, idx = log.front;
            pthread_mutex_lock(&log_mtx);
            for (int i = 0; i < log.size; i++) {
                line += log.items[idx].lines;
                DrawText(log.items[idx].string, 10, screen_height - line*22 - 5, 20, BLACK);
                idx = (idx > 0)? (idx - 1) : log.max_len-1;
            }
            pthread_mutex_unlock(&log_mtx);

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

                // <player_name>: <boids_count> (<areas_size><! if boids_count less than areas_size>)
                DrawText(TextFormat("%s: %d (%d%s", player_joined ? players[player_idx].name : "-", boids_number[team], areas_size[team],
                                    (boids_number[team] < areas_size[team])? ")" : "!)"), 10, 40 + (l++)*20, 20, team_color);
            }

            DrawFPS(10, 10);
        EndDrawing();
    }

    pthread_mutex_destroy(&log_mtx);
    pthread_mutex_destroy(&areas_mtx);

    if (get_input) {
        pthread_mutex_lock(&input_mtx);
        get_input = false;
        pthread_cond_signal(&input_cond);
        pthread_mutex_unlock(&input_mtx);
        
    }
    pthread_mutex_destroy(&input_mtx);
    pthread_cond_destroy(&input_cond);

    free(log.items);

    close(fd);
    #ifdef _WIN32
        WSACleanup();
    #endif
    CloseWindow();

    return 0;
}
