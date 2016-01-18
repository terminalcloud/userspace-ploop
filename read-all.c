#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

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

	size_t size = (size_t)img->clusterSize * img ->bdevSize;
	int fd = open("outfile", O_RDWR|O_CREAT|O_TRUNC|O_DIRECT, 0600);
	if (fd < 0) {
		perror("creat");
		return 1;
	}
	if (ftruncate(fd, size)) {
		perror("ftruncate");
		return 1;
	}

	void *map = mmap(0, size, PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	// Read it!
	printf("read = %zd\n", plus_read(img, size, 0, map));

	printf("closing...\n");
	close(fd);
	plus_close(img);

	return 0;
}
