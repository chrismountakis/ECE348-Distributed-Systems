/* 
   Path traversal and absolute path rejection.
   All three open attempts must return -1. Any success is a security failure.
   Usage: ./test_security <server_ip> <port> 
*/
#include <stdio.h>
#include <stdlib.h>
#include "../client.h"

#define BLOCK_SIZE  512
#define MAX_CACHED  4
#define FRESH_TIME  5

static const char *BAD_PATHS[] = {
    "../etc/passwd",
    "/etc/passwd",
    "../../etc/shadow",
    "../server.c",
};
#define NUM_PATHS (int)(sizeof(BAD_PATHS) / sizeof(BAD_PATHS[0])) // 4*sizeof(char *) / sizeof(char *)

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    const char *ip   = argv[1];
    int         port = atoi(argv[2]);

    if (mynfs_init((char *)ip, port, BLOCK_SIZE, MAX_CACHED, FRESH_TIME) < 0) {
        fprintf(stderr, "FAIL: mynfs_init\n"); return 1;
    }

    int failed = 0;
    for (int i = 0; i < NUM_PATHS; i++) {
        int fd = mynfs_open((char *)BAD_PATHS[i]);
        if (fd >= 0) {
            fprintf(stderr, "FAIL: server accepted path '%s' (fd=%d), path traversal vuln\n",
                    BAD_PATHS[i], fd);
            mynfs_close(fd);
            failed = 1;
        } else {
            printf("[security] '%s' correctly rejected\n", BAD_PATHS[i]);
        }
    }

    if (failed) return 1;
    printf("PASS: all bad paths rejected\n");
    return 0;
}
