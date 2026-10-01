#include <stdlib.h>
#include <string.h>
#include "client_table.h"

static cft_entry *cft_head = NULL;
static int        next_fd  = 3;     // start above stdin/stdout/stderr

cft_entry *cft_find_by_fd(int fd) {
    cft_entry *curr = cft_head;
    while (curr) {
        if (curr->fd == fd) return curr;
        curr = curr->next;
    }
    return NULL;
}

cft_entry *cft_find_by_fname(const char *fname) {
    cft_entry *curr = cft_head;
    while (curr) {
        if (strcmp(curr->fname, fname) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

cft_entry *cft_insert(uint64_t fid, const char *fname) {
    cft_entry *e = malloc(sizeof(cft_entry));
    if (!e) return NULL;
    e->fd       = next_fd++;
    e->fid      = fid;
    e->position = 0;
    strncpy(e->fname, fname, MYNFS_FNAME_MAX - 1);
    e->fname[MYNFS_FNAME_MAX - 1] = '\0';
    e->next  = cft_head;
    cft_head = e;
    return e;
}

void cft_remove(int fd) {
    cft_entry **pp = &cft_head;
    while (*pp) {
        if ((*pp)->fd == fd) {
            cft_entry *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}