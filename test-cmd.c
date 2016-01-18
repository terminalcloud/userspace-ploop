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
	printf("Usage: %s CMDFILE\n", basename(self));
	printf("CMDFILE is a file consisting of commands\n");
	printf("The following commands are defined:\n");
	printf("add DELTA		-- add a delta file\n");
	printf("open MODE		-- open a set of added deltas\n");
	printf("			   MODE is one of r, rw, w\n");
	printf("read OFFSET SIZE FILE	-- read a block of data\n");
	printf("write OFFSET SIZE FILE	-- write a block of data\n");
	printf("close		-- close the set\n");
	exit(x);
}

int main(int argc, char **argv)
{
	struct plus_image *img = NULL;
	self = argv[0];
	argv++; argc--;
	char *deltas[128];
	int num_deltas = 0;

	if (argc != 1) {
		fprintf(stderr, "Error: invalid number of arguments\n");
		usage(2);
	}

	const char *cmdfile = argv[0];
	FILE *f = fopen(cmdfile, "r");
	if (!f) {
		fprintf(stderr, "Can't open %s: %m\n", cmdfile);
		exit(2);
	}

	char cmd[1024];
	size_t size = 0;
	off_t offset = 0;
	char *file = NULL;
	int lineno = 0;
	while (fgets(cmd, sizeof(cmd), f) != NULL) {
		lineno++;
		int len = strlen(cmd);
		if (cmd[len-1] != '\n') {
			fprintf(stderr, "Bad command on line %d "
				"(no newline or truncated)\n", lineno);
			exit(2);
		}
		// remove the newline
		cmd[len-1] = '\0';

		printf("CMD %s\n", cmd);
		if (strncmp(cmd, "add ", 4) == 0) {
			deltas[num_deltas++] = strdup(cmd + 4);
		} else if (strncmp(cmd, "open ", 5) == 0) {
			// FIXME: ignore the mode for now
			if (num_deltas == 0) {
				fprintf(stderr, "No deltas added before open\n");
				exit(2);
			}
			if (img) {
				fprintf(stderr, "Ploop already opened\n");
				exit(2);
			}
			img = plus_open(num_deltas, deltas, O_RDWR);
			if (!img) {
				fprintf(stderr, "Can't open ploop\n");
				exit(1);
			}
		} else if (strncmp(cmd, "read ", 5) == 0) {
			if (!img) {
				fprintf(stderr, "Ploop not opened\n");
				exit(2);
			}
			if (sscanf(cmd + 5, "%zd %zu %ms", &offset, &size, &file) != 3) {
				fprintf(stderr, "Can't parse command %s\n", cmd);
				exit(2);
			}

			int fd = open(file, O_WRONLY|O_CREAT|O_EXCL, 0600);
			if (fd < 0) {
				fprintf(stderr, "Can't open %s for writing: %m\n", file);
				exit(1);
			}
			void *map = mmap(0, size, PROT_WRITE, MAP_SHARED, fd, 0);
			if (map == MAP_FAILED) {
				fprintf(stderr, "Can't mmap %s: %m\n", file);
				exit(1);
			}
			free(file); file = NULL;

			ssize_t ret = plus_read(img, size, offset, map);
			if (ret != size) {
				fprintf(stderr, "READ failed: %zd\n", ret);
				exit(1);
			}

			munmap(map, size);
			close(fd);
		} else if (strncmp(cmd, "write ", 6) == 0) {
			if (!img) {
				fprintf(stderr, "Ploop not opened\n");
				exit(2);
			}
			if (sscanf(cmd + 6, "%zd %zu %ms", &offset, &size, &file) != 3) {
				fprintf(stderr, "Can't parse command %s\n", cmd);
				exit(2);
			}

			int fd = open(file, O_RDONLY);
			if (fd < 0) {
				fprintf(stderr, "Can't open %s for reading: %m\n", file);
				exit(1);
			}
			void *map = mmap(0, size, PROT_READ, MAP_SHARED, fd, 0);
			if (map == MAP_FAILED) {
				fprintf(stderr, "Can't mmap %s: %m\n", file);
				exit(1);
			}
			free(file); file = NULL;

			ssize_t ret = plus_write(img, size, offset, map);
			if (ret != size) {
				fprintf(stderr, "WRITE failed: %zd\n", ret);
				exit(1);
			}

			munmap(map, size);
			close(fd);
		} else if (strncmp(cmd, "close ", 6) == 0) {
			if (!img) {
				fprintf(stderr, "Ploop not opened\n");
				exit(2);
			}
			plus_close(img);
			img = NULL;
		} else {
			fprintf(stderr, "Unknown cmd: %s\n", cmd);
			exit(2);
		}
	}

	return 0;
}
