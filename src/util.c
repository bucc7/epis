/* util.c - buffers, strings, files, encodings */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/rand.h>

#include "epis.h"

/* abort the program with a formatted message */
void die(const char *fmt, ...)
{
	va_list ap;

	fputs("epis: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* print a formatted warning */
void warnx(const char *fmt, ...)
{
	va_list ap;

	fputs("epis: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xcalloc(size_t n, size_t m)
{
	void *p = calloc(n ? n : 1, m ? m : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

/* allocate a formatted string */
char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	char *p;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	p = xmalloc(n + 1);
	va_start(ap, fmt);
	vsnprintf(p, n + 1, fmt, ap);
	va_end(ap);
	return p;
}

void buf_add(struct buf *b, const void *p, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		b->p = xrealloc(b->p, b->cap);
	}
	memcpy(b->p + b->len, p, n);
	b->len += n;
	b->p[b->len] = 0;
}

void buf_puts(struct buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

void buf_putc(struct buf *b, int c)
{
	char ch = c;

	buf_add(b, &ch, 1);
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	buf_add(b, "", 0);
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		b->p = xrealloc(b->p, b->cap);
	}
	va_start(ap, fmt);
	vsnprintf(b->p + b->len, n + 1, fmt, ap);
	va_end(ap);
	b->len += n;
}

void buf_reset(struct buf *b)
{
	b->len = 0;
	if (b->p)
		b->p[0] = 0;
}

void buf_free(struct buf *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

/* return the buffer contents as an owned string and reset the buffer */
char *buf_take(struct buf *b)
{
	char *p;

	buf_add(b, "", 0);
	p = b->p;
	b->p = NULL;
	b->len = b->cap = 0;
	return p;
}

/* strip leading and trailing whitespace in place */
char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

int startswith(const char *s, const char *pre)
{
	return strncmp(s, pre, strlen(pre)) == 0;
}

int strcaseprefix(const char *s, const char *pre)
{
	return strncasecmp(s, pre, strlen(pre)) == 0;
}

/* case insensitive substring search */
const char *strcasestr_(const char *h, const char *n)
{
	size_t l = strlen(n);

	for (; *h; h++)
		if (strncasecmp(h, n, l) == 0)
			return h;
	return NULL;
}

/* expand a leading ~ to $HOME */
char *expand_home(const char *path)
{
	const char *home = getenv("HOME");

	if (path[0] == '~' && (path[1] == '/' || !path[1]) && home)
		return xasprintf("%s%s", home, path + 1);
	return xstrdup(path);
}

/* create a directory and all its parents */
int mkdirs(const char *path, int mode)
{
	char *p = xstrdup(path), *s;
	int r = 0;

	for (s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = 0;
		if (mkdir(p, mode) && errno != EEXIST)
			r = -1;
		*s = '/';
	}
	if (mkdir(p, mode) && errno != EEXIST)
		r = -1;
	free(p);
	return r;
}

/* read a whole file into memory */
char *read_file(const char *path, size_t *len)
{
	struct buf b = { 0 };
	char tmp[65536];
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return NULL;
	while ((n = read(fd, tmp, sizeof(tmp))) > 0)
		buf_add(&b, tmp, n);
	close(fd);
	buf_add(&b, "", 0);
	if (len)
		*len = b.len;
	return b.p;
}

/* atomically write a file with the given permissions */
int write_file(const char *path, const char *data, size_t len, int mode)
{
	char *tmp = xasprintf("%s.tmp%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	int r = -1;

	if (fd >= 0) {
		if (write(fd, data, len) == (ssize_t)len && !fsync(fd))
			r = 0;
		close(fd);
		if (r == 0)
			r = rename(tmp, path);
		if (r)
			unlink(tmp);
	}
	free(tmp);
	return r;
}

/* run a shell command and return its trimmed standard output */
char *run_cmd(const char *cmd)
{
	struct buf b = { 0 };
	char tmp[4096];
	size_t n;
	FILE *f = popen(cmd, "r");

	if (!f)
		return NULL;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
		buf_add(&b, tmp, n);
	if (pclose(f) != 0) {
		buf_free(&b);
		return NULL;
	}
	buf_add(&b, "", 0);
	while (b.len && (b.p[b.len - 1] == '\n' || b.p[b.len - 1] == '\r'))
		b.p[--b.len] = 0;
	return b.p;
}

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_encode(struct buf *out, const void *in, size_t n)
{
	const unsigned char *p = in;
	char q[4];
	size_t i;

	for (i = 0; i < n; i += 3) {
		unsigned v = p[i] << 16;

		if (i + 1 < n)
			v |= p[i + 1] << 8;
		if (i + 2 < n)
			v |= p[i + 2];
		q[0] = b64[v >> 18 & 63];
		q[1] = b64[v >> 12 & 63];
		q[2] = i + 1 < n ? b64[v >> 6 & 63] : '=';
		q[3] = i + 2 < n ? b64[v & 63] : '=';
		buf_add(out, q, 4);
	}
	buf_add(out, "", 0);
}

/* decode base64, skipping whitespace and invalid characters */
int b64_decode(struct buf *out, const char *in, size_t n)
{
	unsigned v = 0;
	int bits = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		const char *c = memchr(b64, in[i], 64);

		if (in[i] == '=' || !in[i])
			break;
		if (!c)
			continue;
		v = v << 6 | (unsigned)(c - b64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			buf_putc(out, (v >> bits) & 0xff);
		}
	}
	buf_add(out, "", 0);
	return 0;
}

void url_encode(struct buf *out, const char *s)
{
	for (; *s; s++) {
		unsigned char c = *s;

		if (isalnum(c) || strchr("-._~", c))
			buf_putc(out, c);
		else
			buf_printf(out, "%%%02X", c);
	}
}

/* fill out with 2*nbytes random hex characters */
void random_hex(char *out, size_t nbytes)
{
	unsigned char r[64];
	size_t i;

	if (nbytes > sizeof(r))
		nbytes = sizeof(r);
	RAND_bytes(r, nbytes);
	for (i = 0; i < nbytes; i++)
		sprintf(out + 2 * i, "%02x", r[i]);
}

/* decode one UTF-8 sequence, returns its byte length */
int utf8_decode(const char *s, uint32_t *cp)
{
	const unsigned char *u = (const unsigned char *)s;

	if (u[0] < 0x80) {
		*cp = u[0];
		return 1;
	}
	if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
		*cp = (u[0] & 0x1f) << 6 | (u[1] & 0x3f);
		return 2;
	}
	if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 &&
	    (u[2] & 0xc0) == 0x80) {
		*cp = (u[0] & 0x0f) << 12 | (u[1] & 0x3f) << 6 | (u[2] & 0x3f);
		return 3;
	}
	if ((u[0] & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80 &&
	    (u[2] & 0xc0) == 0x80 && (u[3] & 0xc0) == 0x80) {
		*cp = (u[0] & 0x07) << 18 | (u[1] & 0x3f) << 12 |
		    (u[2] & 0x3f) << 6 | (u[3] & 0x3f);
		return 4;
	}
	*cp = 0xfffd;
	return 1;
}

/* encode a code point as UTF-8, returns the byte length */
int utf8_encode(char *o, uint32_t cp)
{
	if (cp < 0x80) {
		o[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = 0xc0 | cp >> 6;
		o[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = 0xe0 | cp >> 12;
		o[1] = 0x80 | (cp >> 6 & 0x3f);
		o[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	o[0] = 0xf0 | cp >> 18;
	o[1] = 0x80 | (cp >> 12 & 0x3f);
	o[2] = 0x80 | (cp >> 6 & 0x3f);
	o[3] = 0x80 | (cp & 0x3f);
	return 4;
}

/* return $XDG_DATA_HOME/epis, creating it */
char *data_dir(void)
{
	const char *x = getenv("XDG_DATA_HOME");
	char *d = x && *x ? xasprintf("%s/epis", x) :
	    expand_home("~/.local/share/epis");

	mkdirs(d, 0700);
	return d;
}

/* return $XDG_CONFIG_HOME/epis */
char *config_dir(void)
{
	const char *x = getenv("XDG_CONFIG_HOME");

	return x && *x ? xasprintf("%s/epis", x) : expand_home("~/.config/epis");
}
