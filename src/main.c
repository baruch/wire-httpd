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
#include <stdarg.h>

#include "libwire/test/utils.h"

#ifdef NDEBUG
#define DEBUG(fmt, ...)
#else
#define DEBUG(fmt, ...) xlog(fmt, ## __VA_ARGS__)
#endif

#define WEB_POOL_SIZE 128

// DATA_BUF_SIZE is for the data to be read from the filesystem, leave a little
// space since the rounding up to 4K is the stack space for the rest of the
// wire data structures
#define DATA_BUF_SIZE 64*1024
#define WIRE_DATA_SIZE 8*1024

static wire_thread_t wire_thread_main;
static wire_t wire_accept;
static wire_pool_t web_pool;

struct web_data {
	int fd;
	wire_fd_state_t fd_state;
	char url[255];
};

static void xlog(const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	puts(msg);
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

static void error_generic(struct web_data *d, int code, const char *code_str, const char *body, int body_len)
{
	char buf[4096];
	int buf_len = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\nContent-Length: %u\r\nConnection:close\r\n\r\n",
			code, code_str, body_len);

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

static void send_file(int fd, off_t file_size, http_parser *parser, const char *filename) __attribute__((noinline));
static void send_file(int fd, off_t file_size, http_parser *parser, const char *filename)
{
	struct web_data *d = parser->data;
	int ret;
	char data[DATA_BUF_SIZE];
	int buf_len = snprintf(data, sizeof(data), "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n%s\r\n",
			(unsigned)file_size,
			!http_should_keep_alive(parser) ? "Connection: close\r\n" : "");
	if (buf_len > (int)sizeof(data)) {
		error_internal(d, STR_WITH_LEN("Failed to prepare header buffer"));
		return;
	}
	if (buf_write(&d->fd_state, data, buf_len) < 0)
		return;

	off_t offset = 0;

	while (offset < file_size) {
		off_t count = file_size - offset;
		if (count > DATA_BUF_SIZE)
			count = DATA_BUF_SIZE;
		ret = wio_pread(fd, data, count, offset);
		if (ret <= 0) {
			xlog("Error while reading file %s, ret=%d errno=%d: %m", filename, ret, errno);
			return;
		}
		offset += ret;

		if (buf_write(&d->fd_state, data, ret) < 0)
			return;
	}
}

static int on_message_complete(http_parser *parser)
{
	DEBUG("message complete");
	struct web_data *d = parser->data;
	int ret;
	const char *filename = d->url+1;

	int fd = wio_open(filename, O_RDONLY, 0);
	if (fd < 0) {
		error_not_found(d);
		return -1;
	}

	struct stat stbuf;
	ret = wio_fstat(fd, &stbuf);
	if (ret < 0) {
		error_internal(d, STR_WITH_LEN("Error getting info on file\n"));
		goto Exit;
	}

	send_file(fd, stbuf.st_size, parser, filename);

Exit:
	wio_close(fd);
	return -1;
}

static int on_url(http_parser *parser, const char *at, size_t length)
{
	UNUSED(parser);
	DEBUG("URL: %.*s", (int)length, at);
	struct web_data *d = parser->data;

	if (length > sizeof(d->url)) {
		xlog("Error while handling url, it's length is %u and the max length is %u", length, sizeof(d->url));
		// TODO: Instead of shutting down the connection, need to manage a way to report an error to the client
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

	wire_fd_mode_init(&d.fd_state, d.fd);

	set_nonblock(d.fd);

	http_parser_init(&parser, HTTP_REQUEST);
	parser.data = &d;

	char buf[4096];
	do {
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
				wire_fd_wait(&d.fd_state);
				DEBUG("Done waiting");
				continue;
			} else {
				DEBUG("Error receiving from socket %d: %m", d.fd);
				break;
			}
		}

		wire_fd_mode_none(&d.fd_state);

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
	wire_init(&wire_accept, "accept", accept_run, NULL, WIRE_STACK_ALLOC(4096));
	wire_thread_run();
	return 0;
}
