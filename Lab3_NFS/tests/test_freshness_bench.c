/*
   Freshness-time benchmark: multi-pass read of a full file.

   Pass 1 (cold): fetches every block from the server regardless of fresh_t.
   Pass 2 (warm):
     fresh_t=0    -> every block is instantly stale -> server revalidation on every block
     fresh_t=3600 -> every block still fresh        -> pure cache hits, 0 read RPCs

   No sleep needed: the difference is visible immediately because fresh_t=0 never
   considers a block fresh, while fresh_t=3600 keeps everything valid for the run.

   For the comparison to be meaningful, max_cached must be large enough to hold the
   entire file. For test_video.mp4 (~2.7 MB) with block_size=512, use max_cached=6000.

   Run twice to compare:
     ./test_freshness_bench <ip> <port> test_video.mp4 0    6000 2
     ./test_freshness_bench <ip> <port> test_video.mp4 3600 6000 2

   CSV columns (one row per pass):
     fresh_t, max_cached, pass, rpc_count, bytes_sent, bytes_recv,
     cache_hits, cache_misses, elapsed_ms, bytes_read

   Usage: ./test_freshness_bench <server_ip> <port> <file> <fresh_t> <max_cached> <num_passes>
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../client.h"

#define BLOCK_SIZE 512

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char *argv[]) {
    if (argc != 7) {
        fprintf(stderr,
            "Usage: %s <server_ip> <port> <file> <fresh_t> <max_cached> <num_passes>\n"
            "  fresh_t=0    -> revalidate every block on every pass\n"
            "  fresh_t=3600 -> all blocks stay fresh; pass 2+ are pure cache hits\n"
            "  max_cached must fit the entire file (use 6000 for test_video.mp4)\n",
            argv[0]);
        return 1;
    }

    const char *ip         = argv[1];
    int         port       = atoi(argv[2]);
    const char *fname      = argv[3];
    int         fresh_t    = atoi(argv[4]);
    int         max_cached = atoi(argv[5]);
    int         num_passes = atoi(argv[6]);

    if (max_cached < 1)  { fprintf(stderr, "max_cached must be >= 1\n");  return 1; }
    if (num_passes < 1)  { fprintf(stderr, "num_passes must be >= 1\n");  return 1; }

    mynfs_set_verbose(0);
    if (mynfs_init((char *)ip, port, BLOCK_SIZE, max_cached, fresh_t) < 0) {
        fprintf(stderr, "FAIL: mynfs_init\n"); return 1;
    }

    int fd = mynfs_open((char *)fname);
    if (fd < 0) { fprintf(stderr, "FAIL: open '%s'\n", fname); return 1; }

    char *buf = malloc(BLOCK_SIZE);
    if (!buf) { fprintf(stderr, "FAIL: malloc\n"); return 1; }

    printf("# test_freshness_bench  block_size=%d  fresh_t=%d  max_cached=%d  num_passes=%d\n",
           BLOCK_SIZE, fresh_t, max_cached, num_passes);
    printf("fresh_t,max_cached,pass,rpc_count,bytes_sent,bytes_recv,cache_hits,cache_misses,elapsed_ms,bytes_read\n");

    for (int pass = 1; pass <= num_passes; pass++) {
        if (mynfs_lseek(fd, 0, SEEK_SET) < 0) {
            fprintf(stderr, "FAIL: lseek before pass %d\n", pass); return 1;
        }

        mynfs_stats_reset();
        double t0    = now_ms();
        long   total = 0, r;

        while ((r = mynfs_read(fd, buf, BLOCK_SIZE)) > 0)
            total += r;

        double elapsed = now_ms() - t0;
        if (r < 0) { fprintf(stderr, "FAIL: read error in pass %d\n", pass); return 1; }

        mynfs_stats s = mynfs_stats_get();
        printf("%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%.2f,%ld\n",
               fresh_t, max_cached, pass,
               s.rpc_count, s.bytes_sent, s.bytes_recv,
               s.cache_hits, s.cache_misses,
               elapsed, total);
    }

    free(buf);
    mynfs_close(fd);
    return 0;
}
