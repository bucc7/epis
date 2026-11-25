/* net.c - TCP and TLS connections, minimal HTTP/1.1 client */
#include <netdb.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "epis.h"

static SSL_CTX *ctx;

static int tls_wrap(struct conn *c, const char *host)
{
	SSL *ssl;

	if (!ctx) {
		ctx = SSL_CTX_new(TLS_client_method());
		SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
		SSL_CTX_set_default_verify_paths(ctx);
		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
	}
	ssl = SSL_new(ctx);
	SSL_set_fd(ssl, c->fd);
	SSL_set_tlsext_host_name(ssl, host);
	SSL_set1_host(ssl, host);
	if (SSL_connect(ssl) != 1) {
		char e[256];

		ERR_error_string_n(ERR_get_error(), e, sizeof(e));
		warnx("%s: TLS handshake failed: %s", host, e);
		SSL_free(ssl);
		return -1;
	}
	c->ssl = ssl;
	return 0;
}

/* connect to host:port, optionally wrapping the socket in TLS */
struct conn *conn_open(const char *host, int port, int tls)
{
	struct addrinfo hints = { 0 }, *res, *ai;
	struct timeval tv = { 60, 0 };
	struct conn *c;
	char ports[16];
	int fd = -1, e;

	hints.ai_socktype = SOCK_STREAM;
	snprintf(ports, sizeof(ports), "%d", port);
	if ((e = getaddrinfo(host, ports, &hints, &res))) {
		warnx("%s: %s", host, gai_strerror(e));
		return NULL;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		warnx("%s:%d: connection failed", host, port);
		return NULL;
	}
	c = xcalloc(1, sizeof(*c));
	c->fd = fd;
	c->trace = getenv("EPIS_TRACE") != NULL;
	if (tls && tls_wrap(c, host)) {
		conn_close(c);
		return NULL;
	}
	return c;
}

/* upgrade a plain connection to TLS */
int conn_starttls(struct conn *c, const char *host)
{
	c->rpos = c->rlen = 0;
	return tls_wrap(c, host);
}

int conn_write(struct conn *c, const void *p, size_t n)
{
	const char *s = p;

	if (c->trace)
		fprintf(stderr, "> %.*s", (int)(n > 200 ? 200 : n), s);
	while (n > 0) {
		int w = c->ssl ? SSL_write(c->ssl, s, n) : write(c->fd, s, n);

		if (w <= 0)
			return -1;
		s += w;
		n -= w;
	}
	return 0;
}

int conn_printf(struct conn *c, const char *fmt, ...)
{
	char tmp[4096], *p = tmp;
	va_list ap;
	int n, r;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(tmp)) {
		p = xmalloc(n + 1);
		va_start(ap, fmt);
		vsnprintf(p, n + 1, fmt, ap);
		va_end(ap);
	}
	r = conn_write(c, p, n);
	if (p != tmp)
		free(p);
	return r;
}

static int fill(struct conn *c)
{
	int n;

	n = c->ssl ? SSL_read(c->ssl, c->rbuf, sizeof(c->rbuf)) :
	    read(c->fd, c->rbuf, sizeof(c->rbuf));
	if (n <= 0)
		return -1;
	c->rpos = 0;
	c->rlen = n;
	return 0;
}

/* read one CRLF terminated line, without the terminator */
int conn_getline(struct conn *c, struct buf *line)
{
	buf_reset(line);
	for (;;) {
		char *nl;

		if (c->rpos == c->rlen && fill(c))
			return -1;
		nl = memchr(c->rbuf + c->rpos, '\n', c->rlen - c->rpos);
		if (nl) {
			buf_add(line, c->rbuf + c->rpos,
				nl - (c->rbuf + c->rpos));
			c->rpos = nl - c->rbuf + 1;
			if (line->len && line->p[line->len - 1] == '\r')
				line->p[--line->len] = 0;
			if (c->trace)
				fprintf(stderr, "< %.200s\n", line->p);
			return 0;
		}
		buf_add(line, c->rbuf + c->rpos, c->rlen - c->rpos);
		c->rpos = c->rlen;
	}
}

/* read exactly n bytes */
int conn_read(struct conn *c, size_t n, struct buf *out)
{
	while (n > 0) {
		size_t k;

		if (c->rpos == c->rlen && fill(c))
			return -1;
		k = c->rlen - c->rpos;
		if (k > n)
			k = n;
		buf_add(out, c->rbuf + c->rpos, k);
		c->rpos += k;
		n -= k;
	}
	return 0;
}

void conn_close(struct conn *c)
{
	if (!c)
		return;
	if (c->ssl) {
		SSL_shutdown(c->ssl);
		SSL_free(c->ssl);
	}
	close(c->fd);
	free(c);
}

/*
 * perform an HTTP request and store the body in resp. headers holds
 * extra CRLF terminated header lines. Returns the status code or -1.
 */
int http_request(const char *method, const char *url, const char *headers,
		 const char *body, size_t blen, struct buf *resp)
{
	struct buf line = { 0 };
	char host[256], *loc = NULL, *cur = NULL;
	const char *p, *path;
	int tls, port, status = -1, chunked = 0, redirects = 0;
	long clen = -1;
	struct conn *c;

again:
	buf_reset(resp);
	tls = startswith(url, "https://");
	if (!tls && !startswith(url, "http://"))
		return -1;
	p = url + (tls ? 8 : 7);
	path = strchr(p, '/');
	if (!path)
		path = p + strlen(p);
	snprintf(host, sizeof(host), "%.*s", (int)(path - p), p);
	port = tls ? 443 : 80;
	if (strchr(host, ':')) {
		port = atoi(strchr(host, ':') + 1);
		*strchr(host, ':') = 0;
	}
	if (!(c = conn_open(host, port, tls)))
		goto out;
	conn_printf(c, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: epis/"
		    EPIS_VERSION "\r\nConnection: close\r\n%s", method,
		    *path ? path : "/", host, headers ? headers : "");
	if (body)
		conn_printf(c, "Content-Length: %zu\r\n", blen);
	conn_write(c, "\r\n", 2);
	if (body)
		conn_write(c, body, blen);
	if (conn_getline(c, &line) || sscanf(line.p, "HTTP/%*s %d", &status)
	    != 1) {
		status = -1;
		goto close;
	}
	while (!conn_getline(c, &line) && line.len) {
		if (strcaseprefix(line.p, "content-length:"))
			clen = atol(line.p + 15);
		else if (strcaseprefix(line.p, "transfer-encoding:") &&
			 strcasestr_(line.p, "chunked"))
			chunked = 1;
		else if (strcaseprefix(line.p, "location:")) {
			free(loc);
			loc = xstrdup(trim(line.p + 9));
		}
	}
	if (chunked) {
		for (;;) {
			long n;

			if (conn_getline(c, &line))
				break;
			n = strtol(line.p, NULL, 16);
			if (n <= 0 || conn_read(c, n, resp))
				break;
			conn_getline(c, &line);
		}
	} else if (clen >= 0) {
		conn_read(c, clen, resp);
	} else {
		while (!fill(c))
			buf_add(resp, c->rbuf, c->rlen);
	}
close:
	conn_close(c);
	if (status >= 300 && status < 400 && loc && startswith(loc, "http") &&
	    redirects++ < 5 && !strcmp(method, "GET")) {
		free(cur);
		url = cur = loc;
		loc = NULL;
		clen = -1;
		chunked = 0;
		goto again;
	}
out:
	buf_add(resp, "", 0);
	buf_free(&line);
	free(loc);
	free(cur);
	return status;
}
