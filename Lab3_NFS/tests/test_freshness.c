/* 
   -Test 1 — stale revalidation (automated):
     Reads a block, sleeps past FRESH_TIME so it goes stale, reads again.
     Server replies ERR_NOTMODIFIED (file unchanged) and resets tfresh.
     A third immediate read must be a cache hit. All three reads must return
     byte-identical data.
  
   -Test 2 — FRESH_TIME=0 absolute consistency:
     Set FRESH_TIME=0. Every single read must log
     "Stale revalidation" — never "Cache hit". There is no fresh window so
     the server is contacted on every block access.
  
   Usage: ./test_freshness <server_ip> <port> <file> 
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client.h"

#define BLOCK_SIZE  512
#define MAX_CACHED  4
#define FRESH_TIME  3   

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <file>\n", argv[0]);
        return 1;
    }
    const char *ip   = argv[1];
    int         port = atoi(argv[2]);
    const char *file = argv[3];

    if (mynfs_init((char *)ip, port, BLOCK_SIZE, MAX_CACHED, FRESH_TIME) < 0) {
        fprintf(stderr, "FAIL: mynfs_init\n"); return 1;
    }

    int fd = mynfs_open((char *)file);
    if (fd < 0) { fprintf(stderr, "FAIL: open\n"); return 1; }

    char first[BLOCK_SIZE], stale[BLOCK_SIZE], cached[BLOCK_SIZE];

    // Read 1: cold miss — block cached 
    printf("[freshness] read 1 (expect: Cold miss)\n");
    long r1 = mynfs_read(fd, first, BLOCK_SIZE);
    if (r1 <= 0) { fprintf(stderr, "FAIL: read 1 returned %ld\n", r1); return 1; }

    printf("[freshness] sleeping %d s for block to go stale...\n", FRESH_TIME + 1);
    sleep(FRESH_TIME + 1);

    // Read 2: stale revalidation — expect 'Stale revalidation' + 'Not modified' in log 
    mynfs_lseek(fd, 0, SEEK_SET);
    printf("[freshness] read 2 (expect: Stale revalidation -> Not modified)\n");
    long r2 = mynfs_read(fd, stale, BLOCK_SIZE);
    if (r2 <= 0) { fprintf(stderr, "FAIL: read 2 returned %ld\n", r2); return 1; }

    // Read 3: immediately after — tfresh was just reset, must be a cache hit 
    mynfs_lseek(fd, 0, SEEK_SET);
    printf("[freshness] read 3 (expect: Cache hit)\n");
    long r3 = mynfs_read(fd, cached, BLOCK_SIZE);
    if (r3 <= 0) { fprintf(stderr, "FAIL: read 3 returned %ld\n", r3); return 1; }

    if (r1 != r2 || r2 != r3) {
        fprintf(stderr, "FAIL: byte counts differ (%ld %ld %ld)\n", r1, r2, r3);
        return 1;
    }
    if (memcmp(first, stale, r1) != 0) {
        fprintf(stderr, "FAIL: data changed after stale revalidation\n"); return 1;
    }
    if (memcmp(stale, cached, r2) != 0) {
        fprintf(stderr, "FAIL: data changed on cache hit after revalidation\n"); return 1;
    }

    printf("PASS: all three reads returned identical %ld bytes\n", r1);
    mynfs_close(fd);
    return 0;
}
