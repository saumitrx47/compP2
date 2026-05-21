libanimate:
	@if [ ! -d $@ ]; then \
		echo "ERROR: libanimate not found" > /dev/stderr; \
		echo "Download and unzip libanimate.zip from P2 resources" > /dev/stderr; \
		false; \
	fi


# Makefile for PointerPro Animate Service
# Builds animate_server and animate_client executables with full modular support

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -g -I libanimate/include
LDFLAGS = -pthread
SERVER_LIBS = -Llibanimate/lib -lanimate

# Server source files
SERVER_SRCS = server_main.c connection.c threadpool.c auth.c handles.c rpc.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

# Client source files
CLIENT_SRCS = client_main.c client_connection.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

# Targets
TARGETS = animate_server animate_client

all: $(TARGETS)

# Link server executable
animate_server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SERVER_LIBS) $(LDFLAGS)

# Link client executable
animate_client: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean build artifacts
clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) $(TARGETS)
	rm -f FIFO_* *.log *.dat *.mp4
	rm -f .fuse_hidden*