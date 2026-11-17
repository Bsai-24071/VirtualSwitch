CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -g -O0

OBJ     = frame.o mac_table.o

all: $(OBJ)

frame.o: frame.c frame.h
	$(CC) $(CFLAGS) -c frame.c -o $@

mac_table.o: mac_table.c mac_table.h frame.h
	$(CC) $(CFLAGS) -c mac_table.c -o $@

clean:
	rm -f $(OBJ)

.PHONY: all clean
