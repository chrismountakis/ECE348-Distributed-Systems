#ifndef PROTOCOL_H
#define PROTOCOL_H

#define _FILE_OFFSET_BITS 64
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#define MYNFS_MAX_BLOCK 2048
#define MYNFS_FNAME_MAX 256
#define MYNFS_MAX_MSG   (sizeof(req_header) + MYNFS_MAX_BLOCK + MYNFS_FNAME_MAX)

typedef enum {
    OP_OPEN = 0,
    OP_READ,
    OP_WRITE,
    OP_TRUNCATE, 
} msg_type;

typedef enum {
    ERR_OK = 0,
    ERR_BADFID,
    ERR_NOENT,
    ERR_IO,
    ERR_NOTMODIFIED,
} status_code;

typedef struct __attribute__((packed)) {
    short type;
    uint64_t fid;
    off_t offset;
    off_t length; // READ: N; TRUNCATE: new_size; WRITE: bytes to write (payload); OPEN: fname length (payload)
    time_t tmod;  // READ: -1 = cold miss, >= 0 = cached mtime for conditional fetch
} req_header;

typedef struct __attribute__((packed)) {
    short type;
    short status; 
    uint64_t fid;
    off_t length;
    off_t file_size; 
    time_t tmod;
} resp_header;
#endif 
