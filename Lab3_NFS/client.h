#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "protocol.h"
#include "client_table.h"

typedef struct {
    int valid;          
    uint64_t fid;
    off_t offset;
    off_t length;       // Actual data length in cache block (can be < block_size for EOF)
    time_t tmod;
    time_t tfresh;      
    uint64_t last_used; // logical LRU counter — incremented on every access
    char *data;
} cache_block;

typedef struct {
    char server_ip[256];
    int server_port;
    int block_size;
    int max_cached_blocks;
    int fresh_time;
    
    int socket_fd;
    struct sockaddr_in server_addr;
} mynfs_client_config;

int  mynfs_init(char *server_addr, int server_port, int blockSize, int maxCachedBlocks, int freshT);
int  mynfs_open(char *fname);
long mynfs_read(int fd, void *buf, long n);
long mynfs_write(int fd, void *buf, long n);
long mynfs_lseek(int fd, long pos, int whence);
int  mynfs_truncate(int fd, long len);
int  mynfs_close(int fd);

void mynfs_cache_dump();   // print all cache slots with last_used timestamps

typedef struct {
    long rpc_count;    /* UDP exchanges that received a reply */
    long bytes_sent;   /* request bytes (header + payload) for successful RPCs */
    long bytes_recv;   /* response bytes (header + payload) for successful RPCs */
    long cache_hits;   /* reads served from cache without a server round-trip */
    long cache_misses; /* cold misses + stale revalidations that contacted the server */
} mynfs_stats;

void        mynfs_stats_reset(void);
mynfs_stats mynfs_stats_get(void);
void        mynfs_set_verbose(int verbose); /* 1=on (default), 0=suppress per-op logs */
