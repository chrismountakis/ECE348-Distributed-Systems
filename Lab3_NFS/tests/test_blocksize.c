/*
   Block-size sweep: read a file once with a given block size, print one CSV row.

   Run five times (or via a shell loop) to sweep across block sizes:
     for bs in 128 256 512 1024 2048; do
       ./test_blocksize <ip> <port> <file> $bs
     done

   Max block size is MYNFS_MAX_BLOCK (2048 bytes). Suggested sweep: 128 256 512 1024 2048.

   What this shows:
     - Small blocks: many RPCs, low bytes-per-RPC overhead  -> higher RPC count
     - Large blocks: few RPCs, more bytes-per-RPC           -> lower RPC count, more recv bytes
   The tradeoff is pure network efficiency independent of cache size.

   max_cached is set to 100 (always enough for test_10k.bin at any block size) so that
   cache effects do not interfere with the block-size measurement.

   CSV columns:
     block_size, rpc_count, bytes_sent, bytes_recv,
     cache_hits, cache_misses, elapsed_ms, bytes_read, throughput_kbps

   Usage: ./test_blocksize <server_ip> <port> <file> <block_size>
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../client.h"
#include "../protocol.h"

#define MAX_CACHED  100    /* big enough for any block size over test_10k.bin */
#define FRESH_TIME  3600

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr,
            "Usage: %s <server_ip> <port> <file> <block_size>\n"
            "  block_size must be between 1 and %d\n", argv[0], MYNFS_MAX_BLOCK);
        return 1;
    }

    const char *ip         = argv[1];
    int         port       = atoi(argv[2]);
    const char *fname      = argv[3];
    int         block_size = atoi(argv[4]);

    if (block_size < 1 || block_size > MYNFS_MAX_BLOCK) {
        fprintf(stderr, "block_size must be 1..%d\n", MYNFS_MAX_BLOCK);
        return 1;
    }

    mynfs_set_verbose(0);
    if (mynfs_init((char *)ip, port, block_size, MAX_CACHED, FRESH_TIME) < 0) {
        fprintf(stderr, "FAIL: mynfs_init\n"); return 1;
    }

    int fd = mynfs_open((char *)fname);
    if (fd < 0) { fprintf(stderr, "FAIL: open '%s'\n", fname); return 1; }

    char *buf = malloc(block_size);
    if (!buf) { fprintf(stderr, "FAIL: malloc\n"); return 1; }

    /* Print header only when running the first block size so the caller can
       pipe all runs into one file without duplicate headers. */
    if (block_size == 128 || block_size == atoi(argv[4]))  /* always for single runs */
        printf("block_size,rpc_count,bytes_sent,bytes_recv,cache_hits,cache_misses,elapsed_ms,bytes_read,throughput_kbps\n");

    mynfs_stats_reset();
    double t0    = now_ms();
    long   total = 0, r;

    while ((r = mynfs_read(fd, buf, block_size)) > 0)
        total += r;

    double elapsed = now_ms() - t0;
    if (r < 0) { fprintf(stderr, "FAIL: read error after %ld bytes\n", total); free(buf); return 1; }

    mynfs_stats s = mynfs_stats_get();
    double throughput_kbps = (elapsed > 0.0) ? (total / elapsed * 1000.0 / 1024.0) : 0.0;

    printf("%d,%ld,%ld,%ld,%ld,%ld,%.2f,%ld,%.1f\n",
           block_size,
           s.rpc_count, s.bytes_sent, s.bytes_recv,
           s.cache_hits, s.cache_misses,
           elapsed, total, throughput_kbps);

    free(buf);
    mynfs_close(fd);
    return 0;
}
