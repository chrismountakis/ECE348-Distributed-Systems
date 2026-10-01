/* 
   data integrity, file creation, binary copy.

   Copies src -> dst over NFS, then re-reads both and compares byte by byte.

   Usage: ./test_integrity <server_ip> <port> <src> <dst> 
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
        fprintf(stderr, "Usage: %s <server_ip> <port> <src> <dst>\n", argv[0]);
        return 1;
    }
    const char *ip   = argv[1];
    int         port = atoi(argv[2]);
    const char *src  = argv[3];
    const char *dst  = argv[4];

    if (mynfs_init((char *)ip, port, BLOCK_SIZE, MAX_CACHED, FRESH_TIME) < 0) {
        fprintf(stderr, "FAIL: mynfs_init\n"); return 1;
    }

    int src_fd = mynfs_open((char *)src);
    int dst_fd = mynfs_open((char *)dst);
    if (src_fd < 0 || dst_fd < 0) {
        fprintf(stderr, "FAIL: open (src=%d dst=%d)\n", src_fd, dst_fd); return 1;
    }

    char buf[BLOCK_SIZE];
    long total = 0, r;
    while ((r = mynfs_read(src_fd, buf, BLOCK_SIZE)) > 0) {
        long w = 0;
        while (w < r) {
            long n = mynfs_write(dst_fd, buf + w, r - w);
            if (n < 0) { fprintf(stderr, "FAIL: write at byte %ld\n", total); return 1; }
            w += n;
        }
        total += r;
    }
    if (r < 0) { fprintf(stderr, "FAIL: read error\n"); return 1; }
    if (total == 0) { fprintf(stderr, "FAIL: src is empty\n"); return 1; }

    // Re-read both from the start and compare byte by byte 
    mynfs_lseek(src_fd, 0, SEEK_SET);
    mynfs_lseek(dst_fd, 0, SEEK_SET);

    char buf2[BLOCK_SIZE];
    long pos = 0;
    while (1) {
        long r1 = mynfs_read(src_fd, buf,  BLOCK_SIZE);
        long r2 = mynfs_read(dst_fd, buf2, BLOCK_SIZE);
        if (r1 < 0 || r2 < 0) { fprintf(stderr, "FAIL: read error during verify\n"); return 1; }
        if (r1 != r2) {
            fprintf(stderr, "FAIL: size mismatch at byte %ld (src=%ld dst=%ld)\n", pos, r1, r2);
            return 1;
        }
        if (r1 == 0) break;
        if (memcmp(buf, buf2, r1) != 0) {
            fprintf(stderr, "FAIL: data mismatch at byte %ld\n", pos);
            return 1;
        }
        pos += r1;
    }

    printf("PASS: %ld bytes copied and verified identical (%s -> %s)\n", pos, src, dst);
    mynfs_close(src_fd);
    mynfs_close(dst_fd);
    return 0;
}
