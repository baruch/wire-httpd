#include <unistd.h>
#include <stdint.h>

void cache_init(void);
const char *cache_get(const char *filename, off_t *file_size, uint32_t *last_modified, int *fd, void **data);
void cache_release(void *data);
