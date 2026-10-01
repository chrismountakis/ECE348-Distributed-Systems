/* 
   Read past EOF returns 0, and the last block of a
   non-block-aligned file is correctly partial.
   Expects a file whose size is NOT a multiple of BLOCK_SIZE.
   test_10k.bin is 10000 bytes: 19 full blocks + 272-byte tail.
  
   Usage: ./test_eof <server_ip> <port> <file> <expected_size> 
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client.h"

#define BLOCK_SIZE  512
#define MAX_CACHED  8
#define FRESH_TIME  30

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <file> <expected_size>\n", argv[0]);
        return 1;
    }
    const char *ip        = argv[1];
    int         port      = atoi(argv[2]);
    const char *file      = argv[3];
    long        file_size = atol(argv[4]);

    if (mynfs_init((char *)ip, port, BLOCK_SIZE, MAX_CACHED, FRESH_TIME) < 0) {
        fprintf(stderr, "FAIL: mynfs_init\n"); return 1;
    }

    int fd = mynfs_open((char *)file);
    if (fd < 0) { fprintf(stderr, "FAIL: open\n"); return 1; }

    /* Seek to end and read — must return 0 */
    long end_pos = mynfs_lseek(fd, 0, SEEK_END);
    if (end_pos != file_size) {
        fprintf(stderr, "FAIL: SEEK_END returned %ld, expected %ld\n", end_pos, file_size);
        return 1;
    }
    char buf[BLOCK_SIZE];
    long r = mynfs_read(fd, buf, BLOCK_SIZE);
    if (r != 0) {
        fprintf(stderr, "FAIL: read at EOF returned %ld (expected 0)\n", r); return 1;
    }
    printf("[eof] read at EOF returned 0: OK\n");

    /* Read entire file block by block, track the last block size */
    mynfs_lseek(fd, 0, SEEK_SET);
    long total = 0;
    long last_r = 0;
    while ((r = mynfs_read(fd, buf, BLOCK_SIZE)) > 0) {
        last_r = r;
        total += r;
    }
    if (r < 0) { fprintf(stderr, "FAIL: read error after %ld bytes\n", total); return 1; }
    if (total != file_size) {
        fprintf(stderr, "FAIL: total bytes read %ld != expected %ld\n", total, file_size);
        return 1;
    }
    printf("[eof] read entire file: %ld bytes total: OK\n", total);

    long expected_tail = file_size % BLOCK_SIZE;
    if (expected_tail == 0) {
        printf("[eof] file is block-aligned, no partial last block to check\n");
    } else if (last_r != expected_tail) {
        fprintf(stderr, "FAIL: last block was %ld bytes, expected %ld\n", last_r, expected_tail);
        return 1;
    } else {
        printf("[eof] partial last block: %ld bytes (expected %ld): OK\n", last_r, expected_tail);
    }

    printf("PASS: EOF and partial-block handling correct\n");
    mynfs_close(fd);
    return 0;
}
