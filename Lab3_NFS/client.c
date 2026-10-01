#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "client.h"

#define TIMEOUT_SEC 1
#define MAX_RETRIES 5

static cache_block *cache_blocks = NULL;
static int          num_cache_blocks = 0;

mynfs_client_config client_config;

static void init_cache() {
    num_cache_blocks = client_config.max_cached_blocks;
    cache_blocks = calloc(num_cache_blocks, sizeof(cache_block));
    if (!cache_blocks) { perror("init_cache: calloc failed"); return; }
    for (int i = 0; i < num_cache_blocks; i++) {
        cache_blocks[i].data = malloc(client_config.block_size);
        if (!cache_blocks[i].data) { perror("init_cache: malloc failed"); return; }
    }
}

static cache_block *cache_find(uint64_t fid, off_t offset) {
    for (int i = 0; i < num_cache_blocks; i++) {
        if (cache_blocks[i].valid && cache_blocks[i].fid == fid && cache_blocks[i].offset == offset)
            return &cache_blocks[i];
    }
    return NULL;
}

static uint64_t lru_clock = 0;

static mynfs_stats g_stats   = {0};
static int         g_verbose = 1;

void mynfs_stats_reset(void) { memset(&g_stats, 0, sizeof(g_stats)); }
mynfs_stats mynfs_stats_get(void) { return g_stats; }
void mynfs_set_verbose(int v) { g_verbose = v; }

static cache_block *cache_evict(void) {
    cache_block *oldest = &cache_blocks[0];
    for (int i = 1; i < num_cache_blocks; i++)
        if (cache_blocks[i].last_used < oldest->last_used)
            oldest = &cache_blocks[i];
    oldest->valid = 0;
    return oldest;
}

static cache_block *cache_alloc(void) {
    if (num_cache_blocks == 0) return NULL;
    for (int i = 0; i < num_cache_blocks; i++)
        if (!cache_blocks[i].valid) return &cache_blocks[i];
    return cache_evict();
}

static int send_recv(void *req, size_t req_len, void *resp_buf, size_t resp_buf_size) {
    for (int i = 0; i < MAX_RETRIES; i++) {
        sendto(client_config.socket_fd, req, req_len, 0,
               (struct sockaddr *)&client_config.server_addr,
               sizeof(client_config.server_addr));

        ssize_t n = recvfrom(client_config.socket_fd, resp_buf, resp_buf_size, 0, NULL, NULL);
        if (n >= (ssize_t)sizeof(resp_header)) {
            g_stats.rpc_count++;
            g_stats.bytes_sent += (long)req_len;
            g_stats.bytes_recv += (long)n;
            return (int)n;
        }
    }
    return -1;
}

// Contacts the server directly for a fresh fid, bypassing the client table.
static uint64_t fetch_fid(const char *fname) {
    size_t fname_len = strlen(fname);
    char req_buf[sizeof(req_header) + MYNFS_FNAME_MAX] = {0};
    req_header *req = (req_header *)req_buf;
    req->type   = OP_OPEN;
    req->length = (off_t)fname_len;
    memcpy(req_buf + sizeof(req_header), fname, fname_len);

    resp_header resp;
    if (send_recv(req_buf, sizeof(req_header) + fname_len, &resp, sizeof(resp_header)) < 0)
        return 0;
    if (resp.status != ERR_OK)
        return 0;
    return resp.fid;
}

static int send_recv_op(req_header *req, size_t req_total_size,
                        void *resp_buf, size_t resp_buf_size,
                        cft_entry *entry) {
    int ret = send_recv(req, req_total_size, resp_buf, resp_buf_size);
    if (ret < (int)sizeof(resp_header)) return -1;

    resp_header *resp = (resp_header *)resp_buf;

    if (resp->status == ERR_BADFID) {
        fprintf(stderr, "[send_recv_op] ERR_BADFID for fid=%lu fname=%s — reopening\n",
                (unsigned long)entry->fid, entry->fname);
        uint64_t new_fid = fetch_fid(entry->fname);
        if (!new_fid) return -1;
        entry->fid = new_fid;
        req->fid   = new_fid;

        ret = send_recv(req, req_total_size, resp_buf, resp_buf_size);
        if (ret < (int)sizeof(resp_header) || (resp->status != ERR_OK && resp->status != ERR_NOTMODIFIED)) return -1;
    } else if (resp->status != ERR_OK && resp->status != ERR_NOTMODIFIED) {
        return -1;
    }
    return ret;
}

int mynfs_init(char *server_addr, int server_port, 
                int blockSize, int maxCachedBlocks, int freshT) {
    strncpy(client_config.server_ip, server_addr, sizeof(client_config.server_ip) - 1);
    client_config.server_ip[sizeof(client_config.server_ip) - 1] = '\0';
    client_config.server_port       = server_port;
    client_config.block_size        = blockSize;
    client_config.max_cached_blocks = maxCachedBlocks;
    client_config.fresh_time        = freshT;

    client_config.socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_config.socket_fd < 0) {
        perror("mynfs_init: socket creation failed");
        return -1;
    }

    memset(&client_config.server_addr, 0, sizeof(client_config.server_addr));
    client_config.server_addr.sin_family = AF_INET;
    client_config.server_addr.sin_port   = htons(server_port);

    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server_addr, NULL, &hints, &res) != 0) {
        fprintf(stderr, "mynfs_init: failed to resolve '%s'\n", server_addr);
        close(client_config.socket_fd);
        return -1;
    }
    client_config.server_addr.sin_addr =
        ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    struct timeval tv = { .tv_sec = TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(client_config.socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    init_cache();

    if (g_verbose)
        printf("[mynfs_init] Ready -> Server: %s:%d | BlockSize: %d | CacheBlocks: %d | Freshness: %ds\n",
               server_addr, server_port, blockSize, maxCachedBlocks, freshT);
    return 0;
}

int mynfs_open(char *fname) {
    uint64_t fid;

    cft_entry *existing = cft_find_by_fname(fname);
    if (existing) {
        fid = existing->fid;
    } else {
        size_t fname_len = strlen(fname);
        char req_buf[sizeof(req_header) + MYNFS_FNAME_MAX] = {0};
        req_header *req = (req_header *)req_buf;
        req->type   = OP_OPEN;
        req->fid    = 0;
        req->offset = 0;
        req->length = (off_t)fname_len;
        memcpy(req_buf + sizeof(req_header), fname, fname_len);

        resp_header *resp = malloc(sizeof(resp_header));
        if (!resp) return -1;

        if (send_recv(req_buf, sizeof(req_header) + fname_len, resp, sizeof(resp_header)) < 0) {
            free(resp);
            return -1;
        }
        if (resp->status != ERR_OK) {
            free(resp);
            return -1;
        }
        fid = resp->fid;
        free(resp);
    }

    cft_entry *entry = cft_insert(fid, fname);
    if (!entry) return -1;
    return entry->fd;
}

long mynfs_read(int fd, void *buf, long n) {
    cft_entry *entry = cft_find_by_fd(fd);
    if (!entry) {
        fprintf(stderr, "mynfs_read: Invalid fd %d\n", fd);
        return -1;
    }

    if (n <= 0) return 0;

    long block_size       = client_config.block_size;
    off_t aligned_offset  = (entry->position / block_size) * block_size;
    off_t internal_offset = entry->position % block_size;

    time_t now = time(NULL);  
    cache_block *cached = cache_find(entry->fid, aligned_offset);                                                                                                                       
    if (cached && (now - cached->tfresh) < client_config.fresh_time) {
        g_stats.cache_hits++;
        cached->last_used = ++lru_clock;
        long available = cached->length - internal_offset;
        if (available <= 0) return 0;
        if (g_verbose)
            printf("    [mynfs_read] Cache hit for fid=%lu offset=%lu (pos=%lu)\n",
                    (unsigned long)entry->fid, (unsigned long)aligned_offset, (unsigned long)entry->position);
        long bytes_to_copy = (n < available) ? n : available;
        if (internal_offset + bytes_to_copy > block_size)                                                                                                                               
            bytes_to_copy = block_size - internal_offset; 
        memcpy(buf, cached->data + internal_offset, bytes_to_copy);                                                                                                                     
        entry->position += bytes_to_copy;
        return bytes_to_copy;                                                                                                                                                           
    }                                                     

    int is_stale = (cached != NULL);
    g_stats.cache_misses++;

    if (g_verbose)
        printf("    [mynfs_read] %s for fid=%lu offset=%lu (pos=%lu)\n",
                is_stale ? "Stale revalidation" : "Cold miss",
                (unsigned long)entry->fid, (unsigned long)aligned_offset, (unsigned long)entry->position);                                                                                   

    req_header req = {0};                                                                                                                                                               
    req.type   = OP_READ;                                 
    req.fid    = entry->fid;
    req.offset = aligned_offset;
    req.length = block_size;                                                                                                                                                            
    req.tmod   = is_stale ? cached->tmod : -1;
                                                                                                                                                                                        
    size_t resp_buf_size = sizeof(resp_header) + block_size;
    char *resp_buf = malloc(resp_buf_size);
    if (!resp_buf) return -1;                                                                                                                                                           

    if (send_recv_op(&req, sizeof(req_header), resp_buf, resp_buf_size, entry) < 0) {                                                                                                   
        free(resp_buf);                                   
        return -1;                                                                                                                                                                      
    }
                                                                                                                                                                                        
    resp_header *resp = (resp_header *)resp_buf;          

    if (resp->status == ERR_NOTMODIFIED) {
        if (g_verbose)
            printf("    [mynfs_read] Not modified — reusing cache offset=%lu\n",
                    (unsigned long)aligned_offset);                                                                                                                                          
        cached->tfresh    = time(NULL);
        cached->last_used = ++lru_clock;
        free(resp_buf);                                   

        long available = cached->length - internal_offset;
        if (available <= 0) return 0;
        long bytes_to_copy = (n < available) ? n : available;                                                                                                                           
        if (internal_offset + bytes_to_copy > block_size)
            bytes_to_copy = block_size - internal_offset;                                                                                                                               
        memcpy(buf, cached->data + internal_offset, bytes_to_copy);
        entry->position += bytes_to_copy;                                                                                                                                               
        return bytes_to_copy;
    }                                                                                                                                                                                   
                                                        
    char *payload = resp_buf + sizeof(resp_header);
    if (g_verbose)
        printf("    [mynfs_read] Block %s (mtime %ld -> %ld) offset=%lu\n",
                is_stale && resp->tmod != cached->tmod ? "modified on server" : "fetched",
                is_stale ? (long)cached->tmod : 0L, (long)resp->tmod, (unsigned long)aligned_offset);

    cache_block *slot = is_stale ? cached : cache_alloc();
    if (slot) {
        slot->valid     = 1;
        slot->fid       = entry->fid;
        slot->offset    = aligned_offset;
        slot->length    = resp->length;
        slot->tmod      = resp->tmod;
        slot->tfresh    = time(NULL);
        slot->last_used = ++lru_clock;
        memcpy(slot->data, payload, resp->length);
    }                                                                                                                                          

    long available = resp->length - internal_offset;                                                                                                                                    
    if (available <= 0) {                                 
        free(resp_buf);
        return 0;
    }
    long bytes_to_copy = (n < available) ? n : available;
    if (internal_offset + bytes_to_copy > block_size)                                                                                                                                   
        bytes_to_copy = block_size - internal_offset;
    memcpy(buf, payload + internal_offset, bytes_to_copy);                                                                                                                              
    entry->position += bytes_to_copy;                                                                                                                                                   

    free(resp_buf);                                                                                                                                                                     
    return bytes_to_copy;
}

long mynfs_write(int fd, void *buf, long n) {
    cft_entry *entry = cft_find_by_fd(fd);
    if (!entry) {
        fprintf(stderr, "mynfs_write: Invalid fd %d\n", fd);
        return -1;
    }

    if (n <= 0) return 0;
    long bytes_to_send = (n > MYNFS_MAX_BLOCK) ? MYNFS_MAX_BLOCK : n;

    size_t req_size = sizeof(req_header) + bytes_to_send;
    char *req_buf = malloc(req_size);
    if (!req_buf) return -1;

    req_header *req = (req_header *)req_buf;
    req->type   = OP_WRITE;
    req->fid    = entry->fid;
    req->offset = entry->position;
    req->length = bytes_to_send;
    memcpy(req_buf + sizeof(req_header), buf, bytes_to_send);

    resp_header resp;
    if (send_recv_op(req, req_size, &resp, sizeof(resp_header), entry) < 0) {
        free(req_buf);
        return -1;
    }

    off_t write_start = entry->position;
    long written = resp.length;
    entry->position += written;

    for (int i = 0; i < num_cache_blocks; i++)
        if (cache_blocks[i].valid && cache_blocks[i].fid == entry->fid &&
            write_start < cache_blocks[i].offset + client_config.block_size &&
            // Wrote block overlaps with cache block
            write_start + written > cache_blocks[i].offset)
            cache_blocks[i].valid = 0;

    free(req_buf);
    return written;
}

long mynfs_lseek(int fd, long pos, int whence) {
    cft_entry *entry = cft_find_by_fd(fd);
    if (!entry) {
        fprintf(stderr, "mynfs_lseek: Invalid fd %d\n", fd);
        return -1;
    }

    off_t new_pos;
    if (whence == SEEK_SET) {
        new_pos = pos;
    } else if (whence == SEEK_CUR) {
        new_pos = entry->position + pos;
    } else if (whence == SEEK_END) {
        req_header req = {0};
        req.type   = OP_READ;
        req.fid    = entry->fid;
        req.offset = 0;
        req.length = 0;
        req.tmod   = -1;

        resp_header resp;
        if (send_recv_op(&req, sizeof(req_header), &resp, sizeof(resp_header), entry) < 0)
            return -1;
        new_pos = resp.file_size + pos;
    } else {
        fprintf(stderr, "mynfs_lseek: Invalid whence %d\n", whence);
        return -1;
    }

    if (new_pos < 0) new_pos = 0;
    entry->position = new_pos;
    return (long)new_pos;
}

int mynfs_truncate(int fd, long len) {
    cft_entry *entry = cft_find_by_fd(fd);
    if (!entry) {
        fprintf(stderr, "mynfs_truncate: Invalid fd %d\n", fd);
        return -1;
    }

    req_header req = {0};
    req.type   = OP_TRUNCATE;
    req.fid    = entry->fid;
    req.offset = 0;
    req.length = len;

    resp_header resp;
    if (send_recv_op(&req, sizeof(req_header), &resp, sizeof(resp_header), entry) < 0)
        return -1;

    for (int i = 0; i < num_cache_blocks; i++)
        if (cache_blocks[i].valid && cache_blocks[i].fid == entry->fid)
            cache_blocks[i].valid = 0;

    return 0;
}

int mynfs_close(int fd) {
    cft_entry *entry = cft_find_by_fd(fd);
    if (!entry) {
        fprintf(stderr, "mynfs_close: Invalid fd %d\n", fd);
        return -1;
    }

    for (int i = 0; i < num_cache_blocks; i++) {           
        if (cache_blocks[i].fid == entry->fid)
            cache_blocks[i].valid = 0;   
    }

    cft_remove(fd);

    return 0;
}

void mynfs_cache_dump() {
    printf("[cache dump] %d slots  (lru_clock=%llu)\n",
           num_cache_blocks, (unsigned long long)lru_clock);
    for (int i = 0; i < num_cache_blocks; i++) {
        cache_block *b = &cache_blocks[i];
        if (!b->valid) {
            printf("  slot %2d: empty\n", i);
        } else {
            printf("  slot %2d: fid=%-12lu offset=%-6lu len=%-4lu  last_used=%llu\n",
                   i,
                   (unsigned long)b->fid,
                   (unsigned long)b->offset,
                   (unsigned long)b->length,
                   (unsigned long long)b->last_used);
        }
    }
}