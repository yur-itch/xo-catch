CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
CPPFLAGS ?=
LDFLAGS ?=

CJSON_LIBS ?= -lcjson

RAYLIB_CFLAGS ?= $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS ?= $(shell pkg-config --libs raylib 2>/dev/null)
ifeq ($(strip $(RAYLIB_LIBS)),)
RAYLIB_LIBS = -lraylib -lm -lpthread -ldl -lrt -lX11 -lGL -lXrandr -lXi -lXcursor -lXinerama
endif

SERVER_SRCS = server.c game_logic.c
CLIENT_SRCS = client.c

.PHONY: all server client clean

all: server client

server: $(SERVER_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDFLAGS) $(CJSON_LIBS)

client: $(CLIENT_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDFLAGS) $(RAYLIB_CFLAGS) $(RAYLIB_LIBS) $(CJSON_LIBS)

clean:
	rm -f server client
