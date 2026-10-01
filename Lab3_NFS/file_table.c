#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "file_table.h"

static ft_entry *head = NULL;

static uint32_t incarnation;
static uint32_t seq_counter;
static const char *INC_FILE = ".mynfs_incarnation";

void ft_init() {
    head = NULL;

    incarnation = 0;
    FILE *f = fopen(INC_FILE, "r");
    if (f) { 
        fscanf(f, "%u", &incarnation); 
        fclose(f); 
    }
    incarnation++;

    f = fopen(INC_FILE, "w");
    if (f) { 
        fprintf(f, "%u\n", incarnation); 
        fflush(f); 
        fsync(fileno(f)); 
        fclose(f); 
    }
    seq_counter = 0;
    printf("[init] incarnation=%u\n", incarnation);
}

uint64_t ft_next_fid(void) {
    seq_counter++;
    return ((uint64_t)incarnation << 32) | seq_counter;
}

ft_entry* ft_find_by_fid(uint64_t fid) {
    ft_entry *curr = head;
    while (curr) {
        if (curr->fid == fid) {
            curr->last_access = time(NULL);
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

ft_entry* ft_find_by_fname(const char *fname) {
    ft_entry *curr = head; 
    while (curr) {
        if(strcmp(curr->fname, fname) == 0) {
            curr->last_access = time(NULL);
            return curr; 
        }
        curr = curr->next;
    }
    return NULL;
}

ft_entry *ft_insert(uint64_t fid, const char *fname, int os_fd) {
    ft_entry *new = malloc(sizeof(ft_entry));
    if(!new) return NULL;
    
    new->fid = fid;
    strncpy(new->fname, fname, MYNFS_FNAME_MAX - 1);
    new->fname[MYNFS_FNAME_MAX - 1] = '\0';
    new->os_fd = os_fd;
    new->last_access = time(NULL);
    new->next = head;
    head = new;
    return new;
}

void ft_remove(uint64_t fid) {
    ft_entry **pp = &head;
    while (*pp) {
        if ((*pp)->fid == fid) {
            ft_entry *dead = *pp;
            *pp = dead->next;
            close(dead->os_fd);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

void ft_gc() {
    time_t now = time(NULL);
    ft_entry *curr = head;
    while (curr) {
        ft_entry *next = curr->next;
        if (now - curr->last_access > FT_GC_INTERVAL) {
            printf("[gc] evicting fid=%lu fname=%s\n", (unsigned long)curr->fid, curr->fname);
            ft_remove(curr->fid);
        }
        curr = next;
    }
}