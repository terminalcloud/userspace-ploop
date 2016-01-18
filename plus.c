#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdbool.h>

#include <linux/types.h>

#include <ploop/ploop_if.h>
#include <ploop/ploop1_image.h>

#include "plus.h"

#define S2B(sec) ((off_t)(sec) << PLOOP1_SECTOR_LOG)

#define PAGE_SIZE	4096

// Size of ploop on-disk image header, in 32-bit words
#define HDR_SIZE_32	16 // sizeof(struct ploop_pvd_header) / sizeof(u32)

static int p_memalign(void **memptr, size_t size)
{
	int ret;

	ret = posix_memalign(memptr, PAGE_SIZE, size);
	if (ret) {
		perror("Memory allocation failed, posix_memalign");
	}

	return ret;
}

static int open_delta(struct plus_image *img, const char *name, int rw)
{
	int level = img->level + 1;
	int fd = -1;

	if (level > img->max_levels) {
		fprintf(stderr, "Error: too much levels %d\n", level);
		return -1;
	}

	fd = open(name, (rw ? O_RDWR : O_RDONLY)|O_DIRECT);
	if (fd < 0) {
		fprintf(stderr, "Can't open \"%s\": %m\n", name);
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
		if (memcmp(pvd->m_Sig, SIGNATURE_STRUCTURED_DISK_V1, sizeof(pvd->m_Sig))) {
			fprintf(stderr, "Image %s is v1 image; not supported\n", name);
		}
		else {
			fprintf(stderr, "Image %s doesn't look like a ploop delta file\n", name);
		}

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
			if (p_memalign(img->buf, clusterSize)) {
				goto err;
			}
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
	// BAT table size
	img->batSize = batSize;

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
		// first few BAT entries of the first block are reserved
		// for the image header so we need to skip it
		int i0 = (b == 0) ? HDR_SIZE_32 : 0;

		// fill in the maps
		u32 *bat = img->buf;
		for (u32 i = i0; i < clusterSize / 4; i++, idx++) {
			if (bat[i] == 0) {
				continue;
			}
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
	if (fd >= 0) {
		close(fd);
	}

	return -1;
}

static int close_deltas(struct plus_image *img)
{
	int l = img->level;
	do {
		close(img->fds[l]);
	} while (l-- >= 0);

	return 0;
}

static void mark_in_use(void *ptr, bool inuse)
{
	// Mark the image as either dirty or clean
	struct ploop_pvd_header *pvd = (struct ploop_pvd_header *)ptr;
	pvd->m_DiskInUse = inuse ? SIGNATURE_DISK_IN_USE : 0;
	// FIXME do we need msync?
	if (msync(ptr, PAGE_SIZE, MS_SYNC)) {
		fprintf(stderr, "%s: msync: %m\n", __func__);
	}
}

struct plus_image *plus_open(int count, char **deltas, int mode)
{
	// Allocate img
	struct plus_image *img = calloc(1, sizeof(struct plus_image));
	if (!img) {
		return NULL;
	}

	// Initialize it
	img->level = -1;
	img->mode = mode;
	img->max_levels = count;
	img->fds = calloc(count, sizeof(*img->fds));
	if (!img->fds) {
		goto err;
	}

	// initial buffer
	if (p_memalign(&img->buf, DEF_CLUSTER)) {
		goto err;
	}

	while (count--) {
		int rw = count == 0 && mode != O_RDONLY;
		if (open_delta(img, *deltas++, rw) < 0) {
			goto err;
		}
	}

	// mmap top delta BAT table for efficient writes
	if (mode != O_RDONLY) {
		int top_level = img->level;
		int wfd = img->fds[top_level];
		size_t len = img->batSize * img->clusterSize;
		const int prot = PROT_READ | PROT_WRITE;

		img->wbat = mmap(NULL, len, prot, MAP_SHARED, wfd, 0);
		if (img->wbat == MAP_FAILED) {
			fprintf(stderr, "mmap failed: %m\n");
			goto err;
		}
		// Mark the image as dirty
		mark_in_use(img->wbat, true);
	}

	img->max_idx = (img->batSize * img->clusterSize / 4) - HDR_SIZE_32;

	printf("Combined map follows:\n");
	for (u32 idx = 0; idx < img->bdevSize; idx++) {
		if (img->map_blk[idx]) {
			printf("%5u -> %2d,%5u\n", idx,
					img->map_lvl[idx],
					img->map_blk[idx]);
		}
	}
	printf("levels: %2d cluster: %5d bat: %5d (max idx: %5d) bdev: %5d alloc: %5d\n\n",
			img->max_levels, img->clusterSize, img->batSize,
			img->max_idx, img->bdevSize, img->allocSize);

	return img;

err:
	plus_close(img);
	return NULL;
}

int plus_close(struct plus_image *img)
{
	if (!img) {
		return 0;
	}

	if (img->mode != O_RDONLY) {
		// Mark the image as clean
		mark_in_use(img->wbat, false);
		// unmap the writeable BAT
		size_t len = img->batSize * img->clusterSize;
		if (munmap(img->wbat, len)) {
			fprintf(stderr, "%s: error in munmap: %m\n", __func__);
		}
	}

	free(img->buf);
	close_deltas(img);

	free(img->map_lvl);
	free(img->map_blk);

	free(img->fds);

	free(img);

	return 0;
}

#define MIN(a, b)	((a) < (b) ? (a) : (b))

// Sanity checks common for read and write
static inline int sanity_checks(const char *func,
		struct plus_image *img, size_t size, off_t offset, void *buf)
{
	if (!img) {
		return -EBADF;
	}

	// Is everything page-aligned?
	if (((size_t)buf % PAGE_SIZE) || (size % PAGE_SIZE) || (offset % PAGE_SIZE)) {
		fprintf(stderr, "%s: buf, size, or offset unaligned\n", func);
		return -EINVAL;
	}

	// Is it past EOF?
	u32 idx = (size + offset) / img->clusterSize;
	if (idx > img->bdevSize) {
		fprintf(stderr, "%s: offset=%zd size=%zd past EOF\n",
				func, size, offset);
		return -EINVAL;
	}

	// is is past BAT table? FIXME: can it ever happen?
	if (idx >= img->max_idx) {
		// TODO: implement BAT growing on writes?
		fprintf(stderr, "%s: offset=%zd size=%zd past BAT\n"
				"(TODO: implement BAT growing)\n",
				func, size, offset);
		return -E2BIG;
	}

	printf("%s offset=%5zd size=%5zd\n", func, offset, size);
	return 0;
}

static int read_block(int fd, void *buf, size_t len, off_t pos)
{
	printf("pread(%d, %p, %zd, %zu) = ", fd, buf, len, pos);
	ssize_t r = pread(fd, buf, len, pos);
	printf("%zd (%m)\n", r);
	if ((size_t)r == len) {
		return 0;
	}
	fprintf(stderr, "Error in pread(%d, %p, %zd, %zu) = %zd: %m\n",
			fd, buf, len, pos, r);
	if (r < 0) { // pread set errno
		return -errno;
	} else { // partial read, return EIO
		return -EIO;
	}
}

ssize_t plus_read(struct plus_image *img, size_t size, off_t offset, void *buf)
{
	int ret = sanity_checks(__func__, img, size, offset, buf);
	if (ret) {
		return ret;
	}

	u32 cluster = img->clusterSize;
	size_t got = 0; // How much we have read so far

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
			int ret = read_block(img->fds[lvl], buf + got, len, pos);
			if (ret) {
				return ret;
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

static int write_bat_entry(struct plus_image *img, u32 idx, u32 cluster)
{
	u32 *bat = (u32*)img->wbat + HDR_SIZE_32;
	if (bat[idx] != 0) {
		fprintf(stderr, "%s: unexpected BAT entry %d -> %d\n",
				__func__, idx, bat[idx]);
		return -1;
	}
	bat[idx] = cluster;

	return 0;
}

ssize_t plus_write(struct plus_image *img, size_t size, off_t offset, void *buf)
{
	int ret = sanity_checks(__func__, img, size, offset, buf);
	if (ret) {
		return ret;
	}
	if (img->mode == O_RDONLY) {
		return -EROFS;
	}

	u32 cluster = img->clusterSize;
	size_t got = 0; // How much have we wrote so far
	int top_level = img->level;
	int wfd = img->fds[top_level];
	u32 allocSize = img->allocSize;

	while (got < size) {
		// Cluster number, and offset within it
		u32 idx = offset / cluster; // cluster number
		u32 off = offset % cluster; // offset within the cluster
		u32 len = MIN(cluster - off, size - got); // how much to write

		int lvl = img->map_lvl[idx];
		int blk = img->map_blk[idx];
		if (blk && lvl == img->level) {
			// top level, existing block, proceed with rewrite
			printf("  W %5d -> %2d, %5d  off=%5d size=%5d\n",
					idx, lvl, blk, off, len);

			// offset in the delta file
			off_t pos = blk * cluster + off;
			printf("pwrite(%d, %p, %d, %zu) = ",
					wfd, buf + got, len, pos);
			ssize_t r = pwrite(wfd, buf + got, len, pos);
			printf("%zd (%m)\n", r);
			if (r != len) {
				fprintf(stderr, "%s: error in pwrite(%d, %p, %d, %zu) = %zd: %m\n",
						__func__, wfd, buf + got, len, pos, r);
				if (r < 0) { // pwrite set errno
					return -errno;
				} else { // wtf just happened? return EIO
					return -EIO;
				}
			}
		} else { // Allocate a new cluster
			void *wbuf = buf + got;

			// 1. Grow image size by one cluster
			allocSize++;
			if (ftruncate(wfd, allocSize * cluster)) {
				fprintf(stderr, "Error in ftruncate: %m\n");
				ret = -errno;
				goto err;
			}

			// 2. Prepare data to be written, note that since
			// this is a new cluster we need to write a whole one
			if (len < cluster) {
				// partial write, need to reconstruct a cluster
				wbuf = img->buf;
				if (blk) {
					// read the old data
					ret = read_block(img->fds[lvl], wbuf,
							cluster, blk * cluster);
					if (ret) {
						goto err;
					}
				} else {
					// just zero out the data
					memset(wbuf, 0, cluster);
				}

				// copy the new data in
				memcpy(wbuf + off, buf + got, len);
			}

			// 3. Write the cluster
			ssize_t r = pwrite(wfd, wbuf, cluster, allocSize * cluster);
			if (r != cluster) {
				fprintf(stderr, "Error in pwrite: %m\n");
				if (r < 0) {
					ret = -errno;
				} else {
					ret = -EIO;
				}
				goto err;
			}

			// FIXME: steps 4 and 5 need to be moved
			// to after writing all the data.

			// 4. Add a BAT entry to internal table
			img->map_lvl[idx] = top_level;
			img->map_blk[idx] = allocSize;

			// 5. Write the new BAT entry
			ret = write_bat_entry(img, idx, allocSize);
			if (ret) {
				goto err;
			}
		}
		got += len;
		offset += len;
	}

	// All data written successfully, need to write metadata
	if (allocSize > img->allocSize) {
		// Update the size
		img->allocSize = allocSize;
	}

	return got;
err:
	if (allocSize > img->allocSize) {
		// ftruncate back to old size
		if (ftruncate(wfd, img->allocSize * cluster)) {
			fprintf(stderr, "Error in ftruncate: %m\n");
			// we already have a (more serious) error,
			// so don't overwrite its code
		}
	}

	return ret;
}
