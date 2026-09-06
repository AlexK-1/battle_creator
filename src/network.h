#ifndef _WIN32
    #include <sys/socket.h>
#else
    #include "winsupport.h"
#endif
#include <stdint.h>
#include <stddef.h>
#include "boids.h"

#ifndef NETWORK_H
#define NETWORK_H

#define DEFAULT_SERVER "127.0.0.1"
#define TCP_PORT 3440
#define UDP_PORT 3441

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
    int8_t team;
} Area;

typedef enum {
    SP_CLOSE,
    SP_APPROVE_PLAYER, // Approve/reject request for new player
    SP_JOIN_PLAYER, // Player joined to the room (sent after the request to create/join to the room)
    SP_NEW_JOIN, // New player joined to the room
    SP_PLAYER_EXIT, // Player left the room
    SP_SEND_AREAS, // Areas of the admin
    SP_START_PLACING, // Admin of the room starts placing of the boids
    SP_PLAYER_READY, // A message, that the player is ready to start the game
    SP_START_GAME, // When all players have placed their boids
    SP_BOIDS_SYNC, // Boids sync server->clients
    SP_INVALID_PACKET, // Client sent an invalid packet
    SP_DISCONNECT_PLAYER, // Server terminated connection with client
    SP_PLAYER_KICKED, // Room's admin kicked player
    SP_CHANGE_TEAM, // Admin changed player's team
    SP_SWAP_TEAMS, // Swap teams of two players
    SP_CHAT_MSG // Message to the room's chat
} SPType; // Server packet type

typedef enum {
    CP_CLOSE,
    CP_UDP_HELLO, // First client->server UDP packet
    CP_NEW_ROOM, // Create new room
    CP_JOIN_ROOM, // Join to the room
    CP_APPROVE_PLAYER, // Approve/reject new player
    CP_SEND_AREAS, // Sending areas of the admin to other players
    CP_START_PLACING, // Admin of the room starts placing of the boids
    CP_SEND_BOIDS, // Sending player's boids before the game starts
    CP_ORDER, // Sending player's order to his boids
    CP_CLOSE_ROOM, // Admin can close the room
    CP_KICK_PLAYER, // Kick the player out of the room
    CP_CHANGE_TEAM, // Change player's team
    CP_SWAP_TEAMS, // Swap teams of two players
    CP_CHAT_MSG // Send a message to the room's chat
} CPType; // Client packet type

typedef struct {
    uint32_t id;
    int8_t team;
    uint8_t ready;
    char name[USERNAME_LEN];
} ClientPlayer;

typedef enum {
    JOIN_OK,
    JOIN_FAILED,
    JOIN_REJECTED,
    JOIN_STATUSES_COUNT
} RoomJoiningStatus;

typedef enum {
    DISCONNECT_KICKED,
    DISCONNECT_ADMIN_CLOSED_ROOM,
    DISCONNECT_PLAYER_EXITED,
    DISCONNECT_ADMIN_EXITED,
    DISCONNECT_PACKET_VIOLATIONS,
    DISCONNECT_SERVER_ERROR,
    DISCONNECT_SERVER_DOWN
} DisconnectionReason;

typedef enum {
    STAGE_NULL = 0,
    STAGE_AREAS,
    STAGE_PLACING,
    STAGE_GAME,
    STAGE_COUNT
} RoomStage;

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

int send_all(int fd, void *buf, size_t n, int flags); // Send n bytes from buf
int send_packet(int fd, uint8_t packet_type, void *buf, uint32_t len, int flags); // Send packet_type + len + buf
int sendto_packet(int fd, uint8_t packet_type, void *buf, uint32_t len, int flags, struct sockaddr *addr, socklen_t addrlen); // sendto analog for send_packet
int recv_all(int fd, void *buf, size_t n, int flags); // Receive n bytes to buf
int recv_packet(int fd, void *buf, uint32_t *len, int flags); // Receive len + buf

#endif // NETWORK_H
