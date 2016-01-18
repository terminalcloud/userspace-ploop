#ifndef _PLUS_H_
#define _PLUS_H_

#include <stdint.h>

typedef uint64_t	u64;
typedef uint32_t	u32;
typedef uint16_t	u16;
typedef uint8_t		u8;

struct plus_image {
	int level;	// current level
	int max_levels;	// total levels
	int mode;	// O_RDONLY, O_WRONLY, or O_RDWR
	u32 clusterSize;// in bytes

	// This is for top (last opened) image
	u32 batSize;	// size of BAT maps, in cluster blocks
	u32 bdevSize;	// size of block device, in cluster blocks
	u32 allocSize;	// size of allocated image file
	void *wbat;	// mmap()'ed metadata (for writing)

	// per-cluster_block mappings, indexed by cluster number
	u8  *map_lvl;	// block -> level mapping
	u32 *map_blk;	// block -> block mapping

	// per-level arrays, size is max_levels
	int *fds;	// opened delta file descriptors

	void *buf;	// page-aligned cluster size buffer
};

struct plus_image *plus_open(int count, char **deltas, int mode);
int plus_close(struct plus_image *img);
ssize_t plus_read(struct plus_image *img, size_t size, off_t offset, void *buf);
ssize_t plus_write(struct plus_image *img, size_t size, off_t offset, void *buf);

#endif // _PLUS_H_
