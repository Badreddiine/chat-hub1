CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -I./common
BINDIR  = bin

all: $(BINDIR) server client

$(BINDIR):
	mkdir -p $(BINDIR)

server: server/chat_server.c common/protocol.h
	$(CC) $(CFLAGS) -o $(BINDIR)/chat_serverd server/chat_server.c

client: client/chat_client.c common/protocol.h
	$(CC) $(CFLAGS) -o $(BINDIR)/chat_client client/chat_client.c

clean:
	rm -rf $(BINDIR)

.PHONY: all server client clean
