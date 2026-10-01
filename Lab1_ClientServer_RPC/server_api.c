#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>

#include "client_store.h"
#include "server_api.h"

#define MAINTENANCE_INTERVAL    2
#define MAX_RETRIES             3

//#define RETRANSMIT         

typedef struct header {
    int svcid;
    int cid;
    int reqid;
    int len;
} header_t;

typedef struct request_node {
    int                 reqid;
    int                 cid;
    int                 svcid;        // To count active requests per service (sendReply)
    int                 client_slot;    
    struct sockaddr_in  cliaddr;
    int                 *payload;
    int                 payload_len;
    struct request_node *next;
} request_node_t;

typedef struct service {
    int             registered;
    request_node_t  *head;        
    request_node_t  *tail;      
    pthread_mutex_t lock;       
    pthread_cond_t  cond;
} service_t;

typedef struct {
    request_node_t  *head;
    request_node_t  *tail;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} reply_queue_t;

static int server_fd = -1;
static service_t services[MAX_SERVICES];

static reply_queue_t reply_queue = {
    .head = NULL,
    .tail = NULL,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static request_node_t *active_requests[MAX_PENDING];
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static int next_reqid = 0;

static int pending_count[MAX_SERVICES] = {0};
static int active_count[MAX_SERVICES]  = {0};
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static header_t process_header(const char *buf) {
    header_t hdr;
    int net_svcid, net_reqid, net_len, net_cid;

    memcpy(&net_svcid,  buf,      4);
    memcpy(&net_cid,    buf + 4,  4);
    memcpy(&net_reqid,  buf + 8,  4);
    memcpy(&net_len,    buf + 12, 4);

    hdr.svcid   = ntohl(net_svcid);
    hdr.cid     = ntohl(net_cid);
    hdr.reqid   = ntohl(net_reqid);
    hdr.len     = ntohl(net_len);

    return hdr;
}

static int enqueue_request(int svc_id, int reqid, int cid, int client_slot,
                           struct sockaddr_in *client_addr, const char *payload, int byte_len) {
    request_node_t *node = malloc(sizeof(request_node_t));
    if (!node) return -1;

    node->reqid         = reqid;
    node->cid           = cid;
    node->svcid         = svc_id;
    node->client_slot   = client_slot;
    node->cliaddr       = *client_addr;
    node->payload_len   = byte_len;
    node->next          = NULL;

    node->payload = malloc(byte_len);
    if (!node->payload) {
        free(node);
        return -1;
    }

    memcpy(node->payload, payload, byte_len);

    service_t *svc = &services[svc_id];
    pthread_mutex_lock(&svc->lock);
    
    if (svc->tail == NULL) 
        svc->head = svc->tail = node;
    else {
        svc->tail->next = node;
        svc->tail = node;
    }

    // Increment pending count for load balancing
    pending_count[svc_id]++;
    
    pthread_cond_signal(&svc->cond);    
    pthread_mutex_unlock(&svc->lock);

    return 0; 
}

static request_node_t *dequeue_request(int svc_id) {
    service_t *svc = &services[svc_id];
    pthread_mutex_lock(&svc->lock);

    while (svc->head == NULL)
        pthread_cond_wait(&svc->cond, &svc->lock);

    request_node_t *node = svc->head;
    svc->head = node->next;
    if (svc->head == NULL) svc->tail = NULL;

    // Decrement pending count for load balancing
    pending_count[svc_id]--;

    pthread_mutex_unlock(&svc->lock);
    return node;
}

static void enqueue_reply(request_node_t *node) {
    pthread_mutex_lock(&reply_queue.lock);

    if (reply_queue.tail == NULL)
        reply_queue.head = reply_queue.tail = node;
    else {
        reply_queue.tail->next = node;
        reply_queue.tail = node;
    }

    pthread_cond_signal(&reply_queue.cond);
    pthread_mutex_unlock(&reply_queue.lock);
}

static request_node_t *dequeue_reply() {
    pthread_mutex_lock(&reply_queue.lock);

    while (reply_queue.head == NULL) 
        pthread_cond_wait(&reply_queue.cond, &reply_queue.lock);

    request_node_t *node = reply_queue.head;
    reply_queue.head = node->next;
    if (reply_queue.head == NULL) reply_queue.tail = NULL;

    pthread_mutex_unlock(&reply_queue.lock);
    return node;
}

static int allocate_slot(request_node_t *node) {
    pthread_mutex_lock(&active_lock);

    for (int i = 0; i < MAX_PENDING; i++) {
        int idx = (next_reqid + i) % MAX_PENDING;
        if (active_requests[idx] == NULL) {
            active_requests[idx] = node;
            next_reqid = (idx + 1) % MAX_PENDING;
            pthread_mutex_unlock(&active_lock);
            return idx;
        }
    }

    pthread_mutex_unlock(&active_lock);

    return -1;  
}

static request_node_t *release_slot(int reqid) {
    if (reqid < 0 || reqid >= MAX_PENDING)
        return NULL;

    pthread_mutex_lock(&active_lock);
    request_node_t *node = active_requests[reqid];
    active_requests[reqid] = NULL;
    pthread_mutex_unlock(&active_lock);

    return node;
}

// Thread functions
static void *discovery_thread(void *arg) {
    int disc_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (disc_fd < 0) return NULL;

    // Allow multiple sockets to bind to the same port for discovery
    int opt = 1;
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    #ifdef SO_REUSEPORT
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    #endif

    struct sockaddr_in disc_addr = {0};
    disc_addr.sin_family = AF_INET;
    disc_addr.sin_addr.s_addr = INADDR_ANY;
    disc_addr.sin_port = htons(DISCOVERY_PORT);

    if (bind(disc_fd, (struct sockaddr *)&disc_addr, sizeof(disc_addr)) < 0) {
        perror("bind disc_fd");
        close(disc_fd);
        return NULL;
    }

    char buf[128];
    struct sockaddr_in cliaddr;
    socklen_t clilen; 
    while (1) {
        clilen = sizeof(cliaddr);
        int n = recvfrom(disc_fd, buf, sizeof(buf), 0, (struct sockaddr *)&cliaddr, &clilen);
        if(n < HEADER_SIZE) continue;
        
        header_t hdr = process_header(buf);

        if (hdr.svcid != DISCOVERY_SVC) 
            continue;

        int reply[4];
        reply[0] = htonl(DISCOVERY_SVC);
        reply[1] = htonl(0);
        reply[2] = htonl(0);
        reply[3] = htonl(0);

        sendto(server_fd, reply, sizeof(reply), 0, (struct sockaddr *)&cliaddr, clilen);      
    }
    return NULL;
}

static void *receiver_thread(void *arg) {
    char buf[1024];
    struct sockaddr_in cliaddr;
    socklen_t clilen; 
    while(1) {
        clilen = sizeof(cliaddr);
        int n = recvfrom(server_fd, buf, 1024, 0, (struct sockaddr *)&cliaddr, &clilen);
        if(n < HEADER_SIZE) continue;

        header_t hdr = process_header(buf);

        // Client ACK for reply
        if (hdr.svcid == REPLY_ACK_SVC) {
            int slot = store_find_idx(hdr.cid);
            if (slot >= 0) {
                client_record_t *rec = store_get_client(slot);
                if (rec && rec->status == WAITING_ACK && rec->last_reqid == hdr.reqid) {
                    set_status(slot, IDLE);
                    printf("Reply ACK received (rid=%d)\n", hdr.reqid);
                }
            }
            continue;
        }
        
        // Client cancellation
        if (hdr.svcid == CANCEL_SVC) {
            int slot = store_find_idx(hdr.cid);
            if (slot >= 0) {
                client_record_t *rec = store_get_client(slot);
                if (rec && rec->last_reqid == hdr.reqid) {
                    set_status(slot, EXPIRED);
                    printf("Request cancelled by client (rid=%d)\n", hdr.reqid);
                }
            }
            continue;
        }

        // Load balancing
        if (hdr.svcid == LOAD_BALANCE_SVC) {
            int svcid = hdr.reqid;
            if (svcid < 0 || svcid >= MAX_SERVICES || !services[svcid].registered) {
                int err[4];
                err[0] = htonl(ERROR_SVC);
                err[1] = htonl(0);
                err[2] = htonl(hdr.reqid);
                err[3] = htonl(0);
                sendto(server_fd, err, sizeof(err), 0, (struct sockaddr *)&cliaddr, clilen);
                continue;
            }

            int resp[4];
            resp[0] = htonl(LOAD_BALANCE_SVC);
            resp[1] = htonl(active_count[svcid]);   // CID field   = active  (being processed)
            resp[2] = htonl(pending_count[svcid]);  // REQID field = pending (queued)
            resp[3] = htonl(0);
            sendto(server_fd, resp, sizeof(resp), 0, (struct sockaddr *)&cliaddr, clilen);
            continue;
        }

        // PONG from heartbeat
        if (hdr.svcid == PING_SVC) {
            int slot = store_find_idx(hdr.cid);
            if(slot >= 0) {
                store_reset_retry(slot);
            }
            continue;
        }

        // Invalid service
        if(hdr.svcid < 0 || hdr.svcid >= MAX_SERVICES || !services[hdr.svcid].registered) {
            int err[4];
            err[0] = htonl(ERROR_SVC);
            err[1] = htonl(0);
            err[2] = htonl(hdr.reqid);
            err[3] = htonl(0);
            sendto(server_fd, &err, sizeof(err), 0, (struct sockaddr *)&cliaddr, clilen);
            continue;
        }

        #ifndef RETRANSMIT
        int slot, assigned_client_id;
        store_result_t status = store_track_request(&cliaddr, hdr.cid, hdr.reqid,
                                                    &slot, &assigned_client_id);
        switch (status) {
            case STORE_OK:
                enqueue_request(hdr.svcid, hdr.reqid, assigned_client_id, slot, &cliaddr,
                                buf + HEADER_SIZE, hdr.len);
                break;
    
            case STORE_DUP_PROCESSING:
                printf("Duplicate (processing): rid=%d\n", hdr.reqid);
                break;

            case STORE_DUP_WAITING_ACK: {
                printf("Duplicate (resending reply): rid=%d\n", hdr.reqid);
                client_record_t *rec = store_get_client(slot);
                if (rec) {
                    store_lock();
                    sendto(server_fd, rec->reply_data, rec->reply_len, 0,
                           (struct sockaddr *)&cliaddr, clilen);
                    store_unlock();
                }
                continue;  
            }

            case STORE_DUP_IDLE:
            case STORE_DUP_EXPIRED:
            case STORE_STALE: {
                printf("Rejected request: rid=%d\n", hdr.reqid);
                int err[4];
                err[0] = htonl(ERROR_SVC);
                err[1] = htonl(0);
                err[2] = htonl(hdr.reqid);
                err[3] = htonl(0);
                sendto(server_fd, err, sizeof(err), 0, (struct sockaddr *)&cliaddr, clilen);
                continue;
            }

            case STORE_FULL: {
                printf("Client table full\n");
                int err[4];
                err[0] = htonl(ERROR_SVC);
                err[1] = htonl(0);
                err[2] = htonl(hdr.reqid);
                err[3] = htonl(0);
                sendto(server_fd, err, sizeof(err), 0, (struct sockaddr *)&cliaddr, clilen);
                continue;
            }
        }

        int ack[4];
        ack[0] = htonl(PING_SVC);
        ack[1] = htonl(assigned_client_id);
        ack[2] = htonl(hdr.reqid);
        ack[3] = htonl(0);
        sendto(server_fd, ack, sizeof(ack), 0, (struct sockaddr *)&cliaddr, clilen);
        #endif
    }

    return NULL;
}

static void *sender_thread(void *arg) {
    while (1) {
        request_node_t *node = dequeue_reply();

        store_lock();
        client_record_t *rec = store_get_client(node->client_slot);
        if (rec->status == EXPIRED || rec->last_reqid != node->reqid) {
            printf("Dropping reply for rid=%d: Client is expired or has cancelled.\n", node->reqid);
            store_unlock();
            free(node->payload);
            free(node);
            continue; 
        }
        store_unlock();

        int packet_size = HEADER_SIZE + node->payload_len;
        char *packet = malloc(packet_size);
        if (!packet) {
            free(node->payload);
            free(node);
            continue;
        }

        int net_svcid   = htonl(REPLY_ACK_SVC);
        int net_cid     = htonl(node->cid);
        int net_reqid   = htonl(node->reqid);
        int net_len     = htonl(node->payload_len);

        memcpy(packet,      &net_svcid, 4);
        memcpy(packet + 4,  &net_cid,   4);
        memcpy(packet + 8,  &net_reqid, 4);
        memcpy(packet + 12, &net_len,   4);
        memcpy(packet + HEADER_SIZE, node->payload, node->payload_len);

        set_waiting_ack(node->client_slot, packet, packet_size);

        sendto(server_fd, packet, packet_size, 0,
               (struct sockaddr *)&node->cliaddr, sizeof(node->cliaddr));

        free(packet);
        free(node->payload);
        free(node);
    }

    return NULL;
}

static void *maintenance_thread(void *arg) {
    while (1) {
        sleep(MAINTENANCE_INTERVAL);

        char ping[HEADER_SIZE];
        int net_ping = htonl(PING_SVC);
        int net_zero = htonl(0);

        int num_clients = store_client_count();
        for (int i = 0; i < num_clients; i++) {
            store_lock();
            client_record_t *rec = store_get_client(i);
            if (!rec || rec->status == EMPTY
                     || rec->status == IDLE
                     || rec->status == EXPIRED) {
                store_unlock();
                continue;
            }

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = rec->addr_ip;
            addr.sin_port = rec->addr_port;
            store_unlock();

            store_inc_retry(i);

            store_lock();
            int retry     = rec->retry_count;
            int status    = rec->status;
            int reqid     = rec->last_reqid;
            int client_id = rec->client_id;
            store_unlock();

            if (retry > MAX_RETRIES) {
                if (status == PROCESSING) {
                    printf("[Timeout] Client died during processing (rid=%d).\n", reqid);
                } else if (status == WAITING_ACK) {
                    printf("[Timeout] Client never ACK'd the reply (rid=%d).\n", reqid);
                }
                set_status(i, EXPIRED);
                store_reset_retry(i);
                continue; 
            }

            if (status == WAITING_ACK) {
                store_lock();
                sendto(server_fd, rec->reply_data, rec->reply_len, 0,
                       (struct sockaddr *)&addr, sizeof(addr));
                store_unlock();
            }
            else if (status == PROCESSING) {
                int net_reqid = htonl(reqid);
                int net_cid = htonl(client_id);
                memcpy(ping,      &net_ping,    4);
                memcpy(ping + 4,  &net_cid,     4);
                memcpy(ping + 8,  &net_reqid,   4);
                memcpy(ping + 12, &net_zero,    4);

                sendto(server_fd, ping, HEADER_SIZE, 0,
                       (struct sockaddr *)&addr, sizeof(addr));
            }
        }
    }

    return NULL;
}


// --- Public API functions --- //
void init(char *config_file, int fixed_port) {
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }
#ifdef SO_REUSEPORT
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in servaddr = {0}; 
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY; 
    servaddr.sin_port = htons(fixed_port); 
    
    if (bind(server_fd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind fixed port failed");
        exit(1);
    }

    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST] = "127.0.0.1";
    if (getifaddrs(&ifaddr) != -1) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET && !(ifa->ifa_flags & IFF_LOOPBACK)) {
                getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
                break; 
            }
        }
        freeifaddrs(ifaddr);
    }

    for (int i = 0; i < MAX_SERVICES; i++) {
        services[i].registered = 0;
        services[i].head = NULL;
        services[i].tail = NULL;
        pthread_mutex_init(&services[i].lock, NULL);
        pthread_cond_init(&services[i].cond, NULL);
    }

    if (store_open(config_file) < 0) {
        fprintf(stderr, "Failed to initialize client store\n");
        exit(1);
    }

    int num_clients = store_client_count();
    for (int i = 0; i < num_clients; i++) {
        client_record_t *rec = store_get_client(i);
        if (rec && rec->status == PROCESSING) {
            set_status(i, IDLE);
            struct sockaddr_in cliaddr = {0};
            cliaddr.sin_family = AF_INET;
            cliaddr.sin_addr.s_addr = rec->addr_ip;
            cliaddr.sin_port = rec->addr_port;

            int err[4];
            err[0] = htonl(ERROR_SVC);
            err[1] = htonl(0);
            err[2] = htonl(rec->last_reqid);
            err[3] = htonl(0);
            sendto(server_fd, err, sizeof(err), 0, (struct sockaddr *)&cliaddr, sizeof(cliaddr));
            printf("Notified client of restart: rid=%d\n", rec->last_reqid);
        }
    }
        
    pthread_t recv_tid, send_tid, maint_tid, disc_tid;
    pthread_create(&recv_tid, NULL, receiver_thread, NULL);
    pthread_create(&send_tid, NULL, sender_thread, NULL);
    pthread_create(&maint_tid, NULL, maintenance_thread, NULL);
    pthread_create(&disc_tid, NULL, discovery_thread, NULL);
    
    pthread_detach(recv_tid);
    pthread_detach(send_tid);
    pthread_detach(maint_tid);
    pthread_detach(disc_tid);

    printf("Server live at %s:%d\n", host, fixed_port);
}

int registerService(int svcid) {
    if (svcid < 0 || svcid >= MAX_SERVICES) 
        return -1;

    service_t *svc = &services[svcid];
    pthread_mutex_lock(&svc->lock);

    if (svc->registered) { 
        pthread_mutex_unlock(&svc->lock);
        return -1; 
    }

    svc->registered = 1;
    pthread_mutex_unlock(&svc->lock);
    return 0;
}

int unregisterService(int svcid) {
    if (svcid < 0 || svcid >= MAX_SERVICES)
        return -1;

    service_t *svc = &services[svcid];
    pthread_mutex_lock(&svc->lock);

    if (!svc->registered) {
        pthread_mutex_unlock(&svc->lock);
        return -1; 
    }

    svc->registered = 0;

    request_node_t *curr = svc->head;
    while (curr) {
        request_node_t *tmp = curr;
        curr = curr->next;

        int err[4];
        err[0] = htonl(ERROR_SVC);
        err[1] = htonl(0);
        err[2] = htonl(tmp->reqid);
        err[3] = htonl(0);
        sendto(server_fd, err, sizeof(err), 0,
            (struct sockaddr *)&tmp->cliaddr, sizeof(tmp->cliaddr));

        set_status(tmp->client_slot, IDLE);
        
        free(tmp->payload);
        free(tmp);
    }
    svc->head = svc->tail = NULL;

    pending_count[svcid] = 0;
    pthread_mutex_lock(&count_lock);
    active_count[svcid] = 0;
    pthread_mutex_unlock(&count_lock);

    pthread_mutex_unlock(&svc->lock);
    return 0;
}

int getRequest(int svcid, void *buf, int *len) {
    if (svcid < 0 || svcid >= MAX_SERVICES)
        return -1;

    request_node_t *node = dequeue_request(svcid);
    if (!node) return -1;

    node->next = NULL;
    int reqid = allocate_slot(node);
    if (reqid < 0) {
        free(node->payload);
        free(node);
        return -1;
    }

    pthread_mutex_lock(&count_lock);
    active_count[svcid]++;
    pthread_mutex_unlock(&count_lock);

    memcpy(buf, node->payload, node->payload_len);
    *len = node->payload_len;

    return reqid;
}

void sendReply(int reqid, void *buf, int len) {
    request_node_t *node = release_slot(reqid);
    if (!node) return;

    pthread_mutex_lock(&count_lock);
    active_count[node->svcid]--;
    pthread_mutex_unlock(&count_lock);

    free(node->payload);
    node->payload = malloc(len);
    memcpy(node->payload, buf, len);
    node->payload_len = len;
    node->next = NULL;

    enqueue_reply(node);
}