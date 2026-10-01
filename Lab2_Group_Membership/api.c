#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <stdatomic.h> 

#include "api.h"

static ProcessState app_state;

static _Atomic long proto_msg_sent      = 0;
static _Atomic long proto_ack_sent      = 0;
static _Atomic long proto_dlvr_sent     = 0;
static _Atomic long proto_dlvr_ack_sent = 0;

void grp_get_stats(long *msg, long *ack, long *dlvr, long *dlvr_ack) {
    if (msg) *msg = atomic_load(&proto_msg_sent);
    if (ack) *ack = atomic_load(&proto_ack_sent);
    if (dlvr) *dlvr = atomic_load(&proto_dlvr_sent);
    if (dlvr_ack) *dlvr_ack = atomic_load(&proto_dlvr_ack_sent);
}

// DEBUG helper (new)
const char *get_msg_type_str(int type) {
    switch(type) {
        case MSG: return "MSG";
        case MSG_ACK: return "MSG_ACK";
        case DELIVER_NR: return "DELIVER_NR";
        case DELIVER_NR_ACK: return "DELIVER_NR_ACK";
        case COORD_STATE_REQ: return "COORD_STATE_REQ";
        case COORD_STATE_REPLY: return "COORD_STATE_REPLY";
        case RECOVERY_DONE: return "RECOVERY_DONE";
        default: return "UNKNOWN";
    }
}

// helpers
static bool mid_eq(MsgID *a, MsgID *b) {
    return a->snd_pid == b->snd_pid && a->seqnum == b->seqnum;
}

static bool is_coordinator(LocalGroup *lg) {
    return lg->member_count > 0 && lg->members[0].pid == lg->my_pid;
}

static void send_udp_to_member(ViewMemberEntry *m, void *data, int len) {
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(m->udp_port)
    };
    if (inet_pton(AF_INET, m->udp_ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "[ERROR] Invalid IP for member %d: %s\n", m->pid, m->udp_ip);
        return;
    }
    
    if (sendto(app_state.udp_socket, data, len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        perror("sendto");
    } else {
        P2P_header *h = (P2P_header *)data;
        int type = ntohl(h->msg_type);
        if (type == MSG) atomic_fetch_add(&proto_msg_sent, 1);
        else if (type == MSG_ACK) atomic_fetch_add(&proto_ack_sent, 1);
        else if (type == DELIVER_NR) atomic_fetch_add(&proto_dlvr_sent, 1);
        else if (type == DELIVER_NR_ACK) atomic_fetch_add(&proto_dlvr_ack_sent, 1);
    }
}
 
static P2P_header make_p2p_hdr(int type, int my_pid, MsgID *mid,
                                const char *grpname, int payload_len) {
    P2P_header h;
    memset(&h, 0, sizeof(h));
    h.msg_type    = htonl(type);
    h.mypid       = htonl(my_pid);
    h.mid.snd_pid = htonl(mid->snd_pid);
    h.mid.seqnum  = htonl(mid->seqnum);
    h.len         = htonl(payload_len);
    strncpy(h.grpname, grpname, MAX_NAME_LEN - 1);
    return h;
}

// Send a P2P_msg to every member except those in skip list
static void broadcast_to_peers(LocalGroup *lg, void *pkt, int pkt_len,
                                const int *skip, int nskip) {
    for (int i = 0; i < lg->member_count; i++) {
        int pid = lg->members[i].pid;
        bool skipped = false;
        for (int s = 0; s < nskip; s++)
            if (pid == skip[s]) { skipped = true; break; }
        if (!skipped)
            send_udp_to_member(&lg->members[i], pkt, pkt_len);
    }
}

static PendingEntry *pending_find(LocalGroup *lg, MsgID *mid) {
    for (int i = 0; i < lg->pending_capacity; i++)
        if (lg->pending[i].active && mid_eq(&lg->pending[i].mid, mid))
            return &lg->pending[i];
    return NULL;
}

static bool mid_exists(LocalGroup *lg, MsgID *mid) {
    PendingEntry *pe = pending_find(lg, mid);
    if (pe != NULL && pe->has_payload) return true;

    int limit = (lg->seen_count < MAX_PENDING) ? lg->seen_count : MAX_PENDING;
    for (int i = 0; i < limit; i++)
        if (mid_eq(&lg->seen_mids[i], mid))
            return true;
    return false;
}

static void mid_record(LocalGroup *lg, MsgID *mid) {
    lg->seen_mids[lg->seen_count % MAX_PENDING] = *mid;
    lg->seen_count++;
}

static PendingEntry *pending_get_or_alloc(LocalGroup *lg, MsgID *mid) {
    PendingEntry *pe = pending_find(lg, mid);
    if (pe) return pe;

    for (int i = 0; i < lg->pending_capacity; i++) {
        if (!lg->pending[i].active) {
            pe = &lg->pending[i];
            memset(pe, 0, sizeof(*pe));
            pe->active = true;
            pe->mid    = *mid;
            pe->delivery_num = -1;
            return pe;
        }
    }

    int old = lg->pending_capacity, cap = old * 2;
    PendingEntry *tmp = realloc(lg->pending, cap * sizeof(PendingEntry));
    if (!tmp) { perror("realloc pending"); return NULL; }
    lg->pending = tmp;
    lg->pending_capacity = cap;
    memset(&lg->pending[old], 0, (cap - old) * sizeof(PendingEntry));

    pe = &lg->pending[old];
    pe->active = true;
    pe->mid    = *mid;
    pe->delivery_num = -1;
    return pe;
}

// Get-or-alloc, then attach payload if not already present. 
static PendingEntry *pending_alloc(LocalGroup *lg, MsgID *mid,
                                    char *payload, int plen) {
    PendingEntry *pe = pending_get_or_alloc(lg, mid);
    if (pe && !pe->has_payload && payload) {
        memcpy(pe->payload, payload, plen);
        pe->payload_len = plen;
        pe->has_payload = true;
    }
    return pe;
}

static bool all_acked_msg(LocalGroup *lg, PendingEntry *pe) {
    for (int i = 0; i < lg->member_count; i++)
        if (!pe->acked_by[lg->members[i].pid])
            return false;
    return true;
}

// Free pending slot if (all acked + have delivery number) 
static void pending_try_free(PendingEntry *pe, LocalGroup *lg) {
    if (pe->acks_all_done &&
        pe->delivery_num != -1 && pe->delivery_num < lg->total_delivered) {
        pe->active = false;
        pthread_cond_broadcast(&app_state.pending_not_full);
    }
}

static DlvrNumEntry *dlvr_find(LocalGroup *lg, MsgID *mid) {
    for (int i = 0; i < lg->dlvr_capacity; i++)
        if (lg->dlvr_buf[i].active && mid_eq(&lg->dlvr_buf[i].mid, mid))
            return &lg->dlvr_buf[i];
    return NULL;
}

static DlvrNumEntry *dlvr_alloc(LocalGroup *lg, MsgID *dlvr_mid,
                                 MsgID *orig_mid, int nr) {
    for (int i = 0; i < lg->dlvr_capacity; i++) {
        if (!lg->dlvr_buf[i].active) {
            DlvrNumEntry *de = &lg->dlvr_buf[i];
            memset(de, 0, sizeof(*de));
            de->active   = true;
            de->mid      = *dlvr_mid;
            de->orig_mid = *orig_mid;
            de->nr       = nr;
            return de;
        }
    }

    int old = lg->dlvr_capacity;
    int cap = old * 2;
    DlvrNumEntry *tmp = realloc(lg->dlvr_buf, cap * sizeof(DlvrNumEntry));
    if (!tmp) { perror("realloc dlvr_buf"); return NULL; }
    lg->dlvr_buf = tmp;
    lg->dlvr_capacity = cap;
    memset(&lg->dlvr_buf[old], 0, (cap - old) * sizeof(DlvrNumEntry));

    DlvrNumEntry *de = &lg->dlvr_buf[old];
    de->active   = true;
    de->mid      = *dlvr_mid;
    de->orig_mid = *orig_mid;
    de->nr       = nr;
    return de;
}

static bool all_acked_dlvr(LocalGroup *lg, DlvrNumEntry *de) {
    for (int i = 0; i < lg->member_count; i++)
        if (!de->acked_by[lg->members[i].pid])
            return false;
    return true;
}

static void build_deliver_nr_pkt(P2P_msg *pkt, int *pkt_len,
                                  LocalGroup *lg, MsgID *dlvr_mid,
                                  MsgID *orig_mid, int nr) {
    DlvrNumPayload dnp;
    dnp.orig_mid.snd_pid = htonl(orig_mid->snd_pid);
    dnp.orig_mid.seqnum  = htonl(orig_mid->seqnum);
    dnp.nr               = htonl(nr);

    pkt->header = make_p2p_hdr(DELIVER_NR, lg->my_pid, dlvr_mid,
                                lg->grpname, sizeof(dnp));
    memcpy(pkt->payload, &dnp, sizeof(dnp));
    *pkt_len = sizeof(P2P_header) + sizeof(dnp);
}

static void send_delivery_num(LocalGroup *lg, MsgID *orig_mid) {
    int nr = lg->coord_nr++;
    
    MsgID dlvr_mid = { .snd_pid = lg->my_pid, .seqnum = nr };

    DlvrNumEntry *de = dlvr_alloc(lg, &dlvr_mid, orig_mid, nr);
    if (!de) { fprintf(stderr, "[ERROR] Failed to allocate DlvrNumEntry\n"); return; }
    de->acked_by[lg->my_pid] = true;

    PendingEntry *pe = pending_get_or_alloc(lg, orig_mid);
    if (!pe) { fprintf(stderr, "[ERROR] CRITICAL: cannot find/alloc msg for DELIVER_NR %d\n", nr); return; }
    pe->delivery_num = nr;

    P2P_msg pkt;
    int pkt_len;
    build_deliver_nr_pkt(&pkt, &pkt_len, lg, &dlvr_mid, orig_mid, nr);

    int skip[] = { lg->my_pid };
    broadcast_to_peers(lg, &pkt, pkt_len, skip, 1);
}

static void forward_delivery_num(LocalGroup *lg, DlvrNumEntry *de,
                                  int forwarder_pid) {
    P2P_msg pkt;
    int pkt_len;
    build_deliver_nr_pkt(&pkt, &pkt_len, lg, &de->mid, &de->orig_mid, de->nr);

    for (int i = 0; i < lg->member_count; i++) {
        int pid = lg->members[i].pid;
        if (pid == lg->my_pid || pid == de->mid.snd_pid || pid == forwarder_pid)
            continue;
        if (!de->acked_by[pid])
            send_udp_to_member(&lg->members[i], &pkt, pkt_len);
    }
}

static void coord_sequence_msg(LocalGroup *lg, int snd_pid) {
    if (!is_coordinator(lg) || lg->is_recovering) 
        return;

    if (lg->recovery_max_dlvr != -1 && lg->total_delivered <= lg->recovery_max_dlvr) 
        return;

    bool progress;
    do {
        progress = false;
        // Check if next expected MSG from this sender has arrived 
        MsgID next = { .snd_pid = snd_pid, .seqnum = lg->expected_seqnum[snd_pid] };
        PendingEntry *pe = pending_find(lg, &next);
        
        if (pe && pe->has_payload) {
            if (pe->delivery_num == -1) {
                send_delivery_num(lg, &next);
                lg->expected_seqnum[snd_pid]++;
                progress = true;
            } else {
                printf("[COORD-DBG] Skipping already-numbered MSG {%d, %d} (delivery_num=%d)\n",
                       next.snd_pid, next.seqnum, pe->delivery_num);
                lg->expected_seqnum[snd_pid]++;
                progress = true;
            }
        }
    } while (progress);
}

static void send_unicast_ack(LocalGroup *lg, P2P_header *hdr, int target_pid) {
    int type = ntohl(hdr->msg_type);
    int ack_type = (type == MSG) ? MSG_ACK : DELIVER_NR_ACK;

    MsgID mid = { .snd_pid = ntohl(hdr->mid.snd_pid),
                  .seqnum  = ntohl(hdr->mid.seqnum) };

    P2P_header ack = make_p2p_hdr(ack_type, lg->my_pid, &mid, lg->grpname, 0);

    for (int i = 0; i < lg->member_count; i++) {
        if (lg->members[i].pid == target_pid) {
            printf("[SEND] %s to PID %d for MSG {%d, %d}\n", get_msg_type_str(ack_type), target_pid, mid.snd_pid, mid.seqnum);
            send_udp_to_member(&lg->members[i], &ack, sizeof(ack));
            return;
        }
    }
}

static void rm_forward(LocalGroup *lg, MsgID *mid, char *payload,
                        int plen, int forwarder_pid) {
    P2P_msg fwd;
    fwd.header = make_p2p_hdr(MSG, lg->my_pid, mid, lg->grpname, plen);
    if (payload) memcpy(fwd.payload, payload, plen);
    int total = sizeof(P2P_header) + plen;

    PendingEntry *pe = pending_find(lg, mid);
    for (int i = 0; i < lg->member_count; i++) {
        int pid = lg->members[i].pid;
        if (pid == lg->my_pid || pid == mid->snd_pid || pid == forwarder_pid)
            continue;
        if (!pe || !pe->acked_by[pid])
            send_udp_to_member(&lg->members[i], &fwd, total);
    }
}

static void rm_record_ack(LocalGroup *lg, MsgID *mid, int from_pid,
                           P2PMessageType type) {
    if (type == DELIVER_NR_ACK) {
        DlvrNumEntry *de = dlvr_find(lg, mid);
        if (de && from_pid >= 0) {
            de->acked_by[from_pid] = true;
            printf("[ACK]: DELIVER_NR_ACK from PID: %d for {%d, %d}\n", from_pid, mid->snd_pid, mid->seqnum);
            if (all_acked_dlvr(lg, de)) {
                de->all_done = true;
                de->active   = false;
            }
        }
    } else {
        PendingEntry *pe = pending_find(lg, mid);
        if (pe && from_pid >= 0) {
            pe->acked_by[from_pid] = true;
            printf("[ACK]: MSG_ACK from PID: %d for {%d, %d}\n", from_pid, mid->snd_pid, mid->seqnum);
            if (all_acked_msg(lg, pe)) {
                pe->acks_all_done = true;
                pending_try_free(pe, lg);
            }
        }
    }
}

static bool is_pid_in_view(LocalGroup *lg, int pid) {
    for (int i = 0; i < lg->member_count; i++)
        if (lg->members[i].pid == pid) return true;
    return false;
}

static bool write_app_queue(LocalGroup *lg, PendingEntry *pe) {
    pthread_mutex_lock(&lg->delivery_mutex);
    
    if (lg->dq_count >= MAX_PENDING) {
        pthread_mutex_unlock(&lg->delivery_mutex);
        return false;
    }

    P2P_msg *slot = &lg->delivery_queue[lg->dq_tail];
    memcpy(slot->payload, pe->payload, pe->payload_len);
    slot->header.len = pe->payload_len;
    
    lg->dq_tail = (lg->dq_tail + 1) % MAX_PENDING;
    lg->dq_count++;
    
    pthread_cond_signal(&lg->delivery_cond);
    pthread_mutex_unlock(&lg->delivery_mutex);
    
    return true;
}

static void trm_try_deliver(LocalGroup *lg) {
    bool progress;
    bool delivered_something = false;
    do {
        progress = false;
        
        // if delivery queue full wait for app to recv
        if (lg->dq_count >= MAX_PENDING) break;

        bool found_entry = false;
        for (int i = 0; i < lg->pending_capacity; i++) {
            PendingEntry *pe = &lg->pending[i];
            if (!pe->active) continue;
            if (pe->delivery_num != lg->total_delivered) continue;

            found_entry = true;

            // If sender is dead and we never got the payload (set void ticket)
            if (!pe->has_payload && !is_pid_in_view(lg, pe->mid.snd_pid)) {
                pe->mid.snd_pid = -1; 
                pe->has_payload = true;
                pe->payload_len = 0;
                pe->acks_all_done = true;
            }

            // Sender alive, payload may still arrive
            if (!pe->has_payload) continue; 

            // Skip void tickets without sending them to the app queue
            if (pe->mid.snd_pid == -1) {
                lg->total_delivered++;
                pe->active = false;
                progress = true;
                break;
            }

            if (!write_app_queue(lg, pe)) {
                continue;
            }
            lg->total_delivered++;
            delivered_something = true;

            if (pe->acks_all_done) {
                pe->active = false;
                pthread_cond_broadcast(&app_state.pending_not_full);
            }
            progress = true;
            break;
        }

        // Gap-skip: no pending entry exists for this delivery_num.
        if (!found_entry && !progress
            && lg->recovery_max_dlvr != -1
            && lg->total_delivered <= lg->recovery_max_dlvr) {
            lg->total_delivered++;
            progress = true;
        }

        // After delivering, let the coordinator assign the next number
        if (delivered_something && is_coordinator(lg) && !lg->is_recovering) {
            for (int i = 0; i < lg->member_count; i++)
                coord_sequence_msg(lg, lg->members[i].pid);
        }
    } while (progress);
}

static int get_max_buffered_dlvr_num(LocalGroup *lg) {
    int max_dlvr = -1;

    // Search in pending messages
    for (int i = 0; i < lg->pending_capacity; i++) {
        if (lg->pending[i].active && lg->pending[i].delivery_num > max_dlvr) {
            max_dlvr = lg->pending[i].delivery_num;
        }
    }

    // Search in delivery number buffer
    for (int i = 0; i < lg->dlvr_capacity; i++) {
        if (lg->dlvr_buf[i].active && lg->dlvr_buf[i].nr > max_dlvr) {
            max_dlvr = lg->dlvr_buf[i].nr;
        }
    }

    return max_dlvr;
}

static void retransmit_coord_state_req(LocalGroup *lg) {
    if (!is_coordinator(lg)) return;
    
    MsgID cntr_mid = { .snd_pid = lg->my_pid, .seqnum = 0 };
    P2P_header req_hdr = make_p2p_hdr(COORD_STATE_REQ, lg->my_pid, &cntr_mid, lg->grpname, 0);
    
    for (int i = 0; i < lg->member_count; i++) {
        int tgt_pid = lg->members[i].pid;
        if (tgt_pid != lg->my_pid && !lg->recovery_replies[tgt_pid]) {
            send_udp_to_member(&lg->members[i], &req_hdr, sizeof(P2P_header));
        }
    }
}

static void retransmit_pending(LocalGroup *lg) {
    // Retransmit unacked MSGs
    for (int i = 0; i < lg->pending_capacity; i++) {
        PendingEntry *pe = &lg->pending[i];
        if (!pe->active || !pe->has_payload || pe->acks_all_done) continue;

        if (++pe->retransmit_count < RETRANSMIT_THRESHOLD) continue;
        pe->retransmit_count = 0;

        P2P_msg fwd;
        fwd.header = make_p2p_hdr(MSG, lg->my_pid, &pe->mid,
                                   lg->grpname, pe->payload_len);
        memcpy(fwd.payload, pe->payload, pe->payload_len);
        int raw = sizeof(P2P_header) + pe->payload_len;

        for (int j = 0; j < lg->member_count; j++) {
            int pid = lg->members[j].pid;
            if (pid == lg->my_pid || pe->acked_by[pid]) continue;
            send_udp_to_member(&lg->members[j], &fwd, raw);
        }
    }

    // Retransmit unacked DELIVER_NRs if not in recovery mode
    if (!lg->is_recovering) {
        for (int i = 0; i < lg->dlvr_capacity; i++) {
            DlvrNumEntry *de = &lg->dlvr_buf[i];
            if (!de->active || de->all_done) continue;

            if (++de->retransmit_count < RETRANSMIT_THRESHOLD) continue;
            de->retransmit_count = 0;

            P2P_msg fwd;
            int raw;
            build_deliver_nr_pkt(&fwd, &raw, lg, &de->mid, &de->orig_mid, de->nr);

            for (int j = 0; j < lg->member_count; j++) {
                int pid = lg->members[j].pid;
                if (pid == lg->my_pid || de->acked_by[pid]) continue;
                send_udp_to_member(&lg->members[j], &fwd, raw);
            }
        }
    }
}

static void finish_recovery(LocalGroup *lg) {
    printf("\n[COORD-DEATH] === FINISH RECOVERY START ===\n");
    printf("[COORD-DEATH] recovery_max_dlvr=%d, recovery_min_delivered=%d, gap=[%d..%d]\n",
           lg->recovery_max_dlvr, lg->recovery_min_delivered,
           lg->recovery_min_delivered + 1, lg->recovery_max_dlvr);

    if (lg->member_count <= 1) {
        printf("[COORD-DEATH] Only member. Skipping gap-fill broadcast (no peers).\n");
        for (int j = 0; j < lg->dlvr_capacity; j++) {
            if (lg->dlvr_buf[j].active && lg->dlvr_buf[j].nr >= lg->recovery_min_delivered + 1
                && lg->dlvr_buf[j].nr <= lg->recovery_max_dlvr) {
                PendingEntry *pe = pending_find(lg, &lg->dlvr_buf[j].orig_mid);
                if (pe) pe->delivery_num = lg->dlvr_buf[j].nr;
            }
        }
    } else {
        int void_count = 0, mapping_count = 0;
        for (int i = lg->recovery_min_delivered + 1; i <= lg->recovery_max_dlvr; i++) {
            MsgID dlvr_mid = { .snd_pid = lg->my_pid, .seqnum = i };
            
            bool found = false;
            MsgID orig_mid;
            for (int j = 0; j < lg->dlvr_capacity; j++) {
                if (lg->dlvr_buf[j].active && lg->dlvr_buf[j].nr == i) {
                    orig_mid = lg->dlvr_buf[j].orig_mid;
                    found = true;
                    break;
                }
            }

            if (!found) {
                orig_mid.snd_pid = -1;
                orig_mid.seqnum = -1;
                void_count++;
            } else {
                printf("[COORD-DEATH] nr %d: recovered mapping -> MSG {%d, %d}\n", i, orig_mid.snd_pid, orig_mid.seqnum);
                mapping_count++;
                PendingEntry *pe = pending_get_or_alloc(lg, &orig_mid);
                if (pe) pe->delivery_num = i;
            }

            P2P_msg pkt;
            int pkt_len;
            build_deliver_nr_pkt(&pkt, &pkt_len, lg, &dlvr_mid, &orig_mid, i);
            broadcast_to_peers(lg, &pkt, pkt_len, &lg->my_pid, 1);
        }
        printf("[COORD-DEATH] Gap-fill complete: %d recovered mappings, %d void tickets\n", mapping_count, void_count);
    }

    printf("[COORD-DEATH] Purging ghost tickets > %d\n", lg->recovery_max_dlvr);
    int purged_dlvr = 0, purged_pending = 0;
    for (int i = 0; i < lg->dlvr_capacity; i++) {
        if (lg->dlvr_buf[i].active && lg->dlvr_buf[i].nr > lg->recovery_max_dlvr) {
            printf("[COORD-DEATH]   Purging dlvr nr=%d orig={%d,%d}\n",
                   lg->dlvr_buf[i].nr, lg->dlvr_buf[i].orig_mid.snd_pid, lg->dlvr_buf[i].orig_mid.seqnum);
            lg->dlvr_buf[i].active = false;
            purged_dlvr++;
        }
    }
    for (int i = 0; i < lg->pending_capacity; i++) {
        if (lg->pending[i].active && lg->pending[i].delivery_num > lg->recovery_max_dlvr) {
            printf("[COORD-DEATH] Stripping ghost nr=%d from MSG {%d,%d}\n",
                   lg->pending[i].delivery_num, lg->pending[i].mid.snd_pid, lg->pending[i].mid.seqnum);
            lg->pending[i].delivery_num = -1;
            purged_pending++;
        }
    }
    printf("[COORD-DEATH] Purged %d dlvr entries, stripped %d pending entries\n", purged_dlvr, purged_pending);

    lg->coord_nr = lg->recovery_max_dlvr + 1;
    lg->is_recovering = false;

    printf("[COORD-DEATH] Setting coord_nr=%d. Broadcasting RECOVERY_DONE. Unfreezing.\n", lg->coord_nr);
    printf("[COORD-DEATH] Expected_seqnum state after recovery:\n");
    for (int i = 0; i < lg->member_count; i++) {
        int pid = lg->members[i].pid;
        printf("Expected_seqnum[%d] = %d\n", pid, lg->expected_seqnum[pid]);
    }
    printf("\n[COORD-DEATH] === FINISH RECOVERY END ===\n");

    RecoveryDonePayload rdp = { .global_max_dlvr = htonl(lg->recovery_max_dlvr) };
    MsgID cntr_mid = { .snd_pid = lg->my_pid, .seqnum = 0 };
    P2P_msg done_pkt;
    done_pkt.header = make_p2p_hdr(RECOVERY_DONE, lg->my_pid, &cntr_mid, lg->grpname, sizeof(rdp));
    memcpy(done_pkt.payload, &rdp, sizeof(rdp));

    int skip[] = { lg->my_pid };
    broadcast_to_peers(lg, &done_pkt, sizeof(P2P_header) + sizeof(rdp), skip, 1);
    
    for (int i = 0; i < lg->member_count; i++)
        coord_sequence_msg(lg, lg->members[i].pid);
    trm_try_deliver(lg);
    
    pthread_cond_broadcast(&lg->recovery_cond);
}

static void send_coord_state_req(LocalGroup *lg) {
    MsgID cntr_mid = { .snd_pid = lg->my_pid, .seqnum = 0 };
    P2P_header req_hdr = make_p2p_hdr(COORD_STATE_REQ, lg->my_pid, &cntr_mid, lg->grpname, 0);

    printf("[COORD-DEATH] I am the new coordinator (PID %d). Starting recovery.\n", lg->my_pid);

    lg->is_recovering = true;
    lg->recovery_replies_count = 0;
    lg->recovery_max_dlvr = -1;
    memset(lg->recovery_replies, 0, sizeof(lg->recovery_replies));

    lg->recovery_replies[lg->my_pid] = true;
    lg->recovery_replies_count = 1;

    int my_max_delivered = lg->total_delivered - 1;
    int my_max_buffered = get_max_buffered_dlvr_num(lg);
    lg->recovery_max_dlvr = (my_max_delivered > my_max_buffered) ? my_max_delivered : my_max_buffered;
    lg->recovery_min_delivered = my_max_delivered;

    printf("[COORD-DEATH] My state: total_delivered=%d, max_buffered=%d, coord_nr=%d\n",
           lg->total_delivered, my_max_buffered, lg->coord_nr);
    printf("[COORD-DEATH] Pending entries still active:\n");
    for (int i = 0; i < lg->pending_capacity; i++) {
        PendingEntry *pe = &lg->pending[i];
        if (pe->active)
            printf("Pending[%d]: MSG {%d,%d} delivery_num=%d has_payload=%d acks_done=%d\n",
                   i, pe->mid.snd_pid, pe->mid.seqnum, pe->delivery_num, pe->has_payload, pe->acks_all_done);
    }
    printf("[COORD-DEATH] DlvrBuf entries still active:\n");
    for (int i = 0; i < lg->dlvr_capacity; i++) {
        DlvrNumEntry *de = &lg->dlvr_buf[i];
        if (de->active)
            printf("dlvr[%d]: nr=%d orig={%d,%d} all_done=%d\n",
                   i, de->nr, de->orig_mid.snd_pid, de->orig_mid.seqnum, de->all_done);
    }

    for (int i = 0; i < lg->member_count; i++) {
        if (lg->members[i].pid != lg->my_pid) {
            printf("[COORD-DEATH] Sending COORD_STATE_REQ to PID %d\n", lg->members[i].pid);
            send_udp_to_member(&lg->members[i], &req_hdr, sizeof(P2P_header));
        }
    }

    if (lg->recovery_replies_count == lg->member_count) {
        printf("[COORD-DEATH] Only member. Finishing recovery via finish_recovery().\n");
        finish_recovery(lg);
    }    
}

static void handle_view_update(int tcp_fd, int plen) {
    char *buf = malloc(plen);
    if (!buf) { perror("malloc view"); return; }
    if (recv_msg(tcp_fd, buf, plen) < 0) {
        perror("recv view"); free(buf); return;
    }

    int offset = 0;

    int view_id = ntohl(*(int *)(buf + offset));            offset += 4;
    char grpname[MAX_NAME_LEN] = {0};
    memcpy(grpname, buf + offset, MAX_NAME_LEN);            offset += MAX_NAME_LEN;
    int member_count = ntohl(*(int *)(buf + offset));        offset += 4;

    if (member_count < 0 || member_count > MAX_MEMBERS) {
        fprintf(stderr, "Invalid member count: %d\n", member_count);
        free(buf); return;
    }

    pthread_mutex_lock(&app_state.state_mutex);

    LocalGroup *lg = NULL;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (app_state.groups[i].is_active &&
            strncmp(app_state.groups[i].grpname, grpname, MAX_NAME_LEN) == 0) {
            lg = &app_state.groups[i];
            break;
        }
    }

    if (lg) {
        int old_coord = (lg->member_count > 0) ? lg->members[0].pid : -1;
        
        char old_ids[MAX_MEMBERS][MAX_NAME_LEN] = {0};
        for (int i = 0; i < lg->member_count; i++)
            strncpy(old_ids[lg->members[i].pid], lg->members[i].myid, MAX_NAME_LEN);

        lg->view_id      = view_id;
        lg->member_count = member_count;

        for (int i = 0; i < member_count; i++) {
            ViewMemberEntry entry;
            memcpy(&entry, buf + offset, sizeof(entry));
            offset += sizeof(entry);

            lg->members[i].pid = ntohl(entry.pid);
            strncpy(lg->members[i].myid,   entry.myid,   MAX_NAME_LEN);
            strncpy(lg->members[i].udp_ip, entry.udp_ip, INET_ADDRSTRLEN);
            lg->members[i].udp_port = ntohs(entry.udp_port);

            if (strncmp(old_ids[lg->members[i].pid], lg->members[i].myid, MAX_NAME_LEN) != 0)
                lg->expected_seqnum[lg->members[i].pid] = 0;
        }

        int new_coord = (lg->member_count > 0) ? lg->members[0].pid : -1;

        if (old_coord != -1 && old_coord != new_coord) {
            printf("[COORD-DEATH] Coordinator died! from PID: %d to PID: %d\n", old_coord, new_coord);
            printf("[COORD-DEATH] view_id=%d, member_count=%d, my_pid=%d, is_new_coord=%s\n",
                   lg->view_id, lg->member_count, lg->my_pid,
                   (new_coord == lg->my_pid) ? "YES" : "NO");
            printf("[COORD-DEATH] State at moment of death: total_delivered=%d, coord_nr=%d, is_recovering=%d\n",
                   lg->total_delivered, lg->coord_nr, lg->is_recovering);
            
            lg->is_recovering = true; 
            if (new_coord == lg->my_pid) 
                send_coord_state_req(lg);
        }        

        for (int j = 0; j < lg->pending_capacity; j++) {
            if (lg->pending[j].active && !lg->pending[j].acks_all_done) {
                if (all_acked_msg(lg, &lg->pending[j])) {
                    lg->pending[j].acks_all_done = true;
                    coord_sequence_msg(lg, lg->pending[j].mid.snd_pid);
                    pending_try_free(&lg->pending[j], lg);
                }
            }
        }
        for (int j = 0; j < lg->dlvr_capacity; j++) {
            if (lg->dlvr_buf[j].active && !lg->dlvr_buf[j].all_done) {
                if (all_acked_dlvr(lg, &lg->dlvr_buf[j])) {
                    lg->dlvr_buf[j].all_done = true;
                    lg->dlvr_buf[j].active   = false;
                }
            }
        }

        if (lg->is_recovering && is_coordinator(lg)) {
            int active_replies = 0;
            for (int i = 0; i < lg->member_count; i++) {
                if (lg->recovery_replies[lg->members[i].pid]) {
                    active_replies++;
                }
            }
            printf("[COORD-DEATH] Dead-node check during recovery: %d/%d survivor replies collected\n",
                   active_replies, lg->member_count);
            if (active_replies >= lg->member_count) {
                printf("[COORD-DEATH] All survivors replied. Finishing recovery now.\n");
                finish_recovery(lg);
            }
        }

        // Notify the application.
        pthread_mutex_lock(&lg->delivery_mutex);
        if (lg->vn_count < MAX_VIEW_NOTIF) {
            ViewChangePayload *slot = &lg->vn_queue[lg->vn_tail];
            slot->view_id      = lg->view_id;
            slot->member_count = lg->member_count;
            for (int i = 0; i < lg->member_count; i++)
                strncpy(slot->member_ids[i], lg->members[i].myid, MAX_NAME_LEN - 1);
            lg->vn_tail = (lg->vn_tail + 1) % MAX_VIEW_NOTIF;
            lg->vn_count++;
            pthread_cond_signal(&lg->delivery_cond);
        }
        pthread_mutex_unlock(&lg->delivery_mutex);
    }

    pthread_mutex_unlock(&app_state.state_mutex);
    free(buf);
}

static LocalGroup *find_group_by_name(const char *name) {
    for (int i = 0; i < MAX_GROUPS; i++)
        if (app_state.groups[i].is_active &&
            strncmp(app_state.groups[i].grpname, name, MAX_NAME_LEN) == 0)
            return &app_state.groups[i];
    return NULL;
}

static void handle_tcp_msg(int tcp_fd) {
    GmsHeader hdr;
    if (recv_msg(tcp_fd, &hdr, sizeof(hdr)) < 0) {
        printf("\nDisconnected from GMS server\n");
        pthread_exit(NULL);
    }

    int type = ntohl(hdr.type);
    int plen = ntohl(hdr.payload_len);

    switch (type) {
    case MSG_PING: {
        GmsHeader pong = { htonl(MSG_PONG), htonl(0) };
        send_msg(tcp_fd, &pong, sizeof(pong));
        break;
    }
    case MSG_JOIN_OK: {
        int net_pid;
        if (recv_msg(tcp_fd, &net_pid, plen) < 0) break;
        pthread_mutex_lock(&app_state.state_mutex);
        app_state.pending_pid = ntohl(net_pid);
        app_state.status = 1;
        pthread_cond_signal(&app_state.cond);
        pthread_mutex_unlock(&app_state.state_mutex);
        break;
    }
    case MSG_JOIN_FAIL:
    case MSG_LEAVE_OK:
    case MSG_LEAVE_FAIL: {
        int s = (type == MSG_JOIN_FAIL) ? -1
              : (type == MSG_LEAVE_OK)  ?  1 : -1;
        pthread_mutex_lock(&app_state.state_mutex);
        app_state.status = s;
        pthread_cond_signal(&app_state.cond);
        pthread_mutex_unlock(&app_state.state_mutex);
        break;
    }
    case MSG_VIEW:
        handle_view_update(tcp_fd, plen);
        break;
    }
}

static void handle_coord_state_req(LocalGroup *lg, int sender_pid) {
    int max_delivered = lg->total_delivered - 1; 
    int max_buffered = get_max_buffered_dlvr_num(lg);
    int count = 0;

    for (int i = 0; i < lg->dlvr_capacity; i++) {
        if (lg->dlvr_buf[i].active && lg->dlvr_buf[i].nr > max_delivered) {
            count++;
        }
    }

    printf("[COORD-DEATH] Replying to new coord PID %d: max_dlvr=%d, max_buf=%d, %d mappings\n",
           sender_pid, max_delivered, max_buffered, count);

    int payload_size = sizeof(CoordStateReplyPayload) + (count * sizeof(DlvrNumPayload));
    char *buf = malloc(sizeof(P2P_header) + payload_size);

    MsgID cntr_mid = { .snd_pid = lg->my_pid, .seqnum = 0 };
    P2P_header hdr = make_p2p_hdr(COORD_STATE_REPLY, lg->my_pid, &cntr_mid, lg->grpname, payload_size);
    memcpy(buf, &hdr, sizeof(hdr));

    CoordStateReplyPayload *reply = (CoordStateReplyPayload *)(buf + sizeof(P2P_header));
    reply->max_delivered = htonl(max_delivered);
    reply->max_buffered = htonl(max_buffered);
    reply->count = htonl(count);

    DlvrNumPayload *mappings = (DlvrNumPayload *)(buf + sizeof(P2P_header) + sizeof(CoordStateReplyPayload));
    int idx = 0;
    for (int i = 0; i < lg->dlvr_capacity; i++) {
        if (lg->dlvr_buf[i].active && lg->dlvr_buf[i].nr > max_delivered) {
            mappings[idx].orig_mid.snd_pid = htonl(lg->dlvr_buf[i].orig_mid.snd_pid);
            mappings[idx].orig_mid.seqnum  = htonl(lg->dlvr_buf[i].orig_mid.seqnum);
            mappings[idx].nr               = htonl(lg->dlvr_buf[i].nr);
            idx++;
        }
    }

    for (int i = 0; i < lg->member_count; i++) {
        if (lg->members[i].pid == sender_pid) {
            send_udp_to_member(&lg->members[i], buf, sizeof(P2P_header) + payload_size);
            break;
        }
    }
    free(buf);
}

static void handle_coord_state_reply(LocalGroup *lg, int sender_pid, CoordStateReplyPayload *payload) {
    if (!lg->is_recovering || lg->recovery_replies[sender_pid]) return; 

    int mem_max_delivered = ntohl(payload->max_delivered);
    int mem_max_buffered = ntohl(payload->max_buffered);
    int count = ntohl(payload->count);

    if (mem_max_delivered < lg->recovery_min_delivered) lg->recovery_min_delivered = mem_max_delivered;
    if (mem_max_buffered > lg->recovery_max_dlvr) lg->recovery_max_dlvr = mem_max_buffered;

    printf("[COORD-DEATH] Reply from PID %d: max_delivered=%d, max_buffered=%d, %d mappings\n",
           sender_pid, mem_max_delivered, mem_max_buffered, count);

    DlvrNumPayload *mappings = (DlvrNumPayload *)((char *)payload + sizeof(CoordStateReplyPayload));
    for (int i = 0; i < count; i++) {
        int dnum = ntohl(mappings[i].nr);
        MsgID orig = { ntohl(mappings[i].orig_mid.snd_pid), ntohl(mappings[i].orig_mid.seqnum) };
        MsgID dummy_mid = { .snd_pid = lg->my_pid, .seqnum = dnum };
        printf("[COORD-DEATH] Mapping: nr=%d. MSG {%d, %d}\n", dnum, orig.snd_pid, orig.seqnum);
        dlvr_alloc(lg, &dummy_mid, &orig, dnum); 
    }

    lg->recovery_replies[sender_pid] = true;
    lg->recovery_replies_count++;

    printf("[COORD-DEATH] Replies collected: %d/%d\n", lg->recovery_replies_count, lg->member_count);
    if (lg->recovery_replies_count == lg->member_count) {
        printf("[COORD-DEATH] All survivors replied! Finishing recovery.\n");
        finish_recovery(lg);
    }
}

static void handle_udp_msg(int udp_fd) {
    char buffer[65536];
    
    struct sockaddr_in sender;
    socklen_t slen = sizeof(sender);

    int n = recvfrom(udp_fd, buffer, sizeof(buffer), 0,
                     (struct sockaddr *)&sender, &slen);
    if (n < (int)sizeof(P2P_header)) return;

    P2P_header *hdr = (P2P_header *)buffer;
    char *payload    = buffer + sizeof(P2P_header);

    int msg_type   = ntohl(hdr->msg_type);
    int sender_pid = ntohl(hdr->mypid);
    int plen       = ntohl(hdr->len);
    MsgID mid      = { ntohl(hdr->mid.snd_pid), ntohl(hdr->mid.seqnum) };

    if (sender_pid < 0 || sender_pid >= MAX_MEMBERS) return;

    printf("\n[RECV]: %s  from PID: %d (MID: {%d, %d}, len: %d)\n", 
           get_msg_type_str(msg_type), sender_pid, mid.snd_pid, mid.seqnum, plen);

    pthread_mutex_lock(&app_state.state_mutex);

    LocalGroup *lg = find_group_by_name(hdr->grpname);
    if (!lg) { pthread_mutex_unlock(&app_state.state_mutex); return; }

    switch (msg_type) {
        case MSG: {
            if(!lg->is_recovering) {
                send_unicast_ack(lg, hdr, sender_pid);
            }

            if (!mid_exists(lg, &mid)) {
                PendingEntry *pe = pending_alloc(lg, &mid, payload, plen);
                if (pe) {
                    pe->acked_by[mid.snd_pid] = true;
                    pe->acked_by[lg->my_pid]  = true;
                    mid_record(lg, &mid);

                    if (mid.snd_pid != lg->my_pid && !lg->is_recovering)
                        rm_forward(lg, &mid, payload, plen, sender_pid);

                    coord_sequence_msg(lg, mid.snd_pid);
                    rm_record_ack(lg, &mid, sender_pid, MSG_ACK);
                }
            } else {
                rm_record_ack(lg, &mid, sender_pid, MSG_ACK);
            }
            trm_try_deliver(lg);
            break;
        }
        case MSG_ACK: {
            rm_record_ack(lg, &mid, sender_pid, MSG_ACK);
            trm_try_deliver(lg);
            break;
        }
        case DELIVER_NR: {
            int curr_coord_pid = (lg->member_count > 0) ? lg->members[0].pid : -1;

            if (lg->is_recovering && mid.snd_pid == curr_coord_pid) {
                printf("[COORD-DEATH] Missed RECOVERY_DONE but got DELIVER_NR from new coord %d. Auto-waking.\n", curr_coord_pid);
                lg->recovery_max_dlvr = get_max_buffered_dlvr_num(lg);
                printf("[COORD-DEATH] Setting recovery_max_dlvr=%d from local state for gap-skip.\n", lg->recovery_max_dlvr);
                lg->is_recovering = false;
                pthread_cond_broadcast(&lg->recovery_cond);
            }        
            if(!lg->is_recovering) {
                send_unicast_ack(lg, hdr, sender_pid);
            }

            DlvrNumPayload *dnp = (DlvrNumPayload *)payload;
            MsgID orig = { ntohl(dnp->orig_mid.snd_pid), ntohl(dnp->orig_mid.seqnum) };
            int nr     = ntohl(dnp->nr);

            if (orig.snd_pid == -1) {
                MsgID void_mid = { .snd_pid = -1, .seqnum = nr };
                PendingEntry *pe = pending_get_or_alloc(lg, &void_mid);
                if (pe) {
                    pe->delivery_num = nr;
                    pe->has_payload = true; 
                    pe->payload_len = 0;
                    pe->acks_all_done = true; 
                }
            }

            if (orig.snd_pid != -1 && orig.seqnum >= lg->expected_seqnum[orig.snd_pid]) {
                lg->expected_seqnum[orig.snd_pid] = orig.seqnum + 1;
            }
            
            DlvrNumEntry *de = dlvr_find(lg, &mid);
            if (!de) {
                de = dlvr_alloc(lg, &mid, &orig, nr);
                if (de) {
                    de->acked_by[mid.snd_pid] = true;
                    de->acked_by[lg->my_pid]  = true;
                    if (all_acked_dlvr(lg, de)) de->all_done = true;

                    PendingEntry *pe = (orig.snd_pid != -1) 
                        ? pending_get_or_alloc(lg, &orig) : NULL;
                    if (pe) pe->delivery_num = nr;

                    if (mid.snd_pid != lg->my_pid && !lg->is_recovering)
                        forward_delivery_num(lg, de, sender_pid);
                    rm_record_ack(lg, &mid, sender_pid, DELIVER_NR_ACK);
                }
            } else {
                rm_record_ack(lg, &mid, sender_pid, DELIVER_NR_ACK);
            }
            trm_try_deliver(lg);
            break;
        }
        case DELIVER_NR_ACK: {
            rm_record_ack(lg, &mid, sender_pid, DELIVER_NR_ACK);
            trm_try_deliver(lg);
            break;
        }
        case COORD_STATE_REQ: {
            printf("[COORD-DEATH] Got COORD_STATE_REQ from new coord PID %d\n", sender_pid);
            handle_coord_state_req(lg, sender_pid);
            break;
        }
        case COORD_STATE_REPLY: {
            handle_coord_state_reply(lg, sender_pid, (CoordStateReplyPayload *)payload);
            break;
        }
        case RECOVERY_DONE: {
            RecoveryDonePayload *rdp = (RecoveryDonePayload *)payload;
            int global_max = ntohl(rdp->global_max_dlvr);
            printf("[COORD-DEATH] RECOVERY_DONE received. global_max=%d. Purging ghosts.\n", global_max);

            lg->recovery_max_dlvr = global_max;

            for (int i = 0; i < lg->dlvr_capacity; i++) {
                if (lg->dlvr_buf[i].active && lg->dlvr_buf[i].nr > global_max) {
                    printf("[COORD-DEATH] Purging ghost dlvr nr=%d\n", lg->dlvr_buf[i].nr);
                    lg->dlvr_buf[i].active = false;
                }
            }
            for (int i = 0; i < lg->pending_capacity; i++) {
                if (lg->pending[i].active && lg->pending[i].delivery_num > global_max) {
                    printf("[COORD-DEATH] Stripping ghost nr=%d from MSG {%d,%d}\n", 
                           lg->pending[i].delivery_num, lg->pending[i].mid.snd_pid, lg->pending[i].mid.seqnum);
                    lg->pending[i].delivery_num = -1; 
                }
            }

            lg->is_recovering = false;
            pthread_cond_broadcast(&lg->recovery_cond);
            trm_try_deliver(lg);
            break;
        }
    }

    pthread_mutex_unlock(&app_state.state_mutex);
}

static void *background_listener(void *arg) {
    (void)arg;

    int tcp_fd = app_state.tcp_socket;
    int udp_fd = app_state.udp_socket;

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd_create"); exit(1); }
    struct itimerspec ts = {
        .it_interval = { RETRANSMIT_INTERVAL, 0 },
        .it_value    = { RETRANSMIT_INTERVAL, 0 }
    };
    timerfd_settime(tfd, 0, &ts, NULL);

    fd_set master;
    FD_ZERO(&master);
    FD_SET(tcp_fd, &master);
    FD_SET(udp_fd, &master);
    FD_SET(tfd,    &master);

    int max_fd = tcp_fd;
    if (udp_fd > max_fd) max_fd = udp_fd;
    if (tfd    > max_fd) max_fd = tfd;

    while (1) {
        fd_set rset = master;
        if (select(max_fd + 1, &rset, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        if (FD_ISSET(tfd, &rset)) {
            unsigned long long exp;
            read(tfd, &exp, sizeof(exp));
            
            pthread_mutex_lock(&app_state.state_mutex);
            for (int g = 0; g < MAX_GROUPS; g++) {
                if (app_state.groups[g].is_active) {
                    if (app_state.groups[g].is_recovering) {
                        retransmit_coord_state_req(&app_state.groups[g]);
                    } else {
                        retransmit_pending(&app_state.groups[g]);
                        trm_try_deliver(&app_state.groups[g]);
                    }
                }
            }
            pthread_mutex_unlock(&app_state.state_mutex);
        }
        if (FD_ISSET(tcp_fd, &rset))
            handle_tcp_msg(tcp_fd);
        if (FD_ISSET(udp_fd, &rset))
            handle_udp_msg(udp_fd);
    }
    close(tfd);
    return NULL;
}

int grp_init(char *gms_addr, int gms_port) {
    // TCP socket to GMS 
    app_state.tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (app_state.tcp_socket < 0) { perror("tcp socket"); return -1; }

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(gms_port)
    };
    if (inet_pton(AF_INET, gms_addr, &srv.sin_addr) <= 0) {
        perror("inet_pton gms"); return -1;
    }
    if (connect(app_state.tcp_socket, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect gms"); return -1;
    }

    // UDP socket for P2P 
    app_state.udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (app_state.udp_socket < 0) { perror("udp socket"); return -1; }

    struct sockaddr_in udp = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY };
    if (bind(app_state.udp_socket, (struct sockaddr *)&udp, sizeof(udp)) < 0) {
        perror("udp bind"); return -1;
    }
    socklen_t slen = sizeof(udp);
    getsockname(app_state.udp_socket, (struct sockaddr *)&udp, &slen);
    app_state.udp_port = ntohs(udp.sin_port);

    // get reachable IP from the connected TCP socket 
    struct sockaddr_in tcp_local;
    slen = sizeof(tcp_local);
    getsockname(app_state.tcp_socket, (struct sockaddr *)&tcp_local, &slen);
    inet_ntop(AF_INET, &tcp_local.sin_addr, app_state.udp_ip, sizeof(app_state.udp_ip));

    for (int i = 0; i < MAX_GROUPS; i++)
        app_state.groups[i].is_active = false;

    pthread_mutex_init(&app_state.state_mutex, NULL);
    pthread_cond_init(&app_state.cond, NULL);
    pthread_cond_init(&app_state.pending_not_full, NULL);

    if (pthread_create(&app_state.listener_thr, NULL, background_listener, NULL) != 0) {
        perror("pthread_create"); return -1;
    }
    pthread_detach(app_state.listener_thr);
    return 0;
}

int grp_join(char *grpname, char *myid) {
    pthread_mutex_lock(&app_state.state_mutex);

    int slot = -1;
    for (int i = 0; i < MAX_GROUPS; i++)
        if (!app_state.groups[i].is_active) { slot = i; break; }
    if (slot < 0) {
        pthread_mutex_unlock(&app_state.state_mutex);
        fprintf(stderr, "grp_join: no free slots\n");
        return -1;
    }

    LocalGroup *lg = &app_state.groups[slot];
    memset(lg, 0, sizeof(*lg));
    lg->recovery_max_dlvr = -1;
    lg->is_active = true;
    lg->my_pid    = -1;
    strncpy(lg->myid,    myid,    MAX_NAME_LEN - 1);
    strncpy(lg->grpname, grpname, MAX_NAME_LEN - 1);

    lg->pending_capacity = MAX_PENDING;
    lg->pending  = calloc(lg->pending_capacity, sizeof(PendingEntry));
    lg->dlvr_capacity = MAX_PENDING;
    lg->dlvr_buf = calloc(lg->dlvr_capacity, sizeof(DlvrNumEntry));

    pthread_mutex_init(&lg->delivery_mutex, NULL);
    pthread_cond_init(&lg->delivery_cond, NULL);
    pthread_cond_init(&lg->recovery_cond, NULL);

    app_state.status = 0;
    pthread_mutex_unlock(&app_state.state_mutex);

    // send JOIN request to GMS
    JoinMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type        = htonl(MSG_JOIN);
    msg.header.payload_len = htonl(sizeof(JoinPayload));
    strncpy(msg.payload.grpname, grpname, MAX_NAME_LEN - 1);
    strncpy(msg.payload.myid,    myid,    MAX_NAME_LEN - 1);
    strncpy(msg.payload.udp_ip,  app_state.udp_ip, INET_ADDRSTRLEN - 1);
    msg.payload.udp_port = htons(app_state.udp_port);

    if (send_msg(app_state.tcp_socket, &msg, sizeof(msg)) < 0) {
        perror("grp_join send"); return -1;
    }

    // Wait for response 
    pthread_mutex_lock(&app_state.state_mutex);
    while (app_state.status == 0)
        pthread_cond_wait(&app_state.cond, &app_state.state_mutex);

    if (app_state.status == -1) {
        free(lg->pending);  lg->pending  = NULL;
        free(lg->dlvr_buf); lg->dlvr_buf = NULL;
        lg->is_active = false;
        pthread_mutex_unlock(&app_state.state_mutex);
        return -1;
    }
    lg->my_pid = app_state.pending_pid;
    pthread_mutex_unlock(&app_state.state_mutex);
    return slot;
}

int grp_leave(int g) {
    if (g < 0 || g >= MAX_GROUPS) return -1;

    pthread_mutex_lock(&app_state.state_mutex);
    LocalGroup *lg = &app_state.groups[g];
    if (!lg->is_active) { pthread_mutex_unlock(&app_state.state_mutex); return -1; }

    // send LEAVE request to GMS
    LeaveMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type        = htonl(MSG_LEAVE);
    msg.header.payload_len = htonl(sizeof(LeavePayload));
    strncpy(msg.payload.grpname, lg->grpname, MAX_NAME_LEN - 1);
    strncpy(msg.payload.myid,    lg->myid,    MAX_NAME_LEN - 1);

    app_state.status = 0;
    pthread_mutex_unlock(&app_state.state_mutex);

    if (send_msg(app_state.tcp_socket, &msg, sizeof(msg)) < 0) {
        perror("grp_leave send"); return -1;
    }

    // Wait for response 
    pthread_mutex_lock(&app_state.state_mutex);
    while (app_state.status == 0)
        pthread_cond_wait(&app_state.cond, &app_state.state_mutex);

    if (app_state.status == 1) {
        pthread_mutex_lock(&lg->delivery_mutex);
        lg->is_active = false;
        pthread_cond_broadcast(&lg->delivery_cond);
        pthread_mutex_unlock(&lg->delivery_mutex);

        pthread_mutex_destroy(&lg->delivery_mutex);
        pthread_cond_destroy(&lg->delivery_cond);
        pthread_cond_destroy(&lg->recovery_cond);
        free(lg->pending);  lg->pending  = NULL;
        free(lg->dlvr_buf); lg->dlvr_buf = NULL;
    }
    int ok = (app_state.status == 1);
    pthread_mutex_unlock(&app_state.state_mutex);
    return ok ? 0 : -1;
}

int grp_send(int g, void *msg, int len) {
    if (g < 0 || g >= MAX_GROUPS || len > MAX_PAYLOAD_LEN) return -1;

    pthread_mutex_lock(&app_state.state_mutex);
    LocalGroup *lg = &app_state.groups[g];
    if (!lg->is_active || !msg) {
        pthread_mutex_unlock(&app_state.state_mutex);
        return -1;
    }

    while (lg->is_recovering) {
        pthread_cond_wait(&lg->recovery_cond, &app_state.state_mutex);
    }

    MsgID mid = { .snd_pid = lg->my_pid, .seqnum = lg->seqnum };

    // block until a pending slot is available 
    PendingEntry *pe = NULL;
    while (!(pe = pending_alloc(lg, &mid, msg, len))) {
        pthread_cond_wait(&app_state.pending_not_full, &app_state.state_mutex);
    }

    // self-buffer 
    mid_record(lg, &mid);
    coord_sequence_msg(lg, lg->my_pid);
    rm_record_ack(lg, &mid, lg->my_pid, MSG_ACK);
    trm_try_deliver(lg);

    int seqnum = lg->seqnum++;

    P2P_msg pkt;
    MsgID net_mid = { .snd_pid = lg->my_pid, .seqnum = seqnum };
    pkt.header = make_p2p_hdr(MSG, lg->my_pid, &net_mid, lg->grpname, len);
    memcpy(pkt.payload, msg, len);
    int total = sizeof(P2P_header) + len;

    // Copy targets to send without holding the lock 
    ViewMemberEntry targets[MAX_MEMBERS];
    int tc = 0;
    for (int i = 0; i < lg->member_count; i++)
        if (lg->members[i].pid != lg->my_pid)
            targets[tc++] = lg->members[i];

    pthread_mutex_unlock(&app_state.state_mutex);

    for (int i = 0; i < tc; i++)
        send_udp_to_member(&targets[i], &pkt, total);

    return 0;
}

int grp_recv(int g, void *msg, int *len, int block) {
    if (g < 0 || g >= MAX_GROUPS) return -1;

    LocalGroup *lg = &app_state.groups[g];
    if (!lg->is_active) return -1;

    pthread_mutex_lock(&lg->delivery_mutex);

    while (lg->dq_count == 0 && lg->vn_count == 0) {
        if (!block) { pthread_mutex_unlock(&lg->delivery_mutex); return -1; }
        pthread_cond_wait(&lg->delivery_cond, &lg->delivery_mutex);
        if (!lg->is_active) { pthread_mutex_unlock(&lg->delivery_mutex); return -2; }
    }

    // View change notification
    if (lg->vn_count > 0) {
        memcpy(msg, &lg->vn_queue[lg->vn_head], sizeof(ViewChangePayload));
        *len = sizeof(ViewChangePayload);
        lg->vn_head = (lg->vn_head + 1) % MAX_VIEW_NOTIF;
        lg->vn_count--;
        pthread_mutex_unlock(&lg->delivery_mutex);
        return 1;
    }

    // Regular message delivery
    P2P_msg *front = &lg->delivery_queue[lg->dq_head];
    *len = front->header.len;
    memcpy(msg, front->payload, *len);
    lg->dq_head = (lg->dq_head + 1) % MAX_PENDING;
    lg->dq_count--;

    pthread_mutex_unlock(&lg->delivery_mutex);
    return 0;
}