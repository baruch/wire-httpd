#include "xlog.h"

#include <stdarg.h>
#include <stdio.h>

void xlog(const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	puts(msg);
}
