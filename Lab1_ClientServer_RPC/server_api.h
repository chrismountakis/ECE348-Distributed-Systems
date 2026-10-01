#ifndef HEADER_API_H
#define HEADER_API_H

#include <stdbool.h>
#include <netinet/in.h>

#define HEADER_SIZE         16
#define MAX_SERVICES        2
#define MAX_PENDING         1024

#define DISCOVERY_PORT      9999

#define PING_SVC            -1
#define ERROR_SVC           -2
#define REPLY_ACK_SVC       -3
#define CANCEL_SVC          -4
#define LOAD_BALANCE_SVC    -5
#define DISCOVERY_SVC       -6  

void init(char *config_file, int fixed_port);
int registerService(int svcid);
int unregisterService(int svcid);
int getRequest(int svcid, void *buf, int *len);
void sendReply(int reqid, void *buf, int len);

#endif /* HEADER_API_H */