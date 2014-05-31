#include "cache.h"

#include "wire_io.h"

const char *cache_get(const char *filename, off_t *file_size)
{
	(void)filename;
	(void)file_size;
	return NULL;
}

void cache_release(const char *filename)
{
	(void)filename;
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

	*file_size = stbuf.st_size;
	*pfd = fd;
	return NULL;
}

void cache_init(void)
{
}
