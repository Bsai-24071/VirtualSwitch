CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -g -O0

SWITCH_SRCS = frame.c mac_table.c common.c queue.c port.c switch.c admin.c logger.c
HOST_SRCS   = frame.c host.c
SWCTL_SRCS  = frame.c common.c logger.c swctl.c

SWITCH_OBJS     = $(patsubst %.c,obj/%.o,$(SWITCH_SRCS))
HOST_OBJS       = $(patsubst %.c,obj/%.o,$(HOST_SRCS))
SWCTL_OBJS      = $(patsubst %.c,obj/%.o,$(SWCTL_SRCS))

SWITCH_TSAN_OBJS = $(patsubst %.c,obj_tsan/%.o,$(SWITCH_SRCS))
HOST_TSAN_OBJS   = $(patsubst %.c,obj_tsan/%.o,$(HOST_SRCS))
SWCTL_TSAN_OBJS  = $(patsubst %.c,obj_tsan/%.o,$(SWCTL_SRCS))

SWITCH_ASAN_OBJS = $(patsubst %.c,obj_asan/%.o,$(SWITCH_SRCS))
HOST_ASAN_OBJS   = $(patsubst %.c,obj_asan/%.o,$(HOST_SRCS))
SWCTL_ASAN_OBJS  = $(patsubst %.c,obj_asan/%.o,$(SWCTL_SRCS))

TSAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all clean tsan asan test

all: switch host swctl

switch: $(SWITCH_OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $^ -lrt

host: $(HOST_OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $^

swctl: $(SWCTL_OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $^ -lrt

obj/%.o: %.c | obj
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

obj:
	mkdir -p obj

tsan: switch_tsan host_tsan swctl_tsan

switch_tsan: $(SWITCH_TSAN_OBJS)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -pthread -o $@ $^ -lrt

host_tsan: $(HOST_TSAN_OBJS)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -pthread -o $@ $^

swctl_tsan: $(SWCTL_TSAN_OBJS)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -pthread -o $@ $^ -lrt

obj_tsan/%.o: %.c | obj_tsan
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -pthread -c -o $@ $<

obj_tsan:
	mkdir -p obj_tsan

asan: switch_asan host_asan swctl_asan

switch_asan: $(SWITCH_ASAN_OBJS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -pthread -o $@ $^ -lrt

host_asan: $(HOST_ASAN_OBJS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -pthread -o $@ $^

swctl_asan: $(SWCTL_ASAN_OBJS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -pthread -o $@ $^ -lrt

obj_asan/%.o: %.c | obj_asan
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -pthread -c -o $@ $<

obj_asan:
	mkdir -p obj_asan

test:
	@for t in tests/t*.sh; do \
		echo "Running $$t ..."; \
		bash $$t || { echo "FAILED: $$t"; exit 1; }; \
	done
	@echo "All tests PASSED."

clean:
	rm -rf obj obj_tsan obj_asan switch host swctl switch_tsan host_tsan swctl_tsan switch_asan host_asan swctl_asan
