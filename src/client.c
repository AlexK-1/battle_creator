#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/types.h>
#include <unistd.h>
#include "boids.h"
#include "sock.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 450

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

int main(int argc, char **argv) {
    /* FORMAT
     Join to the room:
         ./client join [-s|--server] <server_ip> [--room|-r] <room_id> [--name|-n] <username>
     Create new room:
         ./client new [-s|--server] <server_ip> [--players|-p] <players_count> <boids_count>
         where [boids_count] is in the format <team>:<count>, setting <count> to the number of boids in <team>
         Example (4 teams, red and blue - 2000 boids, green - 1000 boids, yellow - 500 boids):
             ./client new 4 r:b:2000 g:1000 y:500
    */
    bool new_room;
    uint32_t room_id = 0;
    int players_number = 0, player_team = TEAM_RED;
    BoidIndex boids_number[TEAMS_COUNT] = { 0 }, total_boids_number = 0;
    char *username = NULL, *server = NULL, *player_team_name = NULL;
    
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
    
    // Create a TCP socket
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(INPUT_PORT);
    socklen_t addrlen = sizeof(servaddr);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, SERVER, &servaddr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        close(client_fd);
        return 1;
    }

    int status = connect(client_fd, (struct sockaddr*)&servaddr, addrlen);
    if (status < 0) {
        perror("connect");
        close(client_fd);
        return 1;
    }

    // Join/new room request
    SPJoined recv_data;
    if (new_room) {
        CPNew data = {.players_number = players_number, .player_team = player_team};
        for (int i = 0; i < TEAMS_COUNT; i++)
            data.boids_number[i] = htons(boids_number[i]);
        strncpy(data.creator, username, USERNAME_LEN-1);
        data.creator[USERNAME_LEN-1] = '\0';

        send_packet(client_fd, CP_NEW_ROOM, &data, sizeof(data), 0);

        uint8_t packet_type;
        recv(client_fd, &packet_type, 1, 0);
        if (packet_type != SP_JOIN_PLAYER) {
            fputs("[*] unable to create a room\n", stderr);
            return 1;
        }
        
        uint32_t data_len;
        recv_packet(client_fd, &recv_data, &data_len, 0);
        if (data_len != sizeof(SPJoined) || recv_data.status != JOIN_OK) {
            fputs("[*] unable to create a room\n", stderr);
            return 1;
        }
    } else {
        CPJoin data = {.room_id = htonl(room_id)};
        strncpy(data.username, username, USERNAME_LEN-1);
        data.username[USERNAME_LEN-1] = '\0';

        send_packet(client_fd, CP_JOIN_ROOM, &data, sizeof(data), 0);

        uint8_t packet_type;
        recv(client_fd, &packet_type, 1, 0);
        if (packet_type != SP_JOIN_PLAYER) {
            fputs("[*] unable to join to the room\n", stderr);
            return 1;
        }
        
        uint32_t data_len;
        recv_packet(client_fd, &recv_data, &data_len, 0);
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

    // Copy joined players
    room_id = ntohl(recv_data.room_id);
    players_number = recv_data.players_number;
    uint8_t player_id = ntohl(recv_data.player_id);
    int joined_players = recv_data.joined_players;
    ClientPlayer players[TEAMS_COUNT] = { 0 };
    for (int i = 0; i < joined_players; i++) {
        memcpy(&players[i], &recv_data.players[i], sizeof(ClientPlayer));
        players[i].id = ntohl(players[i].id);
    }

    total_boids_number = 0;
    for (int i = 0; i < recv_data.players_number; i++) {
        BoidIndex h = htons(recv_data.teams[i]);
        boids_number[i] = h;
        total_boids_number += h;
    }

    printf("[*] %s\n    id: %06x\n    teams: %d\n    creator: %s (id = %d)\n    boids:  %-4d\n    red:    %-4d\n    blue:   %-4d\n    green:  %-4d\n    yellow: %-4d\n",
           new_room? "created a room" : "joined to the room",
           room_id, players_number, players[0].name, players[0].id, total_boids_number,
           boids_number[TEAM_RED],
           boids_number[TEAM_BLUE],
           boids_number[TEAM_GREEN],
           boids_number[TEAM_YELLOW]);

    if (!new_room) {
        printf("[*] players:\n");
        for (int i = 0; i < joined_players; i++) {
            ClientPlayer *op = &players[i];

            char *team = get_team_name(op->team);

            printf("    %s (id = %d) - %s\n", op->name, op->id, team);
        }
        putchar('\n');
    }


    bool ex = false;
    while (1) {
        uint8_t packet_type;
        if (recv(client_fd, &packet_type, 1, 0) <= 0)
            break;

        switch (packet_type) {
        case SP_APPROVE_PLAYER: { // Approje/reject new player
            SPApprove other_player;
            uint32_t packet_len;
            if (recv_packet(client_fd, &other_player, &packet_len, 0)) {
                ex = true;
                break;
            }
            if (packet_len != sizeof(other_player)) {
                ex = true;
                break;
            }
            other_player.fd = ntohl(other_player.fd);

            printf("[?] team of new player '%s' (id=%d) (r/b/g/y or n for reject): ", other_player.username, other_player.fd);

            
            int8_t other_team = -2;
            while (other_team == -2) {
                char other_team_char = getchar();

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
                    break;
                }
                
                if ((other_team != -1) && ((other_team == -2)? (other_team_char != '\n' && other_team_char != '\r') : (boids_number[other_team] == 0))) {
                    printf("enter valid team\n");
                    other_team = -2;
                } else {
                    bool team_used = false;
                    for (int i = 0; i < joined_players; i++) {
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
                send_packet(client_fd, CP_APPROVE_PLAYER, &other_team, 1, 0);
            
            break;   
            }
        case SP_NEW_JOIN: {
            ClientPlayer new_player;
            uint32_t packet_len;
            if (recv_packet(client_fd, &new_player, &packet_len, 0)) {
                ex = true;
                break;
            }
            if (packet_len != sizeof(new_player)) {
                ex = true;
                break;
            }
            new_player.id = ntohl(new_player.id);

            players[joined_players++] = new_player;
            printf("[+] new player '%s' (id = %d) - %s\n", new_player.name, new_player.id, get_team_name(new_player.team));
            
            break;
            }
        case SP_PLAYER_EXIT: {
            uint32_t exited_player, packet_len;
            if (recv_packet(client_fd, &exited_player, &packet_len, 0)) {
                ex = true;
                break;
            }
            if (packet_len != sizeof(exited_player)) {
                ex = true;
                break;
            }
            exited_player = ntohl(exited_player);

            int player_idx = get_player_idx(players, exited_player);
            printf("[-] disconnected player '%s' (id = %d)\n", players[player_idx].name, exited_player);
            
            // delete player from array
            memmove(players + player_idx, players + player_idx + 1,
                    sizeof(players[0]) * (joined_players - player_idx - 1));
            joined_players--;
            
            break;
            }
        }

        if (ex)
            break;
    }
    
    // SetTraceLogLevel(LOG_WARNING);
    // SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    // InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Battle creator");
    // ToggleFullscreen();
    // SetTargetFPS(60);
    
    close(client_fd);    
    return 0;
}
