#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#include "server_api.h"

static bool isPrime(int n) {
    if (n <= 1) return false;
    for (int i = 2; i * i <= n; i++) 
        if (n % i == 0) return false;
    return true;
}

static void isPrimeArray(const int *nums, bool *out, int len) {
    for (int i = 0; i < len; i++)
        out[i] = isPrime(nums[i]);
}

void *prime_worker(void *arg) {
    int svc_id = 0; 
    int network_buf[256]; 
    int host_nums[256];
    int byte_len;

    while (1) {
        int req_handle = getRequest(svc_id, network_buf, &byte_len);

        if (req_handle < 0) 
            continue; 

        int count = byte_len / sizeof(int);

        for (int i = 0; i < count; i++) {
            host_nums[i] = ntohl(network_buf[i]);
        }

        printf("Worker: Processing %d numbers for handle %d\n", count, req_handle);

        bool results[256];
        isPrimeArray(host_nums, results, count);

        sendReply(req_handle, results, count * sizeof(bool));
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if(argc != 4) {
        fprintf(stderr, "Usage: %s <port> <config_file> <num_workers>\n", argv[0]);
        return 1;
    }

    int fixed_port = atoi(argv[1]);
    char* config_file = argv[2];
    int num_workers = atoi(argv[3]);

    init(config_file, fixed_port);

    if (registerService(0) != 0) {
        fprintf(stderr, "Failed to register service 0\n");
        return 1;
    }

    pthread_t workers[num_workers];
    for(int i = 0; i < num_workers; i++) {
        if (pthread_create(&workers[i], NULL, prime_worker, NULL) != 0) {
            perror("pthread_create worker");
            return 1;
        }
    }

    printf("Server is live. Press Ctrl+C to exit.\n");

    for(int i = 0; i < num_workers; i++) {
        pthread_join(workers[i], NULL);
    }

    return 0;
}