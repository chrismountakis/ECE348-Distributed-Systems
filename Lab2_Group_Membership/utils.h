#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>

#define MSG_JOIN        1
#define MSG_LEAVE       2
#define MSG_PING        3
#define MSG_PONG        4
#define MSG_VIEW        5
#define MSG_JOIN_OK     6
#define MSG_JOIN_FAIL   7
#define MSG_LEAVE_OK    8
#define MSG_LEAVE_FAIL  9

#define MAX_NAME_LEN    32
#define MAX_GROUPS      10
#define MAX_MEMBERS     20

typedef struct __attribute__((packed)) {
    int type;
    int payload_len;   
} GmsHeader;
 
typedef struct __attribute__((packed)) {
    char     grpname[MAX_NAME_LEN];
    char     myid[MAX_NAME_LEN];
    char     udp_ip[INET_ADDRSTRLEN];
    unsigned short udp_port;      
} JoinPayload;

typedef struct __attribute__((packed)) {
    char myid[MAX_NAME_LEN];
    char grpname[MAX_NAME_LEN];
} LeavePayload;

typedef struct __attribute__((packed)) {
    char     myid[MAX_NAME_LEN];
    char     udp_ip[INET_ADDRSTRLEN];
    unsigned short udp_port;   
    int pid;       // GMS-assigned unique ID   
} ViewMemberEntry;

int send_msg(int fd, const void *buf, size_t len);
int recv_msg(int fd, void *buf, size_t len);