#include "cache.h"
#include "xlog.h"

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
	bool first_load;
};

static struct cache_item cache[CACHE_SIZE];
static struct buf_item buffers[NUM_BUFFERS];
static int num_cache_items;

static struct buf_item *alloc_buf(void)
{
	int i;
	for (i = 0; i < NUM_BUFFERS; i++) {
		struct buf_item *buf = &buffers[i];
		if (buf->ref_cnt == 0) {
			buf->ref_cnt++;
			return buf;
		}
	}

	return NULL;
}

const char *cache_get(const char *filename, off_t *file_size, void **data)
{
	int i;

	// TODO: Improve this O(n) algorithm
	for (i = 0; i < num_cache_items; i++) {
		struct cache_item *item = &cache[i];
		if (!item->first_load && strcmp(filename, item->filename) == 0) {
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

const char *cache_insert(const char *filename, off_t *file_size, int *pfd, void **data)
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

	*file_size = stbuf.st_size;

	if (stbuf.st_size <= BUFFER_SIZE && num_cache_items < CACHE_SIZE) {
		// Insert into cache
		struct cache_item *item = &cache[num_cache_items++];
		strcpy(item->filename, filename);
		item->stbuf = stbuf;
		item->buf = alloc_buf();
		item->first_load = true;

		int ret = wio_pread(fd, item->buf->buf, stbuf.st_size, 0);
		item->first_load = false;
		if (ret < stbuf.st_size) {
			xlog("Failed to read file %s, expected to read %u got %d: %m", filename, stbuf.st_size, ret);
			num_cache_items--;
		} else {
			// Load succeeded, give the buffer
			item->buf->ref_cnt++;
			*data = item->buf;
			return item->buf->buf;
		}
	}

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
