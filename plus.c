#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <linux/types.h>

#include <ploop/ploop_if.h>
#include <ploop/ploop1_image.h>

#include "plus.h"

#define S2B(sec) ((off_t)(sec) << PLOOP1_SECTOR_LOG)

#define PAGE_SIZE	4096

static int p_memalign(void **memptr, size_t size)
{
	int ret;

	ret = posix_memalign(memptr, PAGE_SIZE, size);
	if (ret)
		perror("Memory allocation failed, posix_memalign");

	return ret;
}

static int open_delta(struct plus_image *img, const char *name) {
	int level = img->level + 1;
	int fd = -1;

	if (level > img->max_levels) {
		fprintf(stderr, "Error: too much levels %d\n", level);
		return -1;
	}

	fd = open(name, O_RDONLY|O_DIRECT);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	// Read the header
	int r = read(fd, img->buf, PAGE_SIZE);
	if (r != PAGE_SIZE) {
		perror("read");
		goto err;
	}
	struct ploop_pvd_header *pvd = (struct ploop_pvd_header *)img->buf;

	// Expect ploop disk
	if (pvd->m_Type != PRL_IMAGE_COMPRESSED) {
		fprintf(stderr, "Image %s doesn't look like a ploop delta file\n", name);
		goto err;
	}
	if (memcmp(pvd->m_Sig, SIGNATURE_STRUCTURED_DISK_V2, sizeof(pvd->m_Sig))) {
		fprintf(stderr, "Image %s doesn't look like a ploop delta file\n", name);
		goto err;
	}
	// Check it's not in use
	if (pvd->m_DiskInUse) {
		fprintf(stderr, "Image %s is in use\n", name);
		goto err;
	}

	// Figure out some metrics
	u32 clusterSize, bdevSize, batSize;

	clusterSize = S2B(pvd->m_Sectors);
	bdevSize = pvd->m_SizeInSectors_v2 >> (ffs(pvd->m_Sectors) - 1);
	batSize = pvd->m_FirstBlockOffset >> (ffs(pvd->m_Sectors) - 1);

	if (level == 0) {
		// cluster size can be different
		if (clusterSize != DEF_CLUSTER) {
			// realloc buf
			free(img->buf);
			if (p_memalign(img->buf, clusterSize))
				goto err;
		}
		img->clusterSize = clusterSize;
	} else {
		// sanity check
		if (clusterSize != img->clusterSize) {
			fprintf(stderr, "Error: img %s got different "
					"cluster size %d\n",
					name, clusterSize);
			goto err;
		}
	}

	struct stat st;
	if (fstat(fd, &st)) {
		perror("stat");
		goto err;
	}

	// Allocated size, i.e. max (last) addressable cluster in the image
	img->allocSize = ((st.st_size + clusterSize - 1) / clusterSize);

	printf("== img %s ==\n", name);
	printf("level: %2d cluster: %5d bat: %5d bdev: %5d alloc: %5d\n\n",
			level, clusterSize, batSize, bdevSize, img->allocSize);

	if (bdevSize != img->bdevSize) {
		// (re)alloc maps
		int lvlsz = sizeof(*img->map_lvl);
		int mapsz = sizeof(*img->map_blk);
		img->map_lvl = realloc(img->map_lvl, bdevSize * lvlsz);
		if (!img->map_lvl) {
			perror("realloc");
			goto err;
		}

		img->map_blk = realloc(img->map_blk, bdevSize * mapsz);
		if (!img->map_blk) {
			perror("realloc");
			goto err;
		}
		if (bdevSize > img->bdevSize) {
			// if we grew, zero the new map area
			u32 add = bdevSize - img->bdevSize;
			memset(img->map_lvl + img->bdevSize, 0, add * lvlsz);
			memset(img->map_blk + img->bdevSize, 0, add * mapsz);
		}
		// save the size
		img->bdevSize = bdevSize;
	}

	// Read the BAT block(s)
	u32 idx = 0;
	for (u32 b = 0; b < batSize; b++) {
		ssize_t r = pread(fd, img->buf, clusterSize, clusterSize * b);
		if (r != clusterSize) {
			perror("pread");
			goto err;
		}
		// first 16 BAT entries (64 bytes) of the first block
		// is the header so we need to skip it
		int i0 = (b == 0) ? 16 : 0;

		// fill in the maps
		u32 *bat = img->buf;
		for (u32 i = i0; i < clusterSize / 4; i++, idx++) {
			if (bat[i] == 0)
				continue;
			// sanity checks
			if (idx > bdevSize) { // and non-zero value
				fprintf(stderr, "Error: BAT entry beyond "
						"block device size "
						"(%u -> %u)\n",
						idx, bat[i]);
				goto err;
			}
			if (bat[i] > img->allocSize) {
				fprintf(stderr, "Error: BAT entry points "
						"past EOF (%u -> %u)\n",
						idx, bat[i]);
				goto err;
			}
			if (bat[i] < batSize) {
				fprintf(stderr, "Error: BAT entry points "
						"to before data blocks "
						"(%u -> %u)\n",
						idx, bat[i]);
				goto err;
			}
			// assign
			img->map_lvl[idx] = level;
			img->map_blk[idx] = bat[i];
			printf("%3d %5u -> %5u\n", level, idx, bat[i]);
		}
	}

	img->fds[level] = fd;
	img->level = level;

	return 0;

err:
	if (fd >= 0)
		close(fd);

	return -1;
}

static int close_deltas(struct plus_image *img) {
	int l = img->level;
	do {
		close(img->fds[l]);
	} while (l-- >= 0);

	return 0;
}

struct plus_image *plus_open(int count, char **deltas) {
	// Allocate img
	struct plus_image *img = calloc(1, sizeof(struct plus_image));
	if (!img)
		return NULL;

	// Initialize it
	img->level = -1;
	img->ro	= 1;
	img->max_levels = count;
	img->fds = calloc(count, sizeof(*img->fds));
	if (!img->fds)
		goto err;

	// initial buffer
	if (p_memalign(&img->buf, DEF_CLUSTER))
		goto err;

	while (count--)
		if (open_delta(img, *deltas++) < 0)
			goto err;

	printf("Combined map follows:\n");
	for (u32 idx = 0; idx < img->bdevSize; idx++) {
		if (img->map_blk[idx])
			printf("%5u -> %2d,%5u\n", idx,
					img->map_lvl[idx],
					img->map_blk[idx]);
	}
	printf("levels: %2d cluster: %5d bat: %5d bdev: %5d alloc: %5d\n\n",
			img->max_levels, img->clusterSize, img->batSize,
			img->bdevSize, img->allocSize);

	return img;

err:
	plus_close(img);
	return NULL;
}

int plus_close(struct plus_image *img) {
	if (!img)
		return 0;

	free(img->buf);
	close_deltas(img);

	free(img->map_lvl);
	free(img->map_blk);

	free(img->fds);

	free(img);

	return 0;
}

#define MIN(a, b)	((a) < (b) ? (a) : (b))

ssize_t plus_read(struct plus_image *img, size_t size, off_t offset, void *buf) {
	if (!img)
		return -EBADF;

	u32 cluster = img->clusterSize;
	size_t got = 0; // How much we have read so far

	// Is it past EOF?
	if (((size + offset) / cluster) > img->bdevSize) {
		fprintf(stderr, "%s: read past EOF\n", __func__);
		return -EINVAL;
	}

	// Is everything page-aligned?
	if (((size_t)buf % PAGE_SIZE) || (size % PAGE_SIZE) || (offset % PAGE_SIZE)) {
		fprintf(stderr, "%s: buf, size, or offset unaligned\n", __func__);
		return -EINVAL;
	}

	printf("READ offset=%5zd size=%5zd\n", offset, size);

	while (got < size) {
		// Cluster number, and offset within it
		u32 idx = offset / cluster; // cluster number
		u32 off = offset % cluster; // offset within the cluster
		u32 len = MIN(cluster - off, size - got); // how much to read

		int lvl = img->map_lvl[idx];
		int blk = img->map_blk[idx];
		printf("  R %5d -> %2d, %5d  off=%5d size=%5d\n",
			idx, lvl, blk, off, len);
		if (blk) {
			// do actual read

			// offset in the delta file
			off_t pos = blk * cluster + off;
			printf("pread(%d, %p, %d, %zu) = ",
					img->fds[lvl], buf + got, len, pos);
			ssize_t r = pread(img->fds[lvl], buf + got, len, pos);
			printf("%zd (%m)\n", r);
			if (r != len) {
				fprintf(stderr, "%s: error in pread(%d, %p, %d, %zu): %m\n",
						__func__, img->fds[lvl], buf + got, len, pos);
				return r;
			}
		}
		else {
			// just zero out buf
//			memset(buf + got, 0, len);
		}
		got += len;
		offset += len;
	}

	return got;
}
