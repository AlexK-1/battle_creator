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

#include "boids.h"
#include "sock.h"
#include "queue.h"

#define MAX_ROOMS 256
#define MAX_EVENTS 1024
#define MAX_APPROVING_QUEUE_LEN 10
#define MAX_PACKET_SIZE (1024*64)

/*
join -> room
connect -> server
*/

typedef struct Player {
    uint32_t fd;
    char name[USERNAME_LEN];
    uint8_t team;
    bool joined;
    struct Room *room;
    struct {
        struct Player **items;
        int front, rear, size;
        size_t max_len;
    } approving_queue;
    struct {
        enum {
            PARSE_TYPE,
            PARSE_LEN,
            PARSE_DATA
        } state;
        uint8_t type;
        uint32_t data_len, bytes_remaining;
        uint8_t *data_buf, recv_buf[1024];
    } net;
} Player;

typedef struct Room {
    Player *players[TEAMS_COUNT];
    BoidIndex teams[TEAMS_COUNT];
    uint8_t players_number, joined_players;
    uint32_t id;
    Point world;
    enum {
        ROOM_AREAS,
        ROOM_PLACING,
        ROOM_GAME
    } status;
} Room;

/* V global variables V */
Player **players = NULL;
Room *rooms[MAX_ROOMS] = { 0 };
int32_t last_room_idx = -1;
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

void close_client(int epfd, int fd) {
    // close files
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    Player *p = players[fd];

    // room
    if (p->joined && p->room != NULL) {
        Room *room = p->room;

        int player_idx = get_player_idx(room->players, p->fd);

        last_room_idx = ((room->id & 0xffff0000) >> 16);
        if (player_idx == 0) { // room's creator disconected, close entire room
            printf("[*] closed room %06x\n", room->id);

            for (int i = 1; i < room->joined_players; i++) {
                Player *op = room->players[i]; // other_player
                op->joined = false;
                close_client(epfd, op->fd);
            }
            while (p->approving_queue.size > 0) {
                Player *op;
                dequeue(p->approving_queue, op);
                op->joined = false;
                close_client(epfd, op->fd);
            }

            free(room);
            rooms[last_room_idx] = NULL;
        } else {
            // delete player from array
            memmove(room->players + player_idx, room->players + player_idx + 1,
                    sizeof(room->players[0]) * (room->joined_players - player_idx - 1));
            room->joined_players--;

            // send a message to all players in the room that the player has disconnected
            uint32_t nfd = htonl(p->fd);
            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i];
                send_packet(op->fd, SP_PLAYER_EXIT, &nfd, sizeof(op->fd), 0);
            }
        }

        last_room_idx--;
    }

    // player
    if (p->approving_queue.max_len != 0) free(p->approving_queue.items);
    free(p);
    players[fd] = NULL;

    printf("[-] fd=%d hung up\n", fd);
}

void process_data(Player *p) {
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
            uint32_t null_room = 0;
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
        for (int j = 0; j < TEAMS_COUNT; j++)
            room->teams[j] = ntohs(data.boids_number[j]);
        p->room = room;
        room->status = ROOM_AREAS;
        rooms[last_room_idx] = room;

        BoidIndex total_boids_number = 0;
        for (int team_idx = 0; team_idx < TEAMS_COUNT; team_idx++)
            total_boids_number += room->teams[team_idx];

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
               room->id, data.players_number, room->world.x, room->world.y, data.creator, p->fd, total_boids_number,
               room->teams[TEAM_RED],
               room->teams[TEAM_BLUE],
               room->teams[TEAM_GREEN],
               room->teams[TEAM_YELLOW]);

        SPJoined send_data = {.room_id = htonl(room->id), .player_id = htonl(p->fd), .players_number = room->players_number,
                              .joined_players = room->joined_players, .player_team = p->team, .world_size = data.world_size, .status = JOIN_OK};

        for (int i = 0; i < room->joined_players; i++) {
            Player *op = room->players[i]; // other_player
            send_data.players[i] = (ClientPlayer){.id = htonl(op->fd), .team = op->team};
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
            
            if (room_owner->approving_queue.size == 1) {
                SPApprove send_data = {htonl(p->fd)};
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
        int8_t data;
        if (p->net.data_len != sizeof(data))
            break;
        memcpy(&data, p->net.data_buf, sizeof(data));

        Player *approving_player = NULL;
        dequeue(p->approving_queue, approving_player);

        if (data == -1) {
            SPJoined send_data = {.status = JOIN_REJECTED};
            send_packet(p->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);
            approving_player->joined = false;
        } else {
            approving_player->joined = true;
            approving_player->team = data;
            approving_player->room = p->room;

            Room *room = p->room;
            room->players[room->joined_players++] = approving_player;

            SPJoined send_data = {.room_id = htonl(room->id), .player_id = htonl(approving_player->fd), .players_number = room->players_number,
                                  .joined_players = room->joined_players, .player_team = approving_player->team,
                                  .world_size = {htons(room->world.x), htons(room->world.y)}, .status = JOIN_OK};

            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i]; // other_player
                send_data.players[i] = (ClientPlayer){.id = htonl(op->fd), .team = op->team};
                strcpy(send_data.players[i].name, op->name);
            }

            for (int i = 0; i < TEAMS_COUNT; i++) {
                send_data.teams[i] = htons(room->teams[i]);
            }

            send_packet(approving_player->fd, SP_JOIN_PLAYER, &send_data, sizeof(send_data), 0);

            // send a message to all players in the room that the player has joined
            ClientPlayer player_data = {.id = htonl(approving_player->fd), .team = approving_player->team};
            strcpy(player_data.name, approving_player->name);
            for (int i = 0; i < room->joined_players; i++) {
                Player *op = room->players[i];
                if (op != approving_player)
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
            approving_player = queue_front(p->approving_queue);
            
            SPApprove send_data = {htonl(approving_player->fd)};
            strcpy(send_data.username, approving_player->name);

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
    }
}

int client_recv(Player *p, int32_t *last_room_idx) {
    while (1) {
        // Recive data
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
        int remaining = n;
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
                    process_data(p);
                    
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
        perror("setsockopt");
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
    servaddr.sin_port = htons(INPUT_PORT);
    servaddr.sin_family = AF_INET;
    socklen_t addrlen = sizeof(servaddr);

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

    // Create epool instance
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

    printf("epoll server on 0.0.0.0:%d\n", INPUT_PORT);
    
    while (1) {
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
                    *player = (Player){.fd = client_fd, .joined = false, .approving_queue = { 0 }};
                    // player->fd = client_fd;
                    // player->joined = false;
                    // player->approving_queue.max_len = 0;
                    // player->approving_queue.items = NULL;
                    players[client_fd] = player;

                    char ip[INET_ADDRSTRLEN];
                    if (!inet_ntop(servaddr.sin_family, &client_addr.sin_addr, ip, sizeof(ip)))
                        strcpy("?", ip);
                    printf("[+] %s:%d (fd=%d)\n", ip, ntohs(client_addr.sin_port), client_fd);

                    event.events = EPOLLIN | EPOLLRDHUP;
                    event.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &event)) {
                        perror("epoll_ctl");
                        free(players[client_fd]);
                        close(client_fd);
                    }
                }
            } else if (events[i].events & EPOLLERR) {
                // Disconnect client
                close_client(epfd, fd);
            } else {
                Player *player = players[fd];
                if (client_recv(player, &last_room_idx)) {
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

    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i] != NULL) {
            free(rooms[i]);
        }
    }
        
    close(epfd);
    close(server_fd);
}
