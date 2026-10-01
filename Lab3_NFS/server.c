#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "protocol.h"
#include "file_table.h"

#define DEFAULT_PORT 10000

static int server_socket;

// ---- helpers ---- // 

static void fill_meta(int os_fd, resp_header *resp) {
    struct stat st;
    if (fstat(os_fd, &st) == 0) {
        resp->file_size = st.st_size;
        resp->tmod      = st.st_mtime;
    } else {
        resp->file_size = 0;
        resp->tmod      = 0;
    }
}

static void send_reply(struct sockaddr_in *cli, socklen_t clilen,
                       resp_header *hdr, void *payload, size_t payload_len) {
    char buf[MYNFS_MAX_MSG];
    memcpy(buf, hdr, sizeof(*hdr));
    if (payload && payload_len > 0)
        memcpy(buf + sizeof(*hdr), payload, payload_len);

    sendto(server_socket, buf, sizeof(*hdr) + payload_len, 0,
           (struct sockaddr *)cli, clilen);
}

static void send_error(struct sockaddr_in *cli, socklen_t clilen,
                       short type, short status) {
    resp_header resp = {0};
    resp.type = type;
    resp.status = status;
    send_reply(cli, clilen, &resp, NULL, 0);
}

static void handle_open(req_header *req, char *payload, 
                struct sockaddr_in *cli, socklen_t clilen) {

    char fname[MYNFS_FNAME_MAX] = {0};
    size_t n = (size_t)req->length;
    if (n >= MYNFS_FNAME_MAX) n = MYNFS_FNAME_MAX - 1;
    memcpy(fname, payload, n);
    fname[n] = '\0';

    // Security check: disallow absolute paths and parent directory references
    if (strstr(fname, "..") || fname[0] == '/') {
        send_error(cli, clilen, OP_OPEN, ERR_NOENT);
        return;
    }

    ft_entry *entry = ft_find_by_fname(fname);

    // New file: open and add to list
    if (!entry) {
        int fd = open(fname, O_RDWR | O_CREAT, 0666);
        if (fd < 0) {
            send_error(cli, clilen, OP_OPEN, ERR_NOENT);
            return;
        }
        uint64_t new_fid = ft_next_fid();
        entry = ft_insert(new_fid, fname, fd);
        if (!entry) {
            close(fd);
            send_error(cli, clilen, OP_OPEN, ERR_IO);
            return;
        }
    }

    resp_header resp = {0};
    resp.type   = OP_OPEN;
    resp.status = ERR_OK;
    resp.fid    = entry->fid;
    fill_meta(entry->os_fd, &resp);
    send_reply(cli, clilen, &resp, NULL, 0);
    printf("[open] fname=%s fid=%lu\n", fname, (unsigned long)entry->fid);
}

static void handle_read(req_header *req, 
                        struct sockaddr_in *cli, socklen_t clilen) {
    
    ft_entry *entry = ft_find_by_fid(req->fid);
    if (!entry) {
        send_error(cli, clilen, OP_READ, ERR_BADFID);
        return;
    }

    if (req->tmod != -1) {
        struct stat st;
        if (fstat(entry->os_fd, &st) == 0 && st.st_mtime == req->tmod) {
            resp_header resp = {0};
            resp.type   = OP_READ;
            resp.status = ERR_NOTMODIFIED;
            resp.fid    = entry->fid;
            fill_meta(entry->os_fd, &resp);
            send_reply(cli, clilen, &resp, NULL, 0);
            printf("[read] fid=%lu off=%ld not modified\n",
                   (unsigned long)entry->fid, (long)req->offset);
            return;
        }
    }

    off_t bytes_req = req->length;
    if (bytes_req > MYNFS_MAX_BLOCK) bytes_req = MYNFS_MAX_BLOCK;

    char buf[MYNFS_MAX_BLOCK];
    ssize_t b_got = pread(entry->os_fd, buf, bytes_req, req->offset);
    if (b_got < 0) {
        send_error(cli, clilen, OP_READ, ERR_IO);
        return;
    }

    resp_header resp = {0};
    resp.type   = OP_READ;
    resp.status = ERR_OK;
    resp.fid    = entry->fid;
    resp.length = b_got;
    fill_meta(entry->os_fd, &resp);
    send_reply(cli, clilen, &resp, buf, (size_t)b_got);
    printf("[read] fid=%lu off=%ld got=%zd\n",
           (unsigned long)entry->fid, (long)req->offset, b_got);
}

static void handle_write(req_header *req, char *payload,
                        struct sockaddr_in *cli, socklen_t clilen) {

    ft_entry *entry = ft_find_by_fid(req->fid);
    if (!entry) {
        send_error(cli, clilen, OP_WRITE, ERR_BADFID);
        return;
    }

    ssize_t b_wrote = pwrite(entry->os_fd, payload, (size_t)req->length, req->offset);
    if (b_wrote < 0) { 
        send_error(cli, clilen, OP_WRITE, ERR_IO); 
        return; 
    }

    resp_header resp = {0};
    resp.type   = OP_WRITE;
    resp.status = ERR_OK;
    resp.fid    = entry->fid;
    resp.length = b_wrote;
    fill_meta(entry->os_fd, &resp);
    send_reply(cli, clilen, &resp, NULL, 0);
    printf("[write] fid=%lu off=%ld wrote=%zd\n",
           (unsigned long)entry->fid, (long)req->offset, b_wrote);
}

static void handle_truncate(req_header *req,
                            struct sockaddr_in *cli, socklen_t clilen) {
    ft_entry *entry = ft_find_by_fid(req->fid);
    if (!entry) { 
        send_error(cli, clilen, OP_TRUNCATE, ERR_BADFID); 
        return; 
    }

    if (ftruncate(entry->os_fd, req->length) < 0) {
        send_error(cli, clilen, OP_TRUNCATE, ERR_IO);
        return;
    }

    resp_header resp = {0};
    resp.type   = OP_TRUNCATE;
    resp.status = ERR_OK;
    resp.fid    = entry->fid;
    fill_meta(entry->os_fd, &resp);
    send_reply(cli, clilen, &resp, NULL, 0);
    printf("[trunc] fid=%lu len=%ld\n",
           (unsigned long)entry->fid, (long)req->length);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <root_dir> [port]\n", argv[0]);
        return 1;
    }

    const char *root_dir = argv[1];
    int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;

    if (chdir(root_dir) < 0) { 
        perror("chdir"); 
        return 1;
    }

    ft_init();

    server_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return 1;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);     
    server_addr.sin_port = htons(port);   
    if (bind(server_socket, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_socket);
        return 1;
    }

    printf("mynfs server: root=%s port=%d\n", root_dir, port);
    
    char buf[MYNFS_MAX_MSG];
    while (1) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        ssize_t n = recvfrom(server_socket, buf, sizeof(buf), 0,
                             (struct sockaddr *)&cli, &clilen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { ft_gc(); continue; }
            perror("recvfrom"); continue;
        }
        if ((size_t)n < sizeof(req_header)) continue;

        req_header *req = (req_header *)buf;
        char *payload   = buf + sizeof(req_header);

        switch (req->type) {
            case OP_OPEN:     handle_open(req, payload, &cli, clilen); break;
            case OP_READ:     handle_read(req, &cli, clilen); break;
            case OP_WRITE:    handle_write(req, payload, &cli, clilen); break;
            case OP_TRUNCATE: handle_truncate(req, &cli, clilen); break;
            default: fprintf(stderr, "unknown type %d\n", req->type);
        }
    }
}