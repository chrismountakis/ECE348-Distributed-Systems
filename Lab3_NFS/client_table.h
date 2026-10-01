#ifndef CLIENT_TABLE_H
#define CLIENT_TABLE_H

#include <stdint.h>
#include <sys/types.h>
#include "protocol.h"

typedef struct cft_entry {
    int      fd;
    uint64_t fid;
    char     fname[MYNFS_FNAME_MAX];
    off_t    position;
    struct cft_entry *next;
} cft_entry;

cft_entry *cft_find_by_fd(int fd);
cft_entry *cft_find_by_fname(const char *fname);
cft_entry *cft_insert(uint64_t fid, const char *fname);
void       cft_remove(int fd);

#endif