/* edit.c - gap buffer with an undo log */
#include <stdlib.h>
#include <string.h>

#include "editor.h"

static void gap_move(struct editor *e, size_t pos)
{
	if (pos < e->gs) {
		size_t n = e->gs - pos;

		memmove(e->b + e->ge - n, e->b + pos, n);
		e->gs -= n;
		e->ge -= n;
	} else if (pos > e->gs) {
		size_t n = pos - e->gs;

		memmove(e->b + e->gs, e->b + e->ge, n);
		e->gs += n;
		e->ge += n;
	}
}

static void gap_reserve(struct editor *e, size_t n)
{
	size_t tail, nc;
	char *nb;

	if (e->ge - e->gs >= n)
		return;
	tail = e->cap - e->ge;
	nc = (e->cap + n) * 2;
	nb = xmalloc(nc);
	memcpy(nb, e->b, e->gs);
	memcpy(nb + nc - tail, e->b + e->ge, tail);
	free(e->b);
	e->b = nb;
	e->ge = nc - tail;
	e->cap = nc;
}

size_t ed_len(struct editor *e)
{
	return e->cap - (e->ge - e->gs);
}

char ed_at(struct editor *e, size_t i)
{
	return i < e->gs ? e->b[i] : e->b[i + e->ge - e->gs];
}

/* copy [a, b) into out, which must hold b - a + 1 bytes */
void ed_copy(struct editor *e, size_t a, size_t b, char *out)
{
	size_t i;

	for (i = a; i < b; i++)
		*out++ = ed_at(e, i);
	*out = 0;
}

char *ed_slice(struct editor *e, size_t a, size_t b)
{
	char *s;

	if (b < a)
		b = a;
	s = xmalloc(b - a + 1);
	ed_copy(e, a, b, s);
	return s;
}

char *ed_text(struct editor *e)
{
	return ed_slice(e, 0, ed_len(e));
}

/* replace bytes without logging; marks move with the text */
static void splice(struct editor *e, size_t pos, size_t ndel, const char *s,
		   size_t n)
{
	int i;

	gap_move(e, pos);
	e->ge += ndel;
	gap_reserve(e, n);
	if (n)
		memcpy(e->b + e->gs, s, n);
	e->gs += n;
	for (i = 0; i < NMARKS; i++) {
		size_t *m = &e->mark[i];

		if (*m == NOMARK || *m < pos)
			continue;
		if (*m >= pos + ndel)
			*m = *m - ndel + n;
		else
			*m = pos;
	}
	e->seq++;
}

static void log_free(struct editor *e, int from)
{
	int i;

	for (i = from; i < e->nlog; i++) {
		free(e->log[i].del);
		free(e->log[i].ins);
	}
	e->nlog = from;
}

/* replace ndel bytes at pos with n bytes of s as an undoable change */
void ed_splice(struct editor *e, size_t pos, size_t ndel, const char *s,
	       size_t n)
{
	struct change *c;

	if (pos > ed_len(e))
		pos = ed_len(e);
	if (ndel > ed_len(e) - pos)
		ndel = ed_len(e) - pos;
	if (!ndel && !n)
		return;
	log_free(e, e->applied);
	c = e->nlog ? &e->log[e->nlog - 1] : NULL;
	/* typing extends the previous entry instead of adding one */
	if (c && c->group == e->group && !ndel && !c->ndel &&
	    pos == c->pos + c->nins) {
		c->ins = xrealloc(c->ins, c->nins + n);
		memcpy(c->ins + c->nins, s, n);
		c->nins += n;
	} else {
		if (e->nlog == e->logcap) {
			e->logcap = e->logcap * 2 + 32;
			e->log = xrealloc(e->log, e->logcap * sizeof(*e->log));
		}
		c = &e->log[e->nlog++];
		c->pos = pos;
		c->cur = e->cur;
		c->ndel = ndel;
		c->nins = n;
		c->group = e->group;
		c->del = ed_slice(e, pos, pos + ndel);
		c->ins = xmalloc(n);
		if (n)
			memcpy(c->ins, s, n);
		e->applied = e->nlog;
	}
	splice(e, pos, ndel, s, n);
}

/* start a new undo step */
void ed_checkpoint(struct editor *e)
{
	e->group++;
}

/* undo the latest step, leaving the cursor where it began */
int ed_undo(struct editor *e)
{
	unsigned g;
	size_t at = NOMARK, saved = 0;
	int line, was;

	if (!e->applied)
		return 0;
	g = e->log[e->applied - 1].group;
	while (e->applied && e->log[e->applied - 1].group == g) {
		struct change *c = &e->log[--e->applied];
		/* a removed line break belongs to the line after it */
		size_t p = c->pos + (c->ndel && c->del[0] == '\n');

		splice(e, c->pos, c->nins, c->del, c->ndel);
		if (p < at)
			at = p;
		saved = c->cur;
	}
	if (saved > ed_len(e))
		saved = ed_len(e);
	line = ed_line_of(e, at);
	was = ed_line_of(e, saved);
	e->cur = was == line || was == line - 1 ? saved :
	    ed_first_nonblank(e, at);
	return 1;
}

int ed_redo(struct editor *e)
{
	unsigned g;
	size_t at = NOMARK;

	if (e->applied == e->nlog)
		return 0;
	g = e->log[e->applied].group;
	while (e->applied < e->nlog && e->log[e->applied].group == g) {
		struct change *c = &e->log[e->applied++];

		splice(e, c->pos, c->ndel, c->ins, c->nins);
		if (c->pos < at)
			at = c->pos;
	}
	e->cur = at;
	return 1;
}

/* insert text at the cursor and move past it */
void ed_insert(struct editor *e, const char *s, size_t n)
{
	ed_splice(e, e->cur, 0, s, n);
	e->cur += n;
}

/* delete [a, b), leaving the cursor at a */
void ed_remove(struct editor *e, size_t a, size_t b)
{
	if (b > ed_len(e))
		b = ed_len(e);
	if (a >= b)
		return;
	ed_splice(e, a, b - a, NULL, 0);
	e->cur = a;
}

void ed_init(struct editor *e, const char *text, int single)
{
	memset(e, 0, sizeof(*e));
	e->single = single;
	e->cap = 256;
	e->b = xmalloc(e->cap);
	e->ge = e->cap;
	e->mode = ED_INSERT;
	e->vis = 20;
	e->cols = 80;
	ed_set(e, text ? text : "");
	e->cur = 0;
}

void ed_free(struct editor *e)
{
	log_free(e, 0);
	free(e->log);
	free(e->b);
	free(e->pend.k);
	free(e->typed.k);
	buf_free(&e->over);
	memset(e, 0, sizeof(*e));
}

/* replace the whole contents and forget the history */
void ed_set(struct editor *e, const char *t)
{
	size_t n = strlen(t);
	int i;

	log_free(e, 0);
	e->applied = 0;
	e->gs = 0;
	e->ge = e->cap;
	gap_reserve(e, n);
	memcpy(e->b, t, n);
	e->gs = n;
	e->cur = n;
	e->vstart = 0;
	e->pend.n = 0;
	for (i = 0; i < NMARKS; i++)
		e->mark[i] = NOMARK;
}

static int utf8_cont(char c)
{
	return ((unsigned char)c & 0xc0) == 0x80;
}

size_t ed_next(struct editor *e, size_t p)
{
	size_t n = ed_len(e);

	if (p < n)
		p++;
	while (p < n && utf8_cont(ed_at(e, p)))
		p++;
	return p;
}

size_t ed_prev(struct editor *e, size_t p)
{
	if (p > 0)
		p--;
	while (p > 0 && utf8_cont(ed_at(e, p)))
		p--;
	return p;
}

size_t ed_line_start(struct editor *e, size_t p)
{
	while (p > 0 && ed_at(e, p - 1) != '\n')
		p--;
	return p;
}

/*
 * the first line break in [p, end), or end. memchr on each side of the gap
 * is far faster than looking at one byte at a time.
 */
static size_t next_break(struct editor *e, size_t p, size_t end)
{
	const char *at, *q;

	if (p < e->gs) {
		size_t stop = end < e->gs ? end : e->gs;

		if ((q = memchr(e->b + p, '\n', stop - p)))
			return q - e->b;
		p = stop;
	}
	if (p >= end)
		return end;
	at = e->b + p + (e->ge - e->gs);
	q = memchr(at, '\n', end - p);
	return q ? p + (size_t)(q - at) : end;
}

size_t ed_line_end(struct editor *e, size_t p)
{
	return next_break(e, p, ed_len(e));
}

/* the 0 based line that holds p */
int ed_line_of(struct editor *e, size_t p)
{
	size_t i;
	int l = 0;

	for (i = next_break(e, 0, p); i < p; i = next_break(e, i + 1, p))
		l++;
	return l;
}

int ed_lines(struct editor *e)
{
	return ed_line_of(e, ed_len(e)) + 1;
}

/* where a 0 based line starts; lines outside the text are clamped */
size_t ed_line_pos(struct editor *e, int line)
{
	size_t i, n = ed_len(e), last = 0;

	for (i = next_break(e, 0, n); i < n && line > 0;
	     i = next_break(e, i + 1, n)) {
		last = i + 1;
		line--;
	}
	return last;
}

/* characters between the start of the line and p */
size_t ed_col(struct editor *e, size_t p)
{
	size_t i = ed_line_start(e, p), n = 0;

	for (; i < p; i = ed_next(e, i))
		n++;
	return n;
}

/* the position of a column, or the end of the line if it is shorter */
size_t ed_at_col(struct editor *e, size_t ls, size_t col)
{
	size_t n = ed_len(e);

	while (col && ls < n && ed_at(e, ls) != '\n') {
		ls = ed_next(e, ls);
		col--;
	}
	return ls;
}

size_t ed_first_nonblank(struct editor *e, size_t p)
{
	size_t n = ed_len(e);

	p = ed_line_start(e, p);
	while (p < n && (ed_at(e, p) == ' ' || ed_at(e, p) == '\t'))
		p++;
	return p;
}

/* screen cells taken by the character at p in a fixed width font */
static int cells(struct editor *e, size_t p)
{
	char s[8];
	uint32_t c;

	if (ed_at(e, p) == '\t')
		return 4;
	if (!((unsigned char)ed_at(e, p) & 0x80))
		return 1;
	ed_copy(e, p, ed_next(e, p), s);
	utf8_decode(s, &c);
	return (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
	    (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
	    (c >= 0xff00 && c <= 0xff60) || c >= 0x1f300 ? 2 : 1;
}

/*
 * where the screen row starting at p ends on a line that ends at end,
 * breaking after a blank when there is one
 */
size_t ed_wrap(struct editor *e, size_t p, size_t end, int cols)
{
	size_t i, brk = 0;
	int w = 0;

	if (cols < 4)
		cols = 4;
	for (i = p; i < end; i = ed_next(e, i)) {
		w += cells(e, i);
		if (w > cols)
			return brk ? brk : i;
		if (ed_at(e, i) == ' ' || ed_at(e, i) == '\t')
			brk = i + 1;
	}
	return end;
}
