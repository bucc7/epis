/* imap.c - IMAP4rev1 client and Maildir synchronization */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "epis.h"

struct imap {
	struct conn *c;
	struct account *a;
	unsigned tag;
	char caps[2048];
	struct buf resp;
};

enum { I_ATOM, I_STR, I_LIST, I_NIL };

struct inode {
	int type;
	const char *s;
	size_t len;
	struct inode *kids;
	int n;
};

typedef void (*untagged_fn)(struct imap *im, struct inode *r, void *ctx);

static void inode_free(struct inode *n)
{
	int i;

	for (i = 0; i < n->n; i++)
		inode_free(&n->kids[i]);
	free(n->kids);
}

/* parse IMAP tokens from p into list node out until end or ')' */
static const char *iparse(const char *p, const char *end, struct inode *out)
{
	out->type = I_LIST;
	while (p < end) {
		struct inode k = { 0 };

		while (p < end && *p == ' ')
			p++;
		if (p >= end || *p == ')' || *p == ']')
			return p < end ? p + 1 : p;
		if (*p == '(' || *p == '[') {
			p = iparse(p + 1, end, &k);
		} else if (*p == '"') {
			const char *s = ++p;

			while (p < end && *p != '"')
				p += *p == '\\' ? 2 : 1;
			k.type = I_STR;
			k.s = s;
			k.len = p - s;
			p++;
		} else if (*p == '{') {
			size_t n = strtoul(p + 1, NULL, 10);

			p = memchr(p, '\n', end - p);
			if (!p)
				return end;
			p++;
			k.type = I_STR;
			k.s = p;
			k.len = n;
			p += n;
		} else {
			const char *s = p;

			while (p < end && *p != ' ' && *p != '(' &&
			       *p != ')' && *p != ']') {
				if (*p == '[') {
					const char *q;

					if (strncasecmp(s, "BODY", 4))
						break;
					q = memchr(p, ']', end - p);
					p = q ? q : end - 1;
				}
				p++;
			}
			if (p == s) {
				p++;
				continue;
			}
			k.type = (p - s == 3 && !strncasecmp(s, "NIL", 3)) ?
			    I_NIL : I_ATOM;
			k.s = s;
			k.len = p - s;
		}
		out->kids = xrealloc(out->kids, (out->n + 1) * sizeof(k));
		out->kids[out->n++] = k;
	}
	return p;
}

static int tok_is(struct inode *n, const char *s)
{
	return n && n->s && n->len == strlen(s) && !strncasecmp(n->s, s,
								 n->len);
}

static struct inode *kid(struct inode *n, int i)
{
	return n && i < n->n ? &n->kids[i] : NULL;
}

/* read one response, including any literals, into im->resp */
static int read_resp(struct imap *im)
{
	struct buf line = { 0 };

	buf_reset(&im->resp);
	for (;;) {
		size_t n;
		char *br;

		if (conn_getline(im->c, &line)) {
			buf_free(&line);
			return -1;
		}
		buf_add(&im->resp, line.p, line.len);
		if (!line.len || line.p[line.len - 1] != '}' ||
		    !(br = strrchr(line.p, '{')))
			break;
		n = strtoul(br + 1, NULL, 10);
		buf_puts(&im->resp, "\r\n");
		if (conn_read(im->c, n, &im->resp)) {
			buf_free(&line);
			return -1;
		}
	}
	buf_free(&line);
	return 0;
}

static void save_caps(struct imap *im, const char *s)
{
	const char *p = strcasestr_(s, "CAPABILITY ");

	if (p)
		snprintf(im->caps, sizeof(im->caps), " %s",
			 p + strlen("CAPABILITY "));
}

/*
 * send a tagged command and dispatch untagged responses to fn until the
 * tagged completion arrives. Returns 0 on OK.
 */
static int imap_cmd(struct imap *im, untagged_fn fn, void *ctx,
		    const char *fmt, ...)
{
	struct buf cmd = { 0 };
	char tag[16];
	va_list ap;
	int n;

	snprintf(tag, sizeof(tag), "E%u", ++im->tag);
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	cmd.p = xmalloc(n + 1);
	va_start(ap, fmt);
	vsnprintf(cmd.p, n + 1, fmt, ap);
	va_end(ap);
	if (conn_printf(im->c, "%s %s\r\n", tag, cmd.p)) {
		free(cmd.p);
		return -1;
	}
	free(cmd.p);
	while (!read_resp(im)) {
		const char *r = im->resp.p;

		if (startswith(r, "* ")) {
			struct inode root = { 0 };

			if (strcasestr_(r, "CAPABILITY "))
				save_caps(im, r);
			if (fn) {
				iparse(r, r + im->resp.len, &root);
				fn(im, &root, ctx);
				inode_free(&root);
			}
		} else if (startswith(r, tag) && r[strlen(tag)] == ' ') {
			r += strlen(tag) + 1;
			if (strcasestr_(r, "CAPABILITY "))
				save_caps(im, r);
			if (strcaseprefix(r, "OK"))
				return 0;
			warnx("IMAP: %s", r);
			return -1;
		}
	}
	return -1;
}

/* append s as an IMAP quoted string */
static void imap_quote(struct buf *b, const char *s)
{
	buf_putc(b, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			buf_putc(b, '\\');
		buf_putc(b, *s);
	}
	buf_putc(b, '"');
}

static int authenticate(struct imap *im)
{
	struct account *a = im->a;
	struct sasl s = { 0 };
	struct buf out = { 0 }, q = { 0 };
	char *tok = NULL, tag[16];
	int r = -1;

	s.user = a->user;
	if (a->oauth) {
		tok = oauth_access_token(a);
		s.pass = tok;
	} else {
		s.pass = account_password(a);
	}
	if (!s.pass) {
		warnx("%s: no password or token available", a->name);
		return -1;
	}
	s.mech = sasl_pick(a->auth, im->caps, a->oauth != NULL);
	if (s.mech == AUTH_NONE || (s.mech == AUTH_PLAIN &&
				    !strcasestr_(im->caps, "AUTH=PLAIN"))) {
		buf_puts(&q, "LOGIN ");
		imap_quote(&q, s.user);
		buf_putc(&q, ' ');
		imap_quote(&q, s.pass);
		r = imap_cmd(im, NULL, NULL, "%s", q.p);
		goto out;
	}
	snprintf(tag, sizeof(tag), "E%u", ++im->tag);
	conn_printf(im->c, "%s AUTHENTICATE %s\r\n", tag, sasl_name(s.mech));
	while (!read_resp(im)) {
		const char *l = im->resp.p;

		if (*l == '+') {
			l += l[1] == ' ' ? 2 : 1;
			if (sasl_step(&s, l, &out)) {
				conn_printf(im->c, "*\r\n");
				continue;
			}
			conn_printf(im->c, "%s\r\n", out.p);
		} else if (startswith(l, tag)) {
			l += strlen(tag) + 1;
			save_caps(im, l);
			r = strcaseprefix(l, "OK") ? 0 : -1;
			if (r)
				warnx("%s: authentication failed: %s",
				      a->name, l);
			break;
		}
	}
out:
	free(tok);
	buf_free(&out);
	buf_free(&q);
	sasl_free(&s);
	return r;
}

/* connect, secure and authenticate an IMAP session */
struct imap *imap_connect(struct account *a)
{
	struct imap *im;

	if (!a->imap_host) {
		warnx("%s: imap_host not set", a->name);
		return NULL;
	}
	im = xcalloc(1, sizeof(*im));
	im->a = a;
	im->c = conn_open(a->imap_host, a->imap_port, a->imap_sec == SEC_TLS);
	if (!im->c || read_resp(im) || !startswith(im->resp.p, "* "))
		goto fail;
	save_caps(im, im->resp.p);
	if (a->imap_sec == SEC_STARTTLS) {
		if (imap_cmd(im, NULL, NULL, "STARTTLS") ||
		    conn_starttls(im->c, a->imap_host))
			goto fail;
		im->caps[0] = 0;
	}
	if (!im->caps[0] && imap_cmd(im, NULL, NULL, "CAPABILITY"))
		goto fail;
	if (authenticate(im))
		goto fail;
	if (imap_cmd(im, NULL, NULL, "CAPABILITY"))
		goto fail;
	return im;
fail:
	imap_close(im);
	return NULL;
}

void imap_close(struct imap *im)
{
	if (!im)
		return;
	if (im->c) {
		conn_printf(im->c, "E%u LOGOUT\r\n", ++im->tag);
		conn_close(im->c);
	}
	buf_free(&im->resp);
	free(im);
}

/* decode a modified UTF-7 mailbox name (RFC 3501 5.1.3) */
static char *mutf7_decode(const char *s, size_t len)
{
	struct buf o = { 0 };
	size_t i;

	for (i = 0; i < len; i++) {
		struct buf raw = { 0 }, b64 = { 0 };
		size_t j, k;

		if (s[i] != '&') {
			buf_putc(&o, s[i]);
			continue;
		}
		for (j = i + 1; j < len && s[j] != '-'; j++)
			buf_putc(&b64, s[j] == ',' ? '/' : s[j]);
		if (j == i + 1) {
			buf_putc(&o, '&');
		} else {
			b64_decode(&raw, b64.p, b64.len);
			for (k = 0; k + 1 < raw.len; k += 2) {
				uint32_t cp = (unsigned char)raw.p[k] << 8 |
				    (unsigned char)raw.p[k + 1];
				char u[4];

				if (cp >= 0xd800 && cp < 0xdc00 &&
				    k + 3 < raw.len) {
					uint32_t lo = (unsigned char)raw.p[k +
						2] << 8 | (unsigned char)
					    raw.p[k + 3];

					cp = 0x10000 + ((cp - 0xd800) << 10) +
					    (lo - 0xdc00);
					k += 2;
				}
				buf_add(&o, u, utf8_encode(u, cp));
			}
		}
		buf_free(&raw);
		buf_free(&b64);
		i = j;
	}
	return buf_take(&o);
}

struct listctx {
	struct folder *f;
	int n;
};

static void on_list(struct imap *im, struct inode *r, void *ctx)
{
	struct listctx *lc = ctx;
	struct inode *fl = kid(r, 2), *d = kid(r, 3), *nm = kid(r, 4);
	struct folder *f;
	int i;

	(void)im;
	if (!tok_is(kid(r, 1), "LIST") || !nm)
		return;
	lc->f = xrealloc(lc->f, (lc->n + 1) * sizeof(*lc->f));
	f = &lc->f[lc->n++];
	f->name = xstrndup(nm->s, nm->len);
	f->delim = d && d->type == I_STR && d->len ? d->s[0] : '/';
	f->noselect = 0;
	for (i = 0; fl && i < fl->n; i++)
		if (tok_is(&fl->kids[i], "\\Noselect") ||
		    tok_is(&fl->kids[i], "\\NonExistent"))
			f->noselect = 1;
}

/* list all mailboxes */
int imap_list(struct imap *im, struct folder **out)
{
	struct listctx lc = { 0 };

	if (imap_cmd(im, on_list, &lc, "LIST \"\" \"*\"")) {
		*out = NULL;
		return -1;
	}
	*out = lc.f;
	return lc.n;
}

static const struct {
	const char *imap;
	int f;
} imapflags[] = {
	{ "\\Seen", F_SEEN }, { "\\Answered", F_REPLIED },
	{ "\\Flagged", F_FLAGGED }, { "\\Draft", F_DRAFT },
	{ "\\Deleted", F_TRASHED }, { "$Forwarded", F_PASSED }
};

#define NIFLAGS (int)(sizeof(imapflags) / sizeof(*imapflags))

static int flags_from_list(struct inode *l)
{
	int f = 0, i, j;

	for (i = 0; l && i < l->n; i++)
		for (j = 0; j < NIFLAGS; j++)
			if (tok_is(&l->kids[i], imapflags[j].imap))
				f |= imapflags[j].f;
	return f;
}

static void flags_to_list(struct buf *b, int f)
{
	int j, first = 1;

	buf_putc(b, '(');
	for (j = 0; j < NIFLAGS; j++)
		if ((f & imapflags[j].f) && imapflags[j].f != F_TRASHED) {
			if (!first)
				buf_putc(b, ' ');
			buf_puts(b, imapflags[j].imap);
			first = 0;
		}
	buf_putc(b, ')');
}

struct rmsg {
	unsigned uid;
	int flags;
};

struct syncctx {
	unsigned uidvalidity;
	struct rmsg *r;
	int nr;
	const char *dir;
	int stored;
	struct rmsg *st;
	int nst;
};

static void on_select(struct imap *im, struct inode *r, void *ctx)
{
	struct syncctx *sc = ctx;
	struct inode *code = kid(r, 2);

	(void)im;
	if (tok_is(kid(r, 1), "OK") && code && code->type == I_LIST &&
	    tok_is(kid(code, 0), "UIDVALIDITY"))
		sc->uidvalidity = strtoul(kid(code, 1)->s, NULL, 10);
}

/* handle a FETCH response, storing bodies when present */
static void on_fetch(struct imap *im, struct inode *r, void *ctx)
{
	struct syncctx *sc = ctx;
	struct inode *l = kid(r, 3), *body = NULL, *fl = NULL;
	unsigned uid = 0;
	int i;

	(void)im;
	if (!tok_is(kid(r, 2), "FETCH") || !l || l->type != I_LIST)
		return;
	for (i = 0; i + 1 < l->n; i += 2) {
		struct inode *k = &l->kids[i], *v = &l->kids[i + 1];

		if (tok_is(k, "UID"))
			uid = strtoul(v->s, NULL, 10);
		else if (tok_is(k, "FLAGS"))
			fl = v;
		else if (k->len >= 6 && !strncasecmp(k->s, "BODY[", 5))
			body = v;
	}
	if (!uid)
		return;
	if (body && body->type == I_STR) {
		char *p = maildir_store(sc->dir, body->s, body->len,
					flags_from_list(fl), uid);

		if (p) {
			sc->st = xrealloc(sc->st, (sc->nst + 1) *
					  sizeof(*sc->st));
			sc->st[sc->nst].uid = uid;
			sc->st[sc->nst++].flags = flags_from_list(fl);
			sc->stored++;
			free(p);
		}
		return;
	}
	sc->r = xrealloc(sc->r, (sc->nr + 1) * sizeof(*sc->r));
	sc->r[sc->nr].uid = uid;
	sc->r[sc->nr++].flags = flags_from_list(fl);
}

static int rmsg_cmp(const void *a, const void *b)
{
	const struct rmsg *x = a, *y = b;

	return x->uid < y->uid ? -1 : x->uid > y->uid;
}

static struct rmsg *rfind(struct rmsg *v, int n, unsigned uid)
{
	struct rmsg key = { uid, 0 };

	return n ? bsearch(&key, v, n, sizeof(*v), rmsg_cmp) : NULL;
}

/* load the last synchronized flags of a folder */
static int state_load(const char *dir, struct rmsg **out, unsigned *uv)
{
	char *p = xasprintf("%s/.epis-state", dir), *d = read_file(p, NULL);
	char *line, *next;
	struct rmsg *v = NULL;
	int n = 0;

	free(p);
	*uv = 0;
	for (line = d; line && *line; line = next) {
		unsigned uid;
		int fl;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		if (sscanf(line, "uidvalidity %u", uv) == 1)
			continue;
		if (sscanf(line, "%u %d", &uid, &fl) == 2) {
			v = xrealloc(v, (n + 1) * sizeof(*v));
			v[n].uid = uid;
			v[n++].flags = fl;
		}
	}
	free(d);
	qsort(v, n, sizeof(*v), rmsg_cmp);
	*out = v;
	return n;
}

static void state_save(const char *dir, struct rmsg *v, int n, unsigned uv)
{
	char *p = xasprintf("%s/.epis-state", dir);
	struct buf b = { 0 };
	int i;

	buf_printf(&b, "uidvalidity %u\n", uv);
	for (i = 0; i < n; i++)
		buf_printf(&b, "%u %d\n", v[i].uid, v[i].flags);
	write_file(p, b.p ? b.p : "", b.len, 0600);
	buf_free(&b);
	free(p);
}

static int append_msg(struct imap *im, struct buf *box, const char *path)
{
	size_t len;
	char *data = read_file(path, &len), tag[16];
	struct buf fl = { 0 };
	int r = -1;

	if (!data)
		return -1;
	flags_to_list(&fl, maildir_flags_parse(path));
	snprintf(tag, sizeof(tag), "E%u", ++im->tag);
	conn_printf(im->c, "%s APPEND %s %s {%zu}\r\n", tag, box->p, fl.p, len);
	while (!read_resp(im)) {
		if (im->resp.p[0] == '+') {
			conn_write(im->c, data, len);
			conn_write(im->c, "\r\n", 2);
		} else if (startswith(im->resp.p, tag)) {
			r = strcaseprefix(im->resp.p + strlen(tag) + 1, "OK")
			    ? 0 : -1;
			break;
		}
	}
	free(data);
	buf_free(&fl);
	return r;
}

/* fetch the bodies of the given UIDs in batches */
static void fetch_new(struct imap *im, struct syncctx *sc, unsigned *uids,
		      int n, int verbose)
{
	struct buf set = { 0 };
	int i, j;

	for (i = 0; i < n; i += 50) {
		buf_reset(&set);
		for (j = i; j < n && j < i + 50; j++)
			buf_printf(&set, "%s%u", j > i ? "," : "", uids[j]);
		imap_cmd(im, on_fetch, sc, "UID FETCH %s (UID FLAGS BODY.PEEK[])",
			 set.p);
		if (verbose)
			fprintf(stderr, "\r  %d/%d", j, n);
	}
	if (verbose && n)
		fputc('\n', stderr);
	buf_free(&set);
}

/*
 * two-way synchronization of one mailbox with its local maildir using a
 * three-way merge of flags against the last synchronized state.
 */
int imap_sync_folder(struct imap *im, struct account *a, const char *fname,
		     char delim, int verbose)
{
	struct syncctx sc = { 0 };
	struct msginfo *loc;
	struct rmsg *base, *seen;
	struct buf box = { 0 }, cmd = { 0 }, del = { 0 };
	unsigned uv, *newuids = NULL;
	char *dec = mutf7_decode(fname, strlen(fname));
	char *local = folder_from_imap(dec, delim);
	char *dir = folder_dir(a->name, local);
	int nloc, nbase, i, nnew = 0, appended = 0;

	free(dec);
	maildir_make(dir);
	imap_quote(&box, fname);
	sc.dir = dir;
	if (imap_cmd(im, on_select, &sc, "SELECT %s", box.p))
		goto out;
	nbase = state_load(dir, &base, &uv);
	nloc = maildir_scan(dir, &loc, 0);

	for (i = 0; i < nloc; i++) {
		if (loc[i].uid && sc.uidvalidity != uv) {
			unlink(loc[i].path);
			loc[i].uid = (unsigned)-1;
		} else if (!loc[i].uid && !append_msg(im, &box, loc[i].path)) {
			unlink(loc[i].path);
			appended++;
		}
	}
	if (sc.uidvalidity != uv)
		nbase = 0;
	imap_cmd(im, on_fetch, &sc, "UID FETCH 1:* (UID FLAGS)");
	qsort(sc.r, sc.nr, sizeof(*sc.r), rmsg_cmp);
	seen = xcalloc(sc.nr + 1, sizeof(*seen));

	for (i = 0; i < nloc; i++) {
		struct rmsg *r, *s;
		int lf, bf;

		if (!loc[i].uid || loc[i].uid == (unsigned)-1)
			continue;
		r = rfind(sc.r, sc.nr, loc[i].uid);
		s = rfind(base, nbase, loc[i].uid);
		if (!r) {
			unlink(loc[i].path);
			continue;
		}
		seen[r - sc.r].uid = 1;
		if (loc[i].flags & F_TRASHED) {
			buf_printf(&del, "%s%u", del.len ? "," : "", r->uid);
			unlink(loc[i].path);
			continue;
		}
		lf = loc[i].flags;
		bf = s ? s->flags : r->flags;
		if (lf != bf) {
			buf_reset(&cmd);
			flags_to_list(&cmd, lf);
			imap_cmd(im, NULL, NULL, "UID STORE %u FLAGS.SILENT %s",
				 r->uid, cmd.p);
		} else if (r->flags != bf) {
			free(maildir_set_flags(loc[i].path, r->flags));
			lf = r->flags;
		}
		sc.st = xrealloc(sc.st, (sc.nst + 1) * sizeof(*sc.st));
		sc.st[sc.nst].uid = r->uid;
		sc.st[sc.nst++].flags = lf;
	}
	for (i = 0; i < sc.nr; i++) {
		if (seen[i].uid)
			continue;
		if (rfind(base, nbase, sc.r[i].uid)) {
			buf_printf(&del, "%s%u", del.len ? "," : "",
				   sc.r[i].uid);
		} else {
			newuids = xrealloc(newuids, (nnew + 1) *
					   sizeof(*newuids));
			newuids[nnew++] = sc.r[i].uid;
		}
	}
	if (del.len) {
		imap_cmd(im, NULL, NULL, "UID STORE %s +FLAGS.SILENT (\\Deleted)",
			 del.p);
		if (strcasestr_(im->caps, "UIDPLUS"))
			imap_cmd(im, NULL, NULL, "UID EXPUNGE %s", del.p);
		else
			imap_cmd(im, NULL, NULL, "EXPUNGE");
	}
	if (verbose)
		fprintf(stderr, "%s: %d new, %d uploaded, %s deleted\n", local,
			nnew, appended, del.len ? "some" : "none");
	fetch_new(im, &sc, newuids, nnew, verbose);
	qsort(sc.st, sc.nst, sizeof(*sc.st), rmsg_cmp);
	state_save(dir, sc.st, sc.nst, sc.uidvalidity);
	free(seen);
	free(base);
	free(newuids);
	msginfo_free(loc, nloc);
out:
	free(sc.r);
	free(sc.st);
	buf_free(&box);
	buf_free(&cmd);
	buf_free(&del);
	free(local);
	free(dir);
	return 0;
}

/* synchronize every selectable mailbox of an account, or just one */
int imap_sync_account(struct account *a, const char *only, int verbose)
{
	struct imap *im = imap_connect(a);
	struct folder *f;
	int n, i;

	if (!im)
		return -1;
	n = imap_list(im, &f);
	for (i = 0; i < n; i++) {
		char *loc = folder_from_imap(f[i].name, f[i].delim);

		if (!f[i].noselect && (!only || !strcmp(only, f[i].name) ||
				       !strcmp(only, loc)))
			imap_sync_folder(im, a, f[i].name, f[i].delim,
					 verbose);
		free(loc);
		free(f[i].name);
	}
	free(f);
	imap_close(im);
	return n < 0 ? -1 : 0;
}
