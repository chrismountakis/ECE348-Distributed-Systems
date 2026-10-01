#ifndef CLIENT_STORE_H
#define CLIENT_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <time.h>

#define MAX_CLIENTS     1024
#define MAX_REPLY_SIZE  4096
#define STORE_MAGIC     0xDEADBEEF  

typedef enum {
    EMPTY = 0,
    IDLE,
    PROCESSING,
    WAITING_ACK,
    EXPIRED
} client_status_t;

typedef enum {
    STORE_OK,              
    STORE_FULL,            
    STORE_STALE,           
    STORE_DUP_IDLE,        
    STORE_DUP_PROCESSING, 
    STORE_DUP_WAITING_ACK, 
    STORE_DUP_EXPIRED,     
} store_result_t;

typedef struct {
    int client_id;
    int addr_ip;
    short unsigned int addr_port;
    short int _pad;
    int  last_reqid;
    int  status;
    int  retry_count;
    int  reply_len;
    char reply_data[MAX_REPLY_SIZE];
} client_record_t;


int  store_open(const char *filename);
void store_close();

int             store_find_idx(int client_id);
client_record_t *store_get_client(int slot);
int             store_client_count();

store_result_t  store_track_request(struct sockaddr_in *addr, int client_id, int reqid,
                                    int *out_slot, int *out_client_id);

void set_status(int slot, client_status_t status);
void set_waiting_ack(int slot, void *reply, int reply_len);

void store_inc_retry(int slot);
void store_reset_retry(int slot);

void store_lock();
void store_unlock();

#endif 