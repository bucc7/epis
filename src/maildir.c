/* maildir.c - Maildir storage */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "epis.h"

static const struct {
	char c;
	int f;
} flagmap[] = {
	{ 'D', F_DRAFT }, { 'F', F_FLAGGED }, { 'P', F_PASSED },
	{ 'R', F_REPLIED }, { 'S', F_SEEN }, { 'T', F_TRASHED }
};

#define NFLAGS (int)(sizeof(flagmap) / sizeof(*flagmap))

/* directory of a folder: <maildir>/<account>/<folder> */
char *folder_dir(const char *acc, const char *folder)
{
	return xasprintf("%s/%s/%s", cfg.maildir, acc, folder);
}

/* map an IMAP mailbox name to a flat local folder name */
char *folder_from_imap(const char *name, char delim)
{
	char *s = xstrdup(name), *p;

	for (p = s; *p; p++)
		if (*p == delim || *p == '/')
			*p = '.';
	return s;
}

int maildir_make(const char *dir)
{
	char *p;
	int r = mkdirs(dir, 0700);
	const char *sub[] = { "cur", "new", "tmp" };
	int i;

	for (i = 0; i < 3; i++) {
		p = xasprintf("%s/%s", dir, sub[i]);
		r |= mkdirs(p, 0700);
		free(p);
	}
	return r;
}

int maildir_flags_parse(const char *path)
{
	const char *p = strrchr(path, '/');
	int f = 0, i;

	p = strstr(p ? p : path, ":2,");
	if (!p)
		return 0;
	for (p += 3; *p; p++)
		for (i = 0; i < NFLAGS; i++)
			if (*p == flagmap[i].c)
				f |= flagmap[i].f;
	return f;
}

void maildir_flags_str(int flags, char *out)
{
	int i;

	for (i = 0; i < NFLAGS; i++)
		if (flags & flagmap[i].f)
			*out++ = flagmap[i].c;
	*out = 0;
}

/* UID stored in the file name as ,U=<uid> (0 when not yet uploaded) */
unsigned maildir_uid(const char *path)
{
	const char *p = strrchr(path, '/');
	const char *u = strstr(p ? p : path, ",U=");

	return u ? (unsigned)strtoul(u + 3, NULL, 10) : 0;
}

static unsigned counter;

static char *unique_name(unsigned uid)
{
	char host[64] = "localhost", *p;

	gethostname(host, sizeof(host) - 1);
	for (p = host; *p; p++)
		if (*p == '/' || *p == ':')
			*p = '_';
	if (uid)
		return xasprintf("%ld.%d_%u.%s,U=%u", (long)time(NULL),
				 (int)getpid(), counter++, host, uid);
	return xasprintf("%ld.%d_%u.%s", (long)time(NULL), (int)getpid(),
			 counter++, host);
}

/* store a message in dir/cur, returns the new path */
char *maildir_store(const char *dir, const char *data, size_t len, int flags,
		    unsigned uid)
{
	char fl[8], *name = unique_name(uid);
	char *tmp = xasprintf("%s/tmp/%s", dir, name), *dst;

	maildir_flags_str(flags, fl);
	dst = xasprintf("%s/cur/%s:2,%s", dir, name, fl);
	free(name);
	maildir_make(dir);
	if (write_file(tmp, data, len, 0600) || rename(tmp, dst)) {
		unlink(tmp);
		free(tmp);
		free(dst);
		return NULL;
	}
	free(tmp);
	return dst;
}

/* rename a message to carry the given flags, moving it to cur */
char *maildir_set_flags(const char *path, int flags)
{
	const char *slash = strrchr(path, '/');
	char fl[8], *base, *dir, *dst, *colon;

	if (!slash)
		return NULL;
	base = xstrdup(slash + 1);
	if ((colon = strstr(base, ":2,")))
		*colon = 0;
	dir = xstrndup(path, slash - path);
	if (strlen(dir) >= 4 && !strcmp(dir + strlen(dir) - 4, "/new"))
		memcpy(dir + strlen(dir) - 3, "cur", 3);
	maildir_flags_str(flags, fl);
	dst = xasprintf("%s/%s:2,%s", dir, base, fl);
	free(base);
	free(dir);
	if (strcmp(path, dst) && rename(path, dst)) {
		free(dst);
		return NULL;
	}
	return dst;
}

/* move a message to another maildir, dropping its UID */
char *maildir_move(const char *path, const char *dstdir)
{
	size_t len;
	char *data = read_file(path, &len), *dst;

	if (!data)
		return NULL;
	dst = maildir_store(dstdir, data, len,
			    maildir_flags_parse(path) & ~F_TRASHED, 0);
	free(data);
	if (dst)
		unlink(path);
	return dst;
}

/* stable message id: "<account>/<folder>/<unique name>" */
char *maildir_id(const char *path)
{
	size_t l = strlen(cfg.maildir);
	const char *p = path, *name = strrchr(path, '/');
	char *colon, *id, *box;

	if (!strncmp(path, cfg.maildir, l) && path[l] == '/')
		p = path + l + 1;
	box = xstrndup(p, name - p);
	if (strlen(box) > 4)
		box[strlen(box) - 4] = 0;
	id = xasprintf("%s/%s", box, name + 1);
	if ((colon = strstr(id + strlen(box), ":2,")))
		*colon = 0;
	free(box);
	return id;
}

/* find the current path of a message id */
char *maildir_resolve(const char *id)
{
	const char *name = strrchr(id, '/');
	const char *sub[] = { "cur", "new" };
	char *box, *dir, *res = NULL;
	struct dirent *de;
	size_t nl;
	DIR *d;
	int i;

	if (!name)
		return access(id, R_OK) ? NULL : xstrdup(id);
	if (id[0] == '/' && !access(id, R_OK))
		return xstrdup(id);
	box = xstrndup(id, name - id);
	nl = strlen(++name);
	for (i = 0; i < 2 && !res; i++) {
		dir = xasprintf("%s/%s/%s", cfg.maildir, box, sub[i]);
		if ((d = opendir(dir))) {
			while ((de = readdir(d)))
				if (!strncmp(de->d_name, name, nl) &&
				    (!de->d_name[nl] || de->d_name[nl] == ':')) {
					res = xasprintf("%s/%s", dir,
							de->d_name);
					break;
				}
			closedir(d);
		}
		free(dir);
	}
	free(box);
	return res;
}

/* read just the header block of a message */
static char *read_headers(const char *path)
{
	FILE *f = fopen(path, "rb");
	struct buf b = { 0 };
	char line[4096];

	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '\n' || (line[0] == '\r' && line[1] == '\n'))
			break;
		buf_puts(&b, line);
		if (b.len > 65536)
			break;
	}
	fclose(f);
	buf_add(&b, "", 0);
	return b.p;
}

/* list the messages of a maildir; headers != 0 also parses headers */
int maildir_scan(const char *dir, struct msginfo **out, int headers)
{
	const char *sub[] = { "new", "cur" };
	struct msginfo *m = NULL;
	struct dirent *de;
	int n = 0, i;
	DIR *d;

	for (i = 0; i < 2; i++) {
		char *sd = xasprintf("%s/%s", dir, sub[i]);

		if (!(d = opendir(sd))) {
			free(sd);
			continue;
		}
		while ((de = readdir(d))) {
			struct msginfo *mi;

			if (de->d_name[0] == '.')
				continue;
			m = xrealloc(m, (n + 1) * sizeof(*m));
			mi = &m[n++];
			memset(mi, 0, sizeof(*mi));
			mi->path = xasprintf("%s/%s", sd, de->d_name);
			mi->flags = maildir_flags_parse(de->d_name);
			mi->uid = maildir_uid(de->d_name);
			if (headers) {
				char *h = read_headers(mi->path), *v;

				mi->from = hdr_decode((v = hdr_get(h, "From"))
						      ? v : "");
				free(v);
				mi->to = hdr_decode((v = hdr_get(h, "To")) ?
						    v : "");
				free(v);
				mi->subject = hdr_decode((v = hdr_get(h,
					"Subject")) ? v : "");
				free(v);
				mi->msgid = hdr_get(h, "Message-ID");
				v = hdr_get(h, "Date");
				mi->date = v ? parse_date(v) : 0;
				free(v);
				free(h);
			}
		}
		closedir(d);
		free(sd);
	}
	*out = m;
	return n;
}

void msginfo_free(struct msginfo *m, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		free(m[i].path);
		free(m[i].from);
		free(m[i].to);
		free(m[i].subject);
		free(m[i].msgid);
	}
	free(m);
}

static int date_cmp(const void *a, const void *b)
{
	const struct msginfo *x = a, *y = b;

	return x->date < y->date ? 1 : x->date > y->date ? -1 : 0;
}

void sort_by_date(struct msginfo *m, int n)
{
	qsort(m, n, sizeof(*m), date_cmp);
}

static int str_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* list the local folders of an account, sorted with INBOX first */
int maildir_folders(const char *acc, char ***out)
{
	char *dir = xasprintf("%s/%s", cfg.maildir, acc), **v = NULL;
	struct dirent *de;
	int n = 0, i;
	DIR *d = opendir(dir);

	if (d) {
		while ((de = readdir(d))) {
			char *cur = xasprintf("%s/%s/cur", dir, de->d_name);

			if (de->d_name[0] != '.' && !access(cur, F_OK)) {
				v = xrealloc(v, (n + 1) * sizeof(*v));
				v[n++] = xstrdup(de->d_name);
			}
			free(cur);
		}
		closedir(d);
	}
	free(dir);
	qsort(v, n, sizeof(*v), str_cmp);
	for (i = 0; i < n; i++)
		if (!strcmp(v[i], "INBOX") && i) {
			char *t = v[i];

			memmove(v + 1, v, i * sizeof(*v));
			v[0] = t;
		}
	*out = v;
	return n;
}

/*
 * move a message to the account trash folder, or mark it trashed when it
 * already is in the trash or there is no trash folder. Returns the new path.
 */
char *maildir_trash(struct account *a, const char *path)
{
	char *dir = folder_dir(a->name, a->trash);
	char *cur = xasprintf("%s/cur", dir), *np;

	if (strncmp(path, dir, strlen(dir)) && !access(cur, F_OK))
		np = maildir_move(path, dir);
	else
		np = maildir_set_flags(path, maildir_flags_parse(path) |
				       F_TRASHED);
	free(dir);
	free(cur);
	return np;
}
