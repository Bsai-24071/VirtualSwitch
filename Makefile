CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -g -O0

SWITCH_SRCS = frame.c mac_table.c common.c queue.c port.c switch.c
HOST_SRCS   = frame.c host.c

SWITCH_OBJS     = $(patsubst %.c,obj/%.o,$(SWITCH_SRCS))
HOST_OBJS       = $(patsubst %.c,obj/%.o,$(HOST_SRCS))

SWITCH_TSAN_OBJS = $(patsubst %.c,obj_tsan/%.o,$(SWITCH_SRCS))
HOST_TSAN_OBJS   = $(patsubst %.c,obj_tsan/%.o,$(HOST_SRCS))

SWITCH_ASAN_OBJS = $(patsubst %.c,obj_asan/%.o,$(SWITCH_SRCS))
HOST_ASAN_OBJS   = $(patsubst %.c,obj_asan/%.o,$(HOST_SRCS))

TSAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all clean tsan asan

all: switch host

switch: $(SWITCH_OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $^

host: $(HOST_OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $^

obj/%.o: %.c | obj
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

obj:
	mkdir -p obj

tsan: switch_tsan host_tsan

switch_tsan: $(SWITCH_TSAN_OBJS)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -pthread -o $@ $^

host_tsan: $(HOST_TSAN_OBJS)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -pthread -o $@ $^

obj_tsan/%.o: %.c | obj_tsan
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -pthread -c -o $@ $<

obj_tsan:
	mkdir -p obj_tsan

asan: switch_asan host_asan

switch_asan: $(SWITCH_ASAN_OBJS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -pthread -o $@ $^

host_asan: $(HOST_ASAN_OBJS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -pthread -o $@ $^

obj_asan/%.o: %.c | obj_asan
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -pthread -c -o $@ $<

obj_asan:
	mkdir -p obj_asan

clean:
	rm -rf obj obj_tsan obj_asan switch host switch_tsan host_tsan switch_asan host_asan
