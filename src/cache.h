#include <unistd.h>
#include <stdint.h>

void cache_init(void);
const char *cache_get(const char *filename, off_t *file_size, char *last_modified, int *fd, void **data);
void cache_release(void *data);
