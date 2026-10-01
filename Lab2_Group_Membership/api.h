#include <arpa/inet.h>
#include <netinet/in.h>

#include "utils.h"

#define MAX_PAYLOAD_LEN 256
#define MAX_PENDING 1000 
#define MAX_VIEW_NOTIF 8

#define RETRANSMIT_INTERVAL 1
#define RETRANSMIT_THRESHOLD 2

typedef struct __attribute__((packed)) {
    GmsHeader header;
    JoinPayload payload;
} JoinMessage;

typedef struct __attribute__((packed)) {
    GmsHeader header;
    LeavePayload payload;
} LeaveMessage;

typedef enum {
    MSG = 1,
    MSG_ACK,
    DELIVER_NR,
    DELIVER_NR_ACK,
    COORD_STATE_REQ,
    COORD_STATE_REPLY,
    RECOVERY_DONE
} P2PMessageType;

typedef struct __attribute__((packed)) {
    int snd_pid;
    int seqnum;
} MsgID;

typedef struct __attribute__((packed)) {
    P2PMessageType msg_type;
    int mypid;
    MsgID mid;
    char grpname[MAX_NAME_LEN];
    int len;
} P2P_header;

typedef struct __attribute__((packed)) {
    P2P_header header;
    char payload[MAX_PAYLOAD_LEN];
} P2P_msg; 

typedef struct __attribute__((packed)) {
    MsgID orig_mid;         // the MSG this number applies to (network byte order)
    int   nr;               // delivery number (network byte order)
} DlvrNumPayload;

typedef struct __attribute__((packed)) {
    int max_delivered;
    int max_buffered;
    int count; // Tells coord how many mappings are attached
} CoordStateReplyPayload;

typedef struct __attribute__((packed)) {
    int global_max_dlvr;
} RecoveryDonePayload;

typedef struct {
    int view_id;
    int member_count;
    char member_ids[MAX_MEMBERS][MAX_NAME_LEN]; 
} ViewChangePayload;

typedef struct {
    bool       active;
    MsgID      mid;

    bool       has_payload;
    char       payload[MAX_PAYLOAD_LEN];
    int        payload_len;
    int        delivery_num;            // -1 = not yet assigned

    bool       acked_by[MAX_MEMBERS];   // PIDs that have sent/acked this msg
    bool       acks_all_done;           // all members acked, retransmission can stop
    int        retransmit_count;        
} PendingEntry;

typedef struct {
    bool  active;
    MsgID mid;                      // {coord_pid, coord_nr} — unique per DELIVER_NR
    MsgID orig_mid;                 // the MSG this number applies to
    int   nr;                       // delivery number assigned
    bool  acked_by[MAX_MEMBERS];
    bool  all_done;
    int   retransmit_count;
} DlvrNumEntry;

typedef struct {
    bool is_active;  
    char myid[MAX_NAME_LEN];             
    char grpname[MAX_NAME_LEN];
    int view_id;   
    int my_pid;            
    ViewMemberEntry members[MAX_MEMBERS]; 
    int member_count;

    int seqnum;

    // TRM state
    int total_delivered;                    // msgs TRM-delivered so far
    int coord_nr;                           // coord counter
    int expected_seqnum[MAX_MEMBERS];       // for causal ordering

    bool is_recovering;                     // true while coordinator asks for state from members 
    bool recovery_replies[MAX_MEMBERS];     // Which members have sent COORD_STATE_REPLY
    int  recovery_replies_count;           
    int  recovery_max_dlvr;
    int  recovery_min_delivered;

    MsgID      seen_mids[MAX_PENDING];      // "mids" deduplication set
    unsigned int seen_count;

    int pending_capacity;
    PendingEntry *pending;      // msgs pending delivery + ack tracking
    
    int dlvr_capacity;
    DlvrNumEntry *dlvr_buf;     // per-DELIVER_NR tracking (retransmission only)

    // Delivery queue for app
    P2P_msg     delivery_queue[MAX_PENDING];
    int         dq_head, dq_tail, dq_count;

    // View-change notification queue 
    ViewChangePayload vn_queue[MAX_VIEW_NOTIF];
    int               vn_head, vn_tail, vn_count;

    pthread_mutex_t delivery_mutex;
    pthread_cond_t  delivery_cond;
    pthread_cond_t  recovery_cond;
} LocalGroup;

typedef struct {
    int pending_pid; 

    char gms_addr[16];
    int gms_port;

    int tcp_socket;                     
    int udp_socket;                   

    char udp_ip[INET_ADDRSTRLEN]; 
    int  udp_port;
    
    LocalGroup groups[MAX_GROUPS];     
    pthread_mutex_t state_mutex;
    pthread_t listener_thr;

    int status;
    pthread_cond_t cond;
    pthread_cond_t pending_not_full;
} ProcessState;

int grp_init(char *gms_addr, int gms_port);
int grp_join(char *grpname, char *myid);
int grp_leave(int g);
int grp_send(int g, void *msg, int len);
int grp_recv(int g, void *msg, int *len, int block);
void grp_get_stats(long *msg, long *ack, long *dlvr, long *dlvr_ack);