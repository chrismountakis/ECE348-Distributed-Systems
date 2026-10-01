#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#include <time.h>
#include "protocol.h"

#define FT_GC_INTERVAL 10

typedef struct ft_entry {
    uint64_t fid;
    char fname[MYNFS_FNAME_MAX];
    int os_fd;
    time_t last_access;
    struct ft_entry *next;
} ft_entry;

void      ft_init();
ft_entry *ft_find_by_fid(uint64_t fid);
ft_entry *ft_find_by_fname(const char *fname);
ft_entry *ft_insert(uint64_t fid, const char *fname, int os_fd);
void      ft_remove(uint64_t fid);
void      ft_gc();
uint64_t  ft_next_fid(void);

#endif