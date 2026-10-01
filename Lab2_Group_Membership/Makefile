CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -pthread

TARGETS = gms member_app

GMS_OBJS = gms.o utils.o
MEMBER_OBJS = member_app.o api.o utils.o

all: $(TARGETS)

gms: $(GMS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

member_app: $(MEMBER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

gms.o: gms.c gms.h utils.h
api.o: api.c api.h utils.h
member_app.o: member_app.c api.h utils.h
utils.o: utils.c utils.h

clean:
	rm -f *.o $(TARGETS)

.PHONY: all clean