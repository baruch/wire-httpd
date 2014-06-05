#ifdef NDEBUG
#define DEBUG(fmt, ...)
#else
#define DEBUG(fmt, ...) xlog(fmt, ## __VA_ARGS__)
#endif

void xlog(const char *fmt, ...);
