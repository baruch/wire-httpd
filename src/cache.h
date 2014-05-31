#include <unistd.h>

void cache_init(void);
const char *cache_get(const char *filename, off_t *file_size);
void cache_release(const char *filename);
const char *cache_insert(const char *filename, off_t *file_size, int *fd);
