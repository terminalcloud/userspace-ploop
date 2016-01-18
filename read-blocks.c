#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mcheck.h>

#include "plus.h"

static const char *self; // argv[0]

static void usage(int x)
{
	printf("Usage: %s BASE_DELTA ... TOP_DELTA\n", basename(self));
	exit(x);
}

int main(int argc, char **argv)
{
	struct plus_image *img;
	self = argv[0];
	argv++; argc--;

	if (argc < 1) {
		fprintf(stderr, "Error: invalid number of arguments\n");
		usage(1);
	}

	img = plus_open(argc, argv, O_RDONLY);
	if (img == NULL)
		return 1;

	size_t size = img->clusterSize;

	void *map = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	size_t blocks[] = { 1247233, 1232897, 1234945, 1236993, 1239041,
		1241089, 1243137, 1245185, 1247233, 1249281, 1251329,
		1253377, 1255425, 1257473, 1259521, 1261569, 0};

	for (int i = 0; blocks[i] != 0; i++) {
		size_t pos = blocks[i] * size;

		// Read it!
		printf("read = %zd\n", plus_read(img, size, pos, map));
	}

	printf("closing...\n");
	plus_close(img);

	return 0;
}
