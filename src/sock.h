#ifndef _WIN32
    #include <sys/socket.h>
    #include <arpa/inet.h>
#else
    #include "winsupport.h"
#endif
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "boids.h"

#ifndef SOCK_H
#define SICK_H

#define SERVER "127.0.0.1"
#define INPUT_PORT 3440
#define SYNC_PORT 3441
#define MAX_STACK_SEND_BUFFER_SIZE (1024*32)

#define USERNAME_LEN 32

/*
CPSomething - client packet "Something"
SPSomething - server packet "Something"
PSomething - client & server packet "something"
ClientSomething - client structure "Something"
ServerSomething - server structure "Something"
*/

typedef struct {
    uint16_t x, y;
} Point;

typedef struct {
    int32_t x, y;
} SPoint; // signed Pint

typedef struct {
    uint16_t x1, y1, x2, y2;
} Rec;

typedef struct {
    int32_t x1, y1, x2, y2;
} SRec; // signed Rec

typedef struct {
    Rec rec;
    uint8_t team;
} Area;

typedef enum {
    SP_CLOSE,
    SP_APPROVE_PLAYER, // Approve/reject request for new player
    SP_JOIN_PLAYER, // Player joined to the room (sent after the request to create/join to the room)
    SP_NEW_JOIN, // New player joined to the room
    SP_PLAYER_EXIT, // Player left the room
    SP_START_PLACING, // Admin of the room starts placing of the boids
    SP_START_GAME // When all players have placed their boids
} SPType; // Server packet type

typedef enum {
    CP_CLOSE,
    CP_NEW_ROOM, // Create new room
    CP_JOIN_ROOM, // Join to the room
    CP_APPROVE_PLAYER, // Approve/reject new player
    CP_START_PLACING, // Admin of the room starts placing of the boids
    CP_SEND_BOIDS // Sending player's boids before the game starts
} CPType; // Client packet type

typedef struct {
    int32_t fd;
    char username[USERNAME_LEN];
} SPApprove;

typedef struct {
    uint32_t id;
    int8_t team;
    char name[USERNAME_LEN];
} ClientPlayer;

typedef enum {
    JOIN_OK,
    JOIN_FAILED,
    JOIN_REJECTED,
} RoomJoiningStatus;

typedef struct {
    uint32_t room_id, player_id;
    uint8_t players_number, joined_players, player_team;
    Point world_size;
    ClientPlayer players[TEAMS_COUNT];
    BoidIndex teams[TEAMS_COUNT];
    uint8_t status;
} SPJoined;

typedef struct {
    uint8_t players_number, player_team;
    Point world_size;
    BoidIndex boids_number[TEAMS_COUNT];
    char creator[USERNAME_LEN];
} CPNew;

typedef struct {
    uint32_t room_id;
    char username[USERNAME_LEN];
} CPJoin;

static inline int send_all(int fd, void *buf, size_t n, int flags) {
    size_t total = 0;
    while (total < n) {
        ssize_t sent = send(fd, (uint8_t*)buf + total, n - total, flags);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            else
                return 1; // Error
        }
        total += sent;
    }
    return 0; // Success
}

// Send packet_type + len + buf
static inline int send_packet(int fd, uint8_t packet_type, void *buf, uint32_t len, int flags) {
    uint32_t total_len = 1 + sizeof(len) + len;

    uint8_t *buffer = NULL;
    uint8_t stack_buffer[MAX_STACK_SEND_BUFFER_SIZE];
    bool use_heap = total_len > MAX_STACK_SEND_BUFFER_SIZE;
    if (use_heap) {
        buffer = malloc(total_len);
    } else {
        buffer = stack_buffer;
    }
    
    buffer[0] = packet_type;
    uint32_t nlen = htonl(len);
    memcpy(buffer+1, &nlen, sizeof(nlen));
    memcpy(buffer+1+sizeof(nlen), buf, len);

    int res = send_all(fd, buffer, total_len, 0);
    
    if (use_heap) free(buffer);

    return res;
}

static inline int recv_all(int fd, void *buf, size_t n, int flags) {
    size_t total = 0;
    while (total < n) {
        ssize_t recived = recv(fd, (uint8_t*)buf + total, n - total, flags);
        if (recived < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            else
                return 1; // Error
        } else if (recived == 0)
            return -1; // No data
        total += recived;
    }
    return 0; // Success
}

// Recive len + buf
static inline int recv_packet(int fd, void *buf, uint32_t *len, int flags) {
    int r;
    if ((r = recv_all(fd, len, sizeof(*len), flags)) != 0)
        return r;
    *len = ntohl(*len);
    if ((r = recv_all(fd, buf, *len, flags)) != 0)
        return r;
    return 0;
}

#endif // SOCK_H
