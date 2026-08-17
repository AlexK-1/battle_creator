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
    SP_ROOM_CLOSED, // Room is closed
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
    char username[USERNAME_LEN];
} SPApprove;

typedef struct {
    uint32_t id;
    int8_t team;
} CPApprove;

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
} RoomJoiningStatus;

typedef struct {
    uint32_t room_id, player_id;
    int32_t player_tcp_fd;
    uint8_t players_number, joined_players, player_team, server_target_tps;
    Point world_size;
    ClientPlayer players[TEAMS_COUNT];
    BoidIndex teams[TEAMS_COUNT];
    uint8_t status, room_stage;
} SPJoined;

typedef struct {
    uint8_t players_number, player_team, hide_areas;
    Point world_size;
    BoidIndex boids_number[TEAMS_COUNT];
    char creator[USERNAME_LEN];
} CPNew;

typedef struct {
    uint32_t room_id;
    char username[USERNAME_LEN];
} CPJoin;

typedef enum {
    STAGE_NULL = 0,
    STAGE_AREAS,
    STAGE_PLACING,
    STAGE_GAME
} RoomStage;

int send_all(int fd, void *buf, size_t n, int flags); // Send n bytes from buf
int send_packet(int fd, uint8_t packet_type, void *buf, uint32_t len, int flags); // Send packet_type + len + buf
int sendto_packet(int fd, uint8_t packet_type, void *buf, uint32_t len, int flags, struct sockaddr *addr, socklen_t addrlen); // sendto analog for send_packet
int recv_all(int fd, void *buf, size_t n, int flags); // Receive n bytes to buf
int recv_packet(int fd, void *buf, uint32_t *len, int flags); // Receive len + buf

#endif // NETWORK_H
