#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "client_store.h"

typedef struct {
    int magic;
    int record_count;
    int next_client_id;
} store_header_t;

static client_record_t  *clients = NULL;
static int              num_clients = 0;
static int              next_client_id = 1;
static FILE             *store_file = NULL;
static pthread_mutex_t  store_mutex = PTHREAD_MUTEX_INITIALIZER;

static void write_record(int index) {
    if (!store_file)
        return;

    long offset = sizeof(store_header_t) + index * sizeof(client_record_t);
    fseek(store_file, offset, SEEK_SET);
    fwrite(&clients[index], sizeof(client_record_t), 1, store_file);
    fflush(store_file);
}

static void write_header() {
    if (!store_file)
        return;

    store_header_t hdr = {
        .magic          = STORE_MAGIC,
        .record_count   = num_clients,
        .next_client_id = next_client_id,
    };

    fseek(store_file, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, store_file);
    fflush(store_file);
}

static int load_from_file(void) {
    store_header_t hdr;

    if (fread(&hdr, sizeof(hdr), 1, store_file) != 1)
        return -1;

    if (hdr.magic != STORE_MAGIC)
        return -1;

    num_clients    = hdr.record_count;
    next_client_id = hdr.next_client_id;

    clients = calloc(MAX_CLIENTS, sizeof(client_record_t));
    if (!clients)
        return -1;

    size_t read = fread(clients, sizeof(client_record_t), num_clients, store_file);
    if (read != (size_t)num_clients)
        num_clients = read;

    return 0;
}

static int find_unlocked(int client_id) {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].client_id == client_id && clients[i].status != EMPTY)
            return i;
    }
    return -1;
}

static int find_empty_slot() {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].status == EMPTY)
            return i;
    }

    if (num_clients < MAX_CLIENTS)
        return num_clients++;

    return -1;
}

int store_open(const char *filename) {
    pthread_mutex_lock(&store_mutex);

    store_file = fopen(filename, "r+b");

    if (store_file && load_from_file() == 0) {
        printf("--- Persistent Store Loaded: %s ---\n", filename);
        printf("%-6s | %-15s | %-5s | %-8s | %-12s\n",
               "CID", "IP Address", "Port", "Last RID", "Status");
        printf("--------------------------------------------------------------------\n");

        for (int i = 0; i < num_clients; i++) {
            if (clients[i].status == EMPTY) continue;

            struct in_addr ip_addr;
            ip_addr.s_addr = clients[i].addr_ip;

            const char* status_str = "UNKNOWN";
            switch(clients[i].status) {
                case IDLE:         status_str = "IDLE"; break;
                case PROCESSING:   status_str = "PROCESSING"; break;
                case WAITING_ACK:  status_str = "WAITING_ACK"; break;
                case EXPIRED:      status_str = "EXPIRED"; break;
                default:           status_str = "EMPTY"; break;
            }

            printf("%-6d | %-15s | %-5u | %-8d | %-12s\n",
                   clients[i].client_id,
                   inet_ntoa(ip_addr),
                   ntohs(clients[i].addr_port),
                   clients[i].last_reqid,
                   status_str);
        }
        printf("--------------------------------------------------------------------\n");
        printf("Total active records: %d  |  next_client_id: %d\n\n",
               num_clients, next_client_id);

        pthread_mutex_unlock(&store_mutex);
        return 0;
    }

    if (store_file) fclose(store_file);
    store_file = fopen(filename, "w+b");

    if (!store_file) {
        pthread_mutex_unlock(&store_mutex);
        return -1;
    }

    clients = calloc(MAX_CLIENTS, sizeof(client_record_t));
    if (!clients) {
        fclose(store_file);
        pthread_mutex_unlock(&store_mutex);
        return -1;
    }

    num_clients    = 0;
    next_client_id = 1;
    write_header();

    printf("Created new client store: %s\n", filename);
    pthread_mutex_unlock(&store_mutex);
    return 0;
}

void store_close() {
    pthread_mutex_lock(&store_mutex);

    if (store_file) {
        fclose(store_file);
        store_file = NULL;
    }

    free(clients);
    clients = NULL;
    num_clients = 0;

    pthread_mutex_unlock(&store_mutex);
}

void store_lock() {
    pthread_mutex_lock(&store_mutex);
}

void store_unlock() {
    pthread_mutex_unlock(&store_mutex);
}

int store_find_idx(int client_id) {
    pthread_mutex_lock(&store_mutex);
    int slot = find_unlocked(client_id);
    pthread_mutex_unlock(&store_mutex);
    return slot;
}

client_record_t *store_get_client(int slot) {
    if (slot < 0 || slot >= num_clients) return NULL;
    return &clients[slot];
}

int store_client_count() {
    return num_clients;
}

store_result_t store_track_request(struct sockaddr_in *addr, int client_id, int reqid,
                                   int *out_slot, int *out_client_id) {
    pthread_mutex_lock(&store_mutex);

    int slot = -1;

    if (client_id != 0) {
        slot = find_unlocked(client_id);

        if (slot >= 0) {
            // Known client — update address in case port changed after restart
            clients[slot].addr_ip   = addr->sin_addr.s_addr;
            clients[slot].addr_port = addr->sin_port;

            *out_slot      = slot;
            *out_client_id = client_id;

            client_record_t *rec = &clients[slot];

            if (rec->last_reqid == reqid) {
                write_record(slot);
                store_result_t status;
                switch (rec->status) {
                    case IDLE:        status = STORE_DUP_IDLE;        break;
                    case PROCESSING:  status = STORE_DUP_PROCESSING;  break;
                    case WAITING_ACK: status = STORE_DUP_WAITING_ACK; break;
                    case EXPIRED:     status = STORE_DUP_EXPIRED;     break;
                    default:          status = STORE_DUP_EXPIRED;     break;
                }
                pthread_mutex_unlock(&store_mutex);
                return status;
            }

            if (reqid < rec->last_reqid) {
                pthread_mutex_unlock(&store_mutex);
                return STORE_STALE;
            }

            rec->last_reqid  = reqid;
            rec->status      = PROCESSING;
            rec->retry_count = 0;
            rec->reply_len   = 0;
            write_record(slot);
            pthread_mutex_unlock(&store_mutex);
            return STORE_OK;
        }
    }

    // New client (client_id == 0) or unknown client_id
    slot = find_empty_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&store_mutex);
        return STORE_FULL;
    }

    int cid = next_client_id++;

    *out_slot      = slot;
    *out_client_id = cid;

    client_record_t *rec = &clients[slot];
    rec->client_id   = cid;
    rec->addr_ip     = addr->sin_addr.s_addr;
    rec->addr_port   = addr->sin_port;
    rec->last_reqid  = reqid;
    rec->status      = PROCESSING;
    rec->retry_count = 0;
    rec->reply_len   = 0;

    write_record(slot);
    write_header();   // Update header with new client count and next_client_id

    pthread_mutex_unlock(&store_mutex);
    return STORE_OK;
}

void set_waiting_ack(int slot, void *reply, int reply_len) {
    if (slot < 0 || slot >= num_clients) return;
    if (reply_len > MAX_REPLY_SIZE) return;

    pthread_mutex_lock(&store_mutex);

    client_record_t *rec = &clients[slot];
    rec->status      = WAITING_ACK;
    rec->retry_count = 0;
    rec->reply_len   = reply_len;

    if (reply && reply_len > 0)
        memcpy(rec->reply_data, reply, reply_len);

    write_record(slot);
    pthread_mutex_unlock(&store_mutex);
}

void set_status(int slot, client_status_t status) {
    if (slot < 0 || slot >= num_clients) return;

    client_record_t *rec = &clients[slot];

    pthread_mutex_lock(&store_mutex);
    rec->status      = status;
    rec->retry_count = 0;
    rec->reply_len   = 0;
    write_record(slot);
    pthread_mutex_unlock(&store_mutex);
}

void store_inc_retry(int slot) {
    if (slot < 0 || slot >= num_clients)
        return;

    pthread_mutex_lock(&store_mutex);
    clients[slot].retry_count++;
    pthread_mutex_unlock(&store_mutex);
}

void store_reset_retry(int slot) {
    if (slot < 0 || slot >= num_clients)
        return;

    pthread_mutex_lock(&store_mutex);
    clients[slot].retry_count = 0;
    pthread_mutex_unlock(&store_mutex);
}