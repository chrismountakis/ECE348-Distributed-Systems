/* 
  Truncate shrink and extend, plus cache invalidation on truncate.

  Write 512 bytes of 0xAB to trunc_test.bin
  Read back — verify 0xAB pattern (cache populated)
  Truncate to 0
  Read from offset 0 — must return 0 bytes (EOF)
  Truncate to 1024 (extend beyond current size of 0)
  Read 1024 bytes — must all be 0x00
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client.h"

#define BLOCK_SIZE   512
#define MAX_CACHED   4
#define FRESH_TIME   30
#define TEST_FILE    "trunc_test.bin"

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

    int fd = mynfs_open(TEST_FILE);
    if (fd < 0) { fprintf(stderr, "FAIL: open\n"); return 1; }

    // Write 512 bytes of 0xAB 
    char pattern[BLOCK_SIZE];
    memset(pattern, 0xAB, BLOCK_SIZE);
    if (mynfs_write(fd, pattern, BLOCK_SIZE) != BLOCK_SIZE) {
        fprintf(stderr, "FAIL: initial write\n"); return 1;
    }

    // lseek at beginning, read back and verify 
    mynfs_lseek(fd, 0, SEEK_SET);
    char readbuf[BLOCK_SIZE * 2];
    long r = mynfs_read(fd, readbuf, BLOCK_SIZE);
    if (r != BLOCK_SIZE) {
        fprintf(stderr, "FAIL: read after write returned %ld (expected %d)\n", r, BLOCK_SIZE);
        return 1;
    }
    for (int i = 0; i < BLOCK_SIZE; i++) {
        if ((unsigned char)readbuf[i] != 0xAB) {
            fprintf(stderr, "FAIL: byte %d is 0x%02x, expected 0xAB\n", i, (unsigned char)readbuf[i]);
            return 1;
        }
    }
    printf("[truncate] write+read 0xAB pattern: OK\n");

    // Truncate to 0 — must also evict the cached block
    if (mynfs_truncate(fd, 0) < 0) {
        fprintf(stderr, "FAIL: truncate to 0\n"); return 1;
    }

    // Read from offset 0 — must get 0 bytes */
    mynfs_lseek(fd, 0, SEEK_SET);
    r = mynfs_read(fd, readbuf, BLOCK_SIZE);
    if (r != 0) {
        fprintf(stderr, "FAIL: read after truncate-to-0 returned %ld (expected 0)\n", r);
        return 1;
    }
    printf("[truncate] read after truncate-to-0: OK (got 0 bytes)\n");

    // Truncate to 1024 
    if (mynfs_truncate(fd, 1024) < 0) {
        fprintf(stderr, "FAIL: truncate to 1024\n"); return 1;
    }

    // Read 1024 bytes, must be 0x00 
    mynfs_lseek(fd, 0, SEEK_SET);
    long total = 0;
    while (total < 1024) {
        r = mynfs_read(fd, readbuf + total, 1024 - total);
        if (r <= 0) break;
        total += r;
    }
    if (total != 1024) {
        fprintf(stderr, "FAIL: read after extend got %ld bytes (expected 1024)\n", total);
        return 1;
    }
    for (int i = 0; i < 1024; i++) {
        if (readbuf[i] != 0x00) {
            fprintf(stderr, "FAIL: byte %d is 0x%02x after extend (expected 0x00)\n",
                    i, (unsigned char)readbuf[i]);
            return 1;
        }
    }
    printf("[truncate] read after extend-to-1024: OK (1024 zero bytes)\n");

    printf("PASS: truncate shrink and extend both correct\n");
    mynfs_close(fd);
    return 0;
}