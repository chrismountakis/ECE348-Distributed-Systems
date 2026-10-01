#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "api.h"

#define MAX_MSG_LEN 256

typedef struct {
    int group_fd;
    char *filename;
    char *my_id;
    int msgs_to_send;
} thread_args_t;

void *receive_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    char msg[sizeof(ViewChangePayload)]; // for both regular messages and view change notifications
    int len, received = 0, rc;
    struct timespec t_start, t_end;

    FILE *recv_file = fopen(args->filename, "w");

    while (1) {
        len = sizeof(msg);
        rc = grp_recv(args->group_fd, msg, &len, 1);

        if (rc == 0) {
            if (received == 0) clock_gettime(CLOCK_MONOTONIC, &t_start);
            received++;
            clock_gettime(CLOCK_MONOTONIC, &t_end);
            if (len < MAX_MSG_LEN) msg[len] = '\0';
            else msg[MAX_MSG_LEN - 1] = '\0';
            fprintf(recv_file, "%s\n", msg);
        } else if (rc == 1) {
            ViewChangePayload *vc = (ViewChangePayload *)msg;
            printf("[VIEW CHANGE] view_id=%d, members (%d):",
                   vc->view_id, vc->member_count);
            for (int i = 0; i < vc->member_count; i++)
                printf(" %s", vc->member_ids[i]);
            printf("\n");
        } else if (rc == -2) {
            break;
        }
    }
    fclose(recv_file);

    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    printf("\n========== MEASUREMENT RESULTS ==========\n");
    if (received > 0 && elapsed > 0)
        printf("[THROUGHPUT] %s: Delivered %d msgs in %.3f s = %.1f msg/s\n",
               args->my_id, received, elapsed, received / elapsed);

    long s_msg, s_ack, s_dlvr, s_dlvr_ack;
    grp_get_stats(&s_msg, &s_ack, &s_dlvr, &s_dlvr_ack);
    long total_proto = s_msg + s_ack + s_dlvr + s_dlvr_ack;

    printf("[STATS] Protocol messages sent by %s:\n", args->my_id);
    printf("  MSG broadcasts : %ld\n", s_msg);
    printf("  MSG_ACK        : %ld\n", s_ack);
    printf("  DELIVER_NR     : %ld\n", s_dlvr);
    printf("  DELIVER_NR_ACK : %ld\n", s_dlvr_ack);
    printf("  TOTAL OVERHEAD FROM THIS NODE: %ld packets\n", total_proto);
    printf("=========================================\n");

    return NULL;
}

void *send_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;

    if (args->msgs_to_send == 0) return NULL;

    printf("[%s] Waiting 3s for group to stabilize...\n", args->my_id);
    sleep(3);

    char msg[MAX_MSG_LEN];
    for (int i = 1; i <= args->msgs_to_send; i++) {
        snprintf(msg, sizeof(msg), "[%s] Message %d", args->my_id, i);
        int len = strlen(msg) + 1;

        if (grp_send(args->group_fd, msg, len) < 0) {
            printf("Error sending message. Exiting send thread.\n");
            break;
        }
        usleep(20);
    }
    printf("[%s] Finished pushing %d messages to network.\n",
           args->my_id, args->msgs_to_send);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        printf("Usage: %s <GMS_IP> <GMS_PORT> <MY_ID> <GROUP_NAME> <MSGS_TO_SEND>\n", argv[0]);
        printf("  MSGS_TO_SEND: messages to send (0 = receive only)\n");
        return 1;
    }

    char *gms_ip     = argv[1];
    int gms_port     = atoi(argv[2]);
    char *my_id      = argv[3];
    char *grp_name   = argv[4];
    int msgs_to_send = atoi(argv[5]);

    char recv_file[256];
    snprintf(recv_file, sizeof(recv_file), "%s_%s_recvfile.txt", grp_name, my_id);

    printf("--- Member App Starting (%s) ---\n", my_id);

    if (grp_init(gms_ip, gms_port) < 0) return 1;
    int group_fd = grp_join(grp_name, my_id);
    if (group_fd < 0) return 1;

    pthread_t recv_thr, send_thr;
    thread_args_t args = {group_fd, recv_file, my_id, msgs_to_send};

    pthread_create(&recv_thr, NULL, receive_thread, &args);
    pthread_create(&send_thr, NULL, send_thread, &args);

    pthread_join(send_thr, NULL);

    printf("\nAll messages sent. Press Enter to leave the group chat...\n");
    getchar();

    grp_leave(group_fd);
    pthread_join(recv_thr, NULL);

    return 0;
}
