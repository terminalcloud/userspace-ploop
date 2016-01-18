INCLUDES=-I.

CFLAGS = -g -Wall -Wextra -O0 -D_GNU_SOURCE -std=gnu99 $(INCLUDES) $(shell pkg-config fuse --cflags)
LDLIBS=$(shell pkg-config fuse --libs)

BINS = read-all read-blocks test-cmd
OBJS = plus.o

all: $(BINS)
.PHONY: all

read-all: read-all.o $(OBJS)
read-blocks: read-blocks.o $(OBJS)
test-cmd: test-cmd.o $(OBJS)

%: %.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(OBJS) $(BINS) $(BINS:%=%.o)
.PHONY: clean

tar:
	tar cf plus.tar.gz
