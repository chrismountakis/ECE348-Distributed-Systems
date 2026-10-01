/*
   Re-read benchmark: cold pass vs warm-cache pass.

   Run TWICE to compare baseline vs cached:
     ./test_cache <ip> <port> <src> <dst1> <dst2> 1   # baseline  (1 cached block)
     ./test_cache <ip> <port> <src> <dst1> <dst2> 20  # with cache (20 cached blocks)

   Pass 1 — cold cache: reads src, writes to dst1. Both configs perform identically here.
   Pass 2 — seek src back to 0, read again, write to dst2.
     baseline (max_cached=1): every block evicted by the next; all cache misses again.
     cached   (max_cached=N): file fits in cache; pass 2 is pure cache hits, 0 server reads.

   Output: one CSV header + two data rows (one per pass) to stdout.
   Collect the rows from both runs to build your graphs.

   CSV columns:
     max_cached, pass, rpc_count, bytes_sent, bytes_recv,
     cache_hits, cache_misses, elapsed_ms, bytes_copied

   Usage: ./test_cache <server_ip> <port> <src> <dst1> <dst2> <max_cached>
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../client.h"

#define BLOCK_SIZE  512
#define FRESH_TIME  3600   /* keep blocks fresh for the whole run */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static long do_pass(int src_fd, int dst_fd, const char *label) {
    char buf[BLOCK_SIZE];
    long total = 0, r;
    printf("# %s\n", label);
    while ((r = mynfs_read(src_fd, buf, BLOCK_SIZE)) > 0) {
        long w = 0;
        while (w < r) {
            long n = mynfs_write(dst_fd, buf + w, r - w);
            if (n < 0) { fprintf(stderr, "write error at byte %ld\n", total); return -1; }
            w += n;
        }
        total += r;
    }
    if (r < 0) { fprintf(stderr, "read error at byte %ld\n", total); return -1; }
    return total;
}

int main(int argc, char *argv[]) {
    if (argc != 7) {
        fprintf(stderr,
            "Usage: %s <server_ip> <port> <src> <dst1> <dst2> <max_cached>\n"
            "  max_cached=1  -> baseline (effectively no caching)\n"
            "  max_cached=N  -> normal cache\n", argv[0]);
        return 1;
    }

    const char *ip         = argv[1];
    int         port       = atoi(argv[2]);
    const char *src_name   = argv[3];
    const char *dst1_name  = argv[4];
    const char *dst2_name  = argv[5];
    int         max_cached = atoi(argv[6]);

    if (max_cached < 1) { fprintf(stderr, "max_cached must be >= 1\n"); return 1; }

    mynfs_set_verbose(0);
    if (mynfs_init((char *)ip, port, BLOCK_SIZE, max_cached, FRESH_TIME) < 0) {
        fprintf(stderr, "FAIL: mynfs_init\n"); return 1;
    }

    int src_fd  = mynfs_open((char *)src_name);
    int dst1_fd = mynfs_open((char *)dst1_name);
    int dst2_fd = mynfs_open((char *)dst2_name);
    if (src_fd < 0 || dst1_fd < 0 || dst2_fd < 0) {
        fprintf(stderr, "FAIL: open (src=%d dst1=%d dst2=%d)\n", src_fd, dst1_fd, dst2_fd);
        return 1;
    }

    printf("# test_cache  block_size=%d  max_cached=%d  fresh_time=%d\n",
           BLOCK_SIZE, max_cached, FRESH_TIME);
    printf("max_cached,pass,rpc_count,bytes_sent,bytes_recv,cache_hits,cache_misses,elapsed_ms,bytes_copied\n");

    /* Pass 1 — cold cache */
    mynfs_stats_reset();
    double t0 = now_ms();
    long b1 = do_pass(src_fd, dst1_fd, "Pass 1: cold cache");
    double elapsed1 = now_ms() - t0;
    if (b1 < 0) return 1;
    mynfs_stats s1 = mynfs_stats_get();
    printf("%d,1,%ld,%ld,%ld,%ld,%ld,%.2f,%ld\n",
           max_cached, s1.rpc_count, s1.bytes_sent, s1.bytes_recv,
           s1.cache_hits, s1.cache_misses, elapsed1, b1);

    /* Seek src back to 0 without closing (preserves cached blocks) */
    if (mynfs_lseek(src_fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "FAIL: lseek\n"); return 1;
    }

    /* Pass 2 — warm cache (or still-cold for baseline) */
    mynfs_stats_reset();
    double t1 = now_ms();
    long b2 = do_pass(src_fd, dst2_fd, "Pass 2: warm cache");
    double elapsed2 = now_ms() - t1;
    if (b2 < 0) return 1;
    mynfs_stats s2 = mynfs_stats_get();
    printf("%d,2,%ld,%ld,%ld,%ld,%ld,%.2f,%ld\n",
           max_cached, s2.rpc_count, s2.bytes_sent, s2.bytes_recv,
           s2.cache_hits, s2.cache_misses, elapsed2, b2);

    mynfs_close(src_fd);
    mynfs_close(dst1_fd);
    mynfs_close(dst2_fd);
    return 0;
}
