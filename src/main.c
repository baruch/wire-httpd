#include "cache.h"
#include "xlog.h"

#include "wire.h"
#include "wire_fd.h"
#include "wire_pool.h"
#include "wire_stack.h"
#include "wire_io.h"
#include "macros.h"
#include "http_parser.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdbool.h>
#include <sys/timerfd.h>

#include "libwire/test/utils.h"
#include "gperf.h"

#define WEB_POOL_SIZE 128

// DATA_BUF_SIZE is for the data to be read from the filesystem, leave a little
// space since the rounding up to 4K is the stack space for the rest of the
// wire data structures
#define DATA_BUF_SIZE 64*1024
#define WIRE_DATA_SIZE 16*1024

static wire_thread_t wire_thread_main;
static wire_t wire_accept;
static wire_pool_t web_pool;

struct web_data {
	int fd;
	bool should_close;
	wire_fd_state_t fd_state;
	char url[255];
};

typedef struct wire_timer {
	int timerfd;
	wire_fd_state_t fd_state;
} wire_timer_t;

static bool timer_start(wire_timer_t *timer, int msecs)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
	if (fd < 0) {
		perror("Failed to create a timerfd");
		return false;
	}

	struct itimerspec timer_val = {
		.it_value = { .tv_sec = msecs / 1000, .tv_nsec = (msecs % 1000) * 1000000}
	};

	int ret = timerfd_settime(fd, 0, &timer_val, NULL);
	if (ret < 0) {
		perror("Failed to set time on timerfd");
		close(fd);
		return false;
	}

	timer->timerfd = fd;

	wire_fd_mode_init(&timer->fd_state, fd);
	wire_fd_mode_read(&timer->fd_state);

	return true;
}

static void timer_stop(wire_timer_t *timer)
{
	wire_fd_mode_none(&timer->fd_state);
	close(timer->timerfd);
}

static bool timer_triggered(wire_timer_t *timer)
{
	return timer->fd_state.wait.triggered;
}

static void timer_list_chain(wire_timer_t *timer, wire_wait_list_t *list)
{
	wire_fd_wait_list_chain(list, &timer->fd_state);
}

static int buf_write(wire_fd_state_t *fd_state, const char *buf, int len)
{
	int sent = 0;
	do {
		int ret = write(fd_state->fd, buf + sent, len - sent);
		if (ret == 0)
			return -1;
		else if (ret > 0) {
			sent += ret;
			if (sent == len)
				return 0;
		} else {
			// Error
			if (errno == EINTR || errno == EAGAIN) {
				wire_fd_mode_write(fd_state);
				wire_fd_wait(fd_state);
				wire_fd_mode_none(fd_state);
			} else {
				xlog("Error while writing into socket %d: %m", fd_state->fd);
				return -1;
			}
		}
	} while (1);
}

static const char *content_type_from_filename(const char *filename)
{
	const char *last_dot = NULL;
	int len = 0;

	for (; *filename; filename++, len++) {
		switch (*filename) {
			case '.': last_dot = filename; len = 0; break;
			case '/': last_dot = NULL; break;
		}
	}

	if (last_dot == NULL)
		return "text/plain";

	const char *suffix = mime_from_suffix_name(last_dot+1, len-1);
	if (suffix)
		return suffix;
	return "application/binary";
}

static void error_generic(struct web_data *d, int code, const char *code_str, const char *body, int body_len) __attribute__((noinline));
static void error_generic(struct web_data *d, int code, const char *code_str, const char *body, int body_len)
{
	char buf[4096];
	int buf_len = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %u\r\nConnection:close\r\n\r\n",
			code, code_str, body_len);

	d->should_close = true;

	if (buf_write(&d->fd_state, buf, buf_len) < 0)
		return;

	if (body_len > 0)
		buf_write(&d->fd_state, body, body_len);
}

#define STR_WITH_LEN(s) s, strlen(s)

static void error_not_found(struct web_data *d) __attribute__((noinline));
static void error_not_found(struct web_data *d)
{
	error_generic(d, 404, "Not Found", STR_WITH_LEN("File not found\n"));
}

static void error_internal(struct web_data *d, const char *msg, int msg_len) __attribute__((noinline));
static void error_internal(struct web_data *d, const char *msg, int msg_len)
{
	error_generic(d, 500, "Internal Failure", msg, msg_len);
}

static void error_invalid(struct web_data *d)
{
	error_generic(d, 405, "Internal Method", STR_WITH_LEN("Invalid method used"));
}

static bool send_header(http_parser *parser, const char *filename, off_t file_size) __attribute__((noinline));
static bool send_header(http_parser *parser, const char *filename, off_t file_size)
{
	char data[2048];
	struct web_data *d = parser->data;
	int buf_len;

	int http_major = 1;
	int http_minor = 1;

	if (http_major > parser->http_major) {
		http_major = parser->http_major;
		http_minor = parser->http_minor;
	} else if (http_major == parser->http_major && http_minor > parser->http_minor) {
		http_minor = parser->http_minor;
	}

	buf_len = snprintf(data, sizeof(data), "HTTP/%d.%d 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: max_age=3600\r\n%s\r\n",
			http_major, http_minor,
			content_type_from_filename(filename),
			(unsigned)file_size,
			!http_should_keep_alive(parser) ? "Connection: close\r\n" : "");
	if (buf_len > (int)sizeof(data)) {
		error_internal(d, STR_WITH_LEN("Failed to prepare header buffer"));
		return false;
	}
	if (buf_write(&d->fd_state, data, buf_len) < 0)
		return false;
	return true;
}

static void send_file(int fd, off_t file_size, http_parser *parser, const char *filename, bool only_head) __attribute__((noinline));
static void send_file(int fd, off_t file_size, http_parser *parser, const char *filename, bool only_head)
{
	struct web_data *d = parser->data;
	char data[DATA_BUF_SIZE];

	if (!send_header(parser, filename, file_size))
		return;

	if (only_head)
		return;

	off_t offset = 0;

	while (offset < file_size) {
		off_t count = file_size - offset;
		if (count > DATA_BUF_SIZE)
			count = DATA_BUF_SIZE;
		int ret = wio_pread(fd, data, count, offset);
		if (ret <= 0) {
			xlog("Error while reading file %s, ret=%d errno=%d: %m", filename, ret, errno);
			return;
		}
		offset += ret;

		if (buf_write(&d->fd_state, data, ret) < 0)
			return;
	}
}

static void send_cached_file(http_parser *parser, const char *filename, const char *buf, off_t buf_len, bool only_head) __attribute__((noinline));
static void send_cached_file(http_parser *parser, const char *filename, const char *buf, off_t buf_len, bool only_head)
{
	struct web_data *d = parser->data;

	if (!send_header(parser, filename, buf_len))
		return;

	if (only_head)
		return;

	if (buf_write(&d->fd_state, buf, buf_len) < 0)
		return;
}

static int on_message_complete(http_parser *parser)
{
	DEBUG("message complete");
	struct web_data *d = parser->data;
	const char *filename = d->url+1;
	off_t buf_len;
	void *release_data;
	int fd;

	if (parser->method != HTTP_GET && parser->method != HTTP_HEAD) {
		error_invalid(d);
		return -1;
	}

	bool only_head = parser->method == HTTP_HEAD;

	const char *buf = cache_get(filename, &buf_len, &fd, &release_data);

	if (buf) {
		// File in cache, send from buffer
		send_cached_file(parser, filename, buf, buf_len, only_head);
		cache_release(release_data);
	} else if (fd >= 0){
		// No space in cache or file too large, need to send it directly, it's already open
		send_file(fd, buf_len, parser, filename, only_head);
		wio_close(fd);
	} else {
		// File is missing or some other error when opening/reading
		switch (fd) {
			case -2: error_not_found(d); break;
			case -3: error_internal(d, STR_WITH_LEN("Error getting info on file\n")); break;
			default: error_internal(d, STR_WITH_LEN("Unknown internal error\n")); break;
		}
	}

	if (!http_should_keep_alive(parser))
		d->should_close = true;

	return -1;
}

static int on_url(http_parser *parser, const char *at, size_t length)
{
	UNUSED(parser);
	DEBUG("URL: %.*s", (int)length, at);
	struct web_data *d = parser->data;

	if (length > sizeof(d->url)) {
		xlog("Error while handling url, it's length is %u and the max length is %u", length, sizeof(d->url));
		error_internal(d, STR_WITH_LEN("url too long\n"));
		return -1;
	}

	memcpy(d->url, at, length);
	d->url[length] = 0;

	return 0;
}

static const struct http_parser_settings parser_settings = {
	.on_message_complete = on_message_complete,
	.on_url = on_url,
};

static void web_run(void *arg)
{
	struct web_data d = {
		.fd = (long int)arg,
	};
	http_parser parser;
	wire_timer_t timer;

	wire_fd_mode_init(&d.fd_state, d.fd);

	set_nonblock(d.fd);

	http_parser_init(&parser, HTTP_REQUEST);
	parser.data = &d;

	char buf[4096];
	bool bail_out = false;
	bool timer_stopped = true;
	do {
		if (timer_stopped) {
			timer_start(&timer, 10*1000);
			timer_stopped = false;
		}
		buf[0] = 0;
		int received = read(d.fd, buf, sizeof(buf));
		DEBUG("Received: %d %d", received, errno);
		if (received == 0) {
			/* Fall-through, tell parser about EOF */
			DEBUG("Received EOF");
		} else if (received < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				DEBUG("Waiting");
				/* Nothing received yet, wait for it */
				wire_fd_mode_read(&d.fd_state);

				wire_wait_list_t wait_list;
				wire_wait_list_init(&wait_list);
				wire_fd_wait_list_chain(&wait_list, &d.fd_state);
				timer_list_chain(&timer, &wait_list);
				wire_list_wait(&wait_list);

				DEBUG("Done waiting");
				if (!timer_triggered(&timer))
					continue;
			} else {
				DEBUG("Error receiving from socket %d: %m", d.fd);
				bail_out = true;
			}
		}

		timer_stop(&timer);
		timer_stopped = true;
		wire_fd_mode_none(&d.fd_state);

		if (bail_out)
			break;

		DEBUG("Processing %d", (int)received);
		size_t processed = http_parser_execute(&parser, &parser_settings, buf, received);
		if (parser.upgrade) {
			/* Upgrade not supported yet */
			xlog("Upgrade no supported, bailing out");
			break;
		} else if (received == 0) {
			// At EOF, exit now
			DEBUG("Received EOF");
			break;
		} else if (processed != (size_t)received) {
			// Error in parsing
			xlog("Not everything was parsed, error is likely, bailing out.");
			break;
		} else if (d.should_close) {
			DEBUG("Closing as requested");
			break;
		}
	} while (1);

	close(d.fd);
	DEBUG("Disconnected %d", d.fd);
}

static void accept_run(void *arg)
{
	UNUSED(arg);
	int port = 9090;
	int fd = socket_setup(port);
	if (fd < 0)
		return;

	xlog("Listening on port %d", port);

	wire_fd_state_t fd_state;
	wire_fd_mode_init(&fd_state, fd);
	wire_fd_mode_read(&fd_state);

	/* To be as fast as possible we want to accept all pending connections
	 * without waiting in between, the throttling will happen by either there
	 * being no more pending listeners to accept or by the wire pool blocking
	 * when it is exhausted.
	 */
	while (1) {
		int new_fd = accept(fd, NULL, NULL);
		if (new_fd >= 0) {
			DEBUG("New connection: %d", new_fd);
			char name[32];
			snprintf(name, sizeof(name), "web %d", new_fd);
			wire_t *task = wire_pool_alloc_block(&web_pool, name, web_run, (void*)(long int)new_fd);
			if (!task) {
				xlog("Web server is busy, sorry");
				close(new_fd);
			}
		} else {
			if (errno == EINTR || errno == EAGAIN) {
				/* Wait for the next connection */
				wire_fd_wait(&fd_state);
			} else {
				xlog("Error accepting from listening socket: %m");
				break;
			}
		}
	}
}

int main()
{
	wire_thread_init(&wire_thread_main);
	wire_stack_fault_detector_install();
	wire_fd_init();
	wire_io_init(32);
	wire_pool_init(&web_pool, NULL, WEB_POOL_SIZE, DATA_BUF_SIZE + WIRE_DATA_SIZE);
	cache_init();
	wire_init(&wire_accept, "accept", accept_run, NULL, WIRE_STACK_ALLOC(4096));
	wire_thread_run();
	return 0;
}
