#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/timerfd.h>

#include <time.h>
#include <ifaddrs.h>

#include <signal.h>
#include "gms.h"
#include "utils.h"

typedef struct {
    char myid[MAX_NAME_LEN];
    int socket_fd;
    char udp_ip[INET_ADDRSTRLEN];
    unsigned short udp_port; // network byte order; GMS only forwards it
    int pid;         
} Member;
 
typedef struct {
    char grpname[MAX_NAME_LEN];
    int view_id;
    bool pids_in_use[MAX_MEMBERS];
    Member members[MAX_MEMBERS];
    int member_count;
} Group;

typedef struct {
    int fd;
    int pending_pings;
} ActiveMember;

static Group groups[MAX_GROUPS];
static int   group_count = 0;

static ActiveMember active_members[MAX_GROUPS * MAX_MEMBERS];
static int act_mem_count = 0;

// PID Allocation Helper
static int find_available_pid(Group *g) {
    for (int i = 0; i < MAX_MEMBERS; i++) {
        if (!g->pids_in_use[i]) 
            return i;
    }
    return -1;
}

// Active member management //
static void active_members_add(int fd) {
    if (act_mem_count >= (MAX_GROUPS * MAX_MEMBERS)) 
        return;
    
    active_members[act_mem_count].fd = fd;
    active_members[act_mem_count].pending_pings = 0;
    act_mem_count++;
}

static void active_members_rmv(int fd) {
    for (int i = 0; i < act_mem_count; i++) {
        if (active_members[i].fd == fd) {
            active_members[i] = active_members[--act_mem_count];
            return;
        }
    }
}

static ActiveMember *active_members_find(int fd) {
    for (int i = 0; i < act_mem_count; i++)
        if (active_members[i].fd == fd)
            return &active_members[i];
    return NULL;
}

// group / member lookup //
static Group *find_group(const char *grpname) {
    for (int i = 0; i < group_count; i++)
        if (strcmp(groups[i].grpname, grpname) == 0)
            return &groups[i];
    return NULL;
}

static Group *find_or_create_group(const char *grpname) {
    Group *g = find_group(grpname);
    if (g) 
        return g;

    if (group_count >= MAX_GROUPS) {
        fprintf(stderr, "GMS: too many groups\n");
        return NULL;
    }
    g = &groups[group_count++];
    memset(g, 0, sizeof(*g));
    strncpy(g->grpname, grpname, sizeof(g->grpname) - 1);
    g->view_id = 0;
    memset(g->pids_in_use, 0, sizeof(g->pids_in_use));
    printf("GMS: created group '%s'\n", g->grpname);
    return g;
}

static int find_member_in_group(Group *g, int fd) {
    for (int i = 0; i < g->member_count; i++) {
        if (g->members[i].socket_fd == fd) {
            return i;
        }
    }
    return -1;
}

static bool id_exists_in_group(Group *g ,const char *myid) { 
    for (int mi = 0; mi < g->member_count; mi++)
        if (strcmp(g->members[mi].myid, myid) == 0)
            return true;
    
    return false;
}

static void grp_update(Group *g) {
    g->view_id++;

    printf("GMS: group '%s' view -> %u  (%d members)\n",
           g->grpname, g->view_id, g->member_count);

    int member_data_size = sizeof(ViewMemberEntry) * g->member_count;
    size_t payload_len   = sizeof(int) + MAX_NAME_LEN + sizeof(int) + member_data_size;
    size_t total_size    = sizeof(GmsHeader) + payload_len;

    char *buf = calloc(1, total_size);
    if (!buf) return; 
    
    int offset = 0;

    // Header
    GmsHeader hdr = { .type = htonl(MSG_VIEW), .payload_len = htonl(payload_len) };
    memcpy(buf + offset, &hdr, sizeof(hdr)); 
    offset += sizeof(hdr);

    // View ID
    int vid = htonl(g->view_id);
    memcpy(buf + offset, &vid, 4); 
    offset += 4;
 
    // Group Name 
    memcpy(buf + offset, g->grpname, MAX_NAME_LEN); 
    offset += MAX_NAME_LEN;

    // Member Count
    int mc = htonl(g->member_count);
    memcpy(buf + offset, &mc, 4); 
    offset += 4;
 
    // Member Entries
    for (int i = 0; i < g->member_count; i++) {
        Member *m = &g->members[i];
        ViewMemberEntry entry = {0};
        strncpy(entry.myid, m->myid, sizeof(entry.myid) - 1);
        strncpy(entry.udp_ip, m->udp_ip, sizeof(entry.udp_ip) - 1);
        entry.udp_port = m->udp_port; // already in network byte order
        entry.pid      = htonl(m->pid);

        memcpy(buf + offset, &entry, sizeof(entry));              
        offset += sizeof(ViewMemberEntry);
    }
 
    for (int mi = 0; mi < g->member_count; mi++)
        send_msg(g->members[mi].socket_fd, buf, total_size);
 
    free(buf);
}

static void remove_member_from_group(Group *g, int member_idx) {
    printf("GMS: Removing '%s' from group '%s'\n", 
            g->members[member_idx].myid, g->grpname);

    // Free up PID and remove member from group
    g->pids_in_use[g->members[member_idx].pid] = false;
    g->members[member_idx] = g->members[g->member_count - 1];
    g->member_count--;

    if (g->member_count > 0) {
        grp_update(g);
    } 
    else {
        printf("GMS: Deleting empty group '%s'.\n", g->grpname);
        int gi = (int)(g - groups); 
        groups[gi] = groups[--group_count];
    }
}

static void cleanup_dead_fd(int fd, fd_set *master_set) {
    int gi = 0;
    while (gi < group_count) {
        int mi = find_member_in_group(&groups[gi], fd);
        if (mi >= 0) {
            int prev = group_count;
            remove_member_from_group(&groups[gi], mi);
            if (group_count < prev) continue;  // group deleted, re-check same gi
        }
        gi++;
    }
    active_members_rmv(fd);
    FD_CLR(fd, master_set);
    close(fd);
}

static void check_all_liveness(fd_set *master_set) {
    GmsHeader ping_hdr = { htonl(MSG_PING), htonl(0) };

    for (int i = act_mem_count - 1; i >= 0; i--) {
        ActiveMember *am = &active_members[i];
        am->pending_pings++;

        if (am->pending_pings > MAX_MISSED_PINGS || 
          send_msg(am->fd, &ping_hdr, sizeof(ping_hdr)) < 0) {
            printf("GMS: fd %d failed liveness check, removing\n", am->fd);
            cleanup_dead_fd(am->fd, master_set);
        }
    }
}

static void handle_join(int client_sd, int payload_len, fd_set *master_set) {
    if (payload_len != (int)sizeof(JoinPayload)) {
        fprintf(stderr, "GMS: Invalid JOIN payload size (%d) from fd %d\n", 
                payload_len, client_sd);
        return; 
    }

    JoinPayload jp;
    if (recv_msg(client_sd, &jp, sizeof(jp)) < 0) {
        cleanup_dead_fd(client_sd, master_set);
        return;
    }

    Group *g = find_or_create_group(jp.grpname);
    if (!g || g->member_count >= MAX_MEMBERS || 
        id_exists_in_group(g, jp.myid) || 
        find_member_in_group(g, client_sd) >= 0) {
            
        GmsHeader fail = { htonl(MSG_JOIN_FAIL), htonl(0) };
        send_msg(client_sd, &fail, sizeof(fail));
        return; 
    }

    int pid = find_available_pid(g);
    if (pid < 0) { 
        GmsHeader fail = { htonl(MSG_JOIN_FAIL), htonl(0) };
        send_msg(client_sd, &fail, sizeof(fail));
        return; 
    }

    // Add member to group
    Member *m = &g->members[g->member_count++];
    strncpy(m->myid, jp.myid, MAX_NAME_LEN - 1);
    strncpy(m->udp_ip, jp.udp_ip, INET_ADDRSTRLEN - 1);

    // Store UDP port in host byte order and send without conversion
    m->udp_port = jp.udp_port;

    m->socket_fd = client_sd;
    m->pid = pid;
    g->pids_in_use[pid] = true;

    // Send join OK and push view change
    GmsHeader ok = { htonl(MSG_JOIN_OK), htonl(sizeof(int)) };
    int net_assigned_pid = htonl(m->pid);
    char join_reply[sizeof(GmsHeader) + sizeof(int)];
    memcpy(join_reply, &ok, sizeof(GmsHeader));
    memcpy(join_reply + sizeof(GmsHeader), &net_assigned_pid, sizeof(int));

    send_msg(client_sd, join_reply, sizeof(join_reply));

    grp_update(g);
}

static void handle_leave(int client_sd, int payload_len, fd_set *master_set) {
    if (payload_len != (int)sizeof(LeavePayload)) {
        fprintf(stderr, "GMS: Invalid LEAVE payload size (%d) from fd %d\n", 
                payload_len, client_sd);
        return; 
    }

    LeavePayload lp;
    if (recv_msg(client_sd, &lp, sizeof(lp)) < 0) {
        cleanup_dead_fd(client_sd, master_set);
        return;
    }

    lp.myid[MAX_NAME_LEN - 1]    = '\0';
    lp.grpname[MAX_NAME_LEN - 1] = '\0';
 
    Group *g = find_group(lp.grpname);
    if (!g) {
        GmsHeader fail = { htonl(MSG_LEAVE_FAIL), htonl(0) };
        send_msg(client_sd, &fail, sizeof(fail));
        return;
    }

    int mi = find_member_in_group(g, client_sd);
    if (mi < 0) {
        GmsHeader fail = { htonl(MSG_LEAVE_FAIL), htonl(0) };
        send_msg(client_sd, &fail, sizeof(fail));
        return;
    }

    remove_member_from_group(g, mi);

    GmsHeader ok = { htonl(MSG_LEAVE_OK), htonl(0) };
    send_msg(client_sd, &ok, sizeof(ok));

    active_members_rmv(client_sd);
    FD_CLR(client_sd, master_set);
    close(client_sd);
}

static void handle_pong(int client_sd) {
    ActiveMember *am = active_members_find(client_sd);
    if (am) 
        am->pending_pings = 0;
}

static int handle_client_message(int client_sd, fd_set *master_set) {
    GmsHeader hdr;
    if (recv_msg(client_sd, &hdr, sizeof(hdr)) < 0) {
        printf("GMS: Connection lost on fd %d\n", client_sd);
        cleanup_dead_fd(client_sd, master_set);
        return -1;
    }

    int type = ntohl(hdr.type);
    int plen = ntohl(hdr.payload_len);

    switch (type) {
        case MSG_JOIN: { 
            handle_join(client_sd, plen, master_set); 
            break;
        }
        case MSG_LEAVE: {
            handle_leave(client_sd, plen, master_set); 
            break;
        }
        case MSG_PONG: {
            handle_pong(client_sd); 
            break;
        }
        default: {   
            fprintf(stderr, "GMS: unknown message type %d on fd %d\n", 
                type, client_sd);
            break;
        }
    }
    return 0;
}

static void run_gms(int listen_sd) {
    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(listen_sd, &master_set);
    
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd_create"); exit(1); }
    
    struct itimerspec ts;
    ts.it_interval.tv_sec = HEARTBEAT_INTERVAL;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = HEARTBEAT_INTERVAL;
    ts.it_value.tv_nsec = 0;
    timerfd_settime(tfd, 0, &ts, NULL);
    FD_SET(tfd, &master_set);

    int fd_max = (listen_sd > tfd) ? listen_sd : tfd;

    printf("GMS: Service started. TimerFD: %d, ListenFD: %d\n", tfd, listen_sd);

    while (1) {
        read_fds = master_set;

        // blocks until a message or the timer fires
        if (select(fd_max + 1, &read_fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Timer fired
        if (FD_ISSET(tfd, &read_fds)) {
            unsigned long long expirations;
            read(tfd, &expirations, sizeof(expirations));
            check_all_liveness(&master_set);
        }

        // New Connection
        if (FD_ISSET(listen_sd, &read_fds)) {
            printf("GMS: New incoming connection...\n");
            int new_sd = accept(listen_sd, NULL, NULL);
            if (new_sd >= 0) {
                active_members_add(new_sd); // Register for heartbeats immediately
                FD_SET(new_sd, &master_set);
                if (new_sd > fd_max) fd_max = new_sd;
            }
        }

        // Handle Client Data
        int current_max = fd_max; 
        for (int fd = 0; fd <= current_max; fd++) {
            if (fd == listen_sd || fd == tfd) continue; 
            if (FD_ISSET(fd, &read_fds)) {   
                handle_client_message(fd, &master_set);
                if (!FD_ISSET(fd, &master_set) && fd == fd_max) {
                    while (fd_max > listen_sd && !FD_ISSET(fd_max, &master_set))
                        fd_max--;
                }
            }
        }
    }
    close(tfd);
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    int port = (argc >= 2) ? atoi(argv[1]) : DEFAULT_PORT;
    int listen_sd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; 
    setsockopt(listen_sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { 
        .sin_family = AF_INET, 
        .sin_port = htons(port), 
        .sin_addr.s_addr = INADDR_ANY 
    };
    if (bind(listen_sd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("GMS: bind failed");
        exit(1);
    }
    if (listen(listen_sd, 10) < 0) {
        perror("GMS: listen failed");
        exit(1);
    }


    ////////

    printf("========================================\n");
    printf("GMS Server Started!\n");
    printf("Listening on Port: %d\n", port);
    printf("Available IP Addresses for clients:\n");

    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == 0) {
        for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            // Only look at IPv4 addresses
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sa->sin_addr, ip_str, INET_ADDRSTRLEN);
                
                printf("  -> Interface '%s': %s\n", ifa->ifa_name, ip_str);
            }
        }
        freeifaddrs(ifap);
    }
    printf("========================================\n\n");

    ///////////

    run_gms(listen_sd);
    
    return 0;
}