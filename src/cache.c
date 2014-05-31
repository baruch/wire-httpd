#include "cache.h"

#include "wire_io.h"

#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>

#define CACHE_SIZE 256
#define SPARE_BUFFERS 64
#define NUM_BUFFERS (CACHE_SIZE + SPARE_BUFFERS)
#define BUFFER_SIZE 1024*1024

struct buf_item {
	int ref_cnt;
	char *buf;
};

struct cache_item {
	char filename[255];
	struct stat stbuf;
	struct buf_item *buf;
};

static struct cache_item cache[CACHE_SIZE];
static struct buf_item buffers[NUM_BUFFERS];
static int last_cache_item;

const char *cache_get(const char *filename, off_t *file_size, void **data)
{
	int i;

	// TODO: Improve this O(n) algorithm
	for (i = 0; i < last_cache_item; i++) {
		struct cache_item *item = &cache[i];
		if (strcmp(filename, item->filename) == 0) {
			// Cache hit
			*file_size = item->stbuf.st_size;
			*data = item->buf;
			item->buf->ref_cnt++;
			return item->buf->buf;
		}
	}

	return NULL;
}

void cache_release(void *data)
{
	struct buf_item *buf = data;
	buf->ref_cnt--;
}

const char *cache_insert(const char *filename, off_t *file_size, int *pfd)
{
	int fd = wio_open(filename, O_RDONLY, 0);
	if (fd < 0) {
		// File not found
		*pfd = -2;
		return NULL;
	}

	struct stat stbuf;
	int ret = wio_fstat(fd, &stbuf);
	if (ret < 0) {
		wio_close(fd);
		*pfd = -3;
		return NULL;
	}

	if (stbuf.st_size <= BUFFER_SIZE) {
		// TODO: Add into cache
	}

	*file_size = stbuf.st_size;
	*pfd = fd;
	return NULL;
}

void cache_init(void)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++) {
		struct buf_item *buf = &buffers[i];
		buf->buf = mmap(NULL, BUFFER_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	}
}
