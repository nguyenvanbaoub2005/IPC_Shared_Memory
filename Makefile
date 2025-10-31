CC = gcc
CFLAGS = -Wall -g -pthread
BINDIR = build
SRCDIR = src
INCLUDEDIR = include

all: $(BINDIR)/socket_server $(BINDIR)/socket_client

$(BINDIR)/socket_server: $(SRCDIR)/socket_server.c $(SRCDIR)/server_utils.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) $^ -o $@

$(BINDIR)/socket_client: $(SRCDIR)/socket_client.c $(SRCDIR)/client_utils.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -I$(INCLUDEDIR) $^ -o $@

clean:
	rm -rf $(BINDIR)/*