/* 
   GC recovery via ERR_BADFID transparent reopen.

   Reads one block (server assigns fid), sleeps past FT_GC_INTERVAL (10 s)
   so the server GCs the fid, then reads a different block that was not
   cached — forcing a server request with the now-dead fid.
   The client must transparently recover (ERR_BADFID -> reopen -> retry)
   with no error visible to the caller.
 
   Requires: server compiled with FT_GC_INTERVAL=10 (the default).

   Usage: ./test_gc <server_ip> <port> <file> 
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client.h"

#define BLOCK_SIZE    512
#define MAX_CACHED    4
#define FRESH_TIME    60   // keep block 0 fresh across the sleep 
#define SLEEP_SECS    12   // > FT_GC_INTERVAL (10) 

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

    // Read block 0 — server creates fid, block goes into cache 
    char block0[BLOCK_SIZE];
    long r = mynfs_read(fd, block0, BLOCK_SIZE);
    if (r <= 0) { fprintf(stderr, "FAIL: initial read returned %ld\n", r); return 1; }
    printf("[gc] read block 0 (%ld bytes) — fid assigned by server\n", r);

    printf("[gc] sleeping %d s — server will GC the fid after %d s of inactivity...\n",
           SLEEP_SECS, SLEEP_SECS - 2);
    sleep(SLEEP_SECS);

    // Read block 1 — not cached, must contact server with the old (GC'd) fid.
    // send_recv_op should catch ERR_BADFID, reopen, and retry transparently. 
    char block1[BLOCK_SIZE];
    printf("[gc] reading block 1 — expect ERR_BADFID recovery in log\n");
    long r2 = mynfs_read(fd, block1, BLOCK_SIZE);
    if (r2 < 0) {
        fprintf(stderr, "FAIL: read after GC returned %ld — recovery failed\n", r2);
        return 1;
    }

    // Re-read block 0 — still in cache (FRESH_TIME=60), must be a cache hit 
    mynfs_lseek(fd, 0, SEEK_SET);
    char block0_again[BLOCK_SIZE];
    printf("[gc] re-reading block 0 (expect: Cache hit)\n");
    long r3 = mynfs_read(fd, block0_again, BLOCK_SIZE);
    if (r3 != r) {
        fprintf(stderr, "FAIL: block 0 size changed after GC (%ld -> %ld)\n", r, r3);
        return 1;
    }
    if (memcmp(block0, block0_again, r) != 0) {
        fprintf(stderr, "FAIL: block 0 data corrupted after GC recovery\n");
        return 1;
    }

    printf("PASS: ERR_BADFID recovery succeeded, block 0 data intact\n");
    mynfs_close(fd);
    return 0;
}