CFLAGS=-ggdb -g3 -std=gnu11 -Wall -Wextra -Wpedantic
LDFLAGS=$(shell pkg-config --cflags --libs fuse3)

all:
	cc -o trfs trfs.c $(CFLAGS) $(LDFLAGS)

clean:
	rm -f trfs

