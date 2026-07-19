#ifndef _WIN32
    #include <sys/socket.h>
    #include <arpa/inet.h>
#else
    #include "winsupport.h"
#endif
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "network.h"

#define MAX_STACK_SEND_BUFFER_SIZE (1024*32)

// Send n bytes from buf
int send_all(int fd, void *buf, size_t n, int flags) {
    if (n == 0) return 0;
    
    size_t total = 0;
    while (total < n) {
        ssize_t sent = send(fd, (char*)buf + total, n - total, flags);
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
int send_packet(int fd, uint8_t packet_type, void *buf, uint32_t len, int flags) {
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
    if (len > 0)
        memcpy(buffer+1+sizeof(nlen), buf, len);

    int res = send_all(fd, buffer, total_len, flags);
    
    if (use_heap) free(buffer);

    return res;
}

// sendto analog for send_packet
int sendto_packet(int fd, uint8_t packet_type, void *buf, uint32_t len, int flags, struct sockaddr *addr, socklen_t addrlen) {
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
    if (len > 0)
        memcpy(buffer+1+sizeof(nlen), buf, len);

    int res = sendto(fd, (void*)buffer, total_len, flags, addr, addrlen);
    
    if (use_heap) free(buffer);

    return res;
}

// Receive n bytes to buf
int recv_all(int fd, void *buf, size_t n, int flags) {
    if (n == 0) return 0;
    
    size_t total = 0;
    while (total < n) {
        ssize_t received = recv(fd, (char*)buf + total, n - total, flags);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            else
                return 1; // Error
        } else if (received == 0)
            return -1; // No data
        total += received;
    }
    return 0; // Success
}

// Receive len + buf
int recv_packet(int fd, void *buf, uint32_t *len, int flags) {
    int r;
    if ((r = recv_all(fd, len, sizeof(*len), flags)) != 0)
        return r;
    *len = ntohl(*len);
    if ((r = recv_all(fd, buf, *len, flags)) != 0)
        return r;
    return 0;
}

