#include <sys/socket.h>
#include "utils.h"

#include <stdio.h>

int send_msg(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t total_sent = len;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) 
            return -1;
        p += n;
        len -= n;
    }
    return total_sent;
}

int recv_msg(int fd, void *buf, size_t len) {
    char *p = buf;
    size_t total_received = len;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) 
            return -1;
        p += n;
        len -= n;
    }
    return total_received;
}