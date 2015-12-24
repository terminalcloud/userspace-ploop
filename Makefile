INCLUDES=-I.

CFLAGS = -g -Wall -Wextra -O0 -D_GNU_SOURCE -std=gnu99 $(INCLUDES) $(shell pkg-config fuse --cflags)
LDLIBS=$(shell pkg-config fuse --libs)

BINS = plus
OBJS = plus.o main.o

all: $(BINS)
.PHONY: all

plus: $(OBJS) main.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(OBJS) $(BINS)
.PHONY: clean

tar:
	tar cf plus.tar.gz
