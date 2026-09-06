#ifndef _WIN32
    #include <sys/socket.h>
    #include <sys/select.h>
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
#define RAYGUI_IMPLEMENTATION
// #define RAYGUI_DEBUG_TEXT_BOUNDS
#include <raygui.h>

#include "boids.h"
#include "kdtree.h"
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

#define STYLE_START(control, property, value)                                                        \
    do {                                                                                             \
        int __old_gui_value = GuiGetStyle(control, property);                                        \
        int __gui_style_control = control;                                                           \
        int __gui_style_property = property;                                                         \
        GuiSetStyle(control, property, value)

#define STYLE_END()                                                                                  \
        GuiSetStyle(__gui_style_control, __gui_style_property, __old_gui_value);                     \
    } while (0)

/* <===================================================== LOGGING ====================================================> */

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


/* <==================================================== SOMETHING ===================================================> */

const char *get_team_name(int team) {
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


/* <================================================= NETWORK THREAD =================================================> */

#define INPUT_STRING_LEN 1024

typedef enum {
    MODE_NULL = 0,
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
    MENU_EXIT = -1,
    MENU_NULL = 0,
    MENU_MAIN,
    MENU_NEW,
    MENU_JOIN,
    MENU_LOCAL,
    MENU_LOADING,
    MENU_GAME
} GameMenu;

typedef enum {
    MESSAGE_INFO,
    MESSAGE_WARNING,
    MESSAGE_ERROR
} MenuMessageType;

typedef struct {
    // String input
    bool get_input, input_received, typing_keyboard_input;
    char input_string[INPUT_STRING_LEN];
    
    // Message in menu
    const char *message_text;
    MenuMessageType message_type;
    bool save_message; // Save the message when moving to the next menu
    
    // Network
    int tcp_fd, udp_fd;
    struct sockaddr_in udp_servaddr;
    bool udp_opened;

    // Global game
    bool running, game_initialized, net_thread_running, local_game, run_game, reset_game;
    BoidIndex boids_number[TEAMS_COUNT], total_boids_number;
    int screen_width, screen_height;
    struct {
        int x, y;
    } world_size;
    RoomStage stage;
    GameMode mode;
    GameMenu menu;
    Log log;

    // Boids
    ClientBoid *boids; // ClientBoid = BaseBoids + VisualizationBoidsPart
    OrderBoidPart *order_parts; // Additional OrderBoidPart for boids from ClientBoid array, needed in the local mode
    BoidIndex boids_count; // Number of placed boids
    Grid grid;
    int chunk_size;
    int chunk_size_local;
    int chunk_size_multiplayer;

    // Multiplayer game
    int players_number, joined_players;
    ClientPlayer players[TEAMS_COUNT];
    bool teams_used[TEAMS_COUNT];
    uint32_t room_id, player_id;
    bool new_room, hide_areas;
    BoidTeam player_team;
    uint32_t approved_player_id;
    char approved_player_username[USERNAME_LEN], username[USERNAME_LEN], server[INET_ADDRSTRLEN];
    int tcp_port, udp_port;

    // Game control
    bool show_log, show_grid, show_health, game_paused, show_arrow, autoselect_mode, show_gui;
    bool is_dragging_border, change_boids_direction, exit_game_message;
    int brush_size;
    BoidAction action;
    Camera2D camera;
    Vector2 mouse_position, mouse_delta;
    float mouse_wheel;
    unsigned long long events, gui_events;
    unsigned short modifiers;
    bool show_line;
    
    // Changes from the second (network) thread
    GameMenu next_menu;
    bool change_menu;

    // Selection
    int selecting_team;
    bool selecting, select_mode,
         clear_order, change_boids_action, change_selection_team;
    Vector2 selection_start;

    // Areas
    Area areas[MAX_AREAS_COUNT];
    uint16_t areas_count;

    // Server TPS
    enum {
        TPS_NUM = 0,
        TPS_PERCENT,
        TPS_HIDE,
        TPS_DISPLAY_TYPES_NUMBER
    } tps_display_type;
    int server_tps, server_target_tps;
} GameContext;

GameContext ctx = {.running = true, .stage = STAGE_AREAS, .mode = MODE_WAIT, .chunk_size = DEFAULT_CLIENT_CHUNK_SIZE_PIXELS,
                   .chunk_size_local = DEFAULT_SERVER_CHUNK_SIZE_PIXELS, .chunk_size_multiplayer = DEFAULT_CLIENT_CHUNK_SIZE_PIXELS,
                   .players_number = DEFAULT_PLAYERS_COUNT, .world_size = {DEFAULT_WORLD_SIZE_X, DEFAULT_WORLD_SIZE_Y},
                   .username = DEFAULT_USERNAME, .server = DEFAULT_SERVER, .tcp_port = TCP_PORT, .udp_port = UDP_PORT,
                   .show_log = true, .autoselect_mode = true, .show_gui = true, .brush_size = 1, .action = ACT_STOP,
                   .selecting_team = TEAM_RED, .tps_display_type = TPS_NUM};

// The message will disappear after the first moving to next menu if save_message is not true
void set_menu_message(MenuMessageType type, bool save_message, const char *msg) {
    if (ctx.message_text != NULL && ctx.message_type > type)
        return;
    ctx.message_type = type;
    ctx.save_message = save_message;
    ctx.message_text = msg;
}

void clear_menu_message(void) {
    ctx.message_text = NULL;
}

pthread_t net_thread;
pthread_mutex_t areas_mtx;
pthread_mutex_t boids_mtx;
pthread_mutex_t running_mtx;
pthread_mutex_t next_menu_mtx;
pthread_mutex_t input_mtx;
pthread_mutex_t players_mtx;

#define CHECK_LEAST_SIZE(size)                                                                                              \
    do {                                                                                                                    \
        if (packet_size < (uint32_t)(size)) {                                                                               \
            log_message(&ctx.log, L_ERROR, "invalid SP#%d packet length (expected at least %u bytes, %u bytes received)\n", \
                        packet_type, (uint32_t)(size), packet_size);                                                        \
            return 1;                                                                                                       \
        }                                                                                                                   \
    } while (0)

#define CHECK_SIZE(size)                                                                                                    \
    do {                                                                                                                    \
        if (packet_size != (uint32_t)(size)) {                                                                              \
            log_message(&ctx.log, L_ERROR, "invalid SP#%d packet length (expected %u bytes, %u bytes received)\n",          \
                        packet_type, (uint32_t)(size), packet_size);                                                        \
            return 1;                                                                                                       \
        }                                                                                                                   \
    } while (0)

#define CHECK_FIELD(expr)                                                                                                   \
    do {                                                                                                                    \
        if (!(expr)) {                                                                                                      \
            log_message(&ctx.log, L_ERROR, "invalid SP#%d packet: field check failed\n", packet_type);                      \
            pthread_mutex_unlock(&areas_mtx);                                                                               \
            pthread_mutex_unlock(&boids_mtx);                                                                               \
            pthread_mutex_unlock(&running_mtx);                                                                             \
            pthread_mutex_unlock(&next_menu_mtx);                                                                           \
            pthread_mutex_unlock(&input_mtx);                                                                               \
            pthread_mutex_unlock(&players_mtx);                                                                             \
            return 1;                                                                                                       \
        }                                                                                                                   \
    } while (0)

int process_data(uint8_t packet_type, uint32_t packet_size, char *packet_data) {
    char *d = packet_data;
    
    switch (packet_type) {
    case SP_JOIN_PLAYER: { // This player has joined (or not) to the room
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

        CHECK_LEAST_SIZE( 1 );
        
        uint8_t join_status = POP_DATA(d, uint8_t);
        CHECK_FIELD(join_status < JOIN_STATUSES_COUNT);
       
        if (join_status == JOIN_OK)
            CHECK_LEAST_SIZE( 1 + 4 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2*TEAMS_COUNT + (4 + 1 + 1 + USERNAME_LEN)*1 );
        
        if (ctx.new_room) {
            if (join_status != JOIN_OK) {
                write_log(L_INFO, "unable to create a room\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_ERROR, true, "Unable to create a room");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);
                
                return 1;
            }
        } else {
            if (join_status == JOIN_REJECTED) {
                write_log(L_INFO, "admin has rejected your joining\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_INFO, true, "Admin has rejected your joining");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                return 1;
            } else if (join_status != JOIN_OK) {
                write_log(L_INFO, "unable to join to the room\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_ERROR, true, "Unable to join to the room");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                return 1;
            }
        }

        pthread_mutex_lock(&players_mtx);

        ctx.room_id = ntohl(POP_DATA(d, uint32_t));
        
        ctx.player_id = ntohl(POP_DATA(d, uint32_t));
        CHECK_FIELD(ctx.player_id > 0);

        int32_t server_tcp_fd = ntohl(POP_DATA(d, int32_t));
        CHECK_FIELD(server_tcp_fd > 0);

        ctx.players_number = POP_DATA(d, uint8_t);
        CHECK_FIELD(ctx.players_number <= TEAMS_COUNT);

        ctx.joined_players = POP_DATA(d, uint8_t);
        CHECK_FIELD(ctx.joined_players <= TEAMS_COUNT);

        ctx.player_team = POP_DATA(d, uint8_t);
        CHECK_FIELD(ctx.player_team < TEAMS_COUNT);

        ctx.server_target_tps = POP_DATA(d, uint8_t);
        
        ctx.stage = POP_DATA(d, uint8_t);
        CHECK_FIELD(ctx.stage < STAGE_COUNT);
        
        CHECK_SIZE( 1 + 4 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2*TEAMS_COUNT + (4 + 1 + 1 + USERNAME_LEN)*ctx.joined_players );
        
        if (ctx.stage == STAGE_AREAS)
            ctx.mode = ctx.new_room ? MODE_AREAS : MODE_WAIT;
        else
            ctx.mode = MODE_SPAWN;

        ctx.world_size.x = ntohs(POP_DATA(d, uint16_t));
        CHECK_FIELD(ctx.world_size.x > 0);
        ctx.world_size.y = ntohs(POP_DATA(d, uint16_t));
        CHECK_FIELD(ctx.world_size.y > 0);

        ctx.total_boids_number = 0;
        for (int i = 0; i < TEAMS_COUNT; i++) {
            BoidIndex boids = htons(POP_DATA(d, uint16_t));
            CHECK_FIELD(boids <= MAX_BOIDS_COUNT);
            ctx.boids_number[i] = boids;
            ctx.total_boids_number += boids;

            ctx.teams_used[i] = boids > 0;
        }

        for (int i = 0; i < ctx.joined_players; i++) {
            ClientPlayer *op = &ctx.players[i];

            op->id = ntohl(POP_DATA(d, uint32_t));
            CHECK_FIELD(op->id > 0);

            op->team = POP_DATA(d, uint8_t);
            CHECK_FIELD(op->team < TEAMS_COUNT);

            op->ready = POP_DATA(d, uint8_t);

            POP_MEM(d, op->name, USERNAME_LEN);
            op->name[USERNAME_LEN-1] = '\0';
            
            if (op->id == ctx.player_id)
                memcpy(ctx.username, op->name, USERNAME_LEN);
        }

        if (ctx.udp_opened) {
            /* CP_UDP_HELLO PACKET FORMAT
            (uint32 player_id) (int32_t player_tcp_fd)
            */
            
            char buf[sizeof(uint32_t) + sizeof(int32_t)];
            char *b = buf;
            PUSH_DATA(b, uint32_t, htonl(ctx.player_id));
            PUSH_DATA(b, int32_t, htonl(server_tcp_fd));

            sendto_packet(ctx.udp_fd, CP_UDP_HELLO, buf, sizeof(buf), 0, (struct sockaddr*)&ctx.udp_servaddr, sizeof(ctx.udp_servaddr));
        }

        pthread_mutex_unlock(&players_mtx);
        
        pthread_mutex_lock(&next_menu_mtx);
        ctx.next_menu = MENU_GAME;
        ctx.change_menu = true;
        pthread_mutex_unlock(&next_menu_mtx);
        
        break;
    }
    case SP_APPROVE_PLAYER: { // Approve/reject new player
        /* SP_APPROVE_PLAYER PACKET FORMAT
        (uint32 player_id) (int8[USERNAME_LEN] username)
        */

        CHECK_SIZE( /*id*/ sizeof(uint32_t) + /*username*/ USERNAME_LEN );

        ctx.approved_player_id = ntohl(POP_DATA(d, uint32_t));
        CHECK_FIELD(ctx.approved_player_id > 0);

        POP_MEM(d, ctx.approved_player_username, USERNAME_LEN);
        ctx.approved_player_username[USERNAME_LEN-1] = '\0';

        pthread_mutex_lock(&input_mtx);
        ctx.get_input = true;
        pthread_mutex_unlock(&input_mtx);

        log_message(&ctx.log, L_QUESTION, "team of new player '%s' (r/b/g/y or n for reject):\n", ctx.approved_player_username);

        break;   
        }
    case SP_NEW_JOIN: {
        /* SP_NEW_JOIN PACKET FORMAT
        (uint32 id) (uint8 team) (uint8 ready) (uint8[USERNAME_LEN] username)
        */
    
        CHECK_SIZE( 4 + 1 + 1 + USERNAME_LEN );
        
        ClientPlayer new_player;
        
        new_player.id = ntohl(POP_DATA(d, uint32_t));
        CHECK_FIELD(new_player.id > 0);
        
        new_player.team = POP_DATA(d, uint8_t);
        CHECK_FIELD(new_player.team < TEAMS_COUNT);
        
        d += 1; // skip ready field
        new_player.ready = false;

        POP_MEM(d, new_player.name, USERNAME_LEN);
        new_player.name[USERNAME_LEN-1] = '\0';
        
        pthread_mutex_lock(&players_mtx);
        ctx.players[ctx.joined_players++] = new_player;
        pthread_mutex_unlock(&players_mtx);
        log_message(&ctx.log, L_JOIN, "new player '%s' - %s\n", new_player.name, get_team_name(new_player.team));

        if (ctx.new_room && ctx.players_number == ctx.joined_players && ctx.stage == STAGE_AREAS) {
            log_message(&ctx.log, L_INFO, "press ENTER to start placing boids\n");
        }
        ctx.approved_player_id = 0;
    
        break;
        }
    case SP_PLAYER_EXIT: {
        /* SP_PLAYER_EXIT PACKET FORMAT
        (uin32_t player_id)
        */
    
        CHECK_SIZE( sizeof(uint32_t) );

        uint32_t exited_player = ntohl(*(uint32_t*)packet_data);

        if ((ctx.stage == STAGE_AREAS || ctx.stage == STAGE_PLACING) && exited_player == ctx.approved_player_id) {
            log_message(&ctx.log, L_DISCONNECT, "player '%s' disconnected\n", ctx.approved_player_username);
            ctx.approved_player_id = 0;

            pthread_mutex_lock(&input_mtx);
            if (ctx.get_input)
                ctx.typing_keyboard_input = false;
            ctx.get_input = false;
            pthread_mutex_unlock(&input_mtx);
        } else {
            pthread_mutex_lock(&players_mtx);
            int player_idx = get_player_idx(ctx.players, exited_player);
            if (player_idx < 0 || player_idx >= ctx.joined_players) {
                pthread_mutex_unlock(&players_mtx);
                break;
            }

            log_message(&ctx.log, L_DISCONNECT, "player '%s' disconnected\n", ctx.players[player_idx].name);
    
            // delete player from array
            memmove(ctx.players + player_idx, ctx.players + player_idx + 1,
                    sizeof(ctx.players[0]) * (ctx.joined_players - player_idx - 1));
            ctx.joined_players--;
            pthread_mutex_unlock(&players_mtx);
        }
    
        break;
        }
    case SP_START_PLACING:
    case SP_SEND_AREAS: {
        /* SP_START_PLACING|SP_SEND_AREAS PACKET FORMAT
        (uint16 areas_count) ( { (uint16 x1) (uint16 y1) (uint16 x2) (uint16 y2) (uint8 team) }[areas_count] areas)
        */

        CHECK_LEAST_SIZE( /*areas_count*/ sizeof(int16_t) );
        
        uint16_t new_areas_count = ntohs(POP_DATA(d, uint16_t));
        CHECK_FIELD(new_areas_count < MAX_AREAS_COUNT);
        
        CHECK_SIZE( 2 + (2 + 2 + 2 + 2 + 1)*new_areas_count );
        
        if (!ctx.new_room) {
            pthread_mutex_lock(&areas_mtx);
            ctx.areas_count = new_areas_count;
            for (int i = 0; i < ctx.areas_count; i++) {
                Area *a = &ctx.areas[i];
                a->rec.x1 = ntohs(POP_DATA(d, uint16_t));
                a->rec.y1 = ntohs(POP_DATA(d, uint16_t));
                a->rec.x2 = ntohs(POP_DATA(d, uint16_t));
                a->rec.y2 = ntohs(POP_DATA(d, uint16_t));
                a->team = POP_DATA(d, uint8_t);
                CHECK_FIELD(a->team < TEAMS_COUNT);
            }
            pthread_mutex_unlock(&areas_mtx);
        }

        if (packet_type == SP_START_PLACING) {
            ctx.mode = MODE_SPAWN;
            ctx.stage = STAGE_PLACING;

            log_message(&ctx.log, L_INFO, "now you can spawn boids on your areas");
            log_message(&ctx.log, L_INFO, "press ENTER when you will ready to start the game\n");
        }
    
        break;
        }
    case SP_PLAYER_READY: {
        /* SP_PLAYER_READY PACKET FORMAT
        (uint32 player_id)
        */

        CHECK_SIZE( sizeof(uint32_t) );

        pthread_mutex_lock(&players_mtx);
        
        uint32_t id = ntohl(*(uint32_t*)packet_data);
        CHECK_FIELD(id > 0);
        
        int idx = get_player_idx(ctx.players, id);
        ctx.players[ctx.players[idx].team].ready = true;
        log_message(&ctx.log, L_INFO, "player '%s' is ready\n", ctx.players[idx].name);

        pthread_mutex_unlock(&players_mtx);
    
        break;
        }
    case SP_START_GAME: {
        /* SP_START_GAME PACKET FORMAT
        (uint16 boids_count) ( { (uint16 x) (uint16 y) (uint8 speed) (uint8 xp) (uint8 team) (uint8 max_health) }[boids_count] boids)
        */

        CHECK_LEAST_SIZE( /*boids_count*/ sizeof(uint16_t) );
    
        uint16_t recv_boids_count = ntohs(POP_DATA(d, uint16_t));
        CHECK_FIELD(recv_boids_count == ctx.total_boids_number);

        CHECK_SIZE( 2 + (2 + 2 + 1 + 1 + 1 + 1)*recv_boids_count );
        
        // ServerStartNetBoid *recv_boids = (ServerStartNetBoid*)(packet_data + sizeof(recv_boids_count));

        pthread_mutex_lock(&boids_mtx);
        for (int i = 0; i < recv_boids_count; i++) {
            ClientBoid boid = {.b = {.action = ACT_STOP, .boid_idx = i},
                               .v = {.direction = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0}}};
            
            boid.b.pos.x = ntohs(POP_DATA(d, uint16_t));
            CHECK_FIELD(boid.b.pos.x <= ctx.world_size.x);
            
            boid.b.pos.y = ntohs(POP_DATA(d, uint16_t));
            CHECK_FIELD(boid.b.pos.y <= ctx.world_size.y);
            
            boid.b.speed = POP_DATA(d, uint8_t) / 100.0f;

            boid.b.xp = POP_DATA(d, uint8_t);
            boid.b.xp = MIN(boid.b.xp, BOID_MAX_XP);
            
            boid.b.team = POP_DATA(d, uint8_t);
            CHECK_FIELD(boid.b.team < TEAMS_COUNT);
            
            boid.b.max_health = boid.b.health = POP_DATA(d, uint8_t);

            ctx.boids[i] = boid;
        }
        ctx.boids_count = recv_boids_count;

        // Reinit grid
        clear_grid(&ctx.grid);
        FILL_GRID(&ctx.grid, ctx.boids, ctx.boids_count);

        pthread_mutex_unlock(&boids_mtx);

        ctx.mode = MODE_SELECT;
        ctx.stage = STAGE_GAME;
        ctx.select_mode = true;

        log_message(&ctx.log, L_INFO, "the game has started\n");
    
        break;
        }
    case SP_BOIDS_SYNC: {
        /* SP_BOIDS_SYNC PACKET FORMAT
        (uint8 current_server_tps) (uint16 boids_count) (uint16 first_boid_index)
        ({
          (uint16 x) (uint16 y) (int8 health) (uint8 xp) (int8 action) (uint8 angle) (int8 vel)
         }[boids_count] boids)
        */

        CHECK_LEAST_SIZE( /*current_server_tps*/ 1 + /*boids_count*/ sizeof(BoidIndex) + /*first_boid_index*/ sizeof(BoidIndex) );

        uint8_t server_tps = POP_DATA(d, uint8_t);
        
        BoidIndex recv_boids_count = ntohs(POP_DATA(d, BoidIndex));
        
        CHECK_SIZE( 1 + 2 + 2 + (2 + 2 + 1 + 1 + 1 + 1 + 1)*recv_boids_count );
        
        ctx.server_tps = server_tps;
        BoidIndex boids_first_index = ntohs(POP_DATA(d, BoidIndex));
        CHECK_FIELD(boids_first_index + recv_boids_count <= ctx.total_boids_number);

        pthread_mutex_lock(&boids_mtx);
        for (int i = 0; i < recv_boids_count; i++) {
            ClientBoid *boid = &ctx.boids[boids_first_index + i];

            uint16_t x = ntohs(POP_DATA(d, uint16_t));
            uint16_t y = ntohs(POP_DATA(d, uint16_t));
            boid->b.health = POP_DATA(d, int8_t);
            boid->b.xp = POP_DATA(d, uint8_t);
            boid->b.xp = MIN(boid->b.xp, BOID_MAX_XP);

            int8_t action = POP_DATA(d, int8_t);
            CHECK_FIELD(action >= 0 && action < ACT_COUNT);
            if (action == ACT_FALL || action == ACT_SURRENDER) {
                boid->v.is_selected = false;
                if ((action == ACT_FALL && boid->b.action != ACT_FALL) ||
                    (action == ACT_SURRENDER && boid->b.action != ACT_SURRENDER))
                    boid->v.sprite_timer = 0;
                d += 2; // skip angle and vel properties
                boid->b.action = action;
                continue;
            }
            boid->b.action = action;

            boid->v.target_pos.x = x;
            boid->v.target_pos.y = y;
            
            uint8_t angle = POP_DATA(d, uint8_t);
            int8_t vel = POP_DATA(d, int8_t);

            boid->b.velocity.x = vel/255.0*BOID_MAX_SPEED * cos(angle/127.0*PI);
            boid->b.velocity.y = vel/255.0*BOID_MAX_SPEED * sin(angle/127.0*PI);
            
            boid->v.go_target = true;
        }

        // Update grid
        clear_grid(&ctx.grid);
        FILL_GRID(&ctx.grid, ctx.boids, ctx.boids_count);

        pthread_mutex_unlock(&boids_mtx);

        break;
        }
    case SP_INVALID_PACKET: {
        write_log(L_INFO, "the server received an invalid packet; check the compatibillity of the server and client verions\n");
        
        break;
        }
    case SP_DISCONNECT_PLAYER: {
        /* SP_DISCONNECT_PLAYER PACKET FORMAT
        (uint8 reason)
        */

        CHECK_SIZE( 1 );

        DisconnectionReason reason = POP_DATA(d, uint8_t);

        switch (reason) {
            case DISCONNECT_KICKED:
                write_log(L_INFO, "admin kicked you out of the room\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_INFO, true, "Admin kicked you out of the room");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                break;
            case DISCONNECT_ADMIN_CLOSED_ROOM:
                write_log(L_INFO, "admin closed the room\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_INFO, true, "Admin closed the room");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                break;
            case DISCONNECT_ADMIN_EXITED:
                write_log(L_INFO, "admin left the room\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_INFO, true, "Admin left the room");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                break;
            case DISCONNECT_PACKET_VIOLATIONS:
                write_log(L_ERROR, "you were kicked out room because of a violations of packet transmission\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_ERROR, true, "You were kicked out because of a violations of packet transmission");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                break;
            case DISCONNECT_SERVER_ERROR:
                write_log(L_ERROR, "an error has occurred on the server\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_ERROR, true, "An error has occurred on the server");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                break;
            case DISCONNECT_SERVER_DOWN:
                write_log(L_ERROR, "the server crashed\n");

                pthread_mutex_lock(&next_menu_mtx);
                set_menu_message(MESSAGE_ERROR, true, "The server crashed :)");
                ctx.next_menu = MENU_MAIN;
                ctx.change_menu = true;
                pthread_mutex_unlock(&next_menu_mtx);

                break;
            default:
                break;
        }

        return 1;
        }
    case SP_PLAYER_KICKED: {
        /* SP_PLAYER_KICKED PACKET FORMAT
        (uint32 player_id)
        */

        CHECK_SIZE( sizeof(uint32_t) );

        uint32_t kicked_player = ntohl(*(uint32_t*)packet_data);
        CHECK_FIELD(kicked_player > 0);

        pthread_mutex_lock(&players_mtx);
        int player_idx = get_player_idx(ctx.players, kicked_player);
        if (player_idx < 0 || player_idx >= ctx.joined_players) {
            pthread_mutex_unlock(&players_mtx);
            return 1;
        }
        
        log_message(&ctx.log, L_DISCONNECT, "player '%s' was kicked out of the room by player '%s'\n", ctx.players[player_idx].name, ctx.players[0].name);

        // delete player from array
        memmove(ctx.players + player_idx, ctx.players + player_idx + 1,
                sizeof(ctx.players[0]) * (ctx.joined_players - player_idx - 1));
        ctx.joined_players--;
        pthread_mutex_unlock(&players_mtx);

        break;
        }
    case SP_CHANGE_TEAM: {
        /* SP_CHANGE_TEAM PACKET FORMAT
        (uint32 player1_id) (uint8 new_team)
        */

        CHECK_SIZE( sizeof(uint32_t) + 1 );

        uint32_t pid = ntohl(POP_DATA(d, uint32_t)); // player ID
        CHECK_FIELD(pid > 0);

        pthread_mutex_lock(&players_mtx);
        int pidx = get_player_idx(ctx.players, pid); // player idx
        CHECK_FIELD(pidx >= 0 && pidx < ctx.joined_players);

        uint8_t team = POP_DATA(d, uint8_t);
        CHECK_FIELD(team < TEAMS_COUNT);
        
        ctx.players[pidx].team = team;
        ctx.player_team = ctx.players[get_player_idx(ctx.players, ctx.player_id)].team;
        
        log_message(&ctx.log, L_INFO, "player '%s' changed team of player '%s' to %s\n",
                    ctx.players[0].name, ctx.players[pidx].name, get_team_name(team));

        pthread_mutex_unlock(&players_mtx);
        break;
        }
    case SP_SWAP_TEAMS: {
        /* SP_SWAP_TEAMS PACKET FORMAT
        (uint32 player1_id) (uint32 player2_id)
        */

        CHECK_SIZE( sizeof(uint32_t)*2 );
        
        uint32_t pid1 = ntohl(POP_DATA(d, uint32_t));
        CHECK_FIELD(pid1 > 0);
        uint32_t pid2 = ntohl(POP_DATA(d, uint32_t));
        CHECK_FIELD(pid2 > 0);
        
        pthread_mutex_lock(&players_mtx);

        int pidx1 = get_player_idx(ctx.players, pid1);
        if (pidx1 < 0 || pidx1 >= ctx.joined_players) {
            pthread_mutex_unlock(&players_mtx);
            break;
        }
        int pidx2 = get_player_idx(ctx.players, pid2);
        if (pidx2 < 0 || pidx2 >= ctx.joined_players) {
            pthread_mutex_unlock(&players_mtx);
            break;
        }

        int team_tmp = ctx.players[pidx1].team;
        ctx.players[pidx1].team = ctx.players[pidx2].team;
        ctx.players[pidx2].team = team_tmp;
        ctx.player_team = ctx.players[get_player_idx(ctx.players, ctx.player_id)].team;
        
        log_message(&ctx.log, L_INFO, "player '%s' changed team of player '%s' to %s and team of player '%s' to %s\n",
                    ctx.players[0].name, ctx.players[pidx1].name, get_team_name(ctx.players[pidx1].team),
                    ctx.players[pidx2].name, get_team_name(ctx.players[pidx2].team));

        pthread_mutex_unlock(&players_mtx);
        break;
        }
    case SP_CHAT_MSG: {
        /* SP_CHAT_MSG PACKET FORMAT
        (uint32 sender_id) (uint16 msg_len) (uint8[msg_len] msg)
        */

        CHECK_LEAST_SIZE( sizeof(uint32_t) + sizeof(uint16_t) );

        uint32_t sender_id = ntohl(POP_DATA(d, uint32_t));
        CHECK_FIELD(sender_id > 0);
        uint32_t msg_len = ntohs(POP_DATA(d, uint16_t));
        
        CHECK_SIZE( sizeof(uint32_t) + sizeof(uint16_t) + msg_len );

        int sender_idx = get_player_idx(ctx.players, sender_id);
        if (sender_idx < 0)
            break;
        
        char *msg = d;

        log_message(&ctx.log, L_CHAT, "@%s: %.*s", ctx.players[sender_idx].name, msg_len, msg);;
        
        break;
        }
    default:
        log_message(&ctx.log, L_ERROR, "unknown package type SP#%d\n", packet_type);
        return 1;
    }

    return 0;
}

#undef CHECK_LEAST_SIZE
#undef CHECK_SIZE
#undef CHECK_FIELD

void *net_thread_fn() {
    ctx.approved_player_id = 0;
    ctx.approved_player_username[0] = '\0';
    
    enum {
        SYNC_PROTO_NONE = 0,
        SYNC_PROTO_TCP,
        SYNC_PROTO_UDP,
    } sync_proto = SYNC_PROTO_NONE;
    
    struct timeval timeout;
    fd_set read_fds;

    while (1) {
        pthread_mutex_lock(&running_mtx);
        if (!ctx.running) {
            pthread_mutex_unlock(&running_mtx);
            break;
        }
        pthread_mutex_unlock(&running_mtx);
        
        pthread_mutex_lock(&input_mtx);
        if (ctx.input_received) {
            if (ctx.approved_player_id != 0) { // 0 is an invalid player id
                bool ok = true;
                int8_t team;

                if (ctx.input_string[0] == 'n') {
                    team = -1; // reject new player
                } else {
                    team = get_team_id(ctx.input_string);
                    if (team < 0) {
                        log_message(&ctx.log, L_WARNING, "enter valid team\n");
                        ctx.get_input = true;
                        ok = false;
                    }
                }

                if (team >= 0) {
                    if (ctx.boids_number[team] == 0) {
                        log_message(&ctx.log, L_WARNING, "enter valid team\n");
                        ctx.get_input = true;
                        ok = false;
                    }

                    bool team_used = false;
                    pthread_mutex_lock(&players_mtx);
                    for (int i = 0; i < ctx.joined_players; i++) {
                        if (ctx.players[i].team == team) {
                            team_used = true;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&players_mtx);
                    if (team_used) {
                        log_message(&ctx.log, L_WARNING, "enter an unused team\n");
                        ctx.get_input = true;
                        ok = false;
                    }
                }
                
                if (ok) {
                    /* CP_APPROVE_PLAYER PACKET FORMAT
                    (uint32 player_id) (int8 team)
                    */

                    const uint32_t packet_size = 4 + 1;
                    char data[packet_size];
                    char *d = data;

                    PUSH_DATA(d, uint32_t, htonl(ctx.approved_player_id));
                    PUSH_DATA(d, int8_t, team);
                    
                    send_packet(ctx.tcp_fd, CP_APPROVE_PLAYER, data, packet_size, 0);
                }
            }
            
            ctx.input_received = false;
        }
        pthread_mutex_unlock(&input_mtx);
        
        FD_ZERO(&read_fds);
        FD_SET(ctx.tcp_fd, &read_fds);
        if (ctx.udp_opened)
            FD_SET(ctx.udp_fd, &read_fds);

        timeout.tv_sec = 0;
        timeout.tv_usec = 100*1000; // 100 ms

        clear_menu_message();

        const int max_fd = ctx.udp_opened? MAX(ctx.tcp_fd, ctx.udp_fd) : ctx.tcp_fd;
        int ready = select(max_fd+1, &read_fds, NULL, NULL, &timeout);
        if (ready > 0) {
            // Check TCP
            if (FD_ISSET(ctx.tcp_fd, &read_fds)) {
                uint32_t packet_type;
                int r = recv(ctx.tcp_fd, (void*)&packet_type, 1, 0);
                if (r == 0) {
                    log_message(&ctx.log, L_DEBUG, "TCP connection closed by the server\n");
                    break;
                } else if (r < 0) {
                    bool err_wouldblock;
                    #ifdef _WIN32
                        int err = WSAGetLastError();
                        err_wouldblock = (err == WSAEWOULDBLOCK);
                    #else
                        err_wouldblock = (errno == EWOULDBLOCK);
                    #endif
                    if (!err_wouldblock) {
                        log_message(&ctx.log, L_ERROR, "failed to receive a packet size by TCP\n");
                        perror("recv");
                        break;
                    }
                } else {
                    uint32_t packet_size;
                    recv_all(ctx.tcp_fd, &packet_size, sizeof(uint32_t), 0);
                    packet_size = ntohl(packet_size);
                    char *packet_data = malloc(packet_size);

                    if (recv_all(ctx.tcp_fd, packet_data, packet_size, 0)) {
                        log_message(&ctx.log, L_ERROR, "error receiving SP#%d packet\n", packet_type);
                        perror("recv_all");
                        free(packet_data);
                        break;
                    }

                    if (process_data(packet_type, packet_size, packet_data)) {
                        free(packet_data);
                        break;
                    }
                    if (packet_type == SP_BOIDS_SYNC && sync_proto != SYNC_PROTO_TCP) {
                        sync_proto = SYNC_PROTO_TCP;
                        log_message(&ctx.log, L_DEBUG, "Using TCP as a protocol for boids sync\n");
                    }
                    free(packet_data);
                }
            }

            // Check UDP
            if (ctx.udp_opened && FD_ISSET(ctx.udp_fd, &read_fds)) {
                char recv_buf[1500];
                socklen_t addrlen = sizeof(ctx.udp_servaddr);
                ssize_t n = recvfrom(ctx.udp_fd, recv_buf, sizeof(recv_buf), 0, (struct sockaddr*)&ctx.udp_servaddr, &addrlen);
                if (n == 0) {
                    log_message(&ctx.log, L_DEBUG, "UDP connection closed\n");
                    break;
                } else if (n < 0) {
                    bool err_wouldblock;
                    #ifdef _WIN32
                        int err = WSAGetLastError();
                        err_wouldblock = (err == WSAEWOULDBLOCK);
                    #else
                        err_wouldblock = (errno == EWOULDBLOCK);
                    #endif
                    if (!err_wouldblock) {
                        log_message(&ctx.log, L_ERROR, "failed to receive data by UDP\n");
                        perror("recv");
                        break;
                    }
                } else {
                    if ((uint32_t)n >= (1 + sizeof(uint32_t))) {
                        // Prepare data to processing
                        uint8_t packet_type = *(uint8_t*)recv_buf;
                        uint32_t packet_size = ntohl(*(uint32_t*)(recv_buf + 1));
                        char *packet_data = recv_buf + 1 + sizeof(packet_size); // Skip header

                        if ((uint32_t)n == (1 + sizeof(packet_size) + packet_size)) {
                            if (process_data(packet_type, packet_size, packet_data))
                                break;
                            if (packet_type == SP_BOIDS_SYNC && sync_proto != SYNC_PROTO_UDP) {
                                sync_proto = SYNC_PROTO_UDP;
                                log_message(&ctx.log, L_DEBUG, "Using UDP as a protocol for boids sync\n");
                            }
                        }
                    }
                }
                        }
        } else if (ready < 0) {
            perror("select");
        }
    }

    pthread_mutex_lock(&next_menu_mtx);
    if (ctx.message_text == NULL) {
        set_menu_message(MESSAGE_INFO, true, "Connection closed");
        ctx.next_menu = MENU_MAIN;
        ctx.change_menu = true;
    }
    pthread_mutex_unlock(&next_menu_mtx);
    
    pthread_mutex_lock(&running_mtx);
    ctx.running = false;
    pthread_mutex_unlock(&running_mtx);

    return NULL;
}


/* <==================================================== COMMANDS ====================================================> */

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
        for (int i = 0; i < ctx.joined_players; i++) {
            if (strncmp(word+1, ctx.players[i].name, len-1) == 0) {
                const char* name = ctx.players[i].name;
                
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

#define CHECK_ARGS(args_number)                                                                    \
    if (argc < (args_number) + 1) {                                                                \
        log_message(&ctx.log, L_WARNING, "invalid number of arguments for %s command\n", argv[0]); \
        return 1;                                                                                  \
    }

#define CHECK_ADMIN()                                                                              \
    if (get_player_idx(ctx.players, ctx.player_id) != 0) {                                         \
        log_message(&ctx.log, L_WARNING, "command %s is not aviable\n", argv[0]);                  \
        return 1;                                                                                  \
    }

#define GET_TEAM(team_id, arg)                                                                     \
    do {                                                                                           \
        (team_id) = get_team_id(arg);                                                              \
        if ((team_id) == -1) {                                                                     \
            log_message(&ctx.log, L_WARNING, "invalid team '%s'\n", (arg));                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define GET_USERNAME(username, arg)                                                                \
    do {                                                                                           \
        (username) = (arg);                                                                        \
        if ((username)[0] != '@') {                                                                \
            log_message(&ctx.log, L_WARNING, "username should start with '@'\n");                  \
            return 1;                                                                              \
        }                                                                                          \
        (username)++; /* skip @ before the username*/                                              \
    } while (0)

#define GET_PLAYER_ID(pid, username)                                                               \
    do {                                                                                           \
        pthread_mutex_lock(&players_mtx);                                                          \
        (pid) = 0;                                                                                 \
        for (int i = 0; i < ctx.joined_players; i++) {                                             \
            if (strcmp((username), ctx.players[i].name) == 0) {                                    \
                (pid) = ctx.players[i].id;                                                         \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
        pthread_mutex_unlock(&players_mtx);                                                        \
                                                                                                   \
        if ((pid) == 0) {                                                                          \
            log_message(&ctx.log, L_WARNING, "there is no player with name '%s'\n", (username));   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int get_command(char *cmd_string) {
    for (int i = 0; i < commands_count; i++) {
        if (strcmp(cmd_string, commands_list[i].full_cmd) == 0 || strcmp(cmd_string, commands_list[i].short_cmd) == 0)
            return i;
    }

    return -1;
}

int process_command(char *command) {
    if (command[0] != '/') {
        log_message(&ctx.log, L_WARNING, "command should start with '/'\n");
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
                    log_message(&ctx.log, L_WARNING, "unknown command %s\n", argv[1]);
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

            log_message(&ctx.log, L_INFO, "%s", message);
            break;
        case CMD_LOG_WARN:
            CHECK_ARGS(1);
            
            log_message(&ctx.log, L_WARNING, "%s", TextJoin(argv+1, argc-1, delimiter));
            break;
        case CMD_LOG_INFO:
            CHECK_ARGS(1);
        
            log_message(&ctx.log, L_INFO, "%s", TextJoin(argv+1, argc-1, delimiter));
            break;
        case CMD_LOG_CLEAR:
            pthread_mutex_lock(&log_mtx);
            ctx.log.size = 0;
            pthread_mutex_unlock(&log_mtx);
            break;
        case CMD_EXIT:
            pthread_mutex_lock(&running_mtx);
            ctx.running = false;
            pthread_mutex_unlock(&running_mtx);
            break;
        case CMD_PLAYERS_LIST:
            strcpy(message, "players:\n");
            pthread_mutex_lock(&players_mtx);
            for (int i = 0; i < ctx.joined_players; i++) {
                ClientPlayer *op = &ctx.players[i];

                const char *team = get_team_name(op->team);

                snprintf(message+strlen(message), sizeof(message), "  %s - %s\n", op->name, team);
            }
            pthread_mutex_unlock(&players_mtx);
            log_message(&ctx.log, L_INFO, "%s", message);
            break;
        case CMD_ROOM_INFO:
            log_message(&ctx.log, L_INFO, "room info\n     id: %06x\n     teams: %d\n     world: %dx%d\n     chunk: %d\n     server tps: %d\n     creator: %s\n"
                            "     boids:  %-4d\n     red:    %-4d\n     blue:   %-4d\n     green:  %-4d\n     yellow: %-4d\n",
                   ctx.room_id, ctx.players_number, ctx.world_size.x, ctx.world_size.y, ctx.chunk_size, ctx.server_target_tps, ctx.players[0].name, ctx.total_boids_number,
                   ctx.boids_number[TEAM_RED],
                   ctx.boids_number[TEAM_BLUE],
                   ctx.boids_number[TEAM_GREEN],
                   ctx.boids_number[TEAM_YELLOW]);
            break;
        case CMD_ROOM_ID:
            log_message(&ctx.log, L_INFO, "%06x\n", ctx.room_id);
            break;
        case CMD_COPY_ROOM_ID:
            SetClipboardText(TextFormat("%06x", ctx.room_id));
            break;
        case CMD_CLOSE_ROOM:
            CHECK_ADMIN();

            send_packet(ctx.tcp_fd, CP_CLOSE_ROOM, NULL, 0, 0);
            break;
        case CMD_KICK_PLAYER:
            CHECK_ARGS(1);
            CHECK_ADMIN();
            
            GET_USERNAME(username, argv[1]);
            GET_PLAYER_ID(pid, username);
            
            /* CP_KICK_PLAYER PACKET FORMAT
            (uint32 player_id)
            */

            pid = htonl(pid);
            send_packet(ctx.tcp_fd, CP_KICK_PLAYER, &pid, sizeof(pid), 0);
            
            break;
        case CMD_CHANGE_TEAM:
            CHECK_ARGS(2);
            CHECK_ADMIN();

            if (ctx.stage != STAGE_AREAS) {
                log_message(&ctx.log, L_WARNING, "you can use %s command only during areas stage\n", argv[0]);
                return 1;
            }

            GET_USERNAME(username, argv[1]);
            GET_PLAYER_ID(pid, username);

            int8_t team;
            GET_TEAM(team, argv[2]);

            if (!ctx.teams_used[team]) {
                log_message(&ctx.log, L_WARNING, "there are no boids for %s team\n", get_team_name(team));
                return 1;
            }

            for (int i = 0; i < ctx.joined_players; i++) {
                if (ctx.players[i].team == team) {
                    log_message(&ctx.log, L_WARNING, "%s team already used\n", get_team_name(team));
                    return 1;
                }
            }

            /* CP_CHANGE_TEAM PACKET FORMAT
            (uint32 player_id) (int8 new_team)
            */
            
            const uint32_t ct_packet_size = 4 + 1;
            // char ct_buf[ct_packet_size]; // error: switch jumps into scope of identifier with variably modified type
            char *ct_buf = malloc(ct_packet_size);
            char *ct_d = ct_buf;
            
            PUSH_DATA(ct_d, uint32_t, htonl(pid));
            PUSH_DATA(ct_d, int8_t, team);
            
            send_packet(ctx.tcp_fd, CP_CHANGE_TEAM, ct_buf, ct_packet_size, 0);
            free(ct_buf);

            break;
        case CMD_SWAP_TEAMS:
            CHECK_ARGS(2);
            CHECK_ADMIN();

            if (ctx.stage != STAGE_AREAS) {
                log_message(&ctx.log, L_WARNING, "you can use %s command only during areas stage\n", argv[0]);
                return 1;
            }

            char *username1, *username2;
            int pid1, pid2;

            GET_USERNAME(username1, argv[1]);
            GET_PLAYER_ID(pid1, username1);

            GET_USERNAME(username2, argv[2]);
            GET_PLAYER_ID(pid2, username2);

            /* CP_SWAP_TEAMS PACKET FORMAT
            (uint32 player1_id) (uint32 player2_id)
            */

            const uint32_t st_packet_size = 4 + 4;
            // char st_buf[st_packet_size]; // error: switch jumps into scope of identifier with variably modified type
            char *st_buf = malloc(st_packet_size);
            char *st_d = st_buf;

            PUSH_DATA(st_d, uint32_t, htonl(pid1));
            PUSH_DATA(st_d, uint32_t, htonl(pid2));

            send_packet(ctx.tcp_fd, CP_SWAP_TEAMS, st_buf, st_packet_size, 0);
            free(st_buf);

            break;
        default:
            log_message(&ctx.log, L_WARNING, "unknown command %s\n", argv[0]);
            return 1;
    }

    return 0;
}

#undef CHECK_ARGS
#undef CHECK_ADMIN
#undef GET_TEAM
#undef GET_USERNAME
#undef GET_PLAYER_ID


/* <================================================== INPUT SYSTEM ==================================================> */

// IE = Input Event
typedef enum {
    IE_SHOW_LOG = 0,
    IE_SHOW_GRID,
    IE_CHANGE_TPS_DISPLAY,
    IE_CHANGE_AUTOSELECT_MODE,
    IE_SHOW_HEALTH,
    IE_CLEAR_ORDERS,
    IE_DELETE_SELECTED_BOIDS,
    IE_PAUSE,
    IE_CHANGE_GUI_DISPLAY,
    IE_EXIT_GAME,
    IE_CHAT_MSG,
    IE_INPUT_START,
    IE_COMMAND,
    IE_INPUT_END,
    IE_START_PLACING,
    IE_READY,
    IE_BRUSH_INCRASE,
    IE_BRUSH_REDUCE,
    IE_TEAM_RED,
    IE_TEAM_BLUE,
    IE_TEAM_GREEN,
    IE_TEAM_YELLOW,
    IE_ERASE_AREAS,
    IE_MODE_SPAWN,
    IE_MODE_SELECT,
    IE_MODE_DELETE,
    IE_MODE_DIRECTION,
    IE_MODE_POINT,
    IE_MODE_LINE,
    IE_APPLY_LINE,
    IE_BOID_ACT_STOP,
    IE_BOID_ACT_ATTACK,
    IE_BOID_ACT_RETREAT,
    IE_CAMERA_MOVE,
    IE_CAMERA_ZOOM_IN,
    IE_CAMERA_ZOOM_OUT,
    IE_BORDER_MOVE,
    IE_BORDER_MOVE_START,
    IE_BORDER_MOVE_END,
    IE_ACTION,
    IE_ACTION_START,
    IE_ACTION_END,

    IE_COUNT // Number of events
} InputEvent;

typedef enum {
    IMOD_CTRL = 1,
    IMOD_ALT = 2,
    IMOD_SHIFT = 4
} InputModificator;

typedef enum {
    KTYPE_PRESS = 0,
    KTYPE_REPEATE,
    KTYPE_DOWN,
    KTYPE_RELEASE,
    KTYPE_UP,
} KeyboardInputType;

typedef enum {
    MTYPE_DOWN = 0,
    MTYPE_PRESS,
    MTYPE_UP,
    MTYPE_RELEASE,
} MouseInputType;

// kb = keyboard button (key)
// mb = mouse button
typedef struct {
    // Keyboard
    KeyboardKey kb1, kb2;
    KeyboardInputType kb_type;
    bool kb_on_input; // get an event when entering an input string
    uint8_t kb_mod; // or'ed keyboard button modifier keys
    // Mouse
    MouseButton mb;
    bool use_mb;
    MouseInputType mb_type;
    uint8_t mb_mod; // or'ed mouse modifier keys
    float mwf; // mouse weel factor
    // Game status
    RoomStage gstage1, gstage2; // game stage
    GameMode gmode; // game mode
    bool gomul; // multiplayer game only
    bool goloc; // local game only
    bool gloc; // local game
} InputBinding;

InputBinding bindings[] = {
    [IE_SHOW_LOG]               = {.kb1 = KEY_L},
    [IE_SHOW_GRID]              = {.kb1 = KEY_K},
    [IE_CHANGE_TPS_DISPLAY]     = {.kb1 = KEY_M, .gstage1 = STAGE_GAME, .gomul = true},
    [IE_CHANGE_AUTOSELECT_MODE] = {.kb1 = KEY_N, .gstage1 = STAGE_GAME},
    [IE_SHOW_HEALTH]            = {.kb1 = KEY_H, .gstage1 = STAGE_GAME},
    [IE_CLEAR_ORDERS]           = {.kb1 = KEY_Z, .gstage1 = STAGE_GAME},
    [IE_DELETE_SELECTED_BOIDS]  = {.kb1 = KEY_X, .gstage1 = STAGE_PLACING, .gmode = MODE_SELECT, .gloc = true},
    [IE_PAUSE]                  = {.kb1 = KEY_SPACE, .goloc = true},
    [IE_CHANGE_GUI_DISPLAY]     = {.kb1 = KEY_I},
    [IE_EXIT_GAME]              = {.kb1 = KEY_Q, .kb_mod = IMOD_CTRL},
    [IE_CHAT_MSG]               = {.kb1 = KEY_SPACE, .gomul = true},
    [IE_INPUT_START]            = {.kb1 = KEY_SPACE, .gomul = true},
    [IE_COMMAND]                = {.kb1 = KEY_SLASH, .kb2 = KEY_KP_DIVIDE, .gomul = true},
    [IE_START_PLACING]          = {.kb1 = KEY_ENTER, .gstage1 = STAGE_AREAS},
    [IE_READY]                  = {.kb1 = KEY_ENTER, .gstage1 = STAGE_PLACING},
    [IE_BRUSH_INCRASE]          = {.kb1 = KEY_P, .kb_type = KTYPE_REPEATE, .mb_mod = IMOD_CTRL, .mwf = 1.0f, .gomul = true},
    [IE_BRUSH_REDUCE]           = {.kb1 = KEY_O, .kb_type = KTYPE_REPEATE, .mb_mod = IMOD_CTRL, .mwf = -1.0f, .gomul = true},
    [IE_TEAM_RED]               = {.kb1 = KEY_Q, .gmode = MODE_AREAS, .gloc = true},
    [IE_TEAM_BLUE]              = {.kb1 = KEY_W, .gmode = MODE_AREAS, .gloc = true},
    [IE_TEAM_GREEN]             = {.kb1 = KEY_E, .gmode = MODE_AREAS, .gloc = true},
    [IE_TEAM_YELLOW]            = {.kb1 = KEY_R, .gmode = MODE_AREAS, .gloc = true},
    [IE_ERASE_AREAS]            = {.kb1 = KEY_Z, .gmode = MODE_AREAS},
    [IE_MODE_SPAWN]             = {.kb1 = KEY_A, .gstage1 = STAGE_PLACING, .gloc = true},
    [IE_MODE_SELECT]            = {.kb1 = KEY_S, .gstage1 = STAGE_GAME, .gstage2 = STAGE_PLACING},
    [IE_MODE_DELETE]            = {.kb1 = KEY_D, .gstage1 = STAGE_PLACING, .gomul = true},
    [IE_MODE_DIRECTION]         = {.kb1 = KEY_D, .gstage1 = STAGE_GAME},
    [IE_MODE_POINT]             = {.kb1 = KEY_F, .gstage1 = STAGE_GAME},
    [IE_MODE_LINE]              = {.kb1 = KEY_G, .gstage1 = STAGE_GAME},
    [IE_APPLY_LINE]             = {.kb1 = KEY_G, .gstage1 = STAGE_GAME, .gmode = MODE_LINE},
    [IE_BOID_ACT_STOP]          = {.kb1 = KEY_ONE, .gstage1 = STAGE_GAME},
    [IE_BOID_ACT_ATTACK]        = {.kb1 = KEY_TWO, .gstage1 = STAGE_GAME},
    [IE_BOID_ACT_RETREAT]       = {.kb1 = KEY_THREE, .gstage1 = STAGE_GAME},
    [IE_CAMERA_MOVE]            = {.mb = MOUSE_BUTTON_LEFT, .use_mb = true},
    [IE_CAMERA_ZOOM_IN]         = {.kb1 = KEY_EQUAL, .kb2 = KEY_KP_ADD,      .kb_type = KTYPE_REPEATE, .mwf = 1.0f},
    [IE_CAMERA_ZOOM_OUT]        = {.kb1 = KEY_MINUS, .kb2 = KEY_KP_SUBTRACT, .kb_type = KTYPE_REPEATE, .mwf = -1.0f},
    [IE_BORDER_MOVE]            = {.mb = MOUSE_BUTTON_LEFT, .use_mb = true, .mb_type = MTYPE_DOWN, .goloc = true},
    [IE_BORDER_MOVE_START]      = {.mb = MOUSE_BUTTON_LEFT, .use_mb = true, .mb_type = MTYPE_PRESS, .goloc = true},
    [IE_BORDER_MOVE_END]        = {.mb = MOUSE_BUTTON_LEFT, .use_mb = true, .mb_type = MTYPE_RELEASE, .goloc = true},
    [IE_ACTION]                 = {.mb = MOUSE_BUTTON_RIGHT, .use_mb = true, .mb_type = MTYPE_DOWN},
    [IE_ACTION_START]           = {.mb = MOUSE_BUTTON_RIGHT, .use_mb = true, .mb_type = MTYPE_PRESS},
    [IE_ACTION_END]             = {.mb = MOUSE_BUTTON_RIGHT, .use_mb = true, .mb_type = MTYPE_RELEASE},
};

#define CLEAR_EVENTS() ctx.events = 0
#define SET_EVENT(e) ctx.events |= (1ull << (e))
#define GET_EVENT(e) (ctx.events & (1ull << (e)))
#define CLEAR_EVENT(e)                           \
    do {                                         \
        ctx.events &= ~(1ull << (e));            \
        cleared_events |= (1ull << (e));         \
    } while (0)

#define CLEAR_MOD() ctx.modifiers = 0
#define SET_MOD(m) ctx.modifiers |= (m)
#define GET_MOD(m) (ctx.modifiers & (m))
#define CHECK_MOD(m) (ctx.modifiers == (m))

#define CLEAR_GUI_EVENTS() ctx.gui_events = 0
#define GUI_EVENT(e) ctx.gui_events |= (1ull << (e))
#define GET_GUI_EVENT(e) (ctx.gui_events & (1ull << (e)))

void handle_input() {
    CLEAR_MOD();
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
        SET_MOD(IMOD_CTRL);
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
        SET_MOD(IMOD_SHIFT);
    if (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT))
        SET_MOD(IMOD_ALT);

    CLEAR_EVENTS();
    unsigned long long cleared_events = 0;

    static int typing_timer = 0;
    if (typing_timer > 0)
        typing_timer--;
    if (ctx.typing_keyboard_input)
        typing_timer = 5;
    
    for (int event = 0; event < IE_COUNT; event++) {
        InputBinding b = bindings[event];
        
        bool (*key_fn)(int key) = IsKeyPressed;
        switch (b.kb_type) {
            case KTYPE_PRESS: key_fn = IsKeyPressed; break;
            case KTYPE_REPEATE: key_fn = IsKeyPressed; break;
            case KTYPE_DOWN: key_fn = IsKeyDown; break;
            case KTYPE_RELEASE: key_fn = IsKeyReleased; break;
            case KTYPE_UP: key_fn = IsKeyDown; break;
        }

        bool (*mouse_button_fn)(int button) = IsMouseButtonDown;
        switch (b.mb_type) {
            case MTYPE_DOWN: mouse_button_fn = IsMouseButtonDown; break;
            case MTYPE_PRESS: mouse_button_fn = IsMouseButtonPressed; break;
            case MTYPE_RELEASE: mouse_button_fn = IsMouseButtonReleased; break;
            case MTYPE_UP: mouse_button_fn = IsMouseButtonUp; break;
        }

        if (!(cleared_events & (1ull << event)) &&
            (b.gstage1 == STAGE_NULL || b.gstage1 == ctx.stage || b.gstage2 == ctx.stage || b.gloc) &&
            (b.gmode == MODE_NULL || b.gmode == ctx.mode || b.gloc) &&
            (!b.gomul || !ctx.local_game) &&
            (!b.goloc || ctx.local_game) &&
            (!ctx.typing_keyboard_input) &&
            ((b.kb1 != KEY_NULL && (key_fn(b.kb1) || (b.kb_type == KTYPE_REPEATE && IsKeyPressedRepeat(b.kb1))) && CHECK_MOD(b.kb_mod)) ||
             (b.kb2 != KEY_NULL && (key_fn(b.kb2) || (b.kb_type == KTYPE_REPEATE && IsKeyPressedRepeat(b.kb2))) && CHECK_MOD(b.kb_mod)) ||
             (((b.use_mb && (mouse_button_fn(b.mb))) || b.mwf*ctx.mouse_wheel > 0) && CHECK_MOD(b.mb_mod)) ||
             GET_GUI_EVENT(event))) {
            SET_EVENT(event);

            float wheel = ctx.mouse_wheel;
            switch (event) {
            case IE_SHOW_LOG: ctx.show_log = !ctx.show_log; break;
            case IE_SHOW_GRID: ctx.show_grid = !ctx.show_grid; break;
            case IE_CHANGE_TPS_DISPLAY: ctx.tps_display_type++; ctx.tps_display_type %= TPS_DISPLAY_TYPES_NUMBER; break;
            case IE_CHANGE_AUTOSELECT_MODE: ctx.autoselect_mode = !ctx.autoselect_mode; break;
            case IE_SHOW_HEALTH: ctx.show_health = !ctx.show_health; break;
            case IE_CLEAR_ORDERS: ctx.clear_order = ctx.select_mode; break;
            case IE_PAUSE: ctx.game_paused = !ctx.game_paused; break;
            case IE_CHANGE_GUI_DISPLAY: ctx.show_gui = !ctx.show_gui; break;
            case IE_EXIT_GAME: ctx.exit_game_message = true; break;

            case IE_CHAT_MSG:
            case IE_INPUT_START:
                pthread_mutex_lock(&input_mtx);
                ctx.typing_keyboard_input = true;
                ctx.input_string[0] = '\0';
                textBoxCursorIndex = 0;
                pthread_mutex_unlock(&input_mtx);
                break;
            case IE_COMMAND:
                pthread_mutex_lock(&input_mtx);
                ctx.typing_keyboard_input = true;
                ctx.input_string[0] = '/'; // Command starts with '/'
                ctx.input_string[1] = '\0';
                textBoxCursorIndex = 1;
                pthread_mutex_unlock(&input_mtx);
                break;
            case IE_START_PLACING:
                if (typing_timer > 0)
                    CLEAR_EVENT(IE_START_PLACING);
                break;
            case IE_READY:
                if (typing_timer > 0)
                    CLEAR_EVENT(IE_READY);
                break;
            case IE_INPUT_END:
                pthread_mutex_lock(&input_mtx);
                ctx.typing_keyboard_input = false;

                if (ctx.get_input) {
                    ctx.get_input = false;
                    ctx.input_received = true;
                    log_message(&ctx.log, L_INPUT, "%s", ctx.input_string);
                } else if (ctx.input_string[0] == '/') { // command
                    log_message(&ctx.log, L_INPUT, "%s", ctx.input_string);
                    process_command(ctx.input_string);
                } else { // message for chat
                    /* CP_CHAT_MSG PACKET FORMAT
                    (uint16 msg_len) (uint8[msg_len] msg)
                    */
                    
                    const int len = strlen(ctx.input_string);
                    uint32_t packet_size = 2 + len+1;
                    char *buf = malloc(packet_size);
                    char *d = buf;

                    PUSH_DATA(d, uint16_t, htons(len+1));
                    PUSH_MEM(d, ctx.input_string, len+1);
                    
                    send_packet(ctx.tcp_fd, CP_CHAT_MSG, buf, packet_size, 0);
                    free(buf);
                }
                pthread_mutex_unlock(&input_mtx);
                break;
            
            case IE_BRUSH_INCRASE:
            case IE_BRUSH_REDUCE:
                if (wheel == 0) wheel = b.mwf;
                ctx.brush_size += wheel * MAX(1, roundf(logf(ctx.brush_size)));
                if (ctx.brush_size < 1) ctx.brush_size = 1;
                break;
            
            case IE_TEAM_RED: ctx.selecting_team = TEAM_RED; ctx.change_selection_team = ctx.select_mode; break;
            case IE_TEAM_BLUE: ctx.selecting_team = TEAM_BLUE; ctx.change_selection_team = ctx.select_mode; break;
            case IE_TEAM_GREEN: ctx.selecting_team = TEAM_GREEN; ctx.change_selection_team = ctx.select_mode; break;
            case IE_TEAM_YELLOW: ctx.selecting_team = TEAM_YELLOW; ctx.change_selection_team = ctx.select_mode; break;
            case IE_ERASE_AREAS: ctx.selecting_team = -1; break;

            case IE_MODE_SPAWN: ctx.mode = MODE_SPAWN; ctx.select_mode = false; break;
            case IE_MODE_SELECT: ctx.mode = MODE_SELECT; ctx.select_mode = true; ctx.selecting = false; break;
            case IE_MODE_DELETE: ctx.mode = MODE_DELETE; ctx.select_mode = false; break;
            case IE_MODE_DIRECTION: ctx.mode = MODE_DIRECTION; ctx.select_mode = true; break;
            case IE_MODE_POINT: ctx.mode = MODE_POINT; ctx.select_mode = true; break;
            case IE_MODE_LINE:
                if (ctx.mode == MODE_LINE)
                    CLEAR_EVENT(IE_MODE_LINE);
                else {
                    ctx.mode = MODE_LINE;
                    ctx.select_mode = true;
                    ctx.show_line = false;
                    CLEAR_EVENT(IE_APPLY_LINE);
                }
                break;
            case IE_APPLY_LINE:
                ctx.change_boids_direction = true;
                ctx.show_line = false;
                CLEAR_EVENT(IE_MODE_LINE);
                break;

            case IE_BOID_ACT_STOP: ctx.action = ACT_STOP; ctx.change_boids_action = ctx.select_mode; break;
            case IE_BOID_ACT_ATTACK: ctx.action = ACT_ATTACK; ctx.change_boids_action = ctx.select_mode; break;
            case IE_BOID_ACT_RETREAT: ctx.action = ACT_RETREAT; ctx.change_boids_action = ctx.select_mode; break;

            case IE_CAMERA_MOVE:
                if (!ctx.is_dragging_border) {
                    ctx.camera.target = Vector2Add(ctx.camera.target, Vector2Scale(ctx.mouse_delta, -1.0f/ctx.camera.zoom));
                    CLEAR_EVENT(IE_BORDER_MOVE);
                } else CLEAR_EVENT(IE_CAMERA_MOVE);
                break;
            case IE_CAMERA_ZOOM_IN:
            case IE_CAMERA_ZOOM_OUT:
                if (wheel == 0) wheel = b.mwf;
                ctx.camera.offset = GetMousePosition();
                ctx.camera.target = ctx.mouse_position;
                ctx.camera.zoom = Clamp(expf(logf(ctx.camera.zoom) + 0.2f*wheel), 1/16.0f, 64.0f);
                break;

            case IE_BORDER_MOVE:
                if (ctx.is_dragging_border) {
                    ctx.world_size.x = MAX((int)ctx.mouse_position.x, 100);
                    ctx.world_size.y = MAX((int)ctx.mouse_position.y, 100);
                    CLEAR_EVENT(IE_CAMERA_MOVE);
                } else CLEAR_EVENT(IE_BORDER_MOVE);
                break;
            case IE_BORDER_MOVE_START:
                ctx.is_dragging_border = Vector2Distance(ctx.mouse_position, (Vector2){ctx.world_size.x, ctx.world_size.y}) <= 50;
                if (!ctx.is_dragging_border) {
                    CLEAR_EVENT(IE_BORDER_MOVE_START);
                }
                break;
            case IE_BORDER_MOVE_END:
                if (!ctx.is_dragging_border) CLEAR_EVENT(IE_BORDER_MOVE_END);
                ctx.is_dragging_border = false;
                break;
            }
        }
    }
}


/* <================================================= BOIDS AND MAIN =================================================> */

// Image button control
int GuiTextureButton(Rectangle bounds, Texture2D texture, Rectangle tex_source, Vector2 tex_position, float tex_scale, float tex_rotation)
{
    int result = RESULT_NONE;
    GuiState state = guiState;

    // Update control
    //--------------------------------------------------------------------
    if ((state != STATE_DISABLED) && !guiLocked && !guiControlExclusiveMode)
    {
        Vector2 mousePoint = GUI_POINTER_POSITION;

        // Check button state
        if (CheckCollisionPointRec(mousePoint, bounds))
        {
            if (GUI_BUTTON_DOWN) state = STATE_PRESSED;
            else state = STATE_FOCUSED;

            if (GUI_BUTTON_RELEASED) result = RESULT_PRESSED;
        }
    }
    //--------------------------------------------------------------------

    // Draw control
    //--------------------------------------------------------------------
    GuiDrawRectangle(bounds, GuiGetStyle(BUTTON, BORDER_WIDTH), GetColor(GuiGetStyle(BUTTON, BORDER + (state*3))), GetColor(GuiGetStyle(BUTTON, BASE + (state*3))));
    float scale_factor = MIN(bounds.width / tex_source.width, bounds.height / tex_source.height) * tex_scale;
    DrawTexturePro(texture, tex_source, (Rectangle){bounds.x + tex_position.x,
                                                    bounds.y + tex_position.y,
                                                    tex_source.width*scale_factor,
                                                    tex_source.height*scale_factor},
                   (Vector2){tex_source.width*scale_factor/2, tex_source.height*scale_factor/2}, tex_rotation, WHITE);
    // GuiDrawText(text, GetTextBounds(BUTTON, bounds), GuiGetStyle(BUTTON, TEXT_ALIGNMENT), GetColor(GuiGetStyle(BUTTON, TEXT + (state*3))));

    if (state == STATE_FOCUSED) GuiTooltip(bounds);
    //------------------------------------------------------------------

    return result;
}

Texture2D generate_boids_texture(Image image, Color clothes_tint) {
    // Get boids image
    Image boids_img = ImageCopy(image);
    ImageCrop(&boids_img, (Rectangle){0, 0,  image.width, 260-1}); // Crop only the boids sprites
    
    // Get a mask of the boids' clothes
    Image boids_mask = ImageCopy(image);
    ImageCrop(&boids_mask, (Rectangle){0, 260, image.width, 260-1}); // Crop only the mask sprites
    ImageAlphaClear(&boids_mask, BLACK, 0.5f); // Replace the transparent color with black

    // Get colored clothes image
    Image clothes_img = ImageCopy(boids_img);
    ImageAlphaMask(&clothes_img, boids_mask);
    ImageColorTint(&clothes_img, clothes_tint);
    UnloadImage(boids_mask);

    // Combine clothes and boids images
    if (boids_img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) ImageFormat(&boids_img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    ImageDraw(&boids_img, clothes_img, (Rectangle){0, 0, clothes_img.width, clothes_img.height},
              (Rectangle){0, 0, boids_img.width, boids_img.height}, WHITE);
    UnloadImage(clothes_img);

    // Generate texture
    Texture2D result_tex = LoadTextureFromImage(boids_img);
    UnloadImage(boids_img);

    return result_tex;
}

GameMenu game_loop(Texture2D texture, Texture2D boids_textures[], bool reset);
GameMenu main_menu(void);
GameMenu new_menu(void);
GameMenu join_menu(void);
GameMenu local_menu(void);
GameMenu loading_menu(void);

void prepare_context(bool local, bool new_room);
void start_net_thread(void);
void init_game(void);
int create_sockets(void);
void send_request_new(void);
void send_request_join(void);
void exit_game(void);

int main(int argc, char **argv) {
    // room/player settings, argparse
    bool print_help = false;
    char *prog = argv[0];

    if (argc > 1 && (strcmp(argv[1], "new") == 0 || strcmp(argv[1], "n") == 0)) {
        ctx.new_room = true;
        ctx.run_game = true;
    } else if (argc > 1 && (strcmp(argv[1], "join") == 0 || strcmp(argv[1], "j") == 0)) {
        ctx.new_room = false;
        ctx.run_game = true;
    } else if (argc > 1 && (strcmp(argv[1], "local") == 0 || strcmp(argv[1], "l") == 0)) {
        ctx.local_game = true;
        ctx.run_game = true;
    } else
        ctx.run_game = false;

    if (ctx.run_game) {
        argv++; argc--;
    }

    if (ctx.run_game)
        ctx.chunk_size = ctx.local_game ? ctx.chunk_size_local : ctx.chunk_size_multiplayer;

    while (--argc) {
        char *arg = *(++argv);

        if (arg[0] == '-') {
            int old_argc = argc;
            bool ext = false; // exit
            
            do {
                // Global flags
                if (strcmp(arg, "--help") == 0 || strncmp(arg, "-h", 2) == 0) {
                    print_help = true;
                    ext = true;
                    break;
                }

                // Flags for non- ./client local
                else if (!ctx.local_game && (strcmp(arg, "--server") == 0 || strcmp(arg, "-s") == 0)) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    strcpy(ctx.server, *(++argv));
                    argc--;
                } else if (!ctx.local_game && (strcmp(arg, "--tcp-port") == 0 || strcmp(arg, "-T") == 0)) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    char *value_str = *(++argv);
                    argc--;

                    char *endp;
                    ctx.tcp_port = strtoul(value_str, &endp, 10);
                    if (*endp != '\0') {
                        ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                    }
                } else if (!ctx.local_game && (strcmp(arg, "--udp-port") == 0 || strcmp(arg, "-U") == 0)) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    char *value_str = *(++argv);
                    argc--;

                    char *endp;
                    ctx.udp_port = strtoul(value_str, &endp, 10);
                    if (*endp != '\0') {
                        ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                    }
                } else if (!ctx.local_game && (strcmp(arg, "--name") == 0 || strcmp(arg, "-n") == 0)) {
                    if (argc == 1) ERRF("no value for option '%s'\n", arg);

                    strcpy(ctx.username, *(++argv));
                    argc--;
                }                    

                // Flags for ./client new|join|local
                else if (ctx.run_game) {
                    
                    if (strcmp(arg, "--chunk") == 0 || strcmp(arg, "-c") == 0) {
                        if (argc == 1) ERRF("no value for option '%s'\n", arg);

                        char *value_str = *(++argv);
                        argc--;

                        char *endp;
                        ctx.chunk_size = strtoul(value_str, &endp, 10);
                        if (*endp != '\0') {
                            ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                        }

                        if (ctx.chunk_size < BOID_SIZE) {
                            ERRF("size of chunk must be greater than or equal to %d\n", BOID_SIZE);
                        }

                        ctx.chunk_size = (ctx.chunk_size / BOID_SIZE) * BOID_SIZE;
                        if (ctx.local_game)
                            ctx.chunk_size_local = ctx.chunk_size;
                        else
                            ctx.chunk_size_multiplayer = ctx.chunk_size;
                    } else if (strcmp(arg, "--hide-gui") == 0 || strncmp(arg, "-i", 2) == 0) {
                        ctx.show_gui = false;
                    }

                    // Local game flags
                    else if (ctx.local_game) {
                        ERRF("unexpected argument '%s'\n", arg);
                    }

                    // Flags fow ./client new
                    else if (ctx.new_room) {
                        if (strcmp(arg, "--players") == 0 || strcmp(arg, "-p") == 0 || (arg[1] == 'p' && isdigit(arg[2]))) {
                            if (argc == 1 && !isdigit(arg[2])) ERRF("no value for option '%s'\n", arg);

                            char *value_str;
                            if (arg[1] == 'p' && strlen(arg) >= 3) {
                                value_str = arg+2;
                                arg += 1;
                            } else {
                                value_str = *(++argv);
                                argc--;
                            }

                            char *endp;
                            ctx.players_number = strtoul(value_str, &endp, 10);
                            if (*endp != '\0') {
                                ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                            }
                            if (ctx.players_number == 0 || ctx.players_number > 4) {
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
                            ctx.player_team = team;
                        } else if (strcmp(arg, "--world") == 0 || strcmp(arg, "-w") == 0) {
                            if (argc == 1) ERRF("no value for option '%s'\n", arg);

                            char *value_str = *(++argv);
                            argc--;
                            if (sscanf(value_str, "%dx%d", &ctx.world_size.x, &ctx.world_size.y) < 2) {
                                ERRF("illegal value '%s' for option '%s'\n", value_str, arg);
                            }
                            ctx.world_size.x = ceilf((float)ctx.world_size.x / BOID_SIZE) * BOID_SIZE;
                            ctx.world_size.y = ceilf((float)ctx.world_size.y / BOID_SIZE) * BOID_SIZE;
                        } else if (strcmp(arg, "--hide-areas") == 0 || strncmp(arg, "-a", 2) == 0) {
                            ctx.hide_areas = true;
                        } else {
                            ERRF("unexpected argument '%s'\n", arg);
                        }
                    }

                    // Flags fow ./client join
                    else {
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
        } else if (ctx.new_room) {
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
                ctx.boids_number[teams[i]] = boids;
        } else if (!ctx.new_room) {
            char *endp;
            ctx.room_id = strtoul(arg, &endp, 16);
            if (*endp != '\0') {
                ERRF("illegal value '%s' for option 'room'\n", arg);
            }
        } else {
            ERRF("unexpected argument '%s'\n", arg);
        }
    }

    if (print_help) {
        if (!ctx.run_game) { // ./client
            printf(
                "Usage: %s [COMMAND] [OPTIONS]\n"
                "\n"
                "A game's client app.\n"
                "\n"
                "Commands:\n"
                "  new - create a new game room (muiltiplayer mode)\n"
                "  join - join to an existing room (multiplayer mode)\n"
                "  local - local game mode\n"
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
                "\n"
                "Run '%s COMMAND --help' for command-specific help.\n",
                prog, DEFAULT_SERVER, TCP_PORT, UDP_PORT, DEFAULT_USERNAME, prog
            );
        } else if (ctx.local_game) { // ./client local
            printf(
                "Usage: %s local [OPTIONS]\n"
                "\n"
                "Run the game in local mode.\n"
                "\n"
                "Options:\n"
                "  -h, --help\n"
                "    Show this message and exit\n"
                "  -c, --chunk <NUM>\n"
                "    Size of chunk in pixels, rounded down to the nearest multiple of %d\n"
                "    (default: %d)\n"
                "  -i, --hide-gui\n"
                "    Do not show GUI buttons\n",
                prog, BOID_SIZE, DEFAULT_SERVER_CHUNK_SIZE_PIXELS);
        } else if (ctx.new_room) { // ./client new
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
                "  -i, --hide-gui\n"
                "    Do not show GUI buttons\n"
                "\n"
                "Examples:\n"
                "  %s new --name bebob --world 1050x1050 -p2 r:30 b:40\n"
                "  %s new -s 192.168.0.1 --chunk 525 -p4 r:b:1500 g:y:1000\n",
                prog, DEFAULT_SERVER, TCP_PORT, UDP_PORT, DEFAULT_USERNAME, BOID_SIZE, DEFAULT_CLIENT_CHUNK_SIZE_PIXELS, DEFAULT_PLAYERS_COUNT, BOID_SIZE,
                DEFAULT_WORLD_SIZE_X, DEFAULT_WORLD_SIZE_Y, prog, prog
            );
        } else { // ./client join
            printf(
                "Usage: %s new [OPTIONS] ROOM\n"
                "\n"
                "Join an existing room.\n"
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
                "  -i, --hide-gui\n"
                "    Do not show GUI buttons\n"
                "\n"
                "Examples:\n"
                "  %s join --name glug -c 525 015dc2\n"
                "  %s join -s 192.168.0.1 -T 1234 1012c4\n",
                prog, DEFAULT_SERVER, TCP_PORT, UDP_PORT, DEFAULT_USERNAME, BOID_SIZE, DEFAULT_CLIENT_CHUNK_SIZE_PIXELS, prog, prog
            );            
        }

        return 0;
    }

    if (ctx.new_room) {
        int teams_number = 0;
        for (int i = 0; i < TEAMS_COUNT; i++) {
            if (ctx.boids_number[i] > 0) {
                teams_number++;
                ctx.total_boids_number += ctx.boids_number[i];
            }
        }

        if (teams_number < ctx.players_number) {
            ERR("you have not set the number of boids for all players\n");
        } else if (teams_number > ctx.players_number) {
            ERRF("the number of players (%d) is not equal to the number of teams (%d)\n", ctx.players_number, teams_number);
        }
        
        if (ctx.total_boids_number > (ctx.world_size.x/BOID_SIZE)*(ctx.world_size.y/BOID_SIZE))
            ERRF("you won't be able to place %d boids in a %dx%d world\n", ctx.total_boids_number, ctx.world_size.x, ctx.world_size.y);
        if (ctx.total_boids_number > MAX_BOIDS_COUNT) {
            ERRF("the number of boids (%u) is greater than max boids count (%u)\n", ctx.total_boids_number, MAX_BOIDS_COUNT);
        }

        if (ctx.boids_number[ctx.player_team] == 0) {
            ERR("select valid team\n");
        }
    }
    
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
    
    // Init log
    init_cstack(ctx.log, MAX_LOG_LEN);

    // Run game for ./client new|join|local
    if (ctx.run_game) {
        if (!ctx.local_game)
            if (create_sockets())
                return 1;
        prepare_context(ctx.local_game, ctx.new_room);
        
        if (ctx.local_game) {
            init_game();
        } else {
            start_net_thread();
            if (ctx.new_room) send_request_new();
            else send_request_join();
        }
        
        ctx.menu = ctx.local_game ? MENU_GAME : MENU_LOADING;
    } else {
        ctx.menu = MENU_MAIN;
    }
    
    // Init Raylib
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_FULLSCREEN_MODE);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Battle creator");
    SetTargetFPS(60);
    SetExitKey(0);

    // Textures
    Image image = LoadImage("resources/texture.png");
    Texture2D texture = LoadTextureFromImage(image);
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    // Get textures of boids
    Color teams_colors[TEAMS_COUNT] = {
        [TEAM_RED] = RED,
        [TEAM_BLUE] = BLUE,
        [TEAM_GREEN] = GREEN,
        [TEAM_YELLOW] = YELLOW,
    };
    Texture2D boids_textures[TEAMS_COUNT];
    for (int team = 0; team < TEAMS_COUNT; team++) {
        boids_textures[team] = generate_boids_texture(image, teams_colors[team]);
        GenTextureMipmaps(&boids_textures[team]);
        SetTextureFilter(boids_textures[team], TEXTURE_FILTER_TRILINEAR);
    }
    UnloadImage(image);

    bool exit_window = false, show_exit_message = false;

    while (!exit_window) {
        // if (WindowShouldClose()) show_exit_message = true;
        if (WindowShouldClose()) exit_window = true;
        if (IsKeyPressed(KEY_ESCAPE)) show_exit_message = !show_exit_message;
        
        ctx.screen_width = GetScreenWidth();
        ctx.screen_height = GetScreenHeight();
        ctx.mouse_position = GetScreenToWorld2D(GetMousePosition(), ctx.camera);
        ctx.mouse_wheel = GetMouseWheelMove();
        ctx.mouse_delta = GetMouseDelta();
        
        BeginDrawing();
        
        ClearBackground(RAYWHITE);

        if (show_exit_message)
            GuiLock();
        
        // Draw menu
        GameMenu next_menu = 0;
        switch (ctx.menu) {
            case MENU_MAIN: next_menu = main_menu(); break;
            case MENU_NEW: next_menu = new_menu(); break;
            case MENU_JOIN: next_menu = join_menu(); break;
            case MENU_LOCAL: next_menu = local_menu(); break;
            case MENU_LOADING: next_menu = loading_menu(); break;
            case MENU_GAME: next_menu = game_loop(texture, boids_textures, ctx.reset_game); ctx.reset_game = false; break;
            default: break;
        }

        pthread_mutex_lock(&next_menu_mtx);
        
        // Change menu
        
        if (ctx.change_menu) {
            next_menu = ctx.next_menu;
            ctx.change_menu = false;
        }
        
        pthread_mutex_unlock(&next_menu_mtx);
        
        if (next_menu == MENU_EXIT) // Exit from the game
            break;
        else if (next_menu == MENU_NULL) // Stay in the current menu
            ;
        else if (next_menu != ctx.menu) { // Change menu
            if ((ctx.menu == MENU_GAME || ctx.menu == MENU_LOADING) && next_menu == MENU_MAIN)
                exit_game();
            if (next_menu == MENU_GAME)
                init_game();
            ctx.menu = next_menu;

            // Hide message if save_message != true
            if (ctx.save_message)
                ctx.save_message = false;
            else
                ctx.message_text = NULL;
        }

        GuiSetState(STATE_NORMAL);
        GuiDisableTooltip();
        
        // Draw menu message
        if (ctx.message_text != NULL && ctx.menu != MENU_GAME) {
            Color msg_color = BLACK;
            int msg_icon = ICON_NONE;
            switch (ctx.message_type) {
                case MESSAGE_INFO: msg_color = BLUE; msg_icon = ICON_INFO_BOX; break;
                case MESSAGE_WARNING: msg_color = ORANGE; msg_icon = ICON_WARNING; break;
                case MESSAGE_ERROR: msg_color = RED; msg_icon = ICON_CIRCLE_WARNING_FILL; break;
            }

            Rectangle msg_rectangle = (Rectangle){0, ctx.screen_height - 30 - 10, ctx.screen_width, 30};

            GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
            GuiSetIconScale(2);
            STYLE_START(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(msg_color));
            STYLE_START(DEFAULT, TEXT_SIZE, 20);
            GuiLabel(msg_rectangle, GuiIconText(msg_icon, ctx.message_text));
            STYLE_END();
            STYLE_END();
            GuiSetIconScale(1);

            if (ctx.message_type == MESSAGE_INFO) {
                if (CheckCollisionPointRec(GUI_POINTER_POSITION, msg_rectangle) && GUI_BUTTON_PRESSED)
                    ctx.message_text = NULL;
            }
        }
        GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
        
        // Display a message before exiting
        if (show_exit_message) {
            GuiUnlock();
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(RAYWHITE, 0.8f));
            int btn_active = -1;
            GuiMessageBox((Rectangle){ (float)GetScreenWidth()/2 - 125, (float)GetScreenHeight()/2 - 50, 250, 100 }, 
                GuiIconText(ICON_EXIT, "Close Window"), "Do you really want to exit?", "Yes;No", &btn_active);

            if ((btn_active == 0) || (btn_active == 2)) show_exit_message = false;
            else if (btn_active == 1 || IsKeyPressed(KEY_ENTER)) exit_window = true;
        }

        EndDrawing();
    }

    log_message(&ctx.log, L_INFO, "exit");
    
    if (ctx.menu == MENU_GAME || ctx.menu == MENU_LOADING)
        exit_game();

    #ifdef _WIN32
        WSACleanup();
    #endif

    free(ctx.log.items);
    ctx.log.items = NULL;
    
    // Close Raylib
    UnloadTexture(texture);
    for (int team = 0; team < TEAMS_COUNT; team++)
        UnloadTexture(boids_textures[team]);
    CloseWindow();

    return 0;
}

int create_sockets(void) {
    // Create a TCP socket
    bool tcp_opened = false;
    ctx.tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx.tcp_fd < 0) {
        perror("socket SOCK_STREAM");
        return 1;
    }

    #ifndef _WIN32
        int opt = 1;
    #else
        char opt = 1;
    #endif
    if (setsockopt(ctx.tcp_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
        perror("setsockopt TCP_NODELAY");
        goto tcp_fail;
    }

    struct sockaddr_in tcp_servaddr = { 0 };
    tcp_servaddr.sin_family = AF_INET;
    tcp_servaddr.sin_port = htons(ctx.tcp_port);
    socklen_t tcp_addrlen = sizeof(tcp_servaddr);

    // Convert IPv4 address from text to binary form
    if (inet_pton(AF_INET, ctx.server, &tcp_servaddr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        goto tcp_fail;
    }

    if (connect(ctx.tcp_fd, (struct sockaddr*)&tcp_servaddr, tcp_addrlen) < 0) {
        perror("connect");
        goto tcp_fail;
    }

    write_log(L_DEBUG, "opened a TCP socket to %s:%d\n", ctx.server, ctx.tcp_port);
    tcp_opened = true;

    tcp_fail: {
        if (!tcp_opened) {
            write_log(L_ERROR, "failed to open a TCP socket to %s:%d\n", ctx.server, ctx.tcp_port);
            close(ctx.tcp_fd);
            #ifdef _WIN32
                WSACleanup();
            #endif
            return 1;
        }
    }

    // Create a UDP socket
    ctx.udp_opened = false;
    ctx.udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.udp_fd < 0) {
        perror("socket SOCK_DGRAM");
        goto udp_fail;
   }

    ctx.udp_servaddr.sin_family = AF_INET;
    ctx.udp_servaddr.sin_port = htons(ctx.udp_port);

    // Convert IPv4  address from text to binary form
    if (inet_pton(AF_INET, ctx.server, &ctx.udp_servaddr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        goto udp_fail;
    }

    write_log(L_DEBUG, "opened a UDP socket to %s:%d\n", ctx.server, ctx.udp_port);
    ctx.udp_opened = true;

    udp_fail: {
        if (!ctx.udp_opened) {
            write_log(L_WARNING, "failed to open a UDP socket to %s:%d\n", ctx.server, ctx.udp_port);
            close(ctx.udp_fd);
        }
    }

    // Make sockets nonblocking
    #ifdef _WIN32
        u_long flag = 1;
        if (ioctlsocket(ctx.tcp_fd, FIONBIO, &flag) != 0) {
            write_log(L_ERROR, "failed to make a TCP socket nonblocking\n");
            perror("ioctlsocket FIONBIO");
            close(ctx.tcp_fd);
            if (ctx.udp_opened)
                close(ctx.udp_fd);
            WSACleanup();
            return 1;
        }
        if (ctx.udp_opened && ioctlsocket(ctx.udp_fd, FIONBIO, &flag) != 0) {
            write_log(L_WARNING, "failed to make a UDP socket nonblocking\n");
            perror("ioctlsocket FIONBIO");
            close(ctx.udp_fd);
            ctx.udp_opened = false;
        }
    #else
        int flags = fcntl(ctx.tcp_fd, F_GETFL, 0);
        if (fcntl(ctx.tcp_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            write_log(L_ERROR, "failed to make a TCP socket nonblocking\n");
            perror("fcntl O_NONBLOCK");
            close(ctx.tcp_fd);
            if (ctx.udp_opened)
                close(ctx.udp_fd);
            return 1;
        }
        if (ctx.udp_opened) {
            int flags = fcntl(ctx.udp_fd, F_GETFL, 0);
            if (fcntl(ctx.udp_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                write_log(L_ERROR, "failed to make a UDP socket nonblocking\n");
                perror("fcntl O_NONBLOCK");
                close(ctx.udp_fd);
                ctx.udp_opened = false;
            }
        }
    #endif
    
    return 0;
}

// Prepare the context for the game
void prepare_context(bool local, bool new_room) {
    if (ctx.game_initialized)
        return;
    
    ctx.running = true;

    ctx.approved_player_id = 0;
    ctx.areas_count = 0;

    ctx.brush_size = 1;
    ctx.action = ACT_STOP;
    ctx.tps_display_type = TPS_NUM;
    ctx.selecting_team = TEAM_RED;
    ctx.selecting = false;
    ctx.select_mode = false;
    ctx.clear_order = false;
    ctx.change_boids_action = false;
    ctx.change_boids_direction = false;

    ctx.show_log = true;
    ctx.show_grid = false;
    ctx.show_health = false;
    ctx.game_paused = false;
    ctx.show_arrow = false;
    ctx.autoselect_mode = true;
    
    ctx.get_input = false;
    ctx.input_received = false;
    ctx.typing_keyboard_input = false;
    
    ctx.local_game = local;
    ctx.new_room = new_room;
    
    if (ctx.local_game) {
        ctx.stage = STAGE_GAME;
        ctx.mode = MODE_SPAWN;
        ctx.world_size.x = DEFAULT_WORLD_SIZE_X;
        ctx.world_size.y = DEFAULT_WORLD_SIZE_Y;
    } else if (ctx.new_room) {
        ctx.stage = STAGE_AREAS;
        ctx.mode = MODE_AREAS;
    } else {
        ctx.stage = STAGE_AREAS;
        ctx.mode = MODE_WAIT;
    }

    CLEAR_EVENTS();
    CLEAR_GUI_EVENTS();
    CLEAR_MOD();
    
    clear_cstack(ctx.log);
    clear_menu_message();

    ctx.game_initialized = false;
    ctx.net_thread_running = false;
    ctx.reset_game = true;
}

// Start a thread to receive messages from the server
void start_net_thread(void) {
    if (ctx.local_game || ctx.net_thread_running)
        return;

    pthread_mutex_init(&areas_mtx, NULL);
    pthread_mutex_init(&boids_mtx, NULL);
    pthread_mutex_init(&running_mtx, NULL);
    pthread_mutex_init(&next_menu_mtx, NULL);
    pthread_mutex_init(&input_mtx, NULL);
    pthread_mutex_init(&players_mtx, NULL);
    pthread_create(&net_thread, NULL, net_thread_fn, NULL);
    ctx.net_thread_running = true;
}

void init_game(void) {
    if (ctx.game_initialized)
        return;
    
    // Camera
    ctx.camera.zoom = 1.0f;
    ctx.camera.target = (Vector2){ctx.world_size.x/2.0 - GetScreenWidth()/2.0, ctx.world_size.y/2.0 - GetScreenHeight()/2.0};

    // Boids
    if (ctx.local_game) {
        ctx.boids = calloc(MAX_BOIDS_COUNT, sizeof(*ctx.boids));
        ctx.order_parts = calloc(MAX_BOIDS_COUNT, sizeof(*ctx.order_parts));
    } else {
        ctx.boids = calloc(ctx.total_boids_number, sizeof(*ctx.boids));
    }
    ctx.boids_count = 0;

    // Grid of chunks
    INIT_GRID(&ctx.grid, ctx.boids, ctx.boids_count, ctx.world_size.x, ctx.world_size.y, ctx.chunk_size);

    // Logging
    clear_cstack(ctx.log);
    if (ctx.local_game) {
        log_message(&ctx.log, L_INFO, "local game:\n     chunk: %d\n", ctx.chunk_size);
    } else {
        log_message(&ctx.log, L_INFO, "%s\n     id: %06x\n     teams: %d\n     world: %dx%d\n     chunk: %d\n     server tps: %d\n     creator: %s\n"
                        "     boids:  %-4d\n     red:    %-4d\n     blue:   %-4d\n     green:  %-4d\n     yellow: %-4d\n",
               ctx.new_room? "created a room" : "joined to the room",
               ctx.room_id, ctx.players_number, ctx.world_size.x, ctx.world_size.y, ctx.chunk_size, ctx.server_target_tps, ctx.players[0].name, ctx.total_boids_number,
               ctx.boids_number[TEAM_RED],
               ctx.boids_number[TEAM_BLUE],
               ctx.boids_number[TEAM_GREEN],
               ctx.boids_number[TEAM_YELLOW]);

        if (!ctx.new_room) {
            char players_info[LOG_BUF_SIZE] = "players:\n";
            for (int i = 0; i < ctx.joined_players; i++) {
                ClientPlayer *op = &ctx.players[i];

                const char *team = get_team_name(op->team);

                snprintf(players_info+strlen(players_info), sizeof(players_info), "  %s - %s\n", op->name, team);
            }
            log_message(&ctx.log, L_INFO, "%s", players_info);
        }

        log_message(&ctx.log, L_INFO, "your username is '%s' and your team is %s\n", ctx.username, TextToUpper(get_team_name(ctx.player_team)));
    }

    ctx.game_initialized = true;
}

void send_request_new(void) {
    ctx.chunk_size = ctx.chunk_size_multiplayer;

    /* CP_NEW_ROOM
        (uint8 player_team) (uint8 players_number) (uint8 hide_areas) (uint16 world_size_x) (uint16 world_size_y)
    (uint16[TEAMS_COUNT] boids_number) (uint8[USERNALE_LEN] creator)
    */

    const uint32_t packet_size = 1 + 1 + 1 + 2 + 2 + 2*TEAMS_COUNT + USERNAME_LEN;
    char data[packet_size];
    char *d = data;

    PUSH_DATA(d, uint8_t, ctx.player_team);
    PUSH_DATA(d, uint8_t, ctx.players_number);
    PUSH_DATA(d, uint8_t, ctx.hide_areas);
    PUSH_DATA(d, uint16_t, htons(ctx.world_size.x));
    PUSH_DATA(d, uint16_t, htons(ctx.world_size.y));
    for (int i = 0; i < TEAMS_COUNT; i++) {
        PUSH_DATA(d, uint16_t, htons(ctx.boids_number[i]));
    }
    PUSH_MEM(d, ctx.username, USERNAME_LEN);

    send_packet(ctx.tcp_fd, CP_NEW_ROOM, data, packet_size, 0);
}

void send_request_join(void) {
    /* CP_JOIN_ROOM PACKET FORMAT
    (uint32 room_id) (uint8[USERNAME_LEN] username)
    */

    const uint32_t packet_size = 4 + USERNAME_LEN;
    char data[packet_size];
    char *d = data;

    PUSH_DATA(d, uint32_t, htonl(ctx.room_id));
    PUSH_MEM(d, ctx.username, USERNAME_LEN);

    send_packet(ctx.tcp_fd, CP_JOIN_ROOM, data, packet_size, 0);
}

void exit_game(void) {
    if (!ctx.game_initialized)
        return;
    
    if (!ctx.local_game && ctx.net_thread_running) {
        // Close the second (network) thread
        // pthread_cancel(net_thread);
        pthread_mutex_lock(&running_mtx);
        ctx.running = false;
        pthread_mutex_unlock(&running_mtx);
        pthread_join(net_thread, NULL);

        // Close socket
        close(ctx.tcp_fd);
        if (ctx.udp_opened)
            close(ctx.udp_fd);

        // Destroy all mutexes and conditions
        pthread_mutex_destroy(&areas_mtx);
        pthread_mutex_destroy(&boids_mtx);
        pthread_mutex_destroy(&running_mtx);
        pthread_mutex_destroy(&next_menu_mtx);
        pthread_mutex_destroy(&input_mtx);
        pthread_mutex_destroy(&players_mtx);
    }
    ctx.net_thread_running = false;

    // Free allocated memory
    
    free_grid(&ctx.grid);
    free(ctx.boids);
    if (ctx.local_game)
        free(ctx.order_parts);

    ctx.room_id = 0;
    
    ctx.game_initialized = false;
}


/* <================================================== MENU AND GUI ==================================================> */

#define ITEM_WIDTH 500
#define ITEM_HEIGHT 30
#define ITEM_SPACING 15
#define ITEM_INNER_SPACING 5
#define LABEL_SPACING 5
#define CHECKBOX_SIZE 20
#define CHECKBOX_OFFSET -10

GameMenu main_menu(void) {
    const int items_number = 3;
    int y = ctx.screen_height / 2 - (ITEM_HEIGHT*items_number + ITEM_SPACING*(items_number-1)) / 2;

    GameMenu next_menu = 0;

    STYLE_START(DEFAULT, TEXT_SIZE, 20);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
    
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, y, ITEM_WIDTH, ITEM_HEIGHT}, "New room")) next_menu = MENU_NEW;
    y += ITEM_HEIGHT + ITEM_SPACING;

    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, y, ITEM_WIDTH, ITEM_HEIGHT}, "Join room")) next_menu = MENU_JOIN;
    y += ITEM_HEIGHT + ITEM_SPACING;

    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, y, ITEM_WIDTH, ITEM_HEIGHT}, "Local game")) next_menu = MENU_LOCAL;
    y += ITEM_HEIGHT + ITEM_SPACING;
    
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    STYLE_END(); // TEXT_SIZE
    
    return next_menu;
}

#define ITEM_X ctx.screen_width/2.0f - ITEM_WIDTH/2.0f + __label_width + LABEL_SPACING + __margin
#define ITEM_W (ITEM_WIDTH - __label_width - LABEL_SPACING - __margin)
#define ITEM(n, x, y, ...)                                                                                               \
    do {                                                                                                                 \
        int __label_width = MeasureText(n ": ", 20);                                                                     \
        int __margin = (x);                                                                                              \
        STYLE_START(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);                                                           \
        GuiLabel((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f + __margin, (y), ITEM_WIDTH, ITEM_HEIGHT}, n ": "); \
        STYLE_END();                                                                                                     \
        __VA_ARGS__                                                                                                      \
    } while (0)

#define INFOBOX_WIDTH 30
// Draw an "info" box with tooltip
void draw_info(int x, int y, const char *text) {
    Rectangle info_rec = {x, y, INFOBOX_WIDTH, ITEM_HEIGHT};
    GuiLabel(info_rec, GuiIconText(ICON_INFO_BOX, ""));
    
    bool show_tooltip = CheckCollisionPointRec(GUI_POINTER_POSITION, info_rec);
    GuiSetState(show_tooltip ? STATE_FOCUSED : STATE_NORMAL);
    
    if (show_tooltip) {
        bool old_tooltip_state = guiTooltip;
        GuiEnableTooltip();
        GuiSetTooltip(text);
        
        STYLE_START(DEFAULT, TEXT_SIZE, 10);
        GuiTooltip((Rectangle){info_rec.x + INFOBOX_WIDTH + 5, info_rec.y, 30, 1});
        STYLE_END();
        if (!old_tooltip_state)
            GuiDisableTooltip();
    }

    GuiSetState(STATE_NORMAL);
}

GameMenu new_menu(void) {
    const int items_number = 10;
    int y = ctx.screen_height / 2 - (ITEM_HEIGHT*items_number + ITEM_SPACING*(items_number-1)) / 2;

    GameMenu next_menu = 0;

    bool active_dropdown = false;  // true if at least one GuiDropdownBox is active

    static bool active_gui = true; // false if active_dropdown is true;
                                   // All items that may be under GuiDropdownBox should be locked using GuiLock() function
                                   // when active_gui is false

    STYLE_START(DEFAULT, TEXT_SIZE, 20);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
    
    GuiLabel((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, MAX(MIN(ctx.screen_height/4.0f, y - ITEM_HEIGHT - ITEM_SPACING), 0), ITEM_WIDTH, ITEM_HEIGHT}, "Create a new game room");
    
    ITEM("Username", 0, y, {
        static bool username_textbox_mode = false;
        
        STYLE_START(TEXTBOX, TEXT_ALIGNMENT, username_textbox_mode ? TEXT_ALIGN_LEFT : TEXT_ALIGN_CENTER);
        if (GuiTextBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, ctx.username, USERNAME_LEN, username_textbox_mode))
            username_textbox_mode = !username_textbox_mode;
        STYLE_END();
        
        y += ITEM_HEIGHT + ITEM_SPACING;
    });
    
    ITEM("Server IP", 0, y, {
        static bool server_textbox_mode = false;
        
        STYLE_START(TEXTBOX, TEXT_ALIGNMENT, server_textbox_mode ? TEXT_ALIGN_LEFT : TEXT_ALIGN_CENTER);
        if (GuiTextBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, ctx.server, INET_ADDRSTRLEN, server_textbox_mode))
            server_textbox_mode = !server_textbox_mode;
        STYLE_END();

        y += ITEM_HEIGHT + ITEM_INNER_SPACING;
    });

    const int port_textbox_width = 70;
    
    ITEM("TCP port", 50, y, {
        static bool tcp_valuebox_mode = false;
        
        if (GuiValueBox((Rectangle){ITEM_X, y, port_textbox_width, ITEM_HEIGHT}, NULL, &ctx.tcp_port, 0, 65535, tcp_valuebox_mode))
            tcp_valuebox_mode = !tcp_valuebox_mode;
        
        y += ITEM_HEIGHT + ITEM_INNER_SPACING;
    });

    ITEM("UDP port", 50, y, {
        static bool udp_valuebox_mode = false;

        if (GuiValueBox((Rectangle){ITEM_X, y, port_textbox_width, ITEM_HEIGHT}, NULL, &ctx.udp_port, 0, 65535, udp_valuebox_mode))
            udp_valuebox_mode = !udp_valuebox_mode;

        y += ITEM_HEIGHT + ITEM_SPACING;
    });

    ITEM("Chunk size", 0, y, {
        static bool chunk_spinner_mode = false;
        
        if (GuiValueBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, NULL, &ctx.chunk_size_multiplayer, BOID_SIZE, DEFAULT_SERVER_CHUNK_SIZE_PIXELS, chunk_spinner_mode)) {
            ctx.chunk_size = (ctx.chunk_size / BOID_SIZE) * BOID_SIZE;
            chunk_spinner_mode = !chunk_spinner_mode;
        }
        
        y += ITEM_HEIGHT + ITEM_INNER_SPACING;
    });
    
    ITEM("World", 0, y, {
        static bool worldx_spinner_mode = false;
        static bool worldy_spinner_mode = false;
        const int x_label_width = 30;

        if (GuiValueBox((Rectangle){ITEM_X, y, ITEM_W/2.0f - x_label_width/2.0f, ITEM_HEIGHT}, NULL, &ctx.world_size.x, BOID_SIZE, (65535/BOID_SIZE)*BOID_SIZE, worldx_spinner_mode)) {
            ctx.world_size.x = ceilf((float)ctx.world_size.x / BOID_SIZE) * BOID_SIZE;
            worldx_spinner_mode = !worldx_spinner_mode;
        }
        
        GuiLabel((Rectangle){ITEM_X + ITEM_W/2.0f - x_label_width/2.0f, y, x_label_width, ITEM_HEIGHT}, "x");
        if (GuiValueBox((Rectangle){ITEM_X + ITEM_W/2.0f + x_label_width/2.0f, y, ITEM_W/2.0f - x_label_width/2.0f, ITEM_HEIGHT}, NULL, &ctx.world_size.y, BOID_SIZE, (65535/BOID_SIZE)*BOID_SIZE, worldy_spinner_mode)) {
            ctx.world_size.y = ceilf((float)ctx.world_size.y / BOID_SIZE) * BOID_SIZE;
            worldy_spinner_mode = !worldy_spinner_mode;
        }
        
        y += ITEM_HEIGHT + ITEM_SPACING;
    });

    STYLE_START(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    
    ITEM("Players number", 0, y, {
        GuiLabel((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, TextFormat("%d", ctx.players_number));
        y += ITEM_HEIGHT + ITEM_SPACING;
    });
    STYLE_END();

    // Reserve place for "Boids" item
    int boids_dropdown_y = y;
    static int teams_count = 0;
    y += ITEM_HEIGHT*(teams_count + 1) + ITEM_INNER_SPACING*teams_count + ITEM_SPACING;

    // Reserve place for "Team" item
    int team_dropdown_y = y;
    y += ITEM_HEIGHT + ITEM_SPACING;
    
    ITEM("Hide areas", 0, y, {
        if (!active_gui) GuiLock(); // Lock this item when any GuiDropdownBox is active
        GuiCheckBox((Rectangle){ITEM_X + CHECKBOX_OFFSET, y + ITEM_HEIGHT/2.0f - CHECKBOX_SIZE/2.0f, CHECKBOX_SIZE, CHECKBOX_SIZE}, NULL, &ctx.hide_areas);
        draw_info(ITEM_X + CHECKBOX_OFFSET + CHECKBOX_SIZE + ITEM_INNER_SPACING, y, "Do not show areas to otrher players while admin player draws them");
        GuiUnlock();

        y += ITEM_HEIGHT + ITEM_SPACING;
    });

    // Get warning text
    const char *warning_text = NULL;
    if (ctx.username[0] == '\0')
        warning_text = "Enter username";
    else if (ctx.total_boids_number == 0)
        warning_text = "The number of boids must be greater than 0";
    else if (ctx.players_number < 2)
        warning_text = "There must be at least 2 players in the game";
    else if (teams_count != ctx.players_number)
        warning_text = "Teams in the \"Boids\" fields should not be repeated";
    else if (ctx.boids_number[ctx.player_team] == 0)
        warning_text = "Select valid team in the \"Team\" field";
    else if (ctx.total_boids_number > (ctx.world_size.x/BOID_SIZE)*(ctx.world_size.y/BOID_SIZE))
        warning_text = TextFormat("You won't be able to place %d boids in a %dx%d world", ctx.total_boids_number, ctx.world_size.x, ctx.world_size.y);
    else if (ctx.total_boids_number > MAX_BOIDS_COUNT)
        warning_text = TextFormat("Number of boids (%u) is greater than max boids count (%u)", ctx.total_boids_number, MAX_BOIDS_COUNT);
    set_menu_message(MESSAGE_WARNING, false, warning_text);
    
    // "Back" and "Create" buttons
    const int back_btn_width = ITEM_HEIGHT; // Square button
    if (!active_gui) GuiLock(); // Lock items when any GuiDropdownBox is active
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, y, back_btn_width, ITEM_HEIGHT}, GuiIconText(ICON_EXIT, "")))
        next_menu = MENU_MAIN;
    GuiSetState((warning_text == NULL) ? STATE_NORMAL : STATE_DISABLED);
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f + back_btn_width + ITEM_INNER_SPACING, y, ITEM_WIDTH - back_btn_width - ITEM_INNER_SPACING, ITEM_HEIGHT}, "Create"))
        next_menu = MENU_LOADING;
    y += ITEM_HEIGHT + ITEM_SPACING;
    GuiSetState(STATE_NORMAL);
    GuiUnlock();
    
    // GuiDropdownBox must draw after any other control that can be covered on unfolding
    y = team_dropdown_y;
    ITEM("Team", 0, y, {
        const int dropdown_width = 100;
        static bool team_dropdown_mode = false;

        if (!active_gui && !team_dropdown_mode) GuiLock(); // Lock this GuiDropdownBox when any other GuiDropdownBox is active
        
        if (GuiDropdownBox((Rectangle){ITEM_X, y, dropdown_width, ITEM_HEIGHT}, TEAMS_LIST, (int*)&ctx.player_team, team_dropdown_mode)) {
            team_dropdown_mode = !team_dropdown_mode;
        }
        if (team_dropdown_mode) active_dropdown = true;
        draw_info(ITEM_X + dropdown_width + ITEM_INNER_SPACING, y, "Your team at the beginning");

        GuiUnlock();
    });
    
    y = boids_dropdown_y;
    ITEM("Boids", 0, y, {
        const int dropdown_width = 100;
        const int btn_width = ITEM_HEIGHT; // Square "+" and "-" buttons
        
        static struct {
            int selected_team;
            int boids_number;
            bool dropdown_mode, valuebox_mode;
            bool deleted;
        } teams[TEAMS_COUNT] = { 0 };

        // [+] button
        if (!active_gui) GuiLock();
        bool add_new = false;
        y += ITEM_HEIGHT*teams_count + ITEM_INNER_SPACING*teams_count;
        GuiSetState((teams_count < TEAMS_COUNT) ? STATE_NORMAL : STATE_DISABLED);
        if (GuiButton((Rectangle){ITEM_X, y, btn_width, ITEM_HEIGHT}, "+"))
            add_new = true;
        GuiUnlock();
        y -= ITEM_HEIGHT + ITEM_INNER_SPACING;
        
        // Draw in reverse order
        bool delete = false;
        GuiSetState(STATE_NORMAL);
        for (int i = teams_count-1; i >= 0; i--) {
            if (!active_gui && !teams[i].dropdown_mode) GuiLock(); // Lock this GuiDropdownBox when any other GuiDropdownBox is active
            if (GuiDropdownBox((Rectangle){ITEM_X, y, dropdown_width, ITEM_HEIGHT}, TEAMS_LIST, &teams[i].selected_team, teams[i].dropdown_mode))
                teams[i].dropdown_mode = !teams[i].dropdown_mode;
            if (teams[i].dropdown_mode) active_dropdown = true;
            GuiUnlock();

            if (GuiSpinner((Rectangle){ITEM_X + dropdown_width + ITEM_INNER_SPACING, y, ITEM_W - dropdown_width - ITEM_INNER_SPACING*2 - btn_width, ITEM_HEIGHT}, NULL, &teams[i].boids_number, 0, MAX_BOIDS_COUNT, teams[i].valuebox_mode))
                teams[i].valuebox_mode = !teams[i].valuebox_mode;

            if (GuiButton((Rectangle){ITEM_X + ITEM_W - btn_width, y, btn_width, ITEM_HEIGHT}, "-")) {
                teams[i].deleted = true;
                delete = true;
            }
            
            y -= ITEM_HEIGHT + ITEM_INNER_SPACING;
        }

        if (add_new && teams_count < TEAMS_COUNT) {
            memset(&teams[teams_count], 0, sizeof(teams[0]));
            teams_count++;
        }
        
        if (delete) {
            int offset = 0;
            for (int i = 0; i < teams_count; i++) {
                teams[i - offset] = teams[i];
                if (teams[i].deleted)
                    offset++;
            }
            teams_count -= offset;
        }

        bool teams_used[TEAMS_COUNT] = { 0 };
        
        ctx.total_boids_number = 0;
        ctx.players_number = 0;
        for (int i = 0; i < teams_count; i++) {
            ctx.boids_number[teams[i].selected_team] = teams[i].boids_number;
            ctx.total_boids_number += teams[i].boids_number;
            if (!teams_used[teams[i].selected_team] && teams[i].boids_number > 0)
                ctx.players_number++;
            teams_used[teams[i].selected_team] = true;
        }
        
    });
    
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    STYLE_END(); // TEXT_SIZE

    active_gui = !active_dropdown;

    if (next_menu == MENU_LOADING) {
        if (create_sockets()) {
            set_menu_message(MESSAGE_ERROR, true, "Failed to connect to the server");
            return MENU_MAIN;
        }
        prepare_context(/*local=*/ false, /*new_room=*/ true);
        start_net_thread();
        send_request_new();
    }
    
    return next_menu;
}

GameMenu join_menu(void) {
    const int items_number = 7;
    int y = ctx.screen_height / 2 - (ITEM_HEIGHT*items_number + ITEM_SPACING*(items_number-1)) / 2;

    GameMenu next_menu = 0;

    STYLE_START(DEFAULT, TEXT_SIZE, 20);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
    
    const char *warning_text = NULL;
    
    GuiLabel((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, ctx.screen_height/4.0f, ITEM_WIDTH, ITEM_HEIGHT}, "Join a game room");
    
    ITEM("Username", 0, y, {
        static bool username_textbox_mode = false;
        
        STYLE_START(TEXTBOX, TEXT_ALIGNMENT, username_textbox_mode ? TEXT_ALIGN_LEFT : TEXT_ALIGN_CENTER);
        if (GuiTextBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, ctx.username, USERNAME_LEN, username_textbox_mode))
            username_textbox_mode = !username_textbox_mode;
        STYLE_END();
        
        y += ITEM_HEIGHT + ITEM_SPACING;
    });
    
    ITEM("Server IP", 0, y, {
        static bool server_textbox_mode = false;
        
        STYLE_START(TEXTBOX, TEXT_ALIGNMENT, server_textbox_mode ? TEXT_ALIGN_LEFT : TEXT_ALIGN_CENTER);
        if (GuiTextBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, ctx.server, INET_ADDRSTRLEN, server_textbox_mode))
            server_textbox_mode = !server_textbox_mode;
        STYLE_END();

        y += ITEM_HEIGHT + ITEM_INNER_SPACING;
    });

    const int port_textbox_width = 70;

    ITEM("TCP port", 50, y, {
        static bool tcp_valuebox_mode = false;
        
        if (GuiValueBox((Rectangle){ITEM_X, y, port_textbox_width, ITEM_HEIGHT}, NULL, &ctx.tcp_port, 0, 65535, tcp_valuebox_mode))
            tcp_valuebox_mode = !tcp_valuebox_mode;
        
        y += ITEM_HEIGHT + ITEM_INNER_SPACING;
    });

    ITEM("UDP port", 50, y, {
        static bool udp_valuebox_mode = false;

        if (GuiValueBox((Rectangle){ITEM_X, y, port_textbox_width, ITEM_HEIGHT}, NULL, &ctx.udp_port, 0, 65535, udp_valuebox_mode))
            udp_valuebox_mode = !udp_valuebox_mode;

        y += ITEM_HEIGHT + ITEM_SPACING;
    });
    
    ITEM("Chunk size", 0, y, {
        static bool chunk_spinner_mode = false;
        
        if (GuiValueBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, NULL, &ctx.chunk_size_multiplayer, BOID_SIZE, DEFAULT_SERVER_CHUNK_SIZE_PIXELS, chunk_spinner_mode)) {
            ctx.chunk_size = (ctx.chunk_size / BOID_SIZE) * BOID_SIZE;
            chunk_spinner_mode = !chunk_spinner_mode;
        }
        
        y += ITEM_HEIGHT + ITEM_SPACING;
    });
    
    ITEM("Room ID", 0, y, {
        static bool room_textbox_mode = false;
        static char room_id[6+1] = "000000"; // 6-digit hex number + '\0'

        STYLE_START(TEXTBOX, TEXT_ALIGNMENT, room_textbox_mode ? TEXT_ALIGN_LEFT : TEXT_ALIGN_CENTER);
        if (GuiTextBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, room_id, sizeof(room_id), room_textbox_mode))
            room_textbox_mode = !room_textbox_mode;
        STYLE_END();

        char *endp;
        ctx.room_id = strtoul(room_id, &endp, 16);
        if (*endp != '\0') {
            warning_text = "Invalid room ID";
        }
        
        y += ITEM_HEIGHT + ITEM_SPACING;
    });
    
    set_menu_message(MESSAGE_WARNING, false, warning_text);
    
    // "Back" and "Join" buttons
    const int back_btn_width = ITEM_HEIGHT; // Square button
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, y, back_btn_width, ITEM_HEIGHT}, GuiIconText(ICON_EXIT, "")))
        next_menu = MENU_MAIN;
    GuiSetState((warning_text == NULL) ? STATE_NORMAL : STATE_DISABLED);
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f + back_btn_width + ITEM_INNER_SPACING, y, ITEM_WIDTH - back_btn_width - ITEM_INNER_SPACING, ITEM_HEIGHT}, "Join"))
        next_menu = MENU_LOADING;
    y += ITEM_HEIGHT + ITEM_SPACING;
    GuiSetState(STATE_NORMAL);
    
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    STYLE_END(); // TEXT_SIZE
    
    if (next_menu == MENU_LOADING) {
        if (create_sockets()) {
            set_menu_message(MESSAGE_ERROR, true, "Failed to connect to the server");
            return MENU_MAIN;
        }
        prepare_context(/*local=*/ false, /*new_room=*/ false);
        start_net_thread();
        send_request_join();
    }
    
    return next_menu;
}

GameMenu local_menu(void) {
    const int items_number = 2;
    int y = ctx.screen_height / 2 - (ITEM_HEIGHT*items_number + ITEM_SPACING*(items_number-1)) / 2;

    GameMenu next_menu = 0;

    STYLE_START(DEFAULT, TEXT_SIZE, 20);
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
    
    GuiLabel((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, ctx.screen_height/4.0f, ITEM_WIDTH, ITEM_HEIGHT}, "Launch the game locally");
    
    ITEM("Chunk size", 0, y, {
        static bool chunk_spinner_mode = false;
        
        if (GuiValueBox((Rectangle){ITEM_X, y, ITEM_W, ITEM_HEIGHT}, NULL, &ctx.chunk_size_local, BOID_SIZE, DEFAULT_SERVER_CHUNK_SIZE_PIXELS, chunk_spinner_mode)) {
            ctx.chunk_size = (ctx.chunk_size / BOID_SIZE) * BOID_SIZE;
            chunk_spinner_mode = !chunk_spinner_mode;
        }
        
        y += ITEM_HEIGHT + ITEM_SPACING;
    });
    
    // "Back" and "Run" buttons
    const int back_btn_width = ITEM_HEIGHT; // Square button
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f, y, back_btn_width, ITEM_HEIGHT}, GuiIconText(ICON_EXIT, "")))
        next_menu = MENU_MAIN;
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - ITEM_WIDTH/2.0f + back_btn_width + ITEM_INNER_SPACING, y, ITEM_WIDTH - back_btn_width - ITEM_INNER_SPACING, ITEM_HEIGHT}, "Run"))
        next_menu = MENU_GAME;
    y += ITEM_HEIGHT + ITEM_SPACING;
    
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    STYLE_END(); // TEXT_SIZE
    
    if (next_menu == MENU_GAME) {
        prepare_context(/*local=*/ true, /*new_room=*/ false);
    }
    
    return next_menu;
}

GameMenu loading_menu(void) {
    GameMenu next_menu = 0;
    
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
    
    static int rotation_angle = 0;
    static int segment_angle = 0;
    DrawRing((Vector2){ctx.screen_width/2.0f, ctx.screen_height/2.0f}, 30 - 10, 30, rotation_angle, rotation_angle + segment_angle, 20,
             GetColor(GuiGetStyle(DEFAULT, TEXT_COLOR_NORMAL)));
    
    rotation_angle += 5;
    if (rotation_angle >= 360) rotation_angle -= 360;

    static int timer = 0;
    segment_angle = sinf(timer/100.0f)*363 + 3;
    timer++;
    if (timer > 2*PI*100) timer -= 2*PI*100;

    const int back_btn_width = 150;
    GuiSetState(STATE_NORMAL);
    if (GuiButton((Rectangle){ctx.screen_width/2.0f - back_btn_width/2.0f, ctx.screen_height - ITEM_HEIGHT - ITEM_SPACING, back_btn_width, ITEM_HEIGHT}, GuiIconText(ICON_EXIT, "Back")))
        next_menu = MENU_MAIN;

    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    
    return next_menu;
}

#undef ITEM_WIDTH
#undef ITEM_HEIGHT
#undef ITEM_SPACING
#undef ITEM_INNER_SPACING
#undef LABEL_SPACING
#undef CHECKBOX_OFFSET


/* <====================================================== GAME ======================================================> */

void update_boid_sprite(ClientBoid *boids, BoidIndex boid_index) {
    ClientBoid *boid = &boids[boid_index];
    
    if (boid->b.action == ACT_FALL) {
        boid->v.sprite = SPRITE_FALL;
        if (boid->v.sprite_timer < 45)
            boid->v.sprite_timer++;
        return;
    }
    if (boid->b.action == ACT_SURRENDER) {
        boid->v.sprite = SPRITE_SURRENDER;
        if (boid->v.sprite_timer < 50)
            boid->v.sprite_timer++;
        return;
    }

    // Determine if the boid should fall
    if (boid->v.sprite_timer > 0) boid->v.sprite_timer--;

    if (boid->b.is_fighting && boid->v.sprite_timer == 0 && boid->b.fighting_timer == BOID_MAX_FIGHTING_TIMER) {
        boid->v.sprite_timer = 5;
        if (boid->b.hit) {
            boid->v.sprite = SPRITE_OUCH;
        } else {
            boid->v.sprite = (rand()%2)? SPRITE_HIT_LEFT : SPRITE_HIT_RIGHT;
        }
    }

    if (((boid->v.sprite == SPRITE_HIT_LEFT || boid->v.sprite == SPRITE_HIT_RIGHT || boid->v.sprite == SPRITE_OUCH) && boid->v.sprite_timer == 0) || !boid->b.is_fighting) {
        if (boid->b.action == ACT_ATTACK) boid->v.sprite = SPRITE_ANGRY;
        if (boid->b.action == ACT_RETREAT) boid->v.sprite = SPRITE_SAD;
        if (boid->b.action == ACT_STOP) boid->v.sprite = SPRITE_NORMAL;
        boid->b.hit = false;
    }
    if (boid->b.is_fighting) {
        boid->v.direction = Vector2Add(boid->v.direction, Vector2Scale(Vector2Subtract(boids[boid->b.nearest_enemy_idx].b.pos, boid->b.pos), BOID_FIGHTING_FACTOR));
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

    Rectangle sprite = sprites[boid->v.sprite];
    
    Rectangle dest_rec = {boid->b.pos.x, boid->b.pos.y, BOID_SIZE, BOID_SIZE};
    if (boid->v.sprite == SPRITE_SURRENDER) {
        dest_rec.height = BOID_SIZE * (128/75.0);
    } else if (boid->v.sprite == SPRITE_FALL) {
        dest_rec.width = BOID_SIZE * (132/75.0);
        dest_rec.height = BOID_SIZE * (100/75.0);
    }

    Color tint = WHITE;
    if ((boid->v.sprite == SPRITE_SURRENDER) || (boid->v.sprite == SPRITE_FALL)) {
        tint.a = (255.0/50.0) * (50 - boid->v.sprite_timer);
    }
    
    DrawTexturePro(texture, sprite, dest_rec, (Vector2){BOID_SIZE/2.0, BOID_SIZE/2.0},
                   atan2f(boid->v.direction.y, boid->v.direction.x)*RAD2DEG, tint);
}

void draw_selection(ClientBoid *boid, Texture2D texture, float scale, Color color) {
    static Rectangle white_sprite = {0, 520, 146, 149};
    Rectangle dest_rec = {boid->b.pos.x, boid->b.pos.y, BOID_SIZE*scale, BOID_SIZE*scale};
    
    DrawTexturePro(texture, white_sprite, dest_rec, (Vector2){BOID_SIZE*1.2/2.0, BOID_SIZE*1.2/2.0},
                   atan2f(boid->v.direction.y, boid->v.direction.x)*RAD2DEG, color);
}

typedef struct {
    Vector2 pos, vel;
    int dir;
    int size;
    bool incrase;
    Color color;
} Confetti;

#define MAX_CONFETTI_COUNT 500
void draw_confetti(bool reset) {
    static Confetti confetti[MAX_CONFETTI_COUNT];
    static int confetti_count;
    static bool confetti_initialized = false;

    if (reset) {
        confetti_initialized = false;
    }

    int screen_width = GetScreenWidth();
    int screen_height = GetScreenHeight();

    if (!confetti_initialized) {
        confetti_count = MIN(screen_width * MAX_CONFETTI_COUNT/1920, MAX_CONFETTI_COUNT);
        for (int i = 0; i < confetti_count; i++) {
            Confetti c = {.pos = (Vector2){0, screen_height*0.4f - GetRandomValue(-100, 100)},
                          .vel = Vector2Scale(Vector2Normalize((Vector2){GetRandomValue(0, 1000)/1000.0f, -GetRandomValue(0, 600)/1000.0f}), GetRandomValue(100, 1000)/1000.0f),
                          .dir = GetRandomValue(0, 360),
                          .size = GetRandomValue(1, 10),
                          .incrase = rand()%2,
                          .color = ColorFromHSV(GetRandomValue(0, 360), 0.9f, 0.95f)};
            c.vel.y *= 0.7;
            if (i > confetti_count/2) {
                c.pos.x = screen_width;
                c.vel.x *= -1;
            }
            confetti[i] = c;
        }
        
        confetti_initialized = true;
    } else {
        for (int i = 0; i < confetti_count; i++) {
            Confetti *c = &confetti[i];
            
            c->vel.x *= 0.975f;
            c->vel.y += 0.007f;
            c->pos = Vector2Add(c->pos, Vector2Scale(c->vel, screen_width * 40.0f/1920.0f));

            c->dir += 5;
            if (c->dir > 360) c->dir = 0.0f;

            if (c->incrase) {
                c->size++;
                if (c->size > 10) c->incrase = false;
            } else {
                c->size--;
                if (c->size == 0) c->incrase = true;
            }

            Rectangle rec = {.x = c->pos.x, .y = c->pos.y, .width = 30, .height = 8 + c->size};
            DrawRectanglePro(rec, (Vector2){rec.width/2, rec.height/2}, c->dir, c->color);
        }
    }
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
    /* CP_START_PLACING|CP_SEND_AREAS PACKET FORMAT
    (uint16 areas_count) ( { (uint16 x1) (uint16 y1) (uint16 x2) (uint16 y2) (uint8 team) }[areas_count] areas)
    */

    const uint32_t packet_size = 2 + (2 + 2 + 2 + 2 + 1)*areas_count;
    char *data = malloc(packet_size);
    char *d = data;

    PUSH_DATA(d, uint16_t, htons(areas_count));
    for (int i = 0; i < areas_count; i++) {
        Area *a = &areas[i];
        PUSH_DATA(d, uint16_t, htons(a->rec.x1));
        PUSH_DATA(d, uint16_t, htons(a->rec.y1));
        PUSH_DATA(d, uint16_t, htons(a->rec.x2));
        PUSH_DATA(d, uint16_t, htons(a->rec.y2));
        PUSH_DATA(d, uint8_t, a->team);
    }

    int r = send_packet(fd, package_type, data, packet_size, 0);

    free(data);
    return r;
}

void set_button_tooltip(InputEvent event, const char *format, ...) {
    static char tooltip[64];

    va_list args;
    va_start(args, format);
    vsnprintf(tooltip, 64, format, args);
    va_end(args);
    
    InputBinding bind = bindings[event];
    if (bind.kb1 != KEY_NULL) {
        strcat(tooltip, " (");
    
        if (bind.kb_mod & IMOD_CTRL)
            strcat(tooltip, "Ctrl-");
        if (bind.kb_mod & IMOD_ALT)
            strcat(tooltip, "Alt-");
        if (bind.kb_mod & IMOD_SHIFT)
            strcat(tooltip, "Shift-");

        const char *key = GetKeyName(bind.kb1);
        if (key != NULL) {
            const char k = toupper(*key);
            strncat(tooltip, &k, 1);
        } else if (bind.kb1 == KEY_ENTER || bind.kb1 == KEY_KP_ENTER)
            strcat(tooltip, "Enter");
        else if (bind.kb1 == KEY_SPACE)
            strcat(tooltip, "Space");
        else if (bind.kb1 == KEY_BACKSPACE)
            strcat(tooltip, "Backspace");
        else if (bind.kb1 == KEY_DELETE)
            strcat(tooltip, "Delete");
        else if (bind.kb1 == KEY_TAB)
            strcat(tooltip, "Tab");

        strcat(tooltip, ")");
    }
    
    GuiSetTooltip(tooltip);
}

#define BUTTON_SIZE 60
#define SMALL_BUTTON_SIZE 40
#define BUTTON_DISTANCE 5
#define GROUP_DISTANCE 20
#define BUTTON_MARGIN 5
#define TEXT_MARGIN 10

GameMenu game_loop(Texture2D texture, Texture2D boids_textures[], bool reset) {
    static Vector2 prev_mouse_position = {NAN, NAN};
    
    // Areas selecting
    static Point area_start_selecting = { 0 }, area_end_selecting = { 0 };
    static int areas_size[TEAMS_COUNT];

    // Arrow (direction mode)
    static Vector2 arrow_start;
    
    // Line
    static Vector2 line_points[ORDER_LINE_MAX_POINT];
    static int line_points_count = 0;
    static float line_len = 0.0f;

    if (reset) {
        prev_mouse_position = (Vector2){NAN, NAN};
        for (int i = 0; i < TEAMS_COUNT; i++)
            areas_size[i] = 0;
        line_points_count = 0;
        line_len = 0.0f;
    }
    
    GameMenu next_menu = 0;
    
    // Areas mode
    if (ctx.mode == MODE_AREAS) {
        // Selecting
        if (GET_EVENT(IE_ACTION) && ctx.areas_count < MAX_AREAS_COUNT && (ctx.selecting_team == -1 || ctx.teams_used[ctx.selecting_team])) {
            int x = roundf(ctx.mouse_position.x/BOID_SIZE);
            if (x < 0) x = 0;
            if (x > ctx.world_size.x/BOID_SIZE) x = ctx.world_size.x/BOID_SIZE;

            int y = roundf(ctx.mouse_position.y/BOID_SIZE);
            if (y < 0) y = 0;
            if (y > ctx.world_size.y/BOID_SIZE) y = ctx.world_size.y/BOID_SIZE;

            area_end_selecting = (Point){.x = x, .y = y};
            if (!ctx.selecting) {
                area_start_selecting = area_end_selecting;
                ctx.selecting = true;
            }
        } else if (GET_EVENT(IE_ACTION_END) && (ctx.selecting_team == -1 || ctx.teams_used[ctx.selecting_team])) {
            // Create new area
            
            ctx.selecting = false;
            Rec area = {MIN(area_start_selecting.x, area_end_selecting.x), MIN(area_start_selecting.y, area_end_selecting.y),
                        MAX(area_start_selecting.x, area_end_selecting.x), MAX(area_start_selecting.y, area_end_selecting.y)};

            // If new area is not aempty
            if (area.x1 != area.x2 && area.y1 != area.y2) {
                pthread_mutex_lock(&areas_mtx);
                bool insert_area = true;
                int orig_areas_count = ctx.areas_count;
                // Compare a new area with existing ones
                for (int i = 0; i < orig_areas_count; i++) {
                    int rt = ctx.areas[i].team; // Team of the area
                    Rec r = ctx.areas[i].rec; // Rectangle of the area

                    // If new area smaller than one of the list and belongs to the same team, do not create this area
                    if ((rt == ctx.selecting_team) && (r.x1 <= area.x1) && (r.y1 <= area.y1) && (r.x2 >= area.x2) && (r.y2 >= area.y2)) {
                        insert_area = false;
                        break;
                    }

                    // If the areas overlap
                    Rec intersection = {MAX(area.x1, r.x1), MAX(area.y1, r.y1), MIN(area.x2, r.x2), MIN(area.y2, r.y2)};
                    if (intersection.x1 < intersection.x2 && intersection.y1 < intersection.y2) {
                        // Delete area from the list
                        memmove(&ctx.areas[i], &ctx.areas[i+1], sizeof(*ctx.areas) * (ctx.areas_count - i - 1));
                        ctx.areas_count--;
                        orig_areas_count--;
                        i--;

                        // Divide the old area into non-overlapping new ones
                        if (intersection.y1 != r.y1) ctx.areas[ctx.areas_count++] = (Area){.rec = {r.x1, r.y1, r.x2, intersection.y1}, .team = rt};
                        if (intersection.x1 != r.x1) ctx.areas[ctx.areas_count++] = (Area) {.rec = {r.x1, intersection.y1, intersection.x1, intersection.y2}, .team = rt};
                        if (intersection.x2 != r.x2) ctx.areas[ctx.areas_count++] = (Area){.rec = {intersection.x2, intersection.y1, r.x2, intersection.y2}, .team = rt};
                        if (intersection.y2 != r.y2) ctx.areas[ctx.areas_count++] = (Area){.rec = {r.x1, intersection.y2, r.x2, r.y2}, .team = rt};

                        // Subtract the intersecting part from total areas size
                        areas_size[rt] -= (intersection.x2 - intersection.x1) * (intersection.y2 - intersection.y1);
                    }
                }
                // Add new area
                if (ctx.selecting_team >= 0 && insert_area) {
                    ctx.areas[ctx.areas_count++] = (Area){.rec = area, .team = ctx.selecting_team};
                    areas_size[ctx.selecting_team] += (area.x2 - area.x1) * (area.y2 - area.y1);
                }

                cleanup_areas(ctx.areas, &ctx.areas_count);

                if (!ctx.hide_areas)
                    send_areas(ctx.tcp_fd, CP_SEND_AREAS, ctx.areas, ctx.areas_count);

                pthread_mutex_unlock(&areas_mtx);
            }
        }

        // Start placing
        pthread_mutex_lock(&players_mtx);
        if (GET_EVENT(IE_START_PLACING) && ctx.new_room && ctx.players_number == ctx.joined_players) {
            bool ok = true;
            for (int team = 0; team < TEAMS_COUNT; team++) {
                if (ctx.teams_used[team] && ctx.boids_number[team] > areas_size[team]) {
                    ok = false;
                    break;
                }
            }

            // Send data to the server only if size of areas of all teams is greater than or equal to the number of boids
            if (ok) {
                pthread_mutex_lock(&areas_mtx);
                send_areas(ctx.tcp_fd, CP_START_PLACING, ctx.areas, ctx.areas_count);
                pthread_mutex_unlock(&areas_mtx);
                
            } else {
                log_message(&ctx.log, L_WARNING, "you cannot start placing if size of areas of all teams is less than the number of boids\n");
            }
        }
        pthread_mutex_unlock(&players_mtx);
    }

    // Spawn boids
    BoidIndex deleted_boids_count = 0;
    if (ctx.local_game) {
        static Vector2 prev_pos = {INFINITY, INFINITY};
        if (GET_EVENT(IE_ACTION) && ctx.mode == MODE_SPAWN && (ctx.boids_count < MAX_BOIDS_COUNT)) {
            const int lerp_amt = 10;
            for (int i = 0; i < lerp_amt && ctx.boids_count < MAX_BOIDS_COUNT; i++) {
                Vector2 pos = Vector2Lerp(prev_mouse_position, ctx.mouse_position, (float)i/lerp_amt);
                if (pos.x < 0 || pos.y < 0 || pos.x > ctx.world_size.x || pos.y > ctx.world_size.y)
                    continue;
                if (Vector2Distance(prev_pos, pos) >= 100) {
                    prev_pos = pos;
                } else continue;

                ClientBoid new_boid = { 0 };
                new_boid.b.pos = pos;
                if (ctx.action == ACT_STOP) {
                    new_boid.v.direction = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};
                } else {
                    new_boid.b.velocity = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};
                    new_boid.v.direction = new_boid.b.velocity;
                }
                new_boid.b.speed = GetRandomValue(80, 130)/100.0;
                new_boid.b.health = GetRandomValue(BOID_MAX_HEALTH*0.8, BOID_MAX_HEALTH);
                new_boid.b.xp = GetRandomValue(0, 5);
                new_boid.b.action = ctx.action;
                new_boid.b.team = ctx.selecting_team;
                new_boid.b.boid_idx = ctx.boids_count;
                ctx.order_parts[ctx.boids_count] = (OrderBoidPart){ 0 };

                ctx.boids[ctx.boids_count++] = new_boid;
            }
        }
    } else {
        static SPoint spawn_prev_pos = {-1, -1}, delete_prev_pos = {-1, -1};
        if (ctx.stage == STAGE_PLACING) {
            if (GET_EVENT(IE_ACTION) && ctx.mode == MODE_SPAWN && ctx.boids_count < ctx.boids_number[ctx.player_team] && \
                ctx.mouse_position.x + (int)(ctx.brush_size - ctx.brush_size/2) * BOID_SIZE >= 0 &&
                ctx.mouse_position.x - (int)(ctx.brush_size/2) * BOID_SIZE <= ctx.world_size.x &&
                ctx.mouse_position.y + (int)(ctx.brush_size - ctx.brush_size/2) * BOID_SIZE >= 0 &&
                ctx.mouse_position.y - (int)(ctx.brush_size/2) * BOID_SIZE <= ctx.world_size.y) {

                SPoint pos = {(int)ctx.mouse_position.x/BOID_SIZE, (int)ctx.mouse_position.y/BOID_SIZE};
            
                if (pos.x != spawn_prev_pos.x || pos.y != spawn_prev_pos.y) {
                    SRec brush_rec = {pos.x - ctx.brush_size/2, pos.y - ctx.brush_size/2,
                                     pos.x - ctx.brush_size/2 + ctx.brush_size, pos.y - ctx.brush_size/2 + ctx.brush_size};

                    enum {
                        CELL_UNCHECKED,
                        CELL_USED,
                        CELL_FREE
                    } *cells = calloc(ctx.brush_size*ctx.brush_size, sizeof(*cells));
                
                    Area *area = NULL;
                    for (int cell_x = brush_rec.x1; cell_x < brush_rec.x2; cell_x++) {
                        if (cell_x < 0 || cell_x >= ctx.world_size.x / BOID_SIZE)
                            continue;
                        if (ctx.boids_count >= ctx.boids_number[ctx.player_team])
                            break;

                        for (int cell_y = brush_rec.y1; cell_y < brush_rec.y2; cell_y++) {
                            if (cell_y < 0 || cell_y >= ctx.world_size.y / BOID_SIZE)
                                continue;
                            if (ctx.boids_count >= ctx.boids_number[ctx.player_team])
                                break;
                        
                            bool can_place = false;

                            // Check if new boids is in his team's area
                            if (area != NULL && area->team == (int)ctx.player_team &&
                                cell_x >= area->rec.x1 && cell_x < area->rec.x2 && cell_y >= area->rec.y1 && cell_y < area->rec.y2) {
                                can_place = true;
                            } else {
                                for (int i = 0; i < ctx.areas_count; i++) {
                                    Area *area = &ctx.areas[i];
                                    if (area->team == (int)ctx.player_team &&
                                        cell_x >= area->rec.x1 && cell_x < area->rec.x2 && cell_y >= area->rec.y1 && cell_y < area->rec.y2) {
                                        can_place = true;
                                        break;
                                    }
                                }
                            }

                            if (can_place) {
                                float x = cell_x * BOID_SIZE + BOID_SIZE/2.0;
                                float y = cell_y * BOID_SIZE + BOID_SIZE/2.0;

                                uint16_t chunk_x = x / ctx.grid.chunk_size_pixels;
                                uint16_t chunk_y = y / ctx.grid.chunk_size_pixels;
                                uint32_t chunk_index = chunk_x + chunk_y*ctx.grid.cols;

                                Chunk *chunk = &ctx.grid.chunks[chunk_index];

                                // Check if there are no another boid on the same place
                                int cell_idx = (cell_x - brush_rec.x1) + (cell_y - brush_rec.y1)*ctx.brush_size;
                                int cell_status = cells[cell_idx];
                                if (cell_status == CELL_FREE) {
                                    can_place = true;
                                } else if (cell_status == CELL_UNCHECKED) {
                                    for (int i = 0; i < chunk->count; i++) {
                                        ClientBoid *boid = &ctx.boids[chunk->boids[i]];
                                        Point boid_pos = {(int)boid->b.pos.x/BOID_SIZE, (int)boid->b.pos.y/BOID_SIZE};
                                        int cell_idx = (boid_pos.x - brush_rec.x1) + (boid_pos.y - brush_rec.y1)*ctx.brush_size;
                                        if (boid_pos.x >= brush_rec.x1 && boid_pos.x < brush_rec.x2 && boid_pos.y >= brush_rec.y1 && boid_pos.y < brush_rec.y2)
                                            cells[cell_idx] = CELL_USED;
                                    }

                                    Rec check_border = {MAX(brush_rec.x1, chunk_x*ctx.grid.chunk_size_pixels/BOID_SIZE),
                                                        MAX(brush_rec.y1, chunk_y*ctx.grid.chunk_size_pixels/BOID_SIZE),
                                                        MIN(brush_rec.x2, (chunk_x+1)*ctx.grid.chunk_size_pixels/BOID_SIZE),
                                                        MIN(brush_rec.y2, (chunk_y+1)*ctx.grid.chunk_size_pixels/BOID_SIZE)};
                                    for (int check_cell_x = check_border.x1; check_cell_x < check_border.x2; check_cell_x++) {
                                        for (int check_cell_y = check_border.y1; check_cell_y < check_border.y2; check_cell_y++) {
                                            int check_cell_idx = (check_cell_x - brush_rec.x1) + (check_cell_y - brush_rec.y1)*ctx.brush_size;
                                            if (cells[check_cell_idx] == CELL_UNCHECKED)
                                                cells[check_cell_idx] = CELL_FREE;
                                        }
                                    }

                                    can_place = cells[cell_idx] == CELL_FREE;
                                } else {
                                    can_place = false;
                                }

                                // If all checks are passed, create a new boid
                                if (can_place) {
                                    ClientBoid new_boid = {.b = {.pos = {x, y}, .team = ctx.player_team, .action = ACT_STOP, .boid_idx = ctx.boids_count},
                                                           .v = {.direction = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0}}};
                                    ctx.boids[ctx.boids_count++] = new_boid;
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
            if (GET_EVENT(IE_ACTION) && ctx.mode == MODE_DELETE && ctx.boids_count > 0 &&
                ctx.mouse_position.x + (int)(ctx.brush_size - ctx.brush_size/2) * BOID_SIZE >= 0 &&
                ctx.mouse_position.x - (int)(ctx.brush_size/2) * BOID_SIZE <= ctx.world_size.x &&
                ctx.mouse_position.y + (int)(ctx.brush_size - ctx.brush_size/2) * BOID_SIZE >= 0 &&
                ctx.mouse_position.y - (int)(ctx.brush_size/2) * BOID_SIZE <= ctx.world_size.y) {
            
                SPoint pos = {(int)ctx.mouse_position.x/BOID_SIZE, (int)ctx.mouse_position.y/BOID_SIZE};
            
                if (pos.x != delete_prev_pos.x || pos.y != delete_prev_pos.y) {
                    SRec brush_rec = {pos.x - ctx.brush_size/2, pos.y - ctx.brush_size/2,
                                     pos.x - ctx.brush_size/2 + ctx.brush_size, pos.y - ctx.brush_size/2 + ctx.brush_size};

                    for (int chunk_x = MAX(brush_rec.x1*BOID_SIZE/ctx.grid.chunk_size_pixels, 0); chunk_x < MIN(brush_rec.x2*BOID_SIZE/ctx.grid.chunk_size_pixels+1, ctx.grid.cols); chunk_x++) {
                        for (int chunk_y = MAX(brush_rec.y1*BOID_SIZE/ctx.grid.chunk_size_pixels, 0); chunk_y < MIN(brush_rec.y2*BOID_SIZE/ctx.grid.chunk_size_pixels+1, ctx.grid.rows); chunk_y++) {
                            uint32_t chunk_index = chunk_x + chunk_y*ctx.grid.cols;
                            Chunk *chunk = &ctx.grid.chunks[chunk_index];
                        
                            for (int i = 0; i < chunk->count; i++) {
                                ClientBoid *boid = &ctx.boids[chunk->boids[i]];
                                if (boid->b.pos.x >= brush_rec.x1*BOID_SIZE && boid->b.pos.x < brush_rec.x2*BOID_SIZE &&
                                    boid->b.pos.y >= brush_rec.y1*BOID_SIZE && boid->b.pos.y < brush_rec.y2*BOID_SIZE) {
                                    boid->b.action = ACT_DELETE;
                                    deleted_boids_count++;
                                }
                            }
                        }
                    }

                    spawn_prev_pos = (SPoint){-1, -1};
                    delete_prev_pos = pos;
                }
            }

        
            // Send boids and a message that the player is ready to start the game
            if (GET_EVENT(IE_READY) && !ctx.local_game) {
                if (ctx.boids_count == ctx.boids_number[ctx.player_team]) {
                    StartBoids *data = calloc((ctx.boids_count + 1)*2, sizeof(*data));
                    data[0].team = -1;
                    int index = 0;

                    for (int y = 0; y < ctx.world_size.y / BOID_SIZE; y++) {
                        for (int x = 0; x < ctx.world_size.x / BOID_SIZE; x++) {
                            uint16_t chunk_x = x * BOID_SIZE / ctx.grid.chunk_size_pixels;
                            uint16_t chunk_y = y * BOID_SIZE / ctx.grid.chunk_size_pixels;
                            uint32_t chunk_index = chunk_x + chunk_y*ctx.grid.cols;

                            Chunk *chunk = &ctx.grid.chunks[chunk_index];

                            bool find = false;
                            for (int i = 0; i < chunk->count; i++) {
                                ClientBoid *boid = &ctx.boids[chunk->boids[i]];
                                Point boid_pos = {(int)boid->b.pos.x/BOID_SIZE, (int)boid->b.pos.y/BOID_SIZE};
                                if (boid_pos.x == x && boid_pos.y == y) {
                                    find = true;
                                    break;
                                }
                            }

                            if (find == (data[index].team >= 0))
                                data[index].count++;
                            else
                                data[++index] = (StartBoids){.team = find ? (signed)ctx.player_team : -1, .count = 1};
                        }
                    }

                    uint16_t count = index + 1;
                
                    /* CP_SEND_BOIDS PACKET FORMAT
                    (uint16 count) ({ (uint16 boids_count) (int8 team) }[count] boids)
                    */

                    uint32_t packet_size = 2 + (2 + 1)*count;
                    char *packet_data = malloc(packet_size);
                    char *d = packet_data;

                    PUSH_DATA(d, uint16_t, htons(count));
                    for (int i = 0; i < count; i++) {
                        PUSH_DATA(d, uint16_t, htons(data[i].count));
                        PUSH_DATA(d, int8_t, data[i].team);
                    }
                    
                    send_packet(ctx.tcp_fd, CP_SEND_BOIDS, packet_data, packet_size, 0);
                    log_message(&ctx.log, L_INFO, "wait until other players are ready to start the game\n");

                    free(data);
                    free(packet_data);
                } else {
                    log_message(&ctx.log, L_WARNING, "place all your boids before you start the game\n");
                }
            }
        }
    }

    // Select boids
    if (ctx.mode == MODE_SELECT) {
        if (GET_EVENT(IE_ACTION)) {
            if (!ctx.selecting) {
                ctx.selecting = true;
                ctx.selection_start = ctx.mouse_position;
            }
        } else if (GET_EVENT(IE_ACTION_END)) {
            ctx.selecting = false;
        }
    }
    
    // Direction mode
    Vector2 arrow_vector = Vector2Subtract(ctx.mouse_position, arrow_start);
    Vector2 arrow_vector_norm = Vector2Normalize(arrow_vector);
    if (ctx.mode == MODE_DIRECTION) {
        ctx.show_arrow = GET_EVENT(IE_ACTION);
        if (GET_EVENT(IE_ACTION_END)) {
            ctx.show_arrow = false;
            ctx.change_boids_direction = Vector2LengthSqr(arrow_vector) >= 40*40;
        }
        if (GET_EVENT(IE_ACTION_START))
            arrow_start = ctx.mouse_position;
    }

    // Point mode
    if (ctx.mode == MODE_POINT) {
        if (GET_EVENT(IE_ACTION_END))
            ctx.change_boids_direction = true;
    }

    // Line mode
    if (ctx.mode == MODE_LINE) {
        // Building a line from segments
        if (GET_EVENT(IE_ACTION_START)) {
            float x = ctx.mouse_position.x;
            if (x < 0) x = 0;
            else if (x > ctx.world_size.x) x = ctx.world_size.x;

            float y = ctx.mouse_position.y;
            if (y < 0) y = 0;
            else if (y > ctx.world_size.y) y = ctx.world_size.y;

            Vector2 point_pos = {x, y};

            if (ctx.show_line && (line_points_count < ORDER_LINE_MAX_POINT)) {
                line_len += Vector2Distance(point_pos, line_points[line_points_count-1]);
                line_points[line_points_count++] = point_pos;
            } else if (!ctx.show_line) { // First point
                ctx.show_line = true;
                line_points[0] = point_pos;
                line_len = 0;
                line_points_count = 1;
            }
        }
    }
    if (GET_EVENT(IE_APPLY_LINE) && ctx.local_game) {
        ctx.change_boids_direction = true;
        BaseBoid **b = malloc(ctx.boids_count * sizeof(*b));
        BoidIndex bc = 0;
        for (BoidIndex i = 0; i < ctx.boids_count; i++) {
            ClientBoid *boid = &ctx.boids[i];
            if (boid->v.is_selected && boid->b.action != ACT_DELETE && boid->b.action != ACT_FALL && boid->b.action != ACT_SURRENDER) {
                b[bc++] = &boid->b;
                boid->b.kdtree_is_used = false; // kdtree_is_used == true if boid has already been placed on the line 
            }
        }
        KDNode *tree = CREATE_KDTREE(b, bc, 16); // k-d tree of boids

        float interval = line_len / (bc - 1);
        float remains = 0;
        int point_idx = 0;
        Rectangle rec = {0, 0, ctx.world_size.x, ctx.world_size.y};
    
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
                BaseBoid *nearest_boid = find_nearest_in_kdtree_approx(tree, point, rec);
                if (nearest_boid == NULL) break;
                OrderBoidPart *order_data = &ctx.order_parts[nearest_boid->boid_idx];
            
                order_data->order_vector = point;
                order_data->order_timer = Vector2Distance(nearest_boid->pos, point) / BOID_MIN_SPEED;
                order_data->point_order = true;
                order_data->direction_order = false;
                nearest_boid->kdtree_is_used = true;
            
                path_len += interval;
                point = Vector2Add(point, Vector2Scale(segment_dir, interval));
            }
            remains = path_len - segment_len;
        }

        // I didn't understand why sometimes not all boids are placed,
        // so if there are any, they are placed at the end of the line.
        if (bc - point_idx > 0) {
            for (BoidIndex i = 0; i < bc; i++) {
                BaseBoid *base_boid = b[i];
                if (!base_boid->kdtree_is_used) {
                    OrderBoidPart *order_data = &ctx.order_parts[base_boid->boid_idx];
                    Vector2 point = line_points[line_points_count-1];
                
                    order_data->order_vector = point;
                    order_data->order_timer = Vector2Distance(base_boid->pos, point) / BOID_MIN_SPEED;
                    order_data->point_order = true;
                    order_data->direction_order = false;
                    base_boid->kdtree_is_used = true;
                }
            }
        }

        clear_kdtree(tree);
        free(b);
    }

    // Update boids in selection
    BoidIndex selected_boids_count = 0;
    if (ctx.select_mode) {
        pthread_mutex_lock(&boids_mtx);
        for (BoidIndex i = 0; i < ctx.boids_count; i++) {
           ClientBoid *boid = &ctx.boids[i];
            if ((boid->b.team == ctx.player_team || ctx.local_game) &&
                boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL && boid->b.action != ACT_DELETE) {
                if (ctx.selecting) {
                    boid->v.is_selected = (CHECK_MOD(IMOD_SHIFT) && boid->v.is_selected) || (
                                       (boid->b.pos.x > fmin(ctx.selection_start.x, ctx.mouse_position.x)) &&
                                       (boid->b.pos.x < fmax(ctx.selection_start.x, ctx.mouse_position.x)) &&
                                       (boid->b.pos.y > fmin(ctx.selection_start.y, ctx.mouse_position.y)) &&
                                       (boid->b.pos.y < fmax(ctx.selection_start.y, ctx.mouse_position.y)));
                }

                if (boid->v.is_selected) {
                    // Delete boid
                    if (GET_EVENT(IE_DELETE_SELECTED_BOIDS)) {
                        boid->b.action = ACT_DELETE;
                        boid->v.is_selected = false;
                        deleted_boids_count++;
                        continue;
                    }
                    if (ctx.local_game) {
                        OrderBoidPart *order_data = &ctx.order_parts[boid->b.boid_idx];
                        if (ctx.clear_order) {
                            order_data->direction_order = false;
                            order_data->point_order = false;
                            order_data->order_timer = 0;
                        } else {
                            if (ctx.change_selection_team) {
                                boid->v.is_selected = (boid->b.team == (unsigned)ctx.selecting_team);
                            }

                            if (ctx.change_boids_action) {
                                if ((boid->b.action == ACT_STOP) && (ctx.action != ACT_STOP)) // Randomize boid's speed, if it stops
                                    boid->b.velocity = (Vector2){GetRandomValue(-10, 10)/10.0, GetRandomValue(-10, 10)/10.0};
                                order_data->order_timer = GetRandomValue(20, 30)*60; // 20-30 seconds

                                boid->b.action = ctx.action;
                                if (ctx.action == ACT_STOP) boid->v.sprite = SPRITE_NORMAL;
                                if (ctx.action == ACT_ATTACK) boid->v.sprite = SPRITE_ANGRY;
                                if (ctx.action == ACT_RETREAT) boid->v.sprite = SPRITE_SAD;
                            }

                            if (ctx.change_boids_direction) {
                                if (ctx.mode == MODE_DIRECTION) {
                                    order_data->order_vector = (Vector2LengthSqr(arrow_vector) >= 40*40)? arrow_vector_norm : (Vector2){ 0 };
                                    order_data->direction_order = true;
                                    order_data->point_order = false;
                                } else if (ctx.mode == MODE_POINT) {
                                    order_data->order_vector = ctx.mouse_position;
                                    order_data->point_order = true;
                                    order_data->direction_order = false;
                                }

                                if (ctx.mode == MODE_LINE)
                                    order_data->point_order = true;
                                else
                                    order_data->order_timer = GetRandomValue(30, 45)*60; // 30-45 seconds
                            }
                        }
                    }
                }

                selected_boids_count += boid->v.is_selected;
            }
        }

        pthread_mutex_unlock(&boids_mtx);
    }

    // Remove deleted boids from array
    if (deleted_boids_count > 0) {
        pthread_mutex_lock(&boids_mtx);
        int offset = 0;
        for (int i = 0; i < ctx.boids_count; i++) {
            ctx.boids[i - offset] = ctx.boids[i];
            ctx.boids[i - offset].b.boid_idx = i - offset;
            if (ctx.local_game)
                ctx.order_parts[i - offset] = ctx.order_parts[i];
            ClientBoid *boid = &ctx.boids[i];
            if (boid->b.action == ACT_DELETE)
                offset++;
        }
        ctx.boids_count -= offset;
        pthread_mutex_unlock(&boids_mtx);
    }

    bool change_boids_direction = ctx.change_boids_direction;
    
    // Send new boids action and orders
    if (ctx.stage == STAGE_GAME && !ctx.local_game && (ctx.change_boids_action || ctx.change_boids_direction || ctx.clear_order)) {
        BoidIndex selected_boids[MAX_BOIDS_COUNT] = { 0 };
        BoidIndex selected_boids_count = 0;
        for (BoidIndex i = 0; i < ctx.boids_count; i++) {
            ClientBoid *boid = &ctx.boids[i];
            if (boid->b.team == ctx.player_team && boid->v.is_selected)
                selected_boids[selected_boids_count++] = htons(i);
        }

        /* CP_ORDER PACKET FORMAT
        ORDER_CLEAR - (int8 order_type) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_ACTION - (int8 order_type) (int8 new_action) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_DIRECTION - (int8 order_type) (int32 vector.x*65535) (int32 vector.y*65535) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_POINT - (int8 order_type) (uint16 point.x) (uint16 point.y) (uint16 boids_count) (uint16[boids_count] boids)
        ORDER_LINE - (int8 order_type) (uint8 points_count) ({ (uint16 x) (uint16 y) }[points_count] points) (uint16 boids_count) (uint16[boids_count] boids)
        */

        uint32_t base_packet_size = 1 + sizeof(BoidIndex) + sizeof(BoidIndex)*selected_boids_count; // order_type + boids_count + boids
        uint32_t packet_size = base_packet_size;
        char *data = NULL, *d = NULL;

        if (ctx.clear_order) { // ORDER_CLEAR
            packet_size += 0;
            data = malloc(packet_size); d = data;

            PUSH_DATA(d, int8_t, ORDER_CLEAR);
        } else if (ctx.change_boids_action) { // ORDER_ACTION
            packet_size += 1;
            data = malloc(packet_size); d = data;

            PUSH_DATA(d, int8_t, ORDER_ACTION);
            PUSH_DATA(d, int8_t, ctx.action);
        } else if (ctx.mode == MODE_DIRECTION) { // ORDER_DIRECTION
            packet_size += 4 + 4;
            data = malloc(packet_size); d = data;

            PUSH_DATA(d, int8_t, ORDER_DIRECTION);
            PUSH_DATA(d, int32_t, htonl(arrow_vector_norm.x*65535));
            PUSH_DATA(d, int32_t, htonl(arrow_vector_norm.y*65535));
        } else if (ctx.mode == MODE_POINT) { // ORDER_POINT
            packet_size += 2 + 2;
            data = malloc(packet_size); d = data;

            int x = ctx.mouse_position.x;
            if (x < 0) x = 0;
            else if (x > ctx.world_size.x) x = ctx.world_size.x;

            int y = ctx.mouse_position.y;
            if (y < 0) y = 0;
            else if (y > ctx.world_size.y) y = ctx.world_size.y;

            PUSH_DATA(d, int8_t, ORDER_POINT);
            PUSH_DATA(d, uint16_t, htons(x));
            PUSH_DATA(d, uint16_t, htons(y));
        } else if (ctx.mode == MODE_LINE) { // ORDER_LINE
            packet_size += 1 + (2 + 2)*line_points_count;
            data = malloc(packet_size); d = data;

            PUSH_DATA(d, int8_t, ORDER_LINE);
            PUSH_DATA(d, uint8_t, line_points_count);

            for (int i = 0; i < line_points_count; i++) {
                PUSH_DATA(d, uint16_t, htons(line_points[i].x));
                PUSH_DATA(d, uint16_t, htons(line_points[i].y));
            }
        }

        if (data != NULL) {
            PUSH_DATA(d, uint16_t, htons(selected_boids_count));
            PUSH_MEM(d, selected_boids, sizeof(BoidIndex)*selected_boids_count);

            send_packet(ctx.tcp_fd, CP_ORDER, data, packet_size, 0);
            free(data);
        }

        ctx.change_boids_action = false;
        ctx.change_boids_direction = false;
        ctx.clear_order = false;
    }

    // Update boids
    if (ctx.stage == STAGE_GAME) {
        memset(ctx.boids_number, 0, sizeof(ctx.boids_number)); // Clear boids_number

        pthread_mutex_lock(&boids_mtx);
        if (ctx.game_paused) {
            for (BoidIndex i = 0; i < ctx.boids_count; i++) {
                ClientBoid *boid = &ctx.boids[i];
                if (boid->b.action != ACT_DELETE && boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL)
                    ctx.boids_number[boid->b.team]++;
            }
        } else {
            for (BoidIndex i = 0; i < ctx.boids_count; i++) {
                ClientBoid *boid = &ctx.boids[i];

                if (boid->b.action == ACT_DELETE) continue;

                if (ctx.local_game) {
                    OrderBoidPart *order_data = &ctx.order_parts[boid->b.boid_idx];
                    if (order_data->order_timer > 0) {
                        order_data->order_timer--;

                        if (order_data->point_order && Vector2Distance(order_data->order_vector, boid->b.pos) < BOID_SIZE) {
                            order_data->order_timer = 0;
                            boid->b.action = ACT_STOP;
                        }

                        // Change direction by order
                        Vector2 direction = { 0 };
                        if (order_data->direction_order)
                            direction = order_data->order_vector;
                        else if (order_data->point_order)
                            direction = Vector2Normalize(Vector2Subtract(order_data->order_vector, boid->b.pos));
                        boid->b.velocity = Vector2Add(boid->b.velocity, Vector2Scale(direction, BOID_ORDER_FACTOR));
                    }
                    update_base_boid(ctx.boids, &ctx.grid, i, sizeof(*ctx.boids), /*can_change_action=*/ order_data->order_timer == 0, /*can_fall=*/ true);
                } else
                    update_base_boid(ctx.boids, &ctx.grid, i, sizeof(*ctx.boids), /*can_change_action=*/ true, /*can_fall=*/ false);
                update_boid_sprite(ctx.boids, i);

                if (boid->b.action != ACT_SURRENDER && boid->b.action != ACT_FALL) {
                    if (boid->v.go_target) {
                        Vector2 target_dir = Vector2Subtract(boid->v.target_pos, boid->b.pos);
                        if (Vector2LengthSqr(target_dir) >= BOID_SIZE*BOID_SIZE) {
                            target_dir = Vector2Normalize(target_dir);
                            boid->b.velocity = Vector2Add(boid->b.velocity, Vector2Scale(target_dir, BOID_TARGET_FACTOR));
                        } else {
                            boid->v.go_target = false;
                        }
                    }
                
                    boid_normal_speed(&boid->b);
                    boid_bound(&boid->b, ctx.world_size.x, ctx.world_size.y);

                    boid->v.direction.x = boid->v.direction.x*0.97f + boid->b.velocity.x*0.03f;
                    boid->v.direction.y = boid->v.direction.y*0.97f + boid->b.velocity.y*0.03f;

                    ctx.boids_number[boid->b.team]++;
                } else {
                    boid->v.is_selected = false;
                }

                boid->b.pos = Vector2Add(boid->b.pos, Vector2Scale(boid->b.velocity, boid->b.speed));
            }
        }
        pthread_mutex_unlock(&boids_mtx);
    }

    if (ctx.select_mode) {
        if (change_boids_direction && ctx.autoselect_mode)
            ctx.mode = MODE_SELECT;
        if (ctx.local_game) {
            ctx.change_boids_action = false;
            ctx.change_boids_direction = false;
            ctx.change_selection_team = false;
            ctx.clear_order = false;
        }
    }
    
    // Update grid
    pthread_mutex_lock(&boids_mtx);
    clear_grid(&ctx.grid);
    FILL_GRID(&ctx.grid, ctx.boids, ctx.boids_count);
    pthread_mutex_unlock(&boids_mtx);

    // BEGIN DRAWING -----------------------------------------------------------------------------

    BeginMode2D(ctx.camera);

    // Word border
    DrawRectangleLines(0, 0, ctx.world_size.x, ctx.world_size.y, BLACK);
    if (ctx.local_game)
        DrawCircle(ctx.world_size.x, ctx.world_size.y, 50 + 10*ctx.is_dragging_border, BLACK);

    // Draw areas
    if (ctx.stage == STAGE_AREAS || ctx.stage == STAGE_PLACING) {
        pthread_mutex_lock(&areas_mtx);
        for (int i = 0; i < ctx.areas_count; i++) {
            Area area = ctx.areas[i];

            Color color = RAYWHITE;
            switch (area.team) {
                case TEAM_RED: color = ColorAlpha(RED, (ctx.mode == MODE_AREAS)? 0.20f: 0.08f); break;
                case TEAM_BLUE: color = ColorAlpha(BLUE, (ctx.mode == MODE_AREAS)? 0.20f: 0.08f); break;
                case TEAM_GREEN: color = ColorAlpha(GREEN, (ctx.mode == MODE_AREAS)? 0.20f: 0.08f); break;
                case TEAM_YELLOW: color = ColorAlpha(ORANGE, (ctx.mode == MODE_AREAS)? 0.20f: 0.08f); break;
            }
            DrawRectangle(area.rec.x1 * BOID_SIZE, area.rec.y1 * BOID_SIZE,
                          (area.rec.x2 - area.rec.x1) * BOID_SIZE, (area.rec.y2 - area.rec.y1) * BOID_SIZE, color);
        }
        pthread_mutex_unlock(&areas_mtx);
    }

    // Draw grid
    if (ctx.show_grid && !ctx.local_game) {
        for (int i = 1; i < ctx.world_size.x / BOID_SIZE; i++) {
            DrawLine(i*BOID_SIZE, 0, i*BOID_SIZE, ctx.world_size.y, BLACK);
        }

        for (int i = 1; i < ctx.world_size.x / BOID_SIZE; i++) {
            DrawLine(0, i*BOID_SIZE, ctx.world_size.x, i*BOID_SIZE, BLACK);
        }
    }

    // Draw fallen boids
    pthread_mutex_lock(&boids_mtx);
    for (BoidIndex i = 0; i < ctx.boids_count; i++) {
        ClientBoid *boid = &ctx.boids[i];
        if (boid->v.sprite == SPRITE_FALL)
            draw_boid(boid, boids_textures[boid->b.team]);
    }

    // Draw selection
    for (BoidIndex i = 0; i < ctx.boids_count; i++) {
        ClientBoid *boid = &ctx.boids[i];
        if ((boid->v.sprite != SPRITE_FALL) && (boid->v.is_selected)) {
            draw_selection(boid, texture, 1.2, ORANGE);
        }
    }
    
    // Draw boids
    for (BoidIndex i = 0; i < ctx.boids_count; i++) {
        ClientBoid *boid = &ctx.boids[i];
        if ((boid->v.sprite != SPRITE_FALL) && (boid->b.action != ACT_DELETE)) {
            draw_boid(boid, boids_textures[boid->b.team]);
            // if (show_health) {
            //     const char *text = TextFormat("%d %d", boid->b.health, boid->b.xp);
            //     DrawText(text, boid->b.pos.x - MeasureText(text, 20)/2.0f, boid->b.pos.y - 50, 20, BLACK);
            // }
        }
    }
    pthread_mutex_unlock(&boids_mtx);

    float thick = 4/ctx.camera.zoom;
    
    // Draw area selecting
    if (ctx.mode == MODE_AREAS && ctx.selecting) {
        float rectangleX = fmin(area_start_selecting.x * BOID_SIZE, area_end_selecting.x * BOID_SIZE);
        float rectangleY = fmin(area_start_selecting.y * BOID_SIZE, area_end_selecting.y * BOID_SIZE);
        DrawRectangleLinesEx((Rectangle){rectangleX, rectangleY,
                             abs(area_start_selecting.x * BOID_SIZE - area_end_selecting.x * BOID_SIZE),
                             abs(area_start_selecting.y * BOID_SIZE - area_end_selecting.y * BOID_SIZE)},
                             thick, BLACK);
    }

    // Drawing boids selection box
    if (ctx.mode == MODE_SELECT && ctx.selecting) {
        float rectangleX = fmin(ctx.selection_start.x, ctx.mouse_position.x);
        float rectangleY = fmin(ctx.selection_start.y, ctx.mouse_position.y);
        DrawText(TextFormat("%d", selected_boids_count), rectangleX, rectangleY-(20/ctx.camera.zoom), 20/ctx.camera.zoom, BLACK);
        DrawRectangleLinesEx((Rectangle){rectangleX, rectangleY,
                             fabs(ctx.mouse_position.x - ctx.selection_start.x), fabs(ctx.mouse_position.y - ctx.selection_start.y)},
                             thick, BLACK);
    }

    // Draw arrow (in direction mode)
    if (ctx.mode == MODE_DIRECTION && ctx.show_arrow && (Vector2LengthSqr(arrow_vector) >= powf(40/ctx.camera.zoom, 2))) {
        DrawLineEx(arrow_start, ctx.mouse_position, thick, BLACK);
        DrawLineEx(ctx.mouse_position, Vector2Add(ctx.mouse_position, Vector2Scale(Vector2Rotate(arrow_vector_norm,  160*DEG2RAD), 40/ctx.camera.zoom)), thick, BLACK);
        DrawLineEx(ctx.mouse_position, Vector2Add(ctx.mouse_position, Vector2Scale(Vector2Rotate(arrow_vector_norm, -160*DEG2RAD), 40/ctx.camera.zoom)), thick, BLACK);
    }
    
    // Draw point (in point mode)
    if (ctx.mode == MODE_POINT) {
        DrawCircle(ctx.mouse_position.x, ctx.mouse_position.y, 20/ctx.camera.zoom, (Color){0, 0, 0, 50});
    }

    // Draw lines (in line mode)
    if (ctx.mode == MODE_LINE) {
        if (ctx.show_line) {
            DrawCircle(line_points[0].x, line_points[0].y, 10/ctx.camera.zoom, (Color){0, 0, 0, 50});
            for (uint8_t i = 1; i < line_points_count; i++) {
                DrawCircle(line_points[i].x, line_points[i].y, 10/ctx.camera.zoom, (Color){0, 0, 0, 50});
                DrawLineEx(line_points[i-1], line_points[i], 10/ctx.camera.zoom, (Color){0, 0, 0, 50});
            }
            if (line_points_count < ORDER_LINE_MAX_POINT)
                DrawLineEx(line_points[line_points_count-1], ctx.mouse_position, 10/ctx.camera.zoom, (Color){0, 0, 0, 10});
        }
        DrawCircle(ctx.mouse_position.x, ctx.mouse_position.y, 10/ctx.camera.zoom, (Color){0, 0, 0, 50});
    }
    
    // Draw brush
    if ((ctx.mode == MODE_SPAWN || ctx.mode == MODE_DELETE) && !ctx.local_game) {
        SPoint pos = {(int)ctx.mouse_position.x/BOID_SIZE, (int)ctx.mouse_position.y/BOID_SIZE};
        DrawRectangle(pos.x*BOID_SIZE - ctx.brush_size/2*BOID_SIZE, pos.y*BOID_SIZE - ctx.brush_size/2*BOID_SIZE,
                      ctx.brush_size*BOID_SIZE, ctx.brush_size*BOID_SIZE, (Color){20, 20, 20, 20});
    }
    
    EndMode2D();

    CLEAR_GUI_EVENTS();

    GuiEnableTooltip();
    GuiSetIconScale(2);
    
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
    
    // Draw GUI
    if (ctx.show_gui) {
        // Buttons in the left corner
        int btn_x = BUTTON_MARGIN;
    
        if (ctx.mode == MODE_AREAS || ctx.local_game) {
            for (int team = 0; team < TEAMS_COUNT; team++) {
                if (!ctx.local_game && !ctx.teams_used[team]) continue;
            
                GuiSetState(
                    ((ctx.mode == MODE_SPAWN || ctx.mode == MODE_AREAS) ? ctx.selecting_team == team : GET_EVENT(IE_TEAM_RED+team)) ?
                     STATE_PRESSED : STATE_NORMAL
                );

                const char *str = get_team_name(team);
                set_button_tooltip(IE_TEAM_RED+team, "%c%s color", toupper(str[0]), str+1); // Capitalize team name
        
                if (GuiTextureButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, boids_textures[team],
                                     (Rectangle){0, 0, 128, 150}, (Vector2){BUTTON_SIZE/2.0f, BUTTON_SIZE/2.0f}, 0.7f, -90.0f))
                    GUI_EVENT(IE_TEAM_RED + team);
                btn_x += BUTTON_SIZE + BUTTON_DISTANCE;
            }
        }
        if (ctx.mode == MODE_AREAS) {
            GuiSetState(ctx.selecting_team == -1 ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_ERASE_AREAS, "Erase areas");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_RUBBER, ""))) GUI_EVENT(IE_ERASE_AREAS);
            btn_x += BUTTON_SIZE + GROUP_DISTANCE;
        } else if (ctx.local_game)
            btn_x += GROUP_DISTANCE - BUTTON_DISTANCE;

        if (ctx.mode == MODE_AREAS) {
            bool ok;
            if (ctx.players_number == ctx.joined_players) {
                ok = true;
                for (int team = 0; team < TEAMS_COUNT; team++) {
                    if (ctx.teams_used[team] && ctx.boids_number[team] > areas_size[team]) {
                        ok = false;
                        break;
                    }
                }
            } else ok = false;

            GuiSetState(ok ? STATE_NORMAL : STATE_DISABLED);
            set_button_tooltip(IE_START_PLACING, "Start placing");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_OK_TICK, ""))) GUI_EVENT(IE_START_PLACING);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;
        }

        else if (ctx.stage == STAGE_PLACING) {
            GuiSetState(ctx.mode == MODE_SPAWN ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_MODE_SPAWN, "Spawn mode");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_BOX_MORE, ""))) GUI_EVENT(IE_MODE_SPAWN);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(ctx.mode == MODE_DELETE ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_MODE_DELETE, "Delete mode");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_BIN, ""))) GUI_EVENT(IE_MODE_DELETE);
            btn_x += BUTTON_SIZE + GROUP_DISTANCE;
        
            GuiSetState((ctx.boids_count == ctx.boids_number[ctx.player_team]) ? STATE_NORMAL : STATE_DISABLED);
            set_button_tooltip(IE_READY, "Ready");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_OK_TICK, ""))) GUI_EVENT(IE_READY);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;
        }
    
        else if (ctx.stage == STAGE_GAME) {
            GuiSetState(((ctx.action == ACT_STOP && ctx.mode == MODE_SPAWN) || GET_EVENT(IE_BOID_ACT_STOP)) ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_BOID_ACT_STOP, "Stop action");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_CURSOR_HAND, ""))) GUI_EVENT(IE_BOID_ACT_STOP);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(((ctx.action == ACT_ATTACK && ctx.mode == MODE_SPAWN) || GET_EVENT(IE_BOID_ACT_ATTACK)) ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_BOID_ACT_ATTACK, "Attack action");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_STEP_OUT, ""))) GUI_EVENT(IE_BOID_ACT_ATTACK);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(((ctx.action == ACT_RETREAT && ctx.mode == MODE_SPAWN) || GET_EVENT(IE_BOID_ACT_RETREAT)) ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_BOID_ACT_RETREAT, "Retreat action");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_STEP_INTO, ""))) GUI_EVENT(IE_BOID_ACT_RETREAT);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(GET_EVENT(IE_CLEAR_ORDERS) ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_CLEAR_ORDERS, "Clear orders");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_RESTART, ""))) GUI_EVENT(IE_CLEAR_ORDERS);
            btn_x += BUTTON_SIZE + GROUP_DISTANCE;
    

            if (ctx.local_game) {
                GuiSetState(ctx.mode == MODE_SPAWN ? STATE_PRESSED : STATE_NORMAL);
                set_button_tooltip(IE_MODE_SPAWN, "Spawn mode");
                if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_BOX_MORE, ""))) GUI_EVENT(IE_MODE_SPAWN);
                btn_x += BUTTON_SIZE + BUTTON_DISTANCE;
            }

            GuiSetState(ctx.mode == MODE_SELECT ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_MODE_SELECT, "Select mode");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_BOX_DOTS_BIG, ""))) GUI_EVENT(IE_MODE_SELECT);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(ctx.mode == MODE_DIRECTION ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_MODE_DIRECTION, "Direction mode");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_CURSOR_POINTER, ""))) GUI_EVENT(IE_MODE_DIRECTION);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(ctx.mode == MODE_POINT ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_MODE_POINT, "Point mode");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_TARGET_BIG_FILL, ""))) GUI_EVENT(IE_MODE_POINT);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(ctx.mode == MODE_LINE ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_MODE_LINE, "Line mode");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_LINK_NET, ""))) GUI_EVENT(IE_MODE_LINE);
            btn_x += BUTTON_SIZE + BUTTON_DISTANCE;

            if (ctx.mode == MODE_LINE) {
                GuiSetState(STATE_NORMAL);
                set_button_tooltip(IE_APPLY_LINE, "Apply line");
                if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_OK_TICK, ""))) GUI_EVENT(IE_APPLY_LINE);
                btn_x += BUTTON_SIZE + BUTTON_DISTANCE;
            }

            if (ctx.mode == MODE_SELECT && ctx.local_game) {
                GuiSetState(GET_EVENT(IE_DELETE_SELECTED_BOIDS) ? STATE_PRESSED : STATE_NORMAL);
                set_button_tooltip(IE_DELETE_SELECTED_BOIDS, "Delete selected");
                if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_BIN, ""))) GUI_EVENT(IE_DELETE_SELECTED_BOIDS);
                btn_x += BUTTON_SIZE + BUTTON_DISTANCE;
            }
        }

        // Buttons in the right corner
        btn_x = ctx.screen_width - BUTTON_SIZE - BUTTON_MARGIN;

        if (ctx.local_game) {
            GuiSetState(STATE_NORMAL);
            set_button_tooltip(IE_PAUSE, ctx.game_paused ? "Resume" : "Pause");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ctx.game_paused ? ICON_PLAYER_PLAY : ICON_PLAYER_PAUSE, ""))) GUI_EVENT(IE_PAUSE);
            btn_x -= BUTTON_SIZE + BUTTON_DISTANCE;
        }

        GuiSetState(STATE_NORMAL);
        set_button_tooltip(IE_EXIT_GAME, "Exit");
        if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_EXIT, ""))) GUI_EVENT(IE_EXIT_GAME);
        btn_x -= BUTTON_SIZE + BUTTON_DISTANCE;
        
        if (ctx.stage == STAGE_GAME) {
            GuiSetState(ctx.autoselect_mode ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_CHANGE_AUTOSELECT_MODE, "Auto-selection mode");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_SQUARE_TOGGLE, ""))) GUI_EVENT(IE_CHANGE_AUTOSELECT_MODE);
            btn_x -= BUTTON_SIZE + BUTTON_DISTANCE;
        }

        if (ctx.stage == STAGE_AREAS || ctx.stage == STAGE_PLACING) {
            GuiSetState(ctx.show_grid ? STATE_PRESSED : STATE_NORMAL);
            set_button_tooltip(IE_SHOW_GRID, "%s grid", ctx.show_grid ? "Hide" : "Show");
            if (GuiButton((Rectangle){btn_x, BUTTON_MARGIN, BUTTON_SIZE, BUTTON_SIZE}, GuiIconText(ICON_GRID, ""))) GUI_EVENT(IE_SHOW_GRID);
            btn_x -= BUTTON_SIZE + BUTTON_DISTANCE;
        }

        int btn_y = ctx.screen_height / 2;

        if (ctx.mode == MODE_SPAWN && !ctx.local_game) {
            GuiSetState(STATE_NORMAL);
            set_button_tooltip(IE_BRUSH_REDUCE, "Reduce brush size");
            if (GuiButton((Rectangle){ctx.screen_width - BUTTON_MARGIN - SMALL_BUTTON_SIZE, btn_y, SMALL_BUTTON_SIZE, SMALL_BUTTON_SIZE}, GuiIconText(ICON_BOX_MINUS_FILL, ""))) GUI_EVENT(IE_BRUSH_REDUCE);
            btn_y -= SMALL_BUTTON_SIZE + BUTTON_DISTANCE;

            GuiSetState(STATE_NORMAL);
            set_button_tooltip(IE_BRUSH_INCRASE, "Incrase brush size");
            if (GuiButton((Rectangle){ctx.screen_width - BUTTON_MARGIN - SMALL_BUTTON_SIZE, btn_y, SMALL_BUTTON_SIZE, SMALL_BUTTON_SIZE}, GuiIconText(ICON_BOX_MORE, ""))) GUI_EVENT(IE_BRUSH_INCRASE);
            btn_y -= SMALL_BUTTON_SIZE + BUTTON_DISTANCE;
        }
    }
    
    // // Draw events indicators
    // DrawCircle(20, ctx.screen_height - 20, 10, GRAY);
    // DrawLineEx((Vector2){20, ctx.screen_height - 20}, (Vector2){2 + IE_COUNT*20, ctx.screen_height - 20}, 20, GRAY);
    // DrawCircle(4 + IE_COUNT*20, ctx.screen_height - 20, 10, GRAY);
    // for (int event = 0; event < IE_COUNT; event++) {
    //     DrawCircle(22 + 20*event, ctx.screen_height - 20, 7, GET_EVENT(event) ? GREEN : BLACK);
    //     if (GetMouseX() > (22 + 20*event - 7) && GetMouseX() < (22 + 20*event + 7) && GetMouseY() > ctx.screen_height - 40)
    //         DrawText(TextFormat("%d", event), 15 + 20*event, ctx.screen_height - 45, 15, BLACK);
    // }

    const int y = ctx.show_gui ? (BUTTON_MARGIN + BUTTON_SIZE + 20 + BUTTON_MARGIN) : (TEXT_MARGIN);
    const int x = ctx.screen_width - TEXT_MARGIN;
    
    DrawFPS(x - MeasureText(TextFormat("%d FPS", GetFPS()), 20), y + 22*0);
    
    // Draw "Mode" label
    const char *mode_text = NULL;
    switch (ctx.mode) {
        case MODE_WAIT: mode_text = "Mode: Wait"; break;
        case MODE_AREAS: mode_text = "Mode: Areas"; break;
        case MODE_SPAWN: mode_text = "Mode: Spawn"; break;
        case MODE_DELETE: mode_text = "Mode: Delete"; break;
        case MODE_SELECT: mode_text = "Mode: Select"; break;
        case MODE_DIRECTION: mode_text = "Mode: Direction"; break;
        case MODE_POINT: mode_text = "Mode: Point"; break;
        case MODE_LINE: mode_text = "Mode: Line"; break;
        default: break;
    }
    DrawText(mode_text, x - MeasureText(mode_text, 20), y + 22*1, 20, BLACK);
    
    if (ctx.stage == STAGE_GAME && !ctx.local_game) {
        const char *text;
        switch (ctx.tps_display_type) {
            case TPS_NUM: text = TextFormat("Server TPS: %2d/%2d", ctx.server_tps, ctx.server_target_tps); break;
            case TPS_PERCENT: text = TextFormat("Server TPS: %3.0f%%", (float)ctx.server_tps/ctx.server_target_tps * 100); break;
            case TPS_HIDE: text = GuiIconText(ICON_ARROW_LEFT, ""); break;
            default: text = ""; break;
        }

        if (ctx.show_gui) {
            GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_RIGHT);
            STYLE_START(DEFAULT, TEXT_SIZE, 20);
                const int width = (ctx.tps_display_type == TPS_HIDE) ? 35 : 180;
                GuiSetState(STATE_NORMAL);
                if (GuiLabelButton((Rectangle){ctx.screen_width - TEXT_MARGIN - width, y + 22*2, width, 27}, text)) GUI_EVENT(IE_CHANGE_TPS_DISPLAY);
            STYLE_END();
            GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
        } else {
            DrawText(text, ctx.screen_width - TEXT_MARGIN - MeasureText(text, 20), y + 22*2, 20, BLACK);
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
    
    GuiSetStyle(DEFAULT, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    
    // Draw log
    if (!ctx.local_game) {
        int line = 1;
        if (ctx.show_log) {
            int idx = ctx.log.front;
            pthread_mutex_lock(&log_mtx);
            for (int i = 0; i < ctx.log.size; i++) {
                line += ctx.log.items[idx].lines;
                LogEntry *entry = &ctx.log.items[idx];
            
                // DrawText(log_prefixes[entry->type], 10, screen_height - line*22 - 5, 20, log_colors[entry->type]);
                // DrawText(entry->string, 40, screen_height - line*22 - 5, 20, BLACK);
                DrawText(TextFormat("%s%s", log_prefixes[entry->type], entry->string),
                         TEXT_MARGIN, ctx.screen_height - line*22 - TEXT_MARGIN - 3, 20, log_colors[entry->type]);

                idx = (idx > 0)? (idx - 1) : ctx.log.max_len-1;
            }
            pthread_mutex_unlock(&log_mtx);
        }

        if (ctx.show_gui) {
            line++;
            GuiSetState(STATE_NORMAL);
            if (GuiLabelButton((Rectangle){TEXT_MARGIN-4, ctx.screen_height - line*22 - 22, 30, 30}, GuiIconText(ctx.show_log ? ICON_ARROW_DOWN_FILL : ICON_ARROW_UP_FILL, "")))
                GUI_EVENT(IE_SHOW_LOG);
        }
    }

    // Draw keyboard input
    if (!ctx.local_game) {
        DrawText(log_prefixes[L_INPUT], TEXT_MARGIN, ctx.screen_height - 22 - TEXT_MARGIN, 20, log_colors[L_INPUT]);
        if (!ctx.typing_keyboard_input)
            DrawText("Press '/' or SPACE to start typing . . .", TEXT_MARGIN + MeasureText(log_prefixes[L_INPUT], 20), ctx.screen_height - 23 - TEXT_MARGIN, 20, GRAY);
        else {
            GuiDisableTooltip();
            GuiSetState(STATE_NORMAL);
            STYLE_START(DEFAULT, TEXT_SIZE, 20);
            STYLE_START(TEXTBOX, BORDER_WIDTH, 0);
            STYLE_START(TEXTBOX, BASE_COLOR_PRESSED, 0x00000000);
                const bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
                const char first_char = ctx.input_string[0];
                if (GuiTextBox((Rectangle){TEXT_MARGIN + MeasureText(log_prefixes[L_INPUT], 20), ctx.screen_height - 23 - TEXT_MARGIN, 400, 25},
                               ctx.input_string, LOG_BUF_SIZE, ctx.typing_keyboard_input)) {
                    ctx.typing_keyboard_input = !ctx.typing_keyboard_input;
                    if (enter)
                        GUI_EVENT(IE_INPUT_END);
                }
                if ((first_char == '/') ? (ctx.input_string[0] == '\0') : (first_char == '\0' && ctx.input_string[0] == '\0' && IsKeyPressed(KEY_BACKSPACE))) {
                    ctx.typing_keyboard_input = false;
                }

                if (ctx.typing_keyboard_input) {
                    // Autocomplete last word
                    if (IsKeyPressed(KEY_TAB)) {
                        // Get the word before the cursor
                        char word[LOG_BUF_SIZE];
                        char *word_ptr = ctx.input_string + textBoxCursorIndex;
                        char *cursor_ptr = word_ptr;
                        while (word_ptr > ctx.input_string && !isspace(*(word_ptr-1)))
                            word_ptr--;
                        int word_len = cursor_ptr - word_ptr;
                        memcpy(word, word_ptr, word_len);
                        word[word_len] = '\0';

                        // Get new word
                        char *new_word = autocomple_word(word);
                        int new_word_len = strlen(new_word);
        
                        // Insert new word
                        const int input_len = strlen(ctx.input_string);
                        memmove(cursor_ptr + (new_word_len - word_len), cursor_ptr, input_len - textBoxCursorIndex + (new_word_len - word_len));
                        memmove(word_ptr, new_word, new_word_len);
                        textBoxCursorIndex += new_word_len - word_len;
                    }
                }
            STYLE_END();
            STYLE_END();
            STYLE_END();
        }
    }

    GuiSetIconScale(1);

    // Draw boids number
    if (ctx.local_game) {
        BoidIndex total_boids_number = 0;
        for (int team = 0; team < TEAMS_COUNT; team++)
            total_boids_number += ctx.boids_number[team];

        DrawText(TextFormat("ALL: %2d", total_boids_number), TEXT_MARGIN, y + 0, 20, BLACK);
        DrawText(TextFormat("RED: %2d", ctx.boids_number[TEAM_RED]), TEXT_MARGIN, y + 30, 20, RED);
        DrawText(TextFormat("BLUE: %2d", ctx.boids_number[TEAM_BLUE]), TEXT_MARGIN, y + 50, 20, BLUE);
        DrawText(TextFormat("GREEN: %2d", ctx.boids_number[TEAM_GREEN]), TEXT_MARGIN, y + 70, 20, GREEN);
        DrawText(TextFormat("YELLOW: %2d", ctx.boids_number[TEAM_YELLOW]), TEXT_MARGIN, y + 90, 20, ORANGE);
    } else {
        int text_y = (ctx.mode == MODE_WAIT) ? TEXT_MARGIN : y;
        
        int l = 0; // line
        pthread_mutex_lock(&players_mtx);
        for (int team = 0; team < TEAMS_COUNT; team++) {
            if (!ctx.teams_used[team]) continue;
        
            bool player_joined = false;
            int player_idx = 0;
            for (; player_idx < ctx.joined_players; player_idx++) {
                if (ctx.players[player_idx].team == team) {
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
            if (ctx.stage == STAGE_AREAS) {
                // <player_name>: <target_boids_number> (<areas_size><! if boids_count less than areas_size>)
                str = TextFormat("%s: %d%c(%d%s", player_joined ? ctx.players[player_idx].name : "-", ctx.boids_number[team], ctx.new_room ? ' ' : '\0',
                                        areas_size[team], (ctx.boids_number[team] < areas_size[team])? ")" : "!)");
            } else if (ctx.stage == STAGE_PLACING) {
                if (team == (signed)ctx.player_team) {
                    str = TextFormat("%s: %d (%d left) %s", ctx.players[player_idx].name, ctx.boids_count, ctx.boids_number[team]-ctx.boids_count,
                                            ctx.players[team].ready? "ready" : "");
                } else {
                    if (player_joined)
                        str = TextFormat("%s: - %s", ctx.players[player_idx].name, ctx.players[team].ready? "(ready)" : "");
                    else
                        str = "- : -";
                }
            } else if (ctx.stage == STAGE_GAME) {
                str = TextFormat("%s: %d", ctx.players[player_idx].name, ctx.boids_number[team]);
            }

            if (str != NULL)
                DrawText(str, TEXT_MARGIN, text_y + (l++)*22, 20, team_color);
        }
        pthread_mutex_unlock(&players_mtx);
    }

    if (ctx.stage == STAGE_GAME && !ctx.local_game) {
        static bool win_message = false;
        static uint32_t winner_id;

        if (!win_message) {
            int active_players_count = 0;
            int winner_team = -1;
            for (int team = 0; team < TEAMS_COUNT; team++) {
                if (ctx.boids_number[team] > 0) {
                    active_players_count++;
                    winner_team = team;
                }
            }
            
            if (active_players_count == 1) {
                for (int i = 0; i < ctx.joined_players; i++) {
                    ClientPlayer *p = &ctx.players[i];
                    if (p->team == winner_team) {
                        win_message = true;
                        winner_id = p->id;
                        log_message(&ctx.log, L_INFO, "player '%s' has won!\n", p->name);
                    }
                }
            }
        }

        if (win_message && winner_id == ctx.player_id) {
            static int confetti_timer = 0;
            if (confetti_timer < 60*60) {
                confetti_timer++;
                draw_confetti(reset);
            }
        }
    }

    #undef BUTTON_SIZE
    #undef BUTTON_DISTANCE
    #undef GROUP_DISTANCE
    #undef BUTTON_MARGIN
    #undef TEXT_MARGIN

    if (ctx.exit_game_message) {
        GuiDisableTooltip();
        GuiSetState(STATE_NORMAL);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(RAYWHITE, 0.8f));
        int btn_active = -1;
        GuiMessageBox((Rectangle){ (float)GetScreenWidth()/2 - 125, (float)GetScreenHeight()/2 - 50, 250, 100 }, 
            GuiIconText(ICON_EXIT, "Exiting the game"), "Do you really want to exit?", "Yes;No", &btn_active);

        if (btn_active != -1 || IsKeyPressed(KEY_ENTER)) ctx.exit_game_message = false;
        if (btn_active == 1 || IsKeyPressed(KEY_ENTER)) next_menu = MENU_MAIN;
    }
    
    // END DRAWING -----------------------------------------------------------------------------

    handle_input();

    prev_mouse_position = ctx.mouse_position;

    return next_menu;
}
