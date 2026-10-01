CC      = gcc
CFLAGS  = -Wall -Wextra -g

SERVER_SRCS = server.c file_table.c
CLIENT_SRCS = client.c client_table.c

SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

TESTS = tests/test_restart tests/test_cache \
        tests/test_copy tests/test_freshness tests/test_gc \
        tests/test_truncate tests/test_multi_open tests/test_eof \
        tests/test_security tests/test_blocksize tests/test_freshness_bench

.PHONY: all clean

all: server $(TESTS)

server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TESTS): tests/%: tests/%.c $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -I. -o $@ $< $(CLIENT_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

server.o: server.c protocol.h file_table.h
file_table.o: file_table.c file_table.h protocol.h
client.o: client.c client.h client_table.h protocol.h
client_table.o: client_table.c client_table.h protocol.h

clean:
	rm -f server *.o $(TESTS)
	rm -rf tests/*.dSYM
