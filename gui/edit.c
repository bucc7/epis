/* edit.c - gap buffer text editor with vim emulation */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

static char *reg;
static int reg_lines;

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

char *ed_text(struct editor *e)
{
	size_t n = ed_len(e);
	char *s = xmalloc(n + 1);

	ed_copy(e, 0, n, s);
	return s;
}

void ed_init(struct editor *e, const char *text, int single)
{
	memset(e, 0, sizeof(*e));
	e->single = single;
	e->cap = 256;
	e->b = xmalloc(e->cap);
	e->ge = e->cap;
	e->mode = ED_INSERT;
	ed_set(e, text ? text : "");
	e->cur = 0;
}

void ed_free(struct editor *e)
{
	int i;

	free(e->b);
	for (i = 0; i < e->nundo; i++)
		free(e->undo[i].text);
	memset(e, 0, sizeof(*e));
}

/* replace the whole contents, putting the cursor at the end */
void ed_set(struct editor *e, const char *t)
{
	size_t n = strlen(t);

	e->gs = 0;
	e->ge = e->cap;
	gap_reserve(e, n);
	memcpy(e->b, t, n);
	e->gs = n;
	e->cur = n;
}

static void ed_del(struct editor *e, size_t a, size_t b)
{
	if (b > ed_len(e))
		b = ed_len(e);
	if (a >= b)
		return;
	gap_move(e, a);
	e->ge += b - a;
	e->cur = a;
}

/* insert text at the cursor */
void ed_insert(struct editor *e, const char *s, size_t n)
{
	gap_reserve(e, n);
	gap_move(e, e->cur);
	memcpy(e->b + e->gs, s, n);
	e->gs += n;
	e->cur += n;
}

static void snapshot(struct editor *e)
{
	if (e->nundo == 64) {
		free(e->undo[0].text);
		memmove(e->undo, e->undo + 1, 63 * sizeof(e->undo[0]));
		e->nundo--;
	}
	e->undo[e->nundo].text = ed_text(e);
	e->undo[e->nundo++].cur = e->cur;
}

static void undo(struct editor *e)
{
	if (!e->nundo)
		return;
	e->nundo--;
	ed_set(e, e->undo[e->nundo].text);
	e->cur = e->undo[e->nundo].cur;
	free(e->undo[e->nundo].text);
}

size_t ed_line_start(struct editor *e, size_t p)
{
	while (p > 0 && ed_at(e, p - 1) != '\n')
		p--;
	return p;
}

size_t ed_line_end(struct editor *e, size_t p)
{
	size_t n = ed_len(e);

	while (p < n && ed_at(e, p) != '\n')
		p++;
	return p;
}

int ed_line_of(struct editor *e, size_t p)
{
	size_t i;
	int l = 0;

	for (i = 0; i < p; i++)
		l += ed_at(e, i) == '\n';
	return l;
}

static int utf8_cont(char c)
{
	return ((unsigned char)c & 0xc0) == 0x80;
}

static size_t next_char(struct editor *e, size_t p)
{
	size_t n = ed_len(e);

	if (p < n)
		p++;
	while (p < n && utf8_cont(ed_at(e, p)))
		p++;
	return p;
}

static size_t prev_char(struct editor *e, size_t p)
{
	if (p > 0)
		p--;
	while (p > 0 && utf8_cont(ed_at(e, p)))
		p--;
	return p;
}

static int cls(char c)
{
	if (isspace((unsigned char)c))
		return 0;
	if (isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80)
		return 1;
	return 2;
}

static size_t word_fwd(struct editor *e, size_t p)
{
	size_t n = ed_len(e);
	int c = p < n ? cls(ed_at(e, p)) : 0;

	while (p < n && c && cls(ed_at(e, p)) == c)
		p++;
	while (p < n && !cls(ed_at(e, p)))
		p++;
	return p;
}

static size_t word_end(struct editor *e, size_t p)
{
	size_t n = ed_len(e);
	int c;

	if (p + 1 < n)
		p++;
	while (p < n && !cls(ed_at(e, p)))
		p++;
	c = p < n ? cls(ed_at(e, p)) : 0;
	while (p + 1 < n && cls(ed_at(e, p + 1)) == c)
		p++;
	return p;
}

static size_t word_back(struct editor *e, size_t p)
{
	int c;

	while (p > 0 && !cls(ed_at(e, p - 1)))
		p--;
	c = p > 0 ? cls(ed_at(e, p - 1)) : 0;
	while (p > 0 && cls(ed_at(e, p - 1)) == c && c)
		p--;
	return p;
}

static size_t col_of(struct editor *e, size_t p)
{
	return p - ed_line_start(e, p);
}

static size_t vmove(struct editor *e, size_t p, int dir)
{
	size_t ls = ed_line_start(e, p), le, col = e->want;

	if (dir < 0) {
		if (!ls)
			return p;
		ls = ed_line_start(e, ls - 1);
	} else {
		le = ed_line_end(e, p);
		if (le >= ed_len(e))
			return p;
		ls = le + 1;
	}
	le = ed_line_end(e, ls);
	return ls + col < le ? ls + col : le;
}

static void yank(struct editor *e, size_t a, size_t b, int lines)
{
	free(reg);
	reg = xmalloc(b - a + 1);
	ed_copy(e, a, b, reg);
	reg_lines = lines;
}

/* resolve a motion key into a target position; returns 0 if not a motion */
static int motion(struct editor *e, const char *k, size_t *to, int *lines)
{
	size_t p = e->cur, n = ed_len(e);

	*lines = 0;
	if (!strcmp(k, "h") || !strcmp(k, "<Left>"))
		*to = p > ed_line_start(e, p) ? prev_char(e, p) : p;
	else if (!strcmp(k, "l") || !strcmp(k, "<Right>"))
		*to = p < ed_line_end(e, p) ? next_char(e, p) : p;
	else if (!strcmp(k, "j") || !strcmp(k, "<Down>")) {
		*to = vmove(e, p, 1);
		*lines = 1;
	} else if (!strcmp(k, "k") || !strcmp(k, "<Up>")) {
		*to = vmove(e, p, -1);
		*lines = 1;
	} else if (!strcmp(k, "w"))
		*to = word_fwd(e, p);
	else if (!strcmp(k, "b"))
		*to = word_back(e, p);
	else if (!strcmp(k, "e"))
		*to = word_end(e, p);
	else if (!strcmp(k, "0") || !strcmp(k, "<Home>"))
		*to = ed_line_start(e, p);
	else if (!strcmp(k, "^")) {
		*to = ed_line_start(e, p);
		while (*to < n && (ed_at(e, *to) == ' ' ||
				   ed_at(e, *to) == '\t'))
			(*to)++;
	} else if (!strcmp(k, "$") || !strcmp(k, "<End>"))
		*to = ed_line_end(e, p);
	else if (!strcmp(k, "G")) {
		*to = ed_line_start(e, n);
		*lines = 1;
	} else if (!strcmp(k, "gg")) {
		*to = 0;
		*lines = 1;
	} else if (!strcmp(k, "}")) {
		*to = p;
		while (*to < n && !(ed_at(e, *to) == '\n' && *to + 1 < n &&
				    ed_at(e, *to + 1) == '\n'))
			(*to)++;
		if (*to < n)
			(*to)++;
	} else if (!strcmp(k, "{")) {
		*to = p;
		while (*to > 0 && !(ed_at(e, *to - 1) == '\n' && *to >= 2 &&
				    ed_at(e, *to - 2) == '\n'))
			(*to)--;
		if (*to > 0)
			(*to)--;
	} else
		return 0;
	return 1;
}

static int insert_key(struct editor *e, const char *k, const char *text,
		      int vim)
{
	size_t p = e->cur;

	if (!strcmp(k, "<Esc>")) {
		if (!vim)
			return ED_ESCAPE;
		e->mode = ED_NORMAL;
		if (p > ed_line_start(e, p))
			e->cur = prev_char(e, p);
		return ED_OK;
	}
	if (!strcmp(k, "<CR>")) {
		if (e->single)
			return ED_ENTER;
		snapshot(e);
		ed_insert(e, "\n", 1);
	} else if (!strcmp(k, "<BS>")) {
		if (p)
			ed_del(e, prev_char(e, p), p);
	} else if (!strcmp(k, "<Del>")) {
		ed_del(e, p, next_char(e, p));
		e->cur = p;
	} else if (!strcmp(k, "<C-w>")) {
		ed_del(e, word_back(e, p), p);
	} else if (!strcmp(k, "<C-u>")) {
		ed_del(e, ed_line_start(e, p), p);
	} else if (!strcmp(k, "<Left>")) {
		e->cur = prev_char(e, p);
	} else if (!strcmp(k, "<Right>")) {
		e->cur = next_char(e, p);
	} else if (!strcmp(k, "<Up>") || !strcmp(k, "<Down>")) {
		if (e->single)
			return ED_UNHANDLED;
		e->cur = vmove(e, p, k[2] == 'U' ? -1 : 1);
		return ED_OK;
	} else if (!strcmp(k, "<Home>")) {
		e->cur = ed_line_start(e, p);
	} else if (!strcmp(k, "<End>")) {
		e->cur = ed_line_end(e, p);
	} else if (!strcmp(k, "<Space>")) {
		ed_insert(e, " ", 1);
	} else if (!strcmp(k, "<Tab>") && !e->single) {
		ed_insert(e, "\t", 1);
	} else if (text && *text && (unsigned char)text[0] >= 0x20 &&
		   text[0] != 0x7f) {
		ed_insert(e, text, strlen(text));
	} else {
		return ED_UNHANDLED;
	}
	e->want = col_of(e, e->cur);
	return ED_OK;
}

static void line_range(struct editor *e, size_t a, size_t b, size_t *s,
		       size_t *t)
{
	size_t n = ed_len(e);

	*s = ed_line_start(e, a < b ? a : b);
	*t = ed_line_end(e, a < b ? b : a);
	if (*t < n)
		(*t)++;
	else if (*s > 0)
		(*s)--;
}

static void paste(struct editor *e, int before)
{
	size_t n;

	if (!reg)
		return;
	snapshot(e);
	n = strlen(reg);
	if (reg_lines) {
		size_t at = before ? ed_line_start(e, e->cur) :
		    ed_line_end(e, e->cur);

		e->cur = at;
		if (!before) {
			if (at < ed_len(e)) {
				e->cur = at + 1;
				ed_insert(e, reg, n);
				if (reg[n - 1] != '\n')
					ed_insert(e, "\n", 1);
			} else {
				ed_insert(e, "\n", 1);
				ed_insert(e, reg, reg[n - 1] == '\n' ? n - 1 :
					  n);
			}
		} else {
			ed_insert(e, reg, n);
			if (reg[n - 1] != '\n')
				ed_insert(e, "\n", 1);
		}
		e->cur = before ? at : ed_line_start(e, e->cur - 1);
		return;
	}
	if (!before && e->cur < ed_line_end(e, e->cur))
		e->cur = next_char(e, e->cur);
	ed_insert(e, reg, n);
	e->cur = prev_char(e, e->cur);
}

/* apply operator op (d, c, y) to [a, b) */
static void apply_op(struct editor *e, char op, size_t a, size_t b,
		     int lines)
{
	if (a > b) {
		size_t t = a;

		a = b;
		b = t;
	}
	yank(e, a, b, lines);
	if (op == 'y') {
		e->cur = a;
		return;
	}
	snapshot(e);
	ed_del(e, a, b);
	if (op == 'c') {
		if (lines) {
			ed_insert(e, "\n", 1);
			e->cur--;
		}
		e->mode = ED_INSERT;
	}
}

static int normal_key(struct editor *e, const char *k, const char *text)
{
	char seq[24];
	size_t to, s, t, n = ed_len(e);
	int lines, cnt, i;
	char op = 0;

	if (!strcmp(k, "<Esc>")) {
		if (e->pend[0] || e->count) {
			e->pend[0] = 0;
			e->count = 0;
			return ED_OK;
		}
		if (e->mode == ED_VISUAL || e->mode == ED_VLINE) {
			e->mode = ED_NORMAL;
			return ED_OK;
		}
		return ED_ESCAPE;
	}
	if (!strcmp(k, "<CR>") && e->single)
		return ED_ENTER;
	if (e->pend[0] == 'r' && !e->pend[1]) {
		e->pend[0] = 0;
		if (text && *text && e->cur < ed_line_end(e, e->cur)) {
			snapshot(e);
			ed_del(e, e->cur, next_char(e, e->cur));
			ed_insert(e, text, strlen(text));
			e->cur = prev_char(e, e->cur);
		}
		return ED_OK;
	}
	if (strlen(k) == 1 && isdigit((unsigned char)k[0]) &&
	    (k[0] != '0' || e->count) && !e->pend[0]) {
		e->count = e->count * 10 + k[0] - '0';
		return ED_OK;
	}
	snprintf(seq, sizeof(seq), "%s%s", e->pend, k);
	if (!strcmp(seq, "g") || !strcmp(seq, "d") || !strcmp(seq, "c") ||
	    !strcmp(seq, "y") || !strcmp(seq, "dg") || !strcmp(seq, "cg") ||
	    !strcmp(seq, "yg") || !strcmp(seq, "r")) {
		if (e->mode == ED_VISUAL || e->mode == ED_VLINE) {
			if (strchr("dcy", seq[0]) && !seq[1]) {
				int vl = e->mode == ED_VLINE;

				s = e->vstart;
				t = e->cur;
				if (vl) {
					line_range(e, s, t, &s, &t);
				} else {
					if (s > t) {
						size_t x = s;

						s = t;
						t = x;
					}
					t = next_char(e, t);
				}
				e->mode = ED_NORMAL;
				apply_op(e, seq[0], s, t, vl);
				return ED_OK;
			}
		}
		snprintf(e->pend, sizeof(e->pend), "%s", seq);
		return ED_OK;
	}
	cnt = e->count ? e->count : 1;
	e->count = 0;
	if (strchr("dcy", e->pend[0]) && e->pend[0]) {
		op = e->pend[0];
		memmove(seq, seq + 1, strlen(seq));
	}
	e->pend[0] = 0;
	if (op && seq[0] == op && !seq[1]) {
		s = e->cur;
		for (i = 1; i < cnt; i++)
			s = vmove(e, s, 1);
		line_range(e, e->cur, s, &s, &t);
		apply_op(e, op, s, t, 1);
		return ED_OK;
	}
	if (motion(e, seq, &to, &lines)) {
		for (i = 1; i < cnt; i++) {
			size_t c = e->cur;

			e->cur = to;
			motion(e, seq, &to, &lines);
			e->cur = c;
		}
		if (op) {
			if (lines) {
				line_range(e, e->cur, to, &s, &t);
				apply_op(e, op, s, t, 1);
			} else {
				if (!strcmp(seq, "e") || !strcmp(seq, "$"))
					to = to < n ? next_char(e, to) : to;
				if (op == 'c' && !strcmp(seq, "w")) {
					to = e->cur;
					to = word_end(e, to);
					to = to < n ? next_char(e, to) : to;
				}
				apply_op(e, op, e->cur, to, 0);
			}
			return ED_OK;
		}
		e->cur = to;
		if (!lines)
			e->want = col_of(e, e->cur);
		return ED_OK;
	}
	if (op)
		return ED_OK;
	if (!strcmp(seq, "i")) {
		snapshot(e);
		e->mode = ED_INSERT;
	} else if (!strcmp(seq, "a")) {
		snapshot(e);
		if (e->cur < ed_line_end(e, e->cur))
			e->cur = next_char(e, e->cur);
		e->mode = ED_INSERT;
	} else if (!strcmp(seq, "I")) {
		snapshot(e);
		e->cur = ed_line_start(e, e->cur);
		e->mode = ED_INSERT;
	} else if (!strcmp(seq, "A")) {
		snapshot(e);
		e->cur = ed_line_end(e, e->cur);
		e->mode = ED_INSERT;
	} else if ((!strcmp(seq, "o") || !strcmp(seq, "O")) && !e->single) {
		snapshot(e);
		if (seq[0] == 'o') {
			e->cur = ed_line_end(e, e->cur);
			ed_insert(e, "\n", 1);
		} else {
			e->cur = ed_line_start(e, e->cur);
			ed_insert(e, "\n", 1);
			e->cur--;
		}
		e->mode = ED_INSERT;
	} else if (!strcmp(seq, "x") || !strcmp(seq, "<Del>")) {
		if (e->cur < ed_line_end(e, e->cur)) {
			to = e->cur;
			for (i = 0; i < cnt; i++)
				if (to < ed_line_end(e, e->cur))
					to = next_char(e, to);
			apply_op(e, 'd', e->cur, to, 0);
		}
	} else if (!strcmp(seq, "X")) {
		if (e->cur > ed_line_start(e, e->cur))
			apply_op(e, 'd', prev_char(e, e->cur), e->cur, 0);
	} else if (!strcmp(seq, "D") || !strcmp(seq, "C")) {
		apply_op(e, seq[0] == 'D' ? 'd' : 'c', e->cur,
			 ed_line_end(e, e->cur), 0);
	} else if (!strcmp(seq, "J") && !e->single) {
		to = ed_line_end(e, e->cur);
		if (to < n) {
			snapshot(e);
			e->cur = to;
			ed_del(e, to, to + 1);
			ed_insert(e, " ", 1);
			e->cur = to;
		}
	} else if (!strcmp(seq, "p") || !strcmp(seq, "P")) {
		paste(e, seq[0] == 'P');
	} else if (!strcmp(seq, "u")) {
		undo(e);
	} else if (!strcmp(seq, "v")) {
		e->mode = e->mode == ED_VISUAL ? ED_NORMAL : ED_VISUAL;
		e->vstart = e->cur;
	} else if (!strcmp(seq, "V")) {
		e->mode = e->mode == ED_VLINE ? ED_NORMAL : ED_VLINE;
		e->vstart = e->cur;
	} else if (!strcmp(seq, "x") && e->mode == ED_VISUAL) {
		return normal_key(e, "d", text);
	} else if (!strcmp(seq, "r")) {
		strcpy(e->pend, "r");
	} else {
		return ED_UNHANDLED;
	}
	return ED_OK;
}

/*
 * feed one key token to the editor. key is the normalized token such as
 * "j" or "<CR>", text the characters it produced. Returns ED_UNHANDLED
 * when the key means nothing to the editor.
 */
int ed_key(struct editor *e, const char *key, const char *text, int vim)
{
	if (!vim)
		e->mode = ED_INSERT;
	if (e->mode == ED_INSERT)
		return insert_key(e, key, text, vim);
	return normal_key(e, key, text);
}
