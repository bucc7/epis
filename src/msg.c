/* msg.c - RFC 5322 and MIME parsing and composition */
#include <ctype.h>
#include <iconv.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "epis.h"

/* find a header and return its unfolded value */
char *hdr_get(const char *h, const char *name)
{
	size_t nl = strlen(name);
	const char *p = h;
	struct buf b = { 0 };

	if (!h)
		return NULL;
	while (*p) {
		if (!strncasecmp(p, name, nl) && p[nl] == ':') {
			p += nl + 1;
			for (;;) {
				const char *e = strchr(p, '\n');
				size_t l = e ? (size_t)(e - p) : strlen(p);

				if (l && p[l - 1] == '\r')
					l--;
				buf_add(&b, p, l);
				if (!e || (e[1] != ' ' && e[1] != '\t'))
					break;
				p = e + 1;
			}
			buf_add(&b, "", 0);
			{
				char *t = xstrdup(trim(b.p));

				buf_free(&b);
				return t;
			}
		}
		p = strchr(p, '\n');
		if (!p)
			break;
		p++;
		if (*p == '\r' || *p == '\n')
			break;
	}
	return NULL;
}

/* convert data in the given charset to UTF-8, appending to out */
static void to_utf8(const char *charset, const char *in, size_t len,
		    struct buf *out)
{
	iconv_t cd;
	char tmp[4096];

	if (!charset || !*charset || !strcasecmp(charset, "utf-8") ||
	    !strcasecmp(charset, "us-ascii") ||
	    (cd = iconv_open("UTF-8", charset)) == (iconv_t)-1) {
		buf_add(out, in, len);
		return;
	}
	while (len > 0) {
		char *ip = (char *)in, *op = tmp;
		size_t il = len, ol = sizeof(tmp);
		size_t r = iconv(cd, &ip, &il, &op, &ol);

		buf_add(out, tmp, op - tmp);
		if (r == (size_t)-1 && ip == in && op == tmp) {
			buf_puts(out, "\xef\xbf\xbd");
			ip++;
			il--;
		}
		in = ip;
		len = il;
	}
	iconv_close(cd);
}

static void qp_decode(struct buf *out, const char *s, size_t n, int hdr)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned c;

		if (s[i] == '=' && i + 1 < n && (s[i + 1] == '\n' ||
						 s[i + 1] == '\r')) {
			i += s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n'
			    ? 2 : 1;
		} else if (s[i] == '=' && i + 2 < n && isxdigit((unsigned
				char)s[i + 1]) && sscanf(s + i + 1, "%2x",
							 &c) == 1) {
			buf_putc(out, c);
			i += 2;
		} else if (hdr && s[i] == '_') {
			buf_putc(out, ' ');
		} else {
			buf_putc(out, s[i]);
		}
	}
	buf_add(out, "", 0);
}

/* decode RFC 2047 encoded words into UTF-8 */
char *hdr_decode(const char *s)
{
	struct buf o = { 0 };
	const char *p = s, *last_end = NULL;

	buf_add(&o, "", 0);
	while (*p) {
		const char *q1, *q2, *q3, *e;

		if (p[0] == '=' && p[1] == '?' && (q1 = strchr(p + 2, '?')) &&
		    q1[1] && q1[2] == '?' && (q2 = q1 + 2) &&
		    (e = strstr(q2 + 1, "?="))) {
			char *cs = xstrndup(p + 2, q1 - p - 2), *star;
			struct buf raw = { 0 };
			const char *w;

			if (last_end) {
				for (w = last_end; w < p && isspace(
						(unsigned char)*w); w++)
					;
				if (w == p)
					o.len -= p - last_end;
			}
			if ((star = strchr(cs, '*')))
				*star = 0;
			q3 = q2 + 1;
			if (toupper((unsigned char)q1[1]) == 'B')
				b64_decode(&raw, q3, e - q3);
			else
				qp_decode(&raw, q3, e - q3, 1);
			to_utf8(cs, raw.p ? raw.p : "", raw.len, &o);
			free(cs);
			buf_free(&raw);
			p = e + 2;
			last_end = p;
			continue;
		}
		if (!isspace((unsigned char)*p))
			last_end = NULL;
		buf_putc(&o, *p == '\t' || *p == '\r' || *p == '\n' ? ' ' :
			 *p);
		p++;
	}
	return buf_take(&o);
}

/* extract a parameter such as charset or boundary from a header value */
static char *hdr_param(const char *v, const char *name)
{
	size_t nl = strlen(name);
	const char *p = v;

	while (v && (p = strchr(p, ';'))) {
		p++;
		while (isspace((unsigned char)*p))
			p++;
		if (!strncasecmp(p, name, nl) && (p[nl] == '=' ||
						  (p[nl] == '*' &&
						   strchr(p, '=')))) {
			const char *s = strchr(p, '=') + 1;
			int ext = p[nl] == '*';

			if (*s == '"') {
				const char *e = strchr(++s, '"');

				return xstrndup(s, e ? (size_t)(e - s) :
						strlen(s));
			}
			if (ext && strstr(s, "''"))
				s = strstr(s, "''") + 2;
			{
				size_t l = strcspn(s, "; \t\r\n");
				struct buf b = { 0 };

				if (!ext)
					return xstrndup(s, l);
				qp_decode(&b, s, l, 0);
				return buf_take(&b);
			}
		}
	}
	return NULL;
}

static void walk(struct message *m, const char *data, size_t len,
		 int depth);

static void add_part(struct message *m, const char *hdr, const char *body,
		     size_t blen)
{
	char *ct = hdr_get(hdr, "Content-Type");
	char *cd = hdr_get(hdr, "Content-Disposition");
	char *cte = hdr_get(hdr, "Content-Transfer-Encoding");
	struct part *p;
	char *n;

	m->parts = xrealloc(m->parts, (m->nparts + 1) * sizeof(*m->parts));
	p = &m->parts[m->nparts++];
	memset(p, 0, sizeof(*p));
	p->type = ct ? xstrndup(ct, strcspn(ct, "; \t")) : xstrdup("text/plain");
	p->charset = hdr_param(ct, "charset");
	n = hdr_param(cd, "filename");
	if (!n)
		n = hdr_param(ct, "name");
	p->name = n ? hdr_decode(n) : NULL;
	free(n);
	p->disposition = cd ? xstrndup(cd, strcspn(cd, "; \t")) : NULL;
	p->encoding = cte ? xstrdup(trim(cte)) : NULL;
	p->body = body;
	p->blen = blen;
	free(ct);
	free(cd);
	free(cte);
}

/* split headers from body, returns body offset */
static size_t split_hdr(const char *d, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len; i++) {
		if (d[i] == '\n' && d[i + 1] == '\n')
			return i + 2;
		if (d[i] == '\n' && d[i + 1] == '\r' && i + 2 < len &&
		    d[i + 2] == '\n')
			return i + 3;
	}
	return len;
}

static void walk(struct message *m, const char *d, size_t len, int depth)
{
	size_t bo = split_hdr(d, len);
	char *hdr = xstrndup(d, bo), *ct = hdr_get(hdr, "Content-Type");
	char *bnd = hdr_param(ct, "boundary");

	if (depth < 16 && ct && strcaseprefix(ct, "multipart/") && bnd) {
		size_t bl = strlen(bnd);
		const char *p = d + bo, *end = d + len, *start = NULL;

		while (p < end) {
			const char *nl = memchr(p, '\n', end - p);
			const char *next = nl ? nl + 1 : end;

			if (p[0] == '-' && p[1] == '-' && end - p >= (long)bl
			    + 2 && !strncmp(p + 2, bnd, bl)) {
				if (start) {
					const char *e = p;

					if (e > start && e[-1] == '\n')
						e--;
					if (e > start && e[-1] == '\r')
						e--;
					walk(m, start, e - start, depth + 1);
				}
				if (!strncmp(p + 2 + bl, "--", 2))
					break;
				start = next;
			}
			p = next;
		}
	} else if (depth < 16 && ct && strcaseprefix(ct, "message/rfc822")) {
		add_part(m, hdr, d + bo, len - bo);
		walk(m, d + bo, len - bo, depth + 1);
	} else {
		add_part(m, hdr, d + bo, len - bo);
	}
	free(hdr);
	free(ct);
	free(bnd);
}

/* parse a raw message; takes ownership of raw */
int msg_parse(struct message *m, char *raw, size_t len)
{
	size_t bo = split_hdr(raw, len);

	memset(m, 0, sizeof(*m));
	m->raw = raw;
	m->rawlen = len;
	buf_add(&m->hdr, raw, bo);
	walk(m, raw, len, 0);
	return 0;
}

void msg_free(struct message *m)
{
	int i;

	for (i = 0; i < m->nparts; i++) {
		free(m->parts[i].type);
		free(m->parts[i].name);
		free(m->parts[i].charset);
		free(m->parts[i].encoding);
		free(m->parts[i].disposition);
	}
	free(m->parts);
	free(m->raw);
	buf_free(&m->hdr);
}

/* decoded value of a top level header, or NULL */
char *msg_header(struct message *m, const char *name)
{
	char *v = hdr_get(m->hdr.p, name), *d;

	if (!v)
		return NULL;
	d = hdr_decode(v);
	free(v);
	return d;
}

/* decode the transfer encoding of a part */
int part_decode(struct part *p, struct buf *out)
{
	if (p->encoding && !strcasecmp(p->encoding, "base64"))
		b64_decode(out, p->body, p->blen);
	else if (p->encoding && !strcasecmp(p->encoding, "quoted-printable"))
		qp_decode(out, p->body, p->blen, 0);
	else
		buf_add(out, p->body, p->blen);
	return 0;
}

static const struct {
	const char *name, *val;
} entities[] = {
	{ "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
	{ "apos", "'" }, { "nbsp", " " }, { "#39", "'" }, { "mdash", "-" },
	{ "ndash", "-" }, { "hellip", "..." }, { "copy", "(c)" }
};

/* crude HTML to plain text conversion for messages without text parts */
static void html_text(const char *s, size_t n, struct buf *o)
{
	size_t i, k;
	int skip = 0, space = 0;

	for (i = 0; i < n; i++) {
		if (s[i] == '<') {
			const char *t = s + i + 1;
			const char *e = memchr(t, '>', n - i - 1);

			if (!e)
				break;
			if (strcaseprefix(t, "style") ||
			    strcaseprefix(t, "script"))
				skip = 1;
			else if (strcaseprefix(t, "/style") ||
				 strcaseprefix(t, "/script"))
				skip = 0;
			else if (strcaseprefix(t, "br") ||
				 strcaseprefix(t, "/p") ||
				 strcaseprefix(t, "/div") ||
				 strcaseprefix(t, "/tr") ||
				 strcaseprefix(t, "/h") ||
				 strcaseprefix(t, "li"))
				buf_putc(o, '\n');
			i = e - s;
			space = 0;
			continue;
		}
		if (skip)
			continue;
		if (s[i] == '&') {
			for (k = 0; k < sizeof(entities) / sizeof(*entities);
			     k++) {
				size_t l = strlen(entities[k].name);

				if (!strncmp(s + i + 1, entities[k].name, l) &&
				    s[i + 1 + l] == ';') {
					buf_puts(o, entities[k].val);
					i += l + 1;
					break;
				}
			}
			if (k < sizeof(entities) / sizeof(*entities))
				continue;
		}
		if (isspace((unsigned char)s[i])) {
			if (!space && o->len && o->p[o->len - 1] != '\n')
				buf_putc(o, ' ');
			space = 1;
			continue;
		}
		space = 0;
		buf_putc(o, s[i]);
	}
	buf_add(o, "", 0);
}

static int is_attachment(struct part *p)
{
	return (p->disposition && !strcasecmp(p->disposition, "attachment"))
	    || (p->name && strncasecmp(p->type, "text/", 5));
}

/* readable UTF-8 text of the message body */
char *msg_text(struct message *m)
{
	struct buf o = { 0 }, raw = { 0 };
	int i, found = 0;

	for (i = 0; i < m->nparts; i++) {
		struct part *p = &m->parts[i];

		if (strcasecmp(p->type, "text/plain") || is_attachment(p))
			continue;
		buf_reset(&raw);
		part_decode(p, &raw);
		if (found)
			buf_puts(&o, "\n");
		to_utf8(p->charset, raw.p ? raw.p : "", raw.len, &o);
		found = 1;
	}
	for (i = 0; !found && i < m->nparts; i++) {
		struct part *p = &m->parts[i];
		struct buf h = { 0 };

		if (strcasecmp(p->type, "text/html") || is_attachment(p))
			continue;
		buf_reset(&raw);
		part_decode(p, &raw);
		to_utf8(p->charset, raw.p ? raw.p : "", raw.len, &h);
		html_text(h.p ? h.p : "", h.len, &o);
		buf_free(&h);
		found = 1;
	}
	for (i = 0; i < m->nparts; i++)
		if (is_attachment(&m->parts[i]))
			buf_printf(&o, "\n[attachment: %s (%s, %zu bytes)]",
				   m->parts[i].name ? m->parts[i].name :
				   "unnamed", m->parts[i].type,
				   m->parts[i].blen);
	buf_add(&o, "", 0);
	{
		size_t r, w = 0;

		for (r = 0; r < o.len; r++)
			if (o.p[r] != '\r')
				o.p[w++] = o.p[r];
		o.p[w] = 0;
		o.len = w;
	}
	buf_free(&raw);
	return buf_take(&o);
}

/* parse an RFC 5322 date into a UTC timestamp */
time_t parse_date(const char *s)
{
	static const char *mon[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
		"Sep", "Oct", "Nov", "Dec"
	};
	struct tm tm = { 0 };
	char m[4] = "", zone[8] = "";
	int d, y, H, M, S = 0, i, off = 0;
	const char *p = strchr(s, ',');

	p = p ? p + 1 : s;
	if (sscanf(p, "%d %3s %d %d:%d:%d %7s", &d, m, &y, &H, &M, &S, zone)
	    < 5)
		return 0;
	if (y < 100)
		y += y < 50 ? 2000 : 1900;
	tm.tm_mday = d;
	tm.tm_year = y - 1900;
	tm.tm_hour = H;
	tm.tm_min = M;
	tm.tm_sec = S;
	for (i = 0; i < 12; i++)
		if (!strcasecmp(m, mon[i]))
			tm.tm_mon = i;
	if (zone[0] == '+' || zone[0] == '-') {
		int z = atoi(zone + 1);

		off = (z / 100 * 60 + z % 100) * 60;
		if (zone[0] == '-')
			off = -off;
	}
	return timegm(&tm) - off;
}

/* bare address from "Name <addr>" or "addr" */
char *addr_email(const char *s)
{
	const char *lt = strrchr(s, '<'), *gt;

	if (lt && (gt = strchr(lt, '>')))
		return xstrndup(lt + 1, gt - lt - 1);
	{
		char *t = xstrdup(s), *r = xstrdup(trim(t));

		free(t);
		return r;
	}
}

/* display name of an address, or the address itself */
char *addr_display(const char *s)
{
	const char *lt = strchr(s, '<');
	char *n, *t;

	if (!lt || lt == s)
		return addr_email(s);
	n = xstrndup(s, lt - s);
	t = trim(n);
	if (*t == '"') {
		t++;
		if (*t && t[strlen(t) - 1] == '"')
			t[strlen(t) - 1] = 0;
	}
	t = *t ? xstrdup(t) : addr_email(s);
	free(n);
	return t;
}

/* split a comma separated address list */
int addr_split(const char *s, char ***out)
{
	char **v = NULL;
	int n = 0, q = 0, ang = 0;
	const char *start = s;

	for (;; s++) {
		if (*s == '"')
			q = !q;
		else if (*s == '<')
			ang = 1;
		else if (*s == '>')
			ang = 0;
		if (!*s || ((*s == ',' || *s == ';') && !q && !ang)) {
			char *a = xstrndup(start, s - start), *t = trim(a);

			if (*t) {
				v = xrealloc(v, (n + 1) * sizeof(*v));
				v[n++] = xstrdup(t);
			}
			free(a);
			if (!*s)
				break;
			start = s + 1;
		}
	}
	*out = v;
	return n;
}

static int is_ascii(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s >= 0x80)
			return 0;
	return 1;
}

static void encode_word(struct buf *b, const char *s)
{
	size_t l = strlen(s), i = 0;

	while (i < l) {
		size_t n = l - i > 45 ? 45 : l - i;

		while (i + n < l && ((unsigned char)s[i + n] & 0xc0) == 0x80)
			n--;
		if (i)
			buf_puts(b, "\r\n ");
		buf_puts(b, "=?UTF-8?B?");
		b64_encode(b, s + i, n);
		buf_puts(b, "?=");
		i += n;
	}
}

/* append "Name: value\r\n", encoding non-ASCII text per RFC 2047 */
void hdr_encode(struct buf *b, const char *name, const char *val)
{
	int addr = !strcasecmp(name, "From") || !strcasecmp(name, "To") ||
	    !strcasecmp(name, "Cc") || !strcasecmp(name, "Bcc") ||
	    !strcasecmp(name, "Reply-To");
	char **v;
	int n, i;

	buf_printf(b, "%s: ", name);
	if (is_ascii(val)) {
		buf_puts(b, val);
	} else if (!addr) {
		encode_word(b, val);
	} else {
		n = addr_split(val, &v);
		for (i = 0; i < n; i++) {
			char *d = addr_display(v[i]), *e = addr_email(v[i]);

			if (i)
				buf_puts(b, ",\r\n ");
			if (strcmp(d, e)) {
				encode_word(b, d);
				buf_printf(b, " <%s>", e);
			} else {
				buf_puts(b, e);
			}
			free(d);
			free(e);
			free(v[i]);
		}
		free(v);
	}
	buf_puts(b, "\r\n");
}

/* quoted-printable encode text with CRLF line endings */
static void qp_encode(struct buf *b, const char *s)
{
	int col = 0;

	for (; *s; s++) {
		unsigned char c = *s;

		if (c == '\r')
			continue;
		if (c == '\n') {
			buf_puts(b, "\r\n");
			col = 0;
			continue;
		}
		if (col >= 72) {
			buf_puts(b, "=\r\n");
			col = 0;
		}
		if ((c >= 33 && c <= 126 && c != '=') ||
		    (c == ' ' && s[1] != '\n' && s[1])) {
			buf_putc(b, c);
			col++;
		} else {
			buf_printf(b, "=%02X", c);
			col += 3;
		}
	}
	buf_puts(b, "\r\n");
}

static const char *mime_type(const char *path)
{
	static const struct {
		const char *ext, *type;
	} t[] = {
		{ ".pdf", "application/pdf" }, { ".png", "image/png" },
		{ ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
		{ ".gif", "image/gif" }, { ".txt", "text/plain" },
		{ ".html", "text/html" }, { ".zip", "application/zip" },
		{ ".csv", "text/csv" }, { ".json", "application/json" },
	};
	const char *dot = strrchr(path, '.');
	size_t i;

	for (i = 0; dot && i < sizeof(t) / sizeof(*t); i++)
		if (!strcasecmp(dot, t[i].ext))
			return t[i].type;
	return "application/octet-stream";
}

/* build an RFC 5322 message from a draft */
char *msg_build(struct draft *d, size_t *len)
{
	struct buf b = { 0 };
	char date[64], bnd[40], rnd[33], host[128] = "localhost";
	time_t now = time(NULL);
	int i;

	gethostname(host, sizeof(host) - 1);
	strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z",
		 localtime(&now));
	random_hex(rnd, 16);
	buf_printf(&b, "Date: %s\r\n", date);
	{
		char *e = addr_email(d->from), *at = strchr(e, '@');

		buf_printf(&b, "Message-ID: <%s@%s>\r\n", rnd, at ? at + 1 :
			   host);
		free(e);
	}
	hdr_encode(&b, "From", d->from);
	if (d->to && *d->to)
		hdr_encode(&b, "To", d->to);
	if (d->cc && *d->cc)
		hdr_encode(&b, "Cc", d->cc);
	hdr_encode(&b, "Subject", d->subject ? d->subject : "");
	if (d->in_reply_to && *d->in_reply_to)
		buf_printf(&b, "In-Reply-To: %s\r\n", d->in_reply_to);
	if (d->references && *d->references)
		buf_printf(&b, "References: %s\r\n", d->references);
	buf_puts(&b, "MIME-Version: 1.0\r\nUser-Agent: epis/" EPIS_VERSION
		 "\r\n");
	snprintf(bnd, sizeof(bnd), "epis-%.24s", rnd);
	if (d->nattach)
		buf_printf(&b, "Content-Type: multipart/mixed; boundary=\"%s\""
			   "\r\n\r\n--%s\r\n", bnd, bnd);
	buf_puts(&b, "Content-Type: text/plain; charset=utf-8\r\n"
		 "Content-Transfer-Encoding: quoted-printable\r\n\r\n");
	qp_encode(&b, d->body ? d->body : "");
	for (i = 0; i < d->nattach; i++) {
		size_t fl;
		char *data = read_file(d->attach[i], &fl);
		const char *base = strrchr(d->attach[i], '/');
		size_t k;
		struct buf enc = { 0 };

		if (!data) {
			warnx("cannot read attachment %s", d->attach[i]);
			continue;
		}
		base = base ? base + 1 : d->attach[i];
		buf_printf(&b, "--%s\r\nContent-Type: %s; name=\"%s\"\r\n"
			   "Content-Disposition: attachment; filename=\"%s\""
			   "\r\nContent-Transfer-Encoding: base64\r\n\r\n",
			   bnd, mime_type(base), base, base);
		b64_encode(&enc, data, fl);
		for (k = 0; k < enc.len; k += 76) {
			buf_add(&b, enc.p + k, enc.len - k > 76 ? 76 :
				enc.len - k);
			buf_puts(&b, "\r\n");
		}
		buf_free(&enc);
		free(data);
	}
	if (d->nattach)
		buf_printf(&b, "--%s--\r\n", bnd);
	*len = b.len;
	return buf_take(&b);
}

static char *from_line(struct account *a)
{
	if (a->realname)
		return xasprintf("%s <%s>", a->realname, a->email);
	return xstrdup(a->email);
}

static char *quote_body(struct message *m)
{
	char *t = msg_text(m), *from = msg_header(m, "From"), *date =
	    msg_header(m, "Date"), *line, *next;
	struct buf b = { 0 };

	buf_printf(&b, "\n\nOn %s, %s wrote:\n", date ? date : "?",
		   from ? from : "?");
	for (line = t; line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		buf_printf(&b, ">%s%s\n", *line == '>' ? "" : " ", line);
	}
	free(t);
	free(from);
	free(date);
	return buf_take(&b);
}

/* fill a draft replying to m; all != 0 includes every recipient */
void draft_reply(struct draft *d, struct account *a, struct message *m,
		 int all)
{
	char *subj = msg_header(m, "Subject"), *rt = msg_header(m, "Reply-To");
	char *from = msg_header(m, "From"), *mid = hdr_get(m->hdr.p,
							    "Message-ID");
	char *refs = hdr_get(m->hdr.p, "References");

	memset(d, 0, sizeof(*d));
	d->from = from_line(a);
	d->to = xstrdup(rt ? rt : from ? from : "");
	if (all) {
		struct buf cc = { 0 };
		const char *hs[] = { "To", "Cc" };
		int i, j, n;
		char **v;

		for (i = 0; i < 2; i++) {
			char *h = msg_header(m, hs[i]);

			n = h ? addr_split(h, &v) : 0;
			for (j = 0; j < n; j++) {
				char *e = addr_email(v[j]);

				if (strcasecmp(e, a->email))
					buf_printf(&cc, "%s%s", cc.len ?
						   ", " : "", v[j]);
				free(e);
				free(v[j]);
			}
			if (n)
				free(v);
			free(h);
		}
		d->cc = buf_take(&cc);
	}
	d->subject = subj && strcaseprefix(subj, "re:") ? xstrdup(subj) :
	    xasprintf("Re: %s", subj ? subj : "");
	d->in_reply_to = mid ? xstrdup(mid) : NULL;
	d->references = mid ? xasprintf("%s%s%s", refs ? refs : "", refs ?
					" " : "", mid) : NULL;
	d->body = quote_body(m);
	free(subj);
	free(rt);
	free(from);
	free(mid);
	free(refs);
}

/* fill a draft forwarding m inline */
void draft_forward(struct draft *d, struct account *a, struct message *m)
{
	char *subj = msg_header(m, "Subject"), *t = msg_text(m);
	char *from = msg_header(m, "From"), *date = msg_header(m, "Date");

	memset(d, 0, sizeof(*d));
	d->from = from_line(a);
	d->to = xstrdup("");
	d->subject = xasprintf("Fwd: %s", subj ? subj : "");
	d->body = xasprintf("\n\n---------- Forwarded message ----------\n"
			    "From: %s\nDate: %s\nSubject: %s\n\n%s",
			    from ? from : "", date ? date : "",
			    subj ? subj : "", t);
	free(subj);
	free(t);
	free(from);
	free(date);
}

void draft_free(struct draft *d)
{
	int i;

	free(d->from);
	free(d->to);
	free(d->cc);
	free(d->bcc);
	free(d->subject);
	free(d->body);
	free(d->in_reply_to);
	free(d->references);
	for (i = 0; i < d->nattach; i++)
		free(d->attach[i]);
	free(d->attach);
	memset(d, 0, sizeof(*d));
}

/* send a draft and file a copy in the Sent folder */
int msg_send(struct account *a, struct draft *d)
{
	char **rcpt = NULL, **v, *data, *from;
	const char *lists[3];
	size_t len;
	int n = 0, i, j, k, r;

	if (!d->from)
		d->from = from_line(a);
	lists[0] = d->to;
	lists[1] = d->cc;
	lists[2] = d->bcc;
	for (i = 0; i < 3; i++) {
		k = lists[i] ? addr_split(lists[i], &v) : 0;
		for (j = 0; j < k; j++) {
			rcpt = xrealloc(rcpt, (n + 1) * sizeof(*rcpt));
			rcpt[n++] = addr_email(v[j]);
			free(v[j]);
		}
		if (k)
			free(v);
	}
	if (!n) {
		warnx("no recipients");
		return -1;
	}
	data = msg_build(d, &len);
	from = addr_email(d->from);
	r = smtp_send(a, from, rcpt, n, data, len);
	if (!r && a->save_sent) {
		char *dir = folder_dir(a->name, a->sent);

		free(maildir_store(dir, data, len, F_SEEN, 0));
		free(dir);
	}
	for (i = 0; i < n; i++)
		free(rcpt[i]);
	free(rcpt);
	free(data);
	free(from);
	return r;
}
