CC     = gcc
CFLAGS = -Wall -g
LIBS   = -lpthread

server: server_app.c server_api.c server_api.h client_store.c client_store.h
	$(CC) $(CFLAGS) -o server server_app.c server_api.c client_store.c $(LIBS)

clean:
	rm -f server server*.bin
	rm -rf server.dSYM __pycache__
	rm client*.json 
