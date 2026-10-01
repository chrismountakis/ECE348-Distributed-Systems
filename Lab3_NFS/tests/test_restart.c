/* 
   Server restart recovery.
 
   Start a long copy (e.g. test_video.mp4), then kill and restart the server
   mid-transfer. The sleep between reads keeps the copy slow enough to
   interrupt reliably.
 
    ./app <ip> <port> test_video.mp4 restart_copy.bin   (in one terminal)
    CNTRL-C server                                      (while copy is running)
    ../server ../rootdir <port>                         (restart immediately)
    Log must show ERR_BADFID recovery — copy must complete without error.
    diff ../rootdir/test_video.mp4 ../rootdir/restart_copy.bin
    
    Usage: ./app <server_ip> <port> <src> <dst> 
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client.h"

#define BLOCK_SIZE      512
#define MAX_CACHED      4
#define FRESH_TIME      1

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <src_file> <dst_file>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int         port      = atoi(argv[2]);
    const char *src_name  = argv[3];
    const char *dst_name  = argv[4];

    if (mynfs_init((char *)server_ip, port, BLOCK_SIZE, MAX_CACHED, FRESH_TIME) < 0) {
        fprintf(stderr, "app: mynfs_init failed\n");
        return 1;
    }

    int src_fd = mynfs_open((char *)src_name);
    if (src_fd < 0) {
        fprintf(stderr, "app: failed to open source '%s'\n", src_name);
        return 1;
    }

    int dst_fd = mynfs_open((char *)dst_name);
    if (dst_fd < 0) {
        fprintf(stderr, "app: failed to open destination '%s'\n", dst_name);
        mynfs_close(src_fd);
        return 1;
    }

    char buf[BLOCK_SIZE];
    long total_read    = 0;
    long total_written = 0;
    long r;

    while ((r = mynfs_read(src_fd, buf, BLOCK_SIZE)) > 0) {
        total_read += r;

        sleep(FRESH_TIME + 1);

        long written = 0;
        while (written < r) {
            long w = mynfs_write(dst_fd, buf + written, r - written);
            if (w < 0) {
                fprintf(stderr, "app: write failed after %ld bytes\n", total_written);
                mynfs_close(src_fd);
                mynfs_close(dst_fd);
                return 1;
            }
            written       += w;
            total_written += w;
        }
    }

    if (r < 0) {
        fprintf(stderr, "app: read error after %ld bytes\n", total_read);
        mynfs_close(src_fd);
        mynfs_close(dst_fd);
        return 1;
    }

    printf("app: copied %ld bytes (%s -> %s)\n", total_written, src_name, dst_name);

    mynfs_close(src_fd);
    mynfs_close(dst_fd);
    return 0;
}
