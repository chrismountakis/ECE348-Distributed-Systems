/* 
   Two opens of the same file track independent positions.

   Opens the same file twice (fd1, fd2). Verifies that:
   - Both return the same data at the same logical offset (same fid, same blocks)
   - Advancing fd1 does not move fd2's position

   Usage: ./test_multi_open <server_ip> <port> <file> 
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client.h"

#define BLOCK_SIZE  512
#define MAX_CACHED  8
#define FRESH_TIME  30

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

    int fd1 = mynfs_open((char *)file);
    int fd2 = mynfs_open((char *)file);
    if (fd1 < 0 || fd2 < 0) {
        fprintf(stderr, "FAIL: open (fd1=%d fd2=%d)\n", fd1, fd2); return 1;
    }
    if (fd1 == fd2) {
        fprintf(stderr, "FAIL: both opens returned same fd %d\n", fd1); return 1;
    }
    printf("[multi_open] fd1=%d fd2=%d (different fds, same underlying file)\n", fd1, fd2);

    char buf1[BLOCK_SIZE], buf2[BLOCK_SIZE], buf3[BLOCK_SIZE], buf4[BLOCK_SIZE];

    // Read block 0 from both — must be identical 
    long r1 = mynfs_read(fd1, buf1, BLOCK_SIZE);
    long r2 = mynfs_read(fd2, buf2, BLOCK_SIZE);
    if (r1 <= 0 || r2 <= 0) {
        fprintf(stderr, "FAIL: first reads failed (%ld %ld)\n", r1, r2); return 1;
    }
    if (r1 != r2 || memcmp(buf1, buf2, r1) != 0) {
        fprintf(stderr, "FAIL: block 0 differs between fd1 and fd2\n"); return 1;
    }
    printf("[multi_open] block 0 identical on both fds: OK\n");

    // Read block 1 from both — fd1 is at offset 512, fd2 is also still at offset 512
    // (reads are independent so both should now read the second block) 
    long r3 = mynfs_read(fd1, buf3, BLOCK_SIZE);
    long r4 = mynfs_read(fd2, buf4, BLOCK_SIZE);
    if (r3 <= 0 || r4 <= 0) {
        fprintf(stderr, "FAIL: second reads failed (%ld %ld)\n", r3, r4); return 1;
    }
    if (r3 != r4 || memcmp(buf3, buf4, r3) != 0) {
        fprintf(stderr, "FAIL: block 1 differs between fd1 and fd2\n"); return 1;
    }
    printf("[multi_open] block 1 identical on both fds: OK\n");

    // Advance fd1 by two more blocks without touching fd2 
    char tmp[BLOCK_SIZE];
    mynfs_read(fd1, tmp, BLOCK_SIZE);
    mynfs_read(fd1, tmp, BLOCK_SIZE);

    // fd2 should still be at offset 1024 (block 2) */
    char buf5[BLOCK_SIZE], buf6[BLOCK_SIZE];
    long r5 = mynfs_read(fd2, buf5, BLOCK_SIZE);  // fd2 reads block 2 
    // Now seek fd1 back to block 2 and compare 
    mynfs_lseek(fd1, BLOCK_SIZE * 2, SEEK_SET);
    long r6 = mynfs_read(fd1, buf6, BLOCK_SIZE);
    if (r5 <= 0 || r6 <= 0 || r5 != r6 || memcmp(buf5, buf6, r5) != 0) {
        fprintf(stderr, "FAIL: fd2 position was not independent of fd1\n"); return 1;
    }
    printf("[multi_open] fd2 position unaffected by fd1 advances: OK\n");

    printf("PASS: two fds on same file are fully independent\n");
    mynfs_close(fd1);
    mynfs_close(fd2);
    return 0;
}
