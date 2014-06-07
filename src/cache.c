#include "cache.h"
#include "xlog.h"

#include "wire_io.h"
#include "wire_wait.h"
#include "wire_fd.h"
#include "wire_stack.h"

#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <assert.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#define CACHE_SIZE 256
#define SPARE_BUFFERS 64
#define NUM_BUFFERS (CACHE_SIZE + SPARE_BUFFERS)
#define BUFFER_SIZE 1024*1024

struct buf_item {
	int ref_cnt;
	char *buf;
};

struct cache_item {
	unsigned refresh_counter;
	char filename[255];
	struct stat stbuf;
	struct buf_item *buf;
	struct list_head wakeup_list;
};

struct wakeup_list {
	struct list_head list;
	wire_wait_t wait;
};

/* To try and get all the files in a consistent check the freshness check is
 * triggered for all files together, this will make any staleness differences
 * minimal to the time it will take to reload all files.
 */
static unsigned refresh_counter;
static struct cache_item cache[CACHE_SIZE];
static struct buf_item buffers[NUM_BUFFERS];
static int max_cache_items;
static wire_t refresh_wire;

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

static void free_buf(struct buf_item *buf)
{
	if (buf)
		buf->ref_cnt--;
}

static bool cache_item_unused(struct cache_item *item)
{
	return item && item->filename[0];
}

static void free_cache_item(struct cache_item *item)
{
	memset(item, 0, sizeof(*item));
}

static struct cache_item *_cache_item_alloc(void)
{
	int i;
	for (i = 0; i < max_cache_items; i++) {
		struct cache_item *item = &cache[i];
		if (cache_item_unused(item))
			return item;
	}

	if (max_cache_items < CACHE_SIZE) {
		struct cache_item *item = &cache[max_cache_items];
		max_cache_items++;
		return item;
	}

	return NULL;
}

static struct cache_item *cache_item_alloc(const char *filename)
{
	struct cache_item *item = _cache_item_alloc();
	if (!item)
		return NULL;

	memset(item, 0, sizeof(*item));
	strcpy(item->filename, filename);
	list_head_init(&item->wakeup_list);
	item->refresh_counter = refresh_counter-1;
	return item;
}

static int open_file(const char *filename, struct stat *stbuf)
{
	int fd = wio_open(filename, O_RDONLY, 0);
	if (fd < 0) {
		// File not found
		DEBUG("Failed to open file %s: %m", filename);
		return -2;
	}

	int ret = wio_fstat(fd, stbuf);
	if (ret < 0) {
		DEBUG("Failed to fstat file %s: %m", filename);
		wio_close(fd);
		return -3;
	}

	return fd;
}

static bool stbuf_eq(struct stat *b1, struct stat *b2)
{
	return b1->st_dev == b2->st_dev &&
		   b1->st_ino == b2->st_ino &&
		   b1->st_size == b2->st_size &&
		   b1->st_mtime == b2->st_mtime &&
		   b1->st_ctime == b2->st_ctime;
}

static bool cache_load(struct cache_item *item, struct buf_item *old_buf, off_t *file_size, int *pfd)
{
	struct stat stbuf;
	int fd = open_file(item->filename, &stbuf);

	*pfd = fd;

	if (fd < 0)
		return false;

	*file_size = stbuf.st_size;

	if (stbuf.st_size > BUFFER_SIZE) {
		DEBUG("File %s too large (%u)", item->filename, stbuf.st_size);
		return false;
	}

	// The file wasn't changed, don't waste time loading the new content
	if (old_buf && stbuf_eq(&stbuf, &item->stbuf)) {
		DEBUG("No need to reload data, nothing changed in file %s", item->filename);
		item->buf = old_buf;
		return true;
	}

	// Insert into cache
	item->stbuf = stbuf;
	struct buf_item *buf;
	if (old_buf && old_buf->ref_cnt == 1) {
		// Reuse old buf
		DEBUG("Reuse old buf as it is not being served currently");
		buf = old_buf;
	} else {
		DEBUG("New buf allocated in place of old one");
		free_buf(old_buf);
		buf = alloc_buf();
	}
	int ret = wio_pread(fd, buf->buf, stbuf.st_size, 0);

	if (ret < stbuf.st_size) {
		xlog("Failed to read file %s, expected to read %u got %d: %m", item->filename, stbuf.st_size, ret);
		free_buf(buf);
		return false;
	}

	// Load succeeded, give the buffer
	DEBUG("File successfully loaded %s", item->filename);
	item->refresh_counter = refresh_counter;
	item->buf = buf;
	return true;
}

static struct cache_item *cache_find(const char *filename)
{
	int i;
	// TODO: Improve this O(n) algorithm
	for (i = 0; i < max_cache_items; i++) {
		struct cache_item *item = &cache[i];
		if (strcmp(filename, item->filename) == 0) {
			return item;
		}
	}

	return NULL;
}

const char *cache_get(const char *filename, off_t *file_size, uint32_t *last_modified, int *pfd, void **data)
{
	struct cache_item *item = cache_find(filename);
	if (!item)
		item = cache_item_alloc(filename);

	if (!item) {
		// No place in cache for this file
		struct stat stbuf;
		*pfd = open_file(filename, &stbuf);
		if (*pfd >= 0) {
			*file_size = stbuf.st_size;
			*last_modified = stbuf.st_mtime;
		}
		return NULL;
	}

	if (item->refresh_counter != refresh_counter) {
		// Need to refresh the buffer, everyone else should wait as well
		xlog("Trying to reload file %s", filename);
		struct buf_item *old_buf = item->buf;
		item->buf = NULL;
		item->refresh_counter = refresh_counter;

		// Refresh content
		bool loaded = cache_load(item, old_buf, file_size, pfd);

		// Wakeup the waiters
		struct list_head *head;
		while ( (head = list_head(&item->wakeup_list)) != NULL )
		{
			struct wakeup_list *wake = list_entry(head, struct wakeup_list, list);
			wire_wait_resume(&wake->wait);
			list_del(head);
		}

		if (!loaded) {
			free_cache_item(item);
			return NULL;
		}

		// Cache was loaded, close the fd
		if (*pfd >= 0) {
			wio_close(*pfd);
			*pfd = -1;
		}

		assert(item->buf);
	}

	if (item->buf == NULL) {
		// Cache item is still loading, need to wait
		struct wakeup_list wakeup;
		wire_wait_init(&wakeup.wait);
		list_add_tail(&wakeup.list, &item->wakeup_list);
		wire_wait_single(&wakeup.wait);
		if (item->buf == NULL) {
			*pfd = -1;
			// Load failed, tell the caller to load it itself and report the error
			return NULL;
		}
	}

	// Cache hit
	*file_size = item->stbuf.st_size;
	*last_modified = item->stbuf.st_mtime;
	*data = item->buf;
	item->buf->ref_cnt++;
	return item->buf->buf;
}

void cache_release(void *data)
{
	struct buf_item *buf = data;
	buf->ref_cnt--;
}

static int timer_setup(void)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
	if (fd < 0) {
		perror("Failed to create a timerfd");
		return -1;
	}

	struct itimerspec timer;
	timer.it_value.tv_sec = timer.it_interval.tv_sec = 30;
	timer.it_value.tv_nsec = timer.it_interval.tv_nsec = 0;

	int ret = timerfd_settime(fd, 0, &timer, NULL);
	if (ret < 0) {
		xlog("Error while setting the timerfd: %m");
		close(fd);
		return -1;
	}

	return fd;
}

static int signal_setup(void)
{
	sigset_t sig_set;

	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGUSR1);
	sigaddset(&sig_set, SIGUSR2);

	int ret = pthread_sigmask(SIG_BLOCK, &sig_set, NULL);
	if (ret < 0)
		xlog("Failed to block signals: %m");

	int fd = signalfd(-1, &sig_set, SFD_NONBLOCK|SFD_CLOEXEC);
	if (fd < 0)
		xlog("Failed to create a signalfd: %m");

	return fd;
}

static void cache_refresh_timer(void *arg)
{
	(void)arg;

	int tfd = timer_setup();
	int sfd = signal_setup();
	if (tfd < 0 || sfd < 0) {
		xlog("Failed to start the cache refresh timer");
		return;
	}

	wire_fd_state_t tfd_state;
	wire_fd_mode_init(&tfd_state, tfd);
	wire_fd_mode_read(&tfd_state);

	wire_fd_state_t sfd_state;
	wire_fd_mode_init(&sfd_state, sfd);
	wire_fd_mode_read(&sfd_state);

	wire_wait_list_t wait_list;
	wire_wait_list_init(&wait_list);
	wire_fd_wait_list_chain(&wait_list, &tfd_state);
	wire_fd_wait_list_chain(&wait_list, &sfd_state);

	while (1) {
		wire_list_wait(&wait_list);

		if (tfd_state.wait.triggered) {
			wire_wait_reset(&tfd_state.wait);

			uint64_t timer_val = 0;
			int ret = read(tfd, &timer_val, sizeof(timer_val));
			if (ret < 0) {
				if (errno != EAGAIN) {
					perror("Error reading from timerfd");
					break;
				}
			} else {
				refresh_counter++;
			}
		}

		if (sfd_state.wait.triggered) {
			wire_wait_reset(&sfd_state.wait);

			struct signalfd_siginfo siginfo;
			int ret = read(sfd, &siginfo, sizeof(siginfo));
			if (ret < 0) {
				if (errno != EAGAIN) {
					xlog("Error reading from signalfd: %m");
					break;
				}
			} else {
				xlog("Refresh counter increased by signal");
				refresh_counter++;
			}
		}
	}

	wire_fd_mode_none(&tfd_state);
	wio_close(tfd);

	wire_fd_mode_none(&sfd_state);
	wio_close(sfd);
	xlog("Cache refresh timer exited");
}

void cache_init(void)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++) {
		struct buf_item *buf = &buffers[i];
		buf->buf = mmap(NULL, BUFFER_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	}

	wire_init(&refresh_wire, "cache refresh timer", cache_refresh_timer, NULL, WIRE_STACK_ALLOC(4096));
}
