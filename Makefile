CC = gcc
CFLAGS = -Wall -g -pthread
BINDIR = build
SRCDIR = src
INCLUDEDIR = include

all: $(BINDIR)/shm_server $(BINDIR)/shm_client

$(BINDIR)/shm_server: $(SRCDIR)/shm_server.c $(SRCDIR)/shm_utils.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) $^ -o $@ -lrt

$(BINDIR)/shm_client: $(SRCDIR)/shm_client.c $(SRCDIR)/shm_utils.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) $^ -o $@ -lrt

clean:
	rm -rf $(BINDIR)/*
	rm -f /dev/shm/chat_shm

.PHONY: all clean