/* vim.c - modal editing: commands, motions, operators, registers, macros */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "vim.h"

#define MAXKEYS 200000		/* a macro that runs longer is stopped */

struct vimstate vim;

struct reg {
	char *s;
	int type;
};

static struct reg regs[128];
/* the system clipboard as last seen, to notice copies made elsewhere */
static char *seen;

/* the last change, repeated by "." */
struct change_cmd {
	struct keys k;
	int reg, count;
	/* shape of the selection for a change made in visual mode */
	int vmode, vlines;
	size_t vcols;
};

static struct change_cmd dot, next_dot;
/* set while a motion whose deletes always go to "1 is applied */
static int big_delete;
static int dot_open, replaying, hold;
static unsigned dot_seq;

/* keys waiting to be typed by a macro */
static struct keys ahead;
static int ahead_pos, ahead_steps;
static int rec_reg, last_macro;
static struct buf rec_buf;

static char find_cmd, find_arg[KEYLEN];

static char *history[2][32];
static int nhistory[2], hist_at = -1;

/* a parsed normal or visual mode command */
struct cmd {
	int reg, count;
	/* index of the first key after the register and count */
	int start;
	/* keys [c2a, c2b) hold a count typed after the operator */
	int c2a, c2b;
	char op[4], key[KEYLEN], arg[KEYLEN];
	int doubled;
	/* index of an unfinished "/", "?" or ":" prompt, or -1 */
	int prompt;
	struct buf text;
};

enum { P_MORE, P_OK, P_BAD, P_CANCEL };

static int process(struct editor *e, const char *k);

void vim_msg(int error, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	buf_reset(&vim.msg);
	buf_puts(&vim.msg, tmp);
	vim.msg_err = error;
}

/* the message of the last command, if it left one */
const char *vim_message(int *error)
{
	static struct buf out;

	if (!vim.msg.len)
		return NULL;
	buf_reset(&out);
	buf_puts(&out, vim.msg.p);
	buf_reset(&vim.msg);
	*error = vim.msg_err;
	return out.p;
}

const char *vim_command(void)
{
	return vim.command.p ? vim.command.p : "";
}

/* start an undo step, unless a longer command is grouping its changes */
void vim_checkpoint(struct editor *e)
{
	if (!hold)
		ed_checkpoint(e);
}

/* ---- key tokens ---- */

static void keys_add(struct keys *k, const char *s)
{
	if (k->n == k->cap) {
		k->cap = k->cap * 2 + 8;
		k->k = xrealloc(k->k, k->cap * sizeof(*k->k));
	}
	snprintf(k->k[k->n++], KEYLEN, "%s", s);
}

static void keys_copy(struct keys *dst, const struct keys *src, int from)
{
	int i;

	dst->n = 0;
	for (i = from; i < src->n; i++)
		keys_add(dst, src->k[i]);
}

static int is(const char *a, const char *b)
{
	return !strcmp(a, b);
}

/* whether k is one of the space separated words of set */
static int among(const char *k, const char *set)
{
	size_t l = strlen(k);
	const char *p = set;

	if (!l)
		return 0;
	while ((p = strstr(p, k))) {
		if ((p == set || p[-1] == ' ') && (!p[l] || p[l] == ' '))
			return 1;
		p++;
	}
	return 0;
}

/* the text a key types, or NULL for keys that type nothing */
static const char *key_char(const char *k)
{
	if (k[0] != '<' || !k[1])
		return (unsigned char)k[0] >= 0x20 && k[0] != 0x7f ? k : NULL;
	if (is(k, "<Space>"))
		return " ";
	if (is(k, "<lt>"))
		return "<";
	if (is(k, "<Tab>"))
		return "\t";
	return NULL;
}

/* fold the different spellings of a key into one */
static const char *normalize(const char *k)
{
	static const char *same[][2] = {
		{ "<C-[>", "<Esc>" }, { "<C-c>", "<Esc>" }, { "<C-h>", "<BS>" },
		{ "<S-CR>", "<CR>" }, { "<S-Space>", "<Space>" },
		{ "<S-BS>", "<BS>" }, { "<lt>", "<" }, { "<C-j>", "<CR>" },
	};
	size_t i;

	for (i = 0; i < sizeof(same) / sizeof(*same); i++)
		if (is(k, same[i][0]))
			return same[i][1];
	return k;
}

/*
 * macros live in registers as text, like in vim: control keys as control
 * bytes, other special keys as 0x1f followed by their name
 */
static void encode(struct buf *b, const char *k)
{
	const char *c = key_char(k);

	if (c)
		buf_puts(b, c);
	else if (is(k, "<Esc>"))
		buf_putc(b, 0x1b);
	else if (is(k, "<CR>"))
		buf_putc(b, '\r');
	else if (is(k, "<BS>"))
		buf_putc(b, '\b');
	else if (strlen(k) == 5 && !strncmp(k, "<C-", 3) &&
		 k[3] >= 'a' && k[3] <= 'z' && !strchr("hijm", k[3]))
		buf_putc(b, k[3] - 'a' + 1);
	else
		buf_printf(b, "\x1f%s", k);
}

static void decode(struct keys *out, const char *s)
{
	while (*s) {
		unsigned char c = *s;
		char t[KEYLEN];
		uint32_t cp;
		int n = 1;

		if (c == 0x1f && s[1] == '<' && strchr(s + 1, '>')) {
			n = strchr(s + 1, '>') - s + 1;
			snprintf(t, sizeof(t), "%.*s", n - 1, s + 1);
		} else if (c == 0x1b) {
			strcpy(t, "<Esc>");
		} else if (c == '\r' || c == '\n') {
			strcpy(t, "<CR>");
		} else if (c == '\b' || c == 0x7f) {
			strcpy(t, "<BS>");
		} else if (c == '\t') {
			strcpy(t, "<Tab>");
		} else if (c >= 1 && c <= 26) {
			snprintf(t, sizeof(t), "<C-%c>", 'a' + c - 1);
		} else if (c == ' ') {
			strcpy(t, "<Space>");
		} else {
			n = utf8_decode(s, &cp);
			snprintf(t, sizeof(t), "%.*s", n, s);
		}
		keys_add(out, t);
		s += n;
	}
}

/* ---- registers ---- */

/* the system clipboard with Unix line endings, or NULL */
char *ed_clipboard(void)
{
	char *s = plat_clip_get(), *r, *w;

	if (!s)
		return NULL;
	for (r = w = s; *r; r++)
		if (*r != '\r' || r[1] != '\n')
			*w++ = *r;
	*w = 0;
	return s;
}

static int line_type(const char *s)
{
	return *s && s[strlen(s) - 1] == '\n' ? R_LINE : R_CHAR;
}

static void reg_put(int name, const char *s, int type)
{
	char *c = xstrdup(s);

	free(regs[name & 127].s);
	regs[name & 127].s = c;
	regs[name & 127].type = type;
}

/* store into a named register, appending for an upper case name */
static int reg_named(int name, const char *s, int type)
{
	if (isupper(name)) {
		struct reg *r = &regs[tolower(name)];
		const char *old = r->s ? r->s : "";
		int line = type == R_LINE || (r->s && r->type == R_LINE);
		char *all = xasprintf("%s%s%s", old, line && *old &&
				      old[strlen(old) - 1] != '\n' ? "\n" : "",
				      s);

		name = tolower(name);
		reg_put(name, all, line ? R_LINE : type);
		free(all);
	} else {
		reg_put(name, s, type);
	}
	return name;
}

/* put text that was yanked or deleted into registers as vim does */
void vim_set_register(int name, const char *s, int type, int yank)
{
	int i, slot;

	if (name == '_')
		return;
	if (name == '+' || name == '*') {
		plat_clip_set(s);
		free(seen);
		seen = xstrdup(s);
		reg_put('"', s, type);
		return;
	}
	if (name && name != '"') {
		slot = reg_named(name, s, type);
		reg_put('"', regs[slot].s, regs[slot].type);
		return;
	}
	if (yank) {
		reg_put('0', s, type);
	} else if (type == R_LINE || strchr(s, '\n') || big_delete) {
		for (i = '9'; i > '1'; i--)
			if (regs[i - 1].s)
				reg_put(i, regs[i - 1].s, regs[i - 1].type);
		reg_put('1', s, type);
		if (type != R_LINE && !strchr(s, '\n'))
			reg_put('-', s, type);
	} else {
		reg_put('-', s, type);
	}
	reg_put('"', s, type);
	if (!cfg.clipboard)
		return;
	free(seen);
	if (yank) {
		plat_clip_set(s);
		seen = xstrdup(s);
	} else {
		/* deleted text stays local until something else is copied */
		seen = ed_clipboard();
	}
}

/* the contents of a register, or NULL when it is empty */
const char *vim_register(int name, int *type)
{
	char *c;

	if (!name)
		name = '"';
	if (name == '+' || name == '*') {
		if (!(c = ed_clipboard()))
			return NULL;
		reg_put('+', c, line_type(c));
		free(c);
		name = '+';
	} else if (name == '"' && cfg.clipboard) {
		/* a clipboard changed elsewhere takes over the register */
		c = ed_clipboard();
		if (c && *c && (!seen || strcmp(c, seen))) {
			reg_put('"', c, line_type(c));
			free(seen);
			seen = c;
		} else {
			free(c);
		}
	} else if (name == '/') {
		*type = R_CHAR;
		return vim.pattern;
	}
	name = tolower(name) & 127;
	*type = regs[name].type;
	return regs[name].s && *regs[name].s ? regs[name].s : NULL;
}

/* ---- characters, words and other units ---- */

static char ch(struct editor *e, size_t p)
{
	return p < ed_len(e) ? ed_at(e, p) : 0;
}

static int empty_line(struct editor *e, size_t p)
{
	return (!p || ed_at(e, p - 1) == '\n') &&
	    (p >= ed_len(e) || ed_at(e, p) == '\n');
}

/* 0 for blanks, 1 for word characters, 2 for punctuation */
int vim_class(struct editor *e, size_t p, int big)
{
	unsigned char c = ch(e, p);

	if (!c || c == ' ' || c == '\t' || c == '\n')
		return 0;
	return big || isalnum(c) || c == '_' || c >= 0x80 ? 1 : 2;
}

static size_t word_fwd(struct editor *e, size_t p, int big)
{
	size_t n = ed_len(e), from = p;
	int c = vim_class(e, p, big);

	while (c && p < n && vim_class(e, p, big) == c)
		p = ed_next(e, p);
	while (p < n && !vim_class(e, p, big)) {
		if (p != from && empty_line(e, p))
			break;
		p = ed_next(e, p);
	}
	return p;
}

static size_t word_back(struct editor *e, size_t p, int big)
{
	int c;

	if (!p)
		return 0;
	p = ed_prev(e, p);
	while (p && !vim_class(e, p, big)) {
		if (empty_line(e, p))
			return p;
		p = ed_prev(e, p);
	}
	c = vim_class(e, p, big);
	while (c && p && vim_class(e, ed_prev(e, p), big) == c)
		p = ed_prev(e, p);
	return p;
}

static size_t word_end(struct editor *e, size_t p, int big)
{
	size_t n = ed_len(e);
	int c;

	p = ed_next(e, p);
	while (p < n && !vim_class(e, p, big))
		p = ed_next(e, p);
	c = vim_class(e, p, big);
	while (c && ed_next(e, p) < n &&
	       vim_class(e, ed_next(e, p), big) == c)
		p = ed_next(e, p);
	return p;
}

static size_t word_end_back(struct editor *e, size_t p, int big)
{
	int c = vim_class(e, p, big);

	while (c && p && vim_class(e, ed_prev(e, p), big) == c)
		p = ed_prev(e, p);
	if (!p)
		return 0;
	p = ed_prev(e, p);
	while (p && !vim_class(e, p, big)) {
		if (empty_line(e, p))
			break;
		p = ed_prev(e, p);
	}
	return p;
}

static int line_blank(struct editor *e, int l)
{
	return empty_line(e, ed_line_pos(e, l));
}

static size_t paragraph(struct editor *e, size_t p, int dir)
{
	int l = ed_line_of(e, p), last = ed_lines(e) - 1;

	while (l + dir >= 0 && l + dir <= last && line_blank(e, l))
		l += dir;
	while (l + dir >= 0 && l + dir <= last && !line_blank(e, l))
		l += dir;
	p = ed_line_pos(e, l);
	return dir > 0 && !line_blank(e, l) ? ed_line_end(e, p) : p;
}

static int sentence_start(struct editor *e, size_t p)
{
	size_t q = p;
	int newlines = 0;

	if (empty_line(e, p))
		return 1;
	if (!vim_class(e, p, 1))
		return 0;
	while (q && !vim_class(e, q - 1, 1))
		newlines += ed_at(e, --q) == '\n';
	if (!q || newlines > 1)
		return 1;
	if (q == p)
		return 0;
	while (q && strchr(")]\"'", ed_at(e, q - 1)))
		q--;
	return q && strchr(".!?", ed_at(e, q - 1));
}

static size_t sentence(struct editor *e, size_t p, int dir)
{
	size_t n = ed_len(e);

	if (dir > 0) {
		for (p = ed_next(e, p); p < n; p = ed_next(e, p))
			if (sentence_start(e, p))
				break;
		return p;
	}
	while (p) {
		p = ed_prev(e, p);
		if (sentence_start(e, p))
			break;
	}
	for (n = p; n && !vim_class(e, n - 1, 1); n--)
		if (n > 1 && ed_at(e, n - 1) == '\n' && ed_at(e, n - 2) == '\n')
			return p;
	return n || (p && ed_at(e, 0) == '\n') ? p : 0;
}

/* the bracket of one of the pairs that matches the one at p, or NOMARK */
static size_t match_bracket(struct editor *e, size_t p, const char *pairs)
{
	const char *at = strchr(pairs, ch(e, p));
	int dir, depth = 0;
	char open, other;

	if (!ch(e, p) || !at)
		return NOMARK;
	dir = (at - pairs) % 2 ? -1 : 1;
	open = *at;
	other = at[dir];
	for (;;) {
		if (ed_at(e, p) == open)
			depth++;
		else if (ed_at(e, p) == other && !--depth)
			return p;
		if ((dir < 0 && !p) || (dir > 0 && p + 1 >= ed_len(e)))
			return NOMARK;
		p += dir;
	}
}

/* f, F, t and T within the current line */
static int find_char(struct editor *e, char cmd, const char *c, int count,
		     int repeat, size_t *out)
{
	size_t ls = ed_line_start(e, e->cur), le = ed_line_end(e, e->cur);
	size_t p = e->cur, l = strlen(c), i;
	int dir = islower((unsigned char)cmd) ? 1 : -1;
	int till = cmd == 't' || cmd == 'T';

	/* a repeated till must not stay where it already is */
	if (till && repeat) {
		if (dir > 0 && p < le)
			p = ed_next(e, p);
		else if (dir < 0 && p > ls)
			p = ed_prev(e, p);
	}
	while (count) {
		if (dir > 0) {
			p = ed_next(e, p);
			if (p >= le)
				return 0;
		} else {
			if (p <= ls)
				return 0;
			p = ed_prev(e, p);
		}
		for (i = 0; i < l && p + i < le && ed_at(e, p + i) == c[i]; i++)
			;
		if (i == l)
			count--;
	}
	if (till)
		p = dir > 0 ? ed_prev(e, p) : ed_next(e, p);
	*out = p;
	return 1;
}

/* set the jump mark and move there */
void vim_jump(struct editor *e, size_t to)
{
	e->mark[28] = e->cur;
	e->cur = to;
}

/* keep the cursor on a character, as normal mode requires */
void vim_clamp(struct editor *e)
{
	size_t n = ed_len(e);

	if (e->cur > n)
		e->cur = n;
	while (e->cur && e->cur < n &&
	       ((unsigned char)ed_at(e, e->cur) & 0xc0) == 0x80)
		e->cur--;
	if (e->mode == ED_NORMAL && e->cur == ed_line_end(e, e->cur) &&
	    e->cur > ed_line_start(e, e->cur))
		e->cur = ed_prev(e, e->cur);
}

/* ---- motions ---- */

struct motion {
	size_t to;
	int kind;
};

static int mark_index(int c)
{
	if (c >= 'a' && c <= 'z')
		return c - 'a';
	return c == '<' ? 26 : c == '>' ? 27 : c == '\'' || c == '`' ? 28 : -1;
}

/* move count screen rows, keeping the column within the row */
static int row_motion(struct editor *e, int count, int dir, size_t *to)
{
	size_t ls = ed_line_start(e, e->cur), le = ed_line_end(e, e->cur);
	size_t row = ls, next, prev, p;
	int col = 0;

	while ((next = ed_wrap(e, row, le, e->cols)) < le && e->cur >= next)
		row = next;
	for (p = row; p < e->cur; p = ed_next(e, p))
		col++;
	for (; count; count--) {
		if (dir > 0) {
			next = ed_wrap(e, row, le, e->cols);
			if (next < le) {
				row = next;
			} else if (le < ed_len(e)) {
				row = ls = le + 1;
				le = ed_line_end(e, ls);
			} else {
				return 0;
			}
		} else {
			if (row == ls) {
				if (!ls)
					return 0;
				le = ls - 1;
				ls = ed_line_start(e, le);
				prev = ls;
				while ((next = ed_wrap(e, prev, le, e->cols)) <
				       le)
					prev = next;
			} else {
				prev = ls;
				while ((next = ed_wrap(e, prev, le, e->cols)) <
				       row)
					prev = next;
			}
			row = prev;
		}
	}
	next = ed_wrap(e, row, le, e->cols);
	for (p = row; col && p < next && ed_next(e, p) < next; col--)
		p = ed_next(e, p);
	*to = p;
	return 1;
}

static int word_under(struct editor *e, struct buf *out, int whole)
{
	size_t p = e->cur, le = ed_line_end(e, e->cur), a, b;
	char *w, *s;

	while (p < le && vim_class(e, p, 0) != 1)
		p = ed_next(e, p);
	if (p >= le)
		return 0;
	for (a = p; a > ed_line_start(e, p) &&
	     vim_class(e, ed_prev(e, a), 0) == 1;)
		a = ed_prev(e, a);
	for (b = p; b < le && vim_class(e, b, 0) == 1;)
		b = ed_next(e, b);
	w = ed_slice(e, a, b);
	if (whole)
		buf_puts(out, "\\<");
	for (s = w; *s; s++)
		buf_putc(out, *s);
	if (whole)
		buf_puts(out, "\\>");
	free(w);
	e->cur = a;
	return 1;
}

static int search_motion(struct editor *e, int count, int backward,
			 struct motion *m)
{
	size_t p = e->cur;

	while (count--)
		if (!ex_search(e, p, backward, &p))
			return 0;
	m->to = p;
	return 1;
}

/*
 * compute where a motion leads. count is 0 when none was typed. Returns
 * 0 if the motion cannot be made.
 */
static int motion(struct editor *e, struct cmd *c, int count, int op,
		  struct motion *m)
{
	const char *k = c->key;
	int n = count ? count : 1, i, line, last, big;
	size_t p = e->cur, ls = ed_line_start(e, p), le = ed_line_end(e, p);
	int visual = e->mode >= ED_VISUAL && e->mode <= ED_VBLOCK;

	m->kind = M_EXCL;
	m->to = p;
	if (among(k, "h <Left> <BS>")) {
		if (p == ls)
			return 0;
		while (n-- && p > ls)
			p = ed_prev(e, p);
		m->to = p;
		return 1;
	}
	if (among(k, "l <Right> <Space>")) {
		size_t stop = op || visual ? le : le > ls ? ed_prev(e, le) : ls;

		if (p >= stop)
			return 0;
		while (n-- && p < stop)
			p = ed_next(e, p);
		m->to = p;
		return 1;
	}
	if (among(k, "j k <Down> <Up> + - <CR> _")) {
		int down = among(k, "j <Down> + <CR> _");

		line = ed_line_of(e, p);
		last = ed_lines(e) - 1;
		if (is(k, "_")) {
			if (n > 1 && line == last)
				return 0;
			n--;
		} else if ((down && line == last) || (!down && !line))
			return 0;
		line += down ? n : -n;
		line = line < 0 ? 0 : line > last ? last : line;
		ls = ed_line_pos(e, line);
		if (among(k, "+ - <CR> _")) {
			m->to = ed_first_nonblank(e, ls);
		} else {
			m->to = ed_at_col(e, ls, e->want);
			if (!visual && m->to == ed_line_end(e, ls) &&
			    m->to > ls)
				m->to = ed_prev(e, m->to);
		}
		m->kind = M_LINE;
		return 1;
	}
	if (among(k, "gj gk"))
		return row_motion(e, n, k[1] == 'j' ? 1 : -1, &m->to);
	if (among(k, "g0 g^ g$")) {
		size_t row = ls, next;

		while ((next = ed_wrap(e, row, le, e->cols)) < le && p >= next)
			row = next;
		next = ed_wrap(e, row, le, e->cols);
		if (k[1] == '$') {
			m->to = next > row ? ed_prev(e, next) : row;
			m->kind = M_INCL;
		} else {
			m->to = row;
		}
		return 1;
	}
	if (among(k, "0 <Home>")) {
		m->to = ls;
		return 1;
	}
	if (is(k, "^")) {
		m->to = ed_first_nonblank(e, p);
		return 1;
	}
	if (among(k, "$ <End> g_")) {
		line = ed_line_of(e, p);
		if (n > 1 && line == ed_lines(e) - 1)
			return 0;
		line += n - 1;
		if (line >= ed_lines(e))
			line = ed_lines(e) - 1;
		ls = ed_line_pos(e, line);
		le = ed_line_end(e, ls);
		if (is(k, "g_"))
			while (le > ls && !vim_class(e, ed_prev(e, le), 1))
				le = ed_prev(e, le);
		if (le == ls) {
			m->to = ls;
			return 1;
		}
		m->to = visual && !is(k, "g_") ? le : ed_prev(e, le);
		m->kind = M_INCL;
		return 1;
	}
	if (is(k, "|")) {
		m->to = ed_at_col(e, ls, n - 1);
		return 1;
	}
	if (among(k, "gg G")) {
		last = ed_lines(e) - 1;
		line = count ? count - 1 : k[0] == 'G' ? last : 0;
		line = line > last ? last : line;
		m->to = ed_first_nonblank(e, ed_line_pos(e, line));
		m->kind = M_LINE;
		return 1;
	}
	if (among(k, "H M L")) {
		int top = e->top, bottom = top + e->vis - 1;

		last = ed_lines(e) - 1;
		bottom = bottom > last ? last : bottom;
		line = k[0] == 'H' ? top + n - 1 :
		    k[0] == 'L' ? bottom - n + 1 : (top + bottom) / 2;
		line = line > bottom ? bottom : line < top ? top : line;
		m->to = ed_first_nonblank(e, ed_line_pos(e, line));
		m->kind = M_LINE;
		return 1;
	}
	big = isupper((unsigned char)k[strlen(k) - 1]) ? 1 : 0;
	if (among(k, "w W")) {
		/* "cw" on a word changes up to its end, not the blanks */
		if (op && is(c->op, "c") && vim_class(e, p, big)) {
			int cls = vim_class(e, p, big);

			while (ed_next(e, p) < ed_len(e) &&
			       vim_class(e, ed_next(e, p), big) == cls)
				p = ed_next(e, p);
			for (i = 1; i < n; i++)
				p = word_end(e, p, big);
			m->to = p;
			m->kind = M_INCL;
			return 1;
		}
		for (i = 0; i < n; i++)
			p = word_fwd(e, p, big);
		if (p == e->cur)
			return 0;
		/* an operator stops at the end of the last word's line */
		if (op) {
			size_t q = p, nl;

			if (empty_line(e, e->cur) && n == 1) {
				m->to = e->cur + 1;
				return 1;
			}
			while (q > e->cur && !vim_class(e, q - 1, 1))
				q--;
			for (nl = q; nl < p && ed_at(e, nl) != '\n'; nl++)
				;
			if (q > e->cur && nl < p)
				p = nl;
		}
		m->to = p;
		return 1;
	}
	if (among(k, "b B e E ge gE")) {
		for (i = 0; i < n; i++)
			p = is(k, "b") || is(k, "B") ? word_back(e, p, big) :
			    k[0] == 'g' ? word_end_back(e, p, big) :
			    word_end(e, p, big);
		if (p == e->cur)
			return 0;
		m->to = p;
		if (!among(k, "b B"))
			m->kind = M_INCL;
		return 1;
	}
	if (among(k, "{ } ( )")) {
		for (i = 0; i < n; i++) {
			size_t from = p;

			p = k[0] == '{' || k[0] == '}' ?
			    paragraph(e, p, k[0] == '}' ? 1 : -1) :
			    sentence(e, p, k[0] == ')' ? 1 : -1);
			/* running into an end of the text early cancels */
			if (p == from || (i < n - 1 &&
					  (p >= ed_len(e) || !p)))
				return 0;
		}
		m->to = p;
		return 1;
	}
	if (is(k, "%") && count) {
		/* a count makes it a percentage of the lines */
		if (count > 100)
			return 0;
		line = (count * ed_lines(e) + 99) / 100 - 1;
		m->to = ed_first_nonblank(e, ed_line_pos(e, line));
		m->kind = M_LINE;
		return 1;
	}
	if (is(k, "%")) {
		while (p < le && !strchr("()[]{}", ed_at(e, p)))
			p++;
		if (p >= le || (m->to = match_bracket(e, p, "()[]{}")) == NOMARK)
			return 0;
		m->kind = M_INCL;
		return 1;
	}
	if (among(k, "f F t T ; ,")) {
		char how = k[0];
		const char *what = key_char(c->arg);
		int repeat = how == ';' || how == ',';

		if (repeat) {
			if (!find_cmd)
				return 0;
			how = find_cmd;
			if (k[0] == ',')
				how = islower((unsigned char)how) ?
				    toupper((unsigned char)how) :
				    tolower((unsigned char)how);
			what = find_arg;
		} else {
			if (!what)
				return 0;
			find_cmd = how;
			snprintf(find_arg, sizeof(find_arg), "%s", what);
		}
		if (!find_char(e, how, what, n, repeat, &m->to))
			return 0;
		if (islower((unsigned char)how))
			m->kind = M_INCL;
		return 1;
	}
	if (among(k, "n N")) {
		if (!vim.pattern) {
			vim_msg(1, "no previous search");
			return 0;
		}
		vim.nohl = 0;
		return search_motion(e, n, vim.backward ^ (k[0] == 'N'), m);
	}
	if (among(k, "* # g* g#")) {
		struct buf pat = { 0 };
		int back = k[strlen(k) - 1] == '#', ok;
		size_t at = e->cur;

		if (!word_under(e, &pat, k[0] != 'g'))
			return 0;
		ex_set_pattern(pat.p, back);
		ok = search_motion(e, n, back, m);
		e->cur = at;
		buf_free(&pat);
		return ok;
	}
	if (among(k, "/ ?")) {
		if (c->text.len)
			ex_set_pattern(c->text.p, k[0] == '?');
		else if (!vim.pattern)
			return 0;
		vim.backward = k[0] == '?';
		vim.nohl = 0;
		return search_motion(e, n, vim.backward, m);
	}
	if (among(k, "' `")) {
		const char *a = key_char(c->arg);

		i = a ? mark_index(a[0]) : -1;
		if (i < 0 || e->mark[i] == NOMARK || e->mark[i] > ed_len(e))
			return 0;
		m->to = e->mark[i];
		if (k[0] == '\'') {
			m->to = ed_first_nonblank(e, m->to);
			m->kind = M_LINE;
		}
		return 1;
	}
	return 0;
}

static int is_jump(const char *k)
{
	return among(k, "gg G { } ( ) % n N * # g* g# / ? ' ` H M L");
}

/* ---- text objects ---- */

struct range {
	size_t a, b;
	int type;
};

static void word_object(struct editor *e, int around, int big, int count,
			struct range *r)
{
	size_t ls = ed_line_start(e, e->cur), le = ed_line_end(e, e->cur);
	size_t a = e->cur, b = e->cur;
	int c = vim_class(e, a, big), trail = 0, i;

	while (a > ls && vim_class(e, ed_prev(e, a), big) == c)
		a = ed_prev(e, a);
	while (b < le && vim_class(e, b, big) == c)
		b = ed_next(e, b);
	if (around && ls == le && le < ed_len(e)) {
		b = le + 1;
		while (b < ed_len(e) && !vim_class(e, b, big))
			b++;
		c = vim_class(e, b, big);
		while (c && vim_class(e, b, big) == c)
			b = ed_next(e, b);
		r->a = a;
		r->b = b;
		return;
	}
	for (i = 0; i < count; i++) {
		int next;

		if (i) {
			/* each further count takes the following run */
			next = vim_class(e, b, big);
			while (b < le && vim_class(e, b, big) == next)
				b = ed_next(e, b);
		}
		if (!around)
			continue;
		if (!c || (i && !trail)) {
			next = vim_class(e, b, big);
			while (b < le && vim_class(e, b, big) == next)
				b = ed_next(e, b);
		} else {
			trail = 0;
			while (b < le && !vim_class(e, b, big)) {
				b = ed_next(e, b);
				trail = 1;
			}
		}
	}
	if (around && c && !trail)
		while (a > ls && !vim_class(e, ed_prev(e, a), big))
			a = ed_prev(e, a);
	r->a = a;
	r->b = b;
}

static void sentence_object(struct editor *e, int around, int count,
			    struct range *r)
{
	size_t a = e->cur, b = e->cur, t;

	while (a && !sentence_start(e, a))
		a = ed_prev(e, a);
	while (count--)
		b = sentence(e, b, 1);
	for (t = b; t > a && !vim_class(e, t - 1, 1); t--)
		;
	if (!around)
		b = t;
	if (!around && a == ed_line_start(e, a) && b > a &&
	    b == ed_line_end(e, a)) {
		/* a sentence that is a whole line acts on the line */
		r->type = M_LINE;
		r->a = a;
		r->b = b;
		return;
	}
	if (!around)
		;
	else if (t == b)
		while (a && !vim_class(e, a - 1, 1) && ed_at(e, a - 1) != '\n')
			a--;
	r->a = a;
	r->b = b;
}

static int paragraph_object(struct editor *e, int around, int count,
			    struct range *r)
{
	int first = ed_line_of(e, e->cur), last = first;
	int end = ed_lines(e) - 1, blank = line_blank(e, first), i;

	while (first && line_blank(e, first - 1) == blank)
		first--;
	while (last < end && line_blank(e, last + 1) == blank)
		last++;
	for (i = 0; i < (around ? 2 * count : count) - 1; i++) {
		int next;

		if (last >= end) {
			if (count > 1)
				return 0;
			break;
		}
		next = line_blank(e, last + 1);
		while (last < end && line_blank(e, last + 1) == next)
			last++;
	}
	if (around && !i && !blank)
		while (first && line_blank(e, first - 1))
			first--;
	r->a = ed_line_pos(e, first);
	r->b = ed_line_end(e, ed_line_pos(e, last));
	r->type = M_LINE;
	return 1;
}

static int quote_object(struct editor *e, int around, char q, struct range *r)
{
	size_t p = ed_line_start(e, e->cur), le = ed_line_end(e, e->cur);
	size_t open = NOMARK;

	for (; p < le; p++) {
		if (ed_at(e, p) == '\\') {
			p++;
			continue;
		}
		if (ed_at(e, p) != q)
			continue;
		if (open == NOMARK) {
			open = p;
		} else if (p >= e->cur) {
			break;
		} else {
			open = NOMARK;
		}
	}
	if (p >= le || open == NOMARK)
		return 0;
	r->a = around ? open : open + 1;
	r->b = around ? p + 1 : p;
	if (around) {
		size_t t = r->b;

		while (t < le && (ed_at(e, t) == ' ' || ed_at(e, t) == '\t'))
			t++;
		if (t > r->b)
			r->b = t;
		else
			while (r->a > ed_line_start(e, e->cur) &&
			       !vim_class(e, r->a - 1, 1))
				r->a--;
	}
	return 1;
}

static int bracket_object(struct editor *e, int around, char open, char close,
			  int count, struct range *r)
{
	size_t p = e->cur, end;
	char pair[3];
	int depth = 0, counted = count > 1;

	if (ch(e, p) == open)
		count--;
	while (count && p) {
		p--;
		if (ed_at(e, p) == close)
			depth++;
		else if (ed_at(e, p) == open && (!depth || !depth--))
			count--;
	}
	if (count && counted)
		return 0;
	if (count) {
		/* not inside a pair: take the next one */
		for (p = e->cur; ch(e, p) != open; p++)
			if (p >= ed_len(e))
				return 0;
	}
	pair[0] = open;
	pair[1] = close;
	pair[2] = 0;
	end = match_bracket(e, p, pair);
	if (end == NOMARK)
		return 0;
	r->a = around ? p : p + 1;
	r->b = around ? end + 1 : end;
	/* brackets on lines of their own enclose whole lines */
	if (!around && ch(e, r->a) == '\n' &&
	    ed_first_nonblank(e, end) == end) {
		r->a++;
		if (ed_line_start(e, end) > r->a) {
			r->b = ed_line_start(e, end) - 1;
			r->type = M_LINE;
		} else {
			r->b = r->a;
		}
	}
	return 1;
}

static int text_object(struct editor *e, int around, const char *obj,
		       int count, struct range *r)
{
	char o = strlen(obj) == 1 ? obj[0] : 0;

	r->type = M_EXCL;
	if (is(obj, "<"))
		o = '<';
	switch (o) {
	case 'w':
	case 'W':
		word_object(e, around, o == 'W', count, r);
		return r->b > r->a;
	case 's':
		sentence_object(e, around, count, r);
		return r->b > r->a;
	case 'p':
		return paragraph_object(e, around, count, r);
	case '"':
	case '\'':
	case '`':
		return quote_object(e, around, o, r);
	case '(':
	case ')':
	case 'b':
		return bracket_object(e, around, '(', ')', count, r);
	case '[':
	case ']':
		return bracket_object(e, around, '[', ']', count, r);
	case '{':
	case '}':
	case 'B':
		return bracket_object(e, around, '{', '}', count, r);
	case '<':
	case '>':
		return bracket_object(e, around, '<', '>', count, r);
	}
	return 0;
}

/* ---- changes ---- */

static void start_insert(struct editor *e, int count, int open)
{
	e->mode = ED_INSERT;
	e->icount = count > 0 ? count : 1;
	e->iopen = open;
	e->typed.n = 0;
	e->istart = e->cur;
	buf_reset(&e->over);
}

static void change_case(char *s, int how)
{
	unsigned char *p = (unsigned char *)s;

	for (; *p; p++) {
		if (*p < 0x80) {
			if (how != 'u' && islower(*p))
				*p = toupper(*p);
			else if (how != 'U' && isupper(*p))
				*p = tolower(*p);
		} else if (*p == 0xc3 && p[1]) {
			/* the Latin-1 letters, U+00C0 to U+00FE */
			p++;
			if (how != 'u' && *p >= 0xa0 && *p <= 0xbe &&
			    *p != 0xb7)
				*p -= 0x20;
			else if (how != 'U' && *p >= 0x80 && *p <= 0x9e &&
				 *p != 0x97)
				*p += 0x20;
		}
	}
}

static void recase(struct editor *e, size_t a, size_t b, int how)
{
	char *s = ed_slice(e, a, b);

	/* an undo step even when no letter changes, as in vim */
	change_case(s, how);
	ed_splice(e, a, b - a, s, b - a);
	free(s);
}

static void indent(struct editor *e, int first, int last, int dir, int count)
{
	int l;

	for (l = first; l <= last; l++) {
		size_t ls = ed_line_pos(e, l), p = ls;
		int n = SHIFTWIDTH * count;

		if (dir > 0) {
			if (empty_line(e, ls))
				continue;
			while (n--)
				ed_splice(e, ls, 0, " ", 1);
			continue;
		}
		while (n > 0 && (ch(e, p) == ' ' || ch(e, p) == '\t')) {
			n -= ch(e, p) == '\t' ? SHIFTWIDTH : 1;
			p++;
		}
		ed_splice(e, ls, p - ls, NULL, 0);
	}
	e->cur = ed_first_nonblank(e, ed_line_pos(e, first));
}

/* join the cursor line with the following ones */
static int join(struct editor *e, int lines, int spaces)
{
	int i;

	for (i = 1; i < lines; i++) {
		size_t le = ed_line_end(e, e->cur), p = le + 1;
		const char *sep = "";

		if (le >= ed_len(e))
			return i > 1;
		if (spaces) {
			while (ch(e, p) == ' ' || ch(e, p) == '\t')
				p++;
			if (le > ed_line_start(e, le) &&
			    ed_at(e, le - 1) != ' ' &&
			    ed_at(e, le - 1) != '\t' && ch(e, p) &&
			    ch(e, p) != '\n' && ch(e, p) != ')')
				sep = " ";
		}
		ed_splice(e, le, p - le, sep, strlen(sep));
		e->cur = le;
	}
	return 1;
}

/* delete whole lines [a, b), where b is the end of the last one */
static void delete_lines(struct editor *e, size_t a, size_t b)
{
	if (b < ed_len(e))
		ed_splice(e, a, b + 1 - a, NULL, 0);
	else if (a)
		ed_splice(e, a - 1, b - a + 1, NULL, 0);
	else
		ed_splice(e, 0, b, NULL, 0);
	e->cur = ed_first_nonblank(e, a > ed_len(e) ? ed_len(e) : a);
}

/*
 * apply an operator to [a, b). With M_LINE a is the start of the first
 * line and b the end of the last one.
 */
static int operate(struct editor *e, const char *op, size_t a, size_t b,
		   int kind, int reg, int count)
{
	int first = ed_line_of(e, a);
	int last = ed_line_of(e, b > a && kind != M_LINE ? b - 1 : b);
	char *s, *t;

	if (e->readonly && !is(op, "y"))
		return 0;
	if (!is(op, "y") && kind != M_LINE)
		e->cur = a;
	if (among(op, "d c y")) {
		s = ed_slice(e, a, b);
		if (kind == M_LINE) {
			t = xasprintf("%s\n", s);
			vim_set_register(reg, t, R_LINE, is(op, "y"));
			free(t);
		} else if (b > a) {
			vim_set_register(reg, s, R_CHAR, is(op, "y"));
		}
		free(s);
	}
	if (is(op, "y")) {
		e->cur = kind == M_LINE ?
		    ed_at_col(e, a, ed_col(e, e->cur)) : a;
		if (last > first)
			vim_msg(0, "%d lines yanked", last - first + 1);
	} else if (is(op, "d")) {
		if (kind == M_LINE) {
			delete_lines(e, a, b);
		} else {
			ed_splice(e, a, b - a, NULL, 0);
			e->cur = a;
		}
	} else if (is(op, "c")) {
		ed_splice(e, a, b - a, NULL, 0);
		e->cur = a;
		start_insert(e, 1, 0);
	} else if (is(op, ">") || is(op, "<")) {
		indent(e, first, last, op[0] == '>' ? 1 : -1, count);
	} else if (among(op, "gu gU g~")) {
		size_t keep = e->cur;

		recase(e, a, b, op[1]);
		e->cur = kind == M_LINE && keep >= a && keep <= ed_len(e) ?
		    keep : a;
	} else if (among(op, "gq gw")) {
		size_t back = e->cur;

		ex_reflow(e, first, last);
		if (op[1] == 'w')
			e->cur = back > ed_len(e) ? ed_len(e) : back;
	}
	return 1;
}

/* the range an operator covers when given a motion */
static void motion_range(struct editor *e, const struct motion *m,
			 struct range *r)
{
	size_t a = e->cur < m->to ? e->cur : m->to;
	size_t b = e->cur < m->to ? m->to : e->cur;
	int kind = m->kind;

	/* an exclusive motion into column one stops at the line before */
	if (kind == M_EXCL && b > a && b == ed_line_start(e, b) &&
	    ed_line_start(e, a) < b) {
		b--;
		if (a <= ed_first_nonblank(e, a))
			kind = M_LINE;
	} else if (kind == M_INCL) {
		b = ed_next(e, b);
	}
	if (kind == M_LINE) {
		a = ed_line_start(e, a);
		b = ed_line_end(e, b);
	}
	r->a = a;
	r->b = b;
	r->type = kind;
}

static int paste(struct editor *e, int reg, int count, int before, int after)
{
	int type = R_CHAR, i;
	const char *s = vim_register(reg, &type);
	struct buf b = { 0 };
	size_t at, len;

	if (!s || e->readonly) {
		vim_msg(1, "nothing in the register");
		return 0;
	}
	if (e->single)
		type = R_CHAR;
	if (type == R_BLOCK) {
		int line = ed_line_of(e, e->cur), width = 0;
		size_t col = ed_col(e, e->cur);
		const char *p, *nl;
		size_t first;

		if (!before && !empty_line(e, e->cur))
			col++;
		for (p = s; *p; p = *nl ? nl + 1 : nl) {
			int w = 0;
			const char *q;

			nl = p + strcspn(p, "\n");
			for (q = p; q < nl; q++)
				w += ((unsigned char)*q & 0xc0) != 0x80;
			width = w > width ? w : width;
		}
		first = ed_at_col(e, ed_line_pos(e, line), col);
		for (p = s; *p; p = *nl ? nl + 1 : nl, line++) {
			size_t ls, pos;
			int w = 0;
			const char *q;

			nl = p + strcspn(p, "\n");
			if (line >= ed_lines(e))
				ed_splice(e, ed_len(e), 0, "\n", 1);
			ls = ed_line_pos(e, line);
			while (ed_col(e, ed_line_end(e, ls)) < col)
				ed_splice(e, ed_line_end(e, ls), 0, " ", 1);
			pos = ed_at_col(e, ls, col);
			buf_reset(&b);
			for (i = 0; i < count; i++) {
				buf_add(&b, p, nl - p);
				for (q = p, w = 0; q < nl; q++)
					w += ((unsigned char)*q & 0xc0) != 0x80;
				if (ch(e, pos) && ch(e, pos) != '\n')
					while (w++ < width)
						buf_putc(&b, ' ');
			}
			buf_add(&b, "", 0);
			ed_splice(e, pos, 0, b.p, b.len);
		}
		e->cur = first;
		buf_free(&b);
		return 1;
	}
	for (i = 0; i < count; i++)
		buf_puts(&b, s);
	if (e->single)
		for (len = 0; len < b.len; len++)
			if (b.p[len] == '\n')
				b.p[len] = ' ';
	len = b.len;
	if (type == R_LINE) {
		if (!len || b.p[len - 1] != '\n') {
			buf_putc(&b, '\n');
			len++;
		}
		if (before) {
			at = ed_line_start(e, e->cur);
			ed_splice(e, at, 0, b.p, len);
		} else if (ed_line_end(e, e->cur) < ed_len(e)) {
			at = ed_line_end(e, e->cur) + 1;
			ed_splice(e, at, 0, b.p, len);
		} else {
			at = ed_len(e) + 1;
			ed_splice(e, ed_len(e), 0, "\n", 1);
			ed_splice(e, ed_len(e), 0, b.p, len - 1);
		}
		e->cur = after ? at + len : ed_first_nonblank(e, at);
		if (e->cur > ed_len(e))
			e->cur = ed_line_start(e, ed_len(e));
	} else {
		at = e->cur;
		if (!before && at < ed_line_end(e, at))
			at = ed_next(e, at);
		ed_splice(e, at, 0, b.p, len);
		if (after)
			e->cur = at + len;
		else if (memchr(b.p, '\n', len))
			e->cur = at;
		else
			e->cur = ed_prev(e, at + len);
	}
	buf_free(&b);
	return 1;
}

/* add to the number at or after the cursor */
static int add_number(struct editor *e, long delta)
{
	size_t p = ed_line_start(e, e->cur), le = ed_line_end(e, e->cur);
	size_t a, b;
	char *s, *r;
	int zeros;

	while (p < le) {
		if (!isdigit((unsigned char)ed_at(e, p))) {
			p++;
			continue;
		}
		for (b = p; b < le && isdigit((unsigned char)ed_at(e, b)); b++)
			;
		if (b > e->cur)
			break;
		p = b;
	}
	if (p >= le)
		return 0;
	a = p && ed_at(e, p - 1) == '-' ? p - 1 : p;
	s = ed_slice(e, a, b);
	zeros = s[a < p] == '0' && b - p > 1 ? (int)(b - p) : 0;
	r = strtol(s, NULL, 10) + delta < 0 && zeros ?
	    xasprintf("-%0*ld", zeros, -(strtol(s, NULL, 10) + delta)) :
	    xasprintf("%0*ld", zeros, strtol(s, NULL, 10) + delta);
	ed_splice(e, a, b - a, r, strlen(r));
	e->cur = a + strlen(r) - 1;
	free(s);
	free(r);
	return 1;
}

/* ---- selections ---- */

static int is_visual(struct editor *e)
{
	return e->mode >= ED_VISUAL && e->mode <= ED_VBLOCK;
}

/* the lines and columns a block selection covers */
static void block_bounds(struct editor *e, int *first, int *last, size_t *left,
			 size_t *right)
{
	size_t c1 = ed_col(e, e->vstart), c2 = ed_col(e, e->cur);
	int l1 = ed_line_of(e, e->vstart), l2 = ed_line_of(e, e->cur);

	*first = l1 < l2 ? l1 : l2;
	*last = l1 < l2 ? l2 : l1;
	*left = c1 < c2 ? c1 : c2;
	*right = c1 < c2 ? c2 : c1;
}

/* the selected bytes [*a, *b); returns 0 outside visual mode */
int ed_selection(struct editor *e, size_t *a, size_t *b)
{
	if (!is_visual(e))
		return 0;
	*a = e->vstart < e->cur ? e->vstart : e->cur;
	*b = e->vstart < e->cur ? e->cur : e->vstart;
	if (e->mode == ED_VLINE) {
		*a = ed_line_start(e, *a);
		*b = ed_line_end(e, *b);
		if (*b < ed_len(e))
			(*b)++;
	} else {
		*b = ed_next(e, *b);
	}
	return 1;
}

/*
 * the selected part [*a, *b) of the line [ls, le]; *b is past le when the
 * line break is selected too
 */
int ed_line_selection(struct editor *e, size_t ls, size_t le, size_t *a,
		      size_t *b)
{
	size_t x, y;

	if (!ed_selection(e, &x, &y))
		return 0;
	if (e->mode == ED_VBLOCK) {
		size_t from = e->vstart < e->cur ? e->vstart : e->cur;
		size_t to = e->vstart < e->cur ? e->cur : e->vstart;
		size_t c1 = ed_col(e, e->vstart), c2 = ed_col(e, e->cur);

		if (ls < ed_line_start(e, from) || ls > ed_line_start(e, to))
			return 0;
		*a = ed_at_col(e, ls, c1 < c2 ? c1 : c2);
		*b = ed_at_col(e, ls, c1 < c2 ? c2 : c1);
		if (e->want == WANT_EOL || *b >= le)
			*b = le;
		else
			*b = ed_next(e, *b);
		return *a < *b;
	}
	*a = x > ls ? x : ls;
	*b = y < le + 1 ? y : le + 1;
	return *a < *b;
}

/* a copy of the selected text, the rows of a block on separate lines */
char *ed_selected_text(struct editor *e)
{
	struct buf out = { 0 };
	size_t a, b, ls, left, right;
	int first, last, l;

	if (!ed_selection(e, &a, &b))
		return NULL;
	if (e->mode != ED_VBLOCK)
		return ed_slice(e, a, b > ed_len(e) ? ed_len(e) : b);
	block_bounds(e, &first, &last, &left, &right);
	for (l = first; l <= last; l++) {
		ls = ed_line_pos(e, l);
		if (ed_line_selection(e, ls, ed_line_end(e, ls), &a, &b)) {
			char *s = ed_slice(e, a, b);

			buf_puts(&out, s);
			free(s);
		}
		if (l < last)
			buf_putc(&out, '\n');
	}
	return buf_take(&out);
}

/*
 * the selected part of every selected line. All are measured before any
 * change, as editing moves the positions the selection is made of.
 */
static size_t (*line_parts(struct editor *e, int first, int last))[2]
{
	size_t (*part)[2] = xcalloc(last - first + 1, sizeof(*part));
	int l;

	for (l = first; l <= last; l++) {
		size_t ls = ed_line_pos(e, l), *p = part[l - first];

		if (!ed_line_selection(e, ls, ed_line_end(e, ls), &p[0],
				       &p[1]))
			p[0] = p[1] = NOMARK;
	}
	return part;
}

/* remove the selected part of every line of a block */
static void block_delete(struct editor *e, int first, int last)
{
	size_t (*part)[2] = line_parts(e, first, last);
	int i;

	/* from the bottom, so that the other lines keep their place */
	for (i = last - first; i >= 0; i--)
		if (part[i][0] != NOMARK)
			ed_splice(e, part[i][0], part[i][1] - part[i][0], NULL,
				  0);
	free(part);
}

static void leave_visual(struct editor *e)
{
	size_t a = e->vstart < e->cur ? e->vstart : e->cur;
	size_t b = e->vstart < e->cur ? e->cur : e->vstart;

	e->mark[26] = a > ed_len(e) ? ed_len(e) : a;
	e->mark[27] = b > ed_len(e) ? ed_len(e) : b;
	e->lastvis = e->mode;
	e->mode = ED_NORMAL;
}

/* remove the selection as one change, leaving the mode it came from */
void ed_cut_selection(struct editor *e)
{
	size_t a, b, left, right;
	int first, last;
	int keep = e->typeover;

	if (!ed_selection(e, &a, &b))
		return;
	ed_checkpoint(e);
	if (e->mode == ED_VBLOCK) {
		block_bounds(e, &first, &last, &left, &right);
		a = ed_at_col(e, ed_line_pos(e, first), left);
		block_delete(e, first, last);
		leave_visual(e);
		e->cur = a;
	} else {
		leave_visual(e);
		ed_remove(e, a, b);
	}
	e->typeover = 0;
	if (keep)
		e->mode = ED_INSERT;
	vim_clamp(e);
}

/*
 * select everything. In one line fields, and without vim keys, typing then
 * replaces the selection as it does elsewhere.
 */
void ed_select_all(struct editor *e)
{
	if (!ed_len(e))
		return;
	e->vstart = 0;
	e->cur = ed_prev(e, ed_len(e));
	e->mode = ED_VISUAL;
	e->typeover = e->single || !cfg.vim_edit;
}

/* insert pasted text at the cursor, replacing a visual selection */
void ed_paste(struct editor *e, const char *s)
{
	struct cmd c = { 0 };
	int typing;

	/* a search or command being typed takes the text itself */
	if (ed_prompt(e)) {
		struct keys k = { 0 };
		int i;

		decode(&k, s);
		for (i = 0; i < k.n; i++)
			if (key_char(k.k[i]))
				keys_add(&e->pend, k.k[i]);
		free(k.k);
		return;
	}
	buf_free(&c.text);
	if (is_visual(e)) {
		typing = e->typeover;
		ed_cut_selection(e);
		if (typing)
			e->mode = ED_INSERT;
	} else {
		ed_checkpoint(e);
	}
	e->typeover = 0;
	ed_insert(e, s, strlen(s));
	if (e->mode == ED_NORMAL && *s)
		e->cur = ed_prev(e, e->cur);
}

/* ---- insert and replace modes ---- */

static void replace_back(struct editor *e);

static void erase_back(struct editor *e, size_t from)
{
	if (e->mode == ED_REPLACE) {
		while (from < e->cur)
			replace_back(e);
	} else if (from < e->cur) {
		ed_remove(e, from, e->cur);
	}
}

static void replace_char(struct editor *e, const char *s)
{
	size_t n = strlen(s), old = 0;

	if (e->cur < ed_line_end(e, e->cur))
		old = ed_next(e, e->cur) - e->cur;
	if (old) {
		char *o = ed_slice(e, e->cur, e->cur + old);

		buf_puts(&e->over, o);
		free(o);
	}
	buf_putc(&e->over, (int)old);
	ed_splice(e, e->cur, old, s, n);
	e->cur += n;
}

/* backspace in replace mode brings the old character back */
static void replace_back(struct editor *e)
{
	size_t old, at;

	if (!e->cur)
		return;
	at = ed_prev(e, e->cur);
	/* before the start of the replacing there is nothing to restore */
	if (!e->over.len || at < e->istart || ed_at(e, at) == '\n') {
		e->cur = at;
		return;
	}
	old = (unsigned char)e->over.p[--e->over.len];
	e->over.len -= old;
	ed_splice(e, at, e->cur - at, e->over.p + e->over.len, old);
	e->over.p[e->over.len] = 0;
	e->cur = at;
}

/* the editing keys shared by insert mode and one line fields */
static int insert_edit(struct editor *e, const char *k)
{
	size_t p = e->cur, ls = ed_line_start(e, p), a;
	const char *c;

	if (is(k, "<CR>")) {
		if (e->single)
			return ED_ENTER;
		ed_insert(e, "\n", 1);
	} else if (is(k, "<BS>")) {
		if (e->mode == ED_REPLACE)
			replace_back(e);
		else if (p)
			erase_back(e, ed_prev(e, p));
	} else if (is(k, "<Del>")) {
		if (p < ed_len(e))
			ed_splice(e, p, ed_next(e, p) - p, NULL, 0);
	} else if (is(k, "<C-w>")) {
		a = p;
		if (a > ls) {
			int cls;

			while (a > ls && !vim_class(e, ed_prev(e, a), 0))
				a = ed_prev(e, a);
			cls = a > ls ? vim_class(e, ed_prev(e, a), 0) : 0;
			while (cls && a > ls &&
			       vim_class(e, ed_prev(e, a), 0) == cls)
				a = ed_prev(e, a);
		} else if (a) {
			a--;
		}
		if (p > e->istart && a < e->istart)
			a = e->istart;
		erase_back(e, a);
	} else if (is(k, "<C-u>")) {
		a = e->istart > ls && e->istart < p ? e->istart : ls;
		erase_back(e, a == p && p ? p - 1 : a);
	} else if (is(k, "<C-t>") || is(k, "<C-d>")) {
		size_t col = p - ls, len = ed_len(e);
		int line = ed_line_of(e, p);

		if (k[3] == 't' && empty_line(e, ls))
			ed_splice(e, ls, 0, "    ", SHIFTWIDTH);
		else
			indent(e, line, line, k[3] == 't' ? 1 : -1, 1);
		ls = ed_line_pos(e, line);
		e->cur = ed_len(e) >= len ? ls + col + (ed_len(e) - len) :
		    col > len - ed_len(e) ? ls + col - (len - ed_len(e)) : ls;
	} else if ((c = key_char(k))) {
		if (e->mode == ED_REPLACE)
			replace_char(e, c);
		else
			ed_insert(e, c, strlen(c));
	} else {
		return ED_UNHANDLED;
	}
	return ED_OK;
}

static int cursor_key(struct editor *e, const char *k)
{
	struct cmd c = { 0 };
	struct motion m;

	if (!among(k, "<Left> <Right> <Up> <Down> <Home> <End>"))
		return ED_UNHANDLED;
	if (e->single && among(k, "<Up> <Down>"))
		return ED_UNHANDLED;
	if (is(k, "<Left>")) {
		if (e->cur > ed_line_start(e, e->cur))
			e->cur = ed_prev(e, e->cur);
	} else if (is(k, "<Right>")) {
		if (e->cur < ed_line_end(e, e->cur))
			e->cur = ed_next(e, e->cur);
	} else if (is(k, "<Home>")) {
		e->cur = ed_line_start(e, e->cur);
	} else if (is(k, "<End>")) {
		e->cur = ed_line_end(e, e->cur);
	} else {
		snprintf(c.key, sizeof(c.key), "%s", k);
		if (motion(e, &c, 1, 1, &m))
			e->cur = ed_at_col(e, ed_line_start(e, m.to), e->want);
		return ED_OK;
	}
	e->want = ed_col(e, e->cur);
	return ED_OK;
}

/* repeat the typed text for a count and finish a block insert */
static void leave_insert(struct editor *e)
{
	struct keys again = { 0 };
	int i, j, l, mode = e->mode;

	if (e->icount > 1) {
		keys_copy(&again, &e->typed, 0);
		for (i = 1; i < e->icount; i++) {
			if (e->iopen) {
				e->cur = ed_line_end(e, e->cur);
				ed_insert(e, "\n", 1);
			}
			e->mode = mode;
			for (j = 0; j < again.n; j++)
				insert_edit(e, again.k[j]);
		}
		free(again.k);
		e->icount = 1;
	}
	if (e->bi) {
		char *s = e->cur > e->bi_pos ?
		    ed_slice(e, e->bi_pos, e->cur) : xstrdup("");

		if (*s && !strchr(s, '\n'))
			for (l = e->bi_first + 1; l <= e->bi_last; l++) {
				size_t ls = ed_line_pos(e, l);
				size_t le = ed_line_end(e, ls);

				if (!e->bi_eol && ed_col(e, le) < e->bi_col) {
					if (!e->bi_append)
						continue;
					while (ed_col(e, ed_line_end(e, ls)) <
					       e->bi_col)
						ed_splice(e, ed_line_end(e, ls),
							  0, " ", 1);
				}
				ed_splice(e, e->bi_eol ? ed_line_end(e, ls) :
					  ed_at_col(e, ls, e->bi_col), 0, s,
					  strlen(s));
			}
		free(s);
		e->bi = 0;
		e->cur = e->bi_home;
		e->mode = ED_NORMAL;
	} else {
		e->mode = ED_NORMAL;
		if (e->cur > ed_line_start(e, e->cur))
			e->cur = ed_prev(e, e->cur);
	}
	vim_clamp(e);
	e->want = ed_col(e, e->cur);
}

static int insert_mode(struct editor *e, const char *k)
{
	int r, type;

	if (e->pend.n) {
		/* the register named after <C-r> */
		const char *c = key_char(k), *s;

		e->pend.n = 0;
		if (c && (s = vim_register(c[0] == '"' ? 0 : c[0], &type)))
			ed_insert(e, s, strlen(s));
		return ED_OK;
	}
	if (is(k, "<Esc>")) {
		leave_insert(e);
		return ED_OK;
	}
	if (is(k, "<C-r>")) {
		keys_add(&e->pend, k);
		return ED_OK;
	}
	if (is(k, "<C-o>")) {
		e->ctrl_o = e->mode;
		e->mode = ED_NORMAL;
		return ED_OK;
	}
	if (is(k, "<Insert>")) {
		e->mode = e->mode == ED_INSERT ? ED_REPLACE : ED_INSERT;
		return ED_OK;
	}
	r = cursor_key(e, k);
	if (r == ED_OK) {
		/* moving ends the part that a count would repeat */
		e->typed.n = 0;
		e->icount = 1;
		e->istart = e->cur;
		return r;
	}
	r = insert_edit(e, k);
	if (r == ED_OK) {
		keys_add(&e->typed, k);
		e->want = ed_col(e, e->cur);
	}
	return r;
}

/* ---- parsing ---- */

static const char *simple_motions =
    "h j k l <Left> <Right> <Up> <Down> <BS> <Space> w W b B e E 0 ^ $ "
    "<Home> <End> G { } ( ) % ; , n N * # H M L + - <CR> _ |";

static int is_digit_key(const char *k)
{
	return isdigit((unsigned char)k[0]) && !k[1];
}

static int parse_count(const struct keys *k, int *i)
{
	int n = 0;

	while (*i < k->n && is_digit_key(k->k[*i]) &&
	       (n || k->k[*i][0] != '0')) {
		if (n < 100000)
			n = n * 10 + k->k[*i][0] - '0';
		(*i)++;
	}
	return n;
}

/* a command followed by one more key, such as "f" or "r" */
static int need_arg(const struct keys *k, int i, struct cmd *c)
{
	if (i + 1 >= k->n)
		return P_MORE;
	if (is(k->k[i + 1], "<Esc>"))
		return P_CANCEL;
	snprintf(c->key, sizeof(c->key), "%s", k->k[i]);
	snprintf(c->arg, sizeof(c->arg), "%s", k->k[i + 1]);
	return P_OK;
}

/* a "/", "?" or ":" line, complete once <CR> arrives */
static int parse_prompt(const struct keys *k, int i, struct cmd *c)
{
	int j;

	snprintf(c->key, sizeof(c->key), "%s", k->k[i]);
	c->prompt = i;
	for (j = i + 1; j < k->n; j++) {
		const char *s = key_char(k->k[j]);

		if (is(k->k[j], "<CR>"))
			return P_OK;
		if (s)
			buf_puts(&c->text, s);
	}
	return P_MORE;
}

static int parse_motion(const struct keys *k, int i, struct cmd *c)
{
	const char *t = k->k[i];

	if (among(t, simple_motions)) {
		snprintf(c->key, sizeof(c->key), "%s", t);
		return P_OK;
	}
	if (among(t, "f F t T ' `"))
		return need_arg(k, i, c);
	if (among(t, "/ ?"))
		return parse_prompt(k, i, c);
	if (is(t, "g")) {
		if (i + 1 >= k->n)
			return P_MORE;
		if (!among(k->k[i + 1], "g e E _ j k 0 ^ $ * #"))
			return is(k->k[i + 1], "<Esc>") ? P_CANCEL : P_BAD;
		snprintf(c->key, sizeof(c->key), "g%.4s", k->k[i + 1]);
		return P_OK;
	}
	return P_BAD;
}

static int parse(const struct keys *k, int visual, struct cmd *c)
{
	int i = 0, n, n2;
	const char *t;

	c->reg = c->count = c->doubled = 0;
	c->op[0] = c->key[0] = c->arg[0] = 0;
	c->c2a = c->c2b = 0;
	c->prompt = -1;
	buf_reset(&c->text);
	for (;;) {
		if (i < k->n && is(k->k[i], "\"")) {
			if (i + 1 >= k->n)
				return P_MORE;
			if (strlen(k->k[i + 1]) != 1)
				return P_BAD;
			c->reg = k->k[i + 1][0];
			i += 2;
			continue;
		}
		if ((n = parse_count(k, &i))) {
			c->count = c->count ? c->count * n : n;
			continue;
		}
		break;
	}
	c->start = i;
	if (i >= k->n)
		return P_MORE;
	t = k->k[i];
	if (is(t, "<Esc>")) {
		snprintf(c->key, sizeof(c->key), "<Esc>");
		return i ? P_CANCEL : P_OK;
	}
	if (is(t, ":"))
		return parse_prompt(k, i, c);
	if (is(t, "g") && i + 1 < k->n) {
		t = k->k[i + 1];
		if (among(t, "u U ~ q w")) {
			snprintf(c->op, sizeof(c->op), "g%.2s", t);
			if (visual)
				return P_OK;
			i += 2;
			goto operand;
		}
		if (among(t, "J p P v I")) {
			snprintf(c->key, sizeof(c->key), "g%.4s", t);
			return P_OK;
		}
		t = k->k[i];
	}
	if (is(t, "q")) {
		if (rec_reg) {
			snprintf(c->key, sizeof(c->key), "q");
			return P_OK;
		}
		return need_arg(k, i, c);
	}
	if (among(t, "@ m r z Z"))
		return need_arg(k, i, c);
	if (visual) {
		if (among(t, "i a"))
			return need_arg(k, i, c);
		if (among(t, "d x <Del> X D y Y c s S R C p P J ~ u U > < I A o O "
			  "v V <C-v> <C-d> <C-u> <C-f> <C-b> <C-e> <C-y> "
			  "<PageUp> <PageDown>")) {
			snprintf(c->key, sizeof(c->key), "%s", t);
			return P_OK;
		}
		return parse_motion(k, i, c);
	}
	if (among(t, "d c y > <")) {
		snprintf(c->op, sizeof(c->op), "%s", t);
		i++;
		goto operand;
	}
	if (among(t, "x X s S C D Y J p P o O i I a A u . ~ v V R & <C-r> "
		  "<C-a> <C-x> <C-v> <C-d> <C-u> <C-f> <C-b> <C-e> <C-y> "
		  "<PageUp> <PageDown> <Del> <Insert>")) {
		snprintf(c->key, sizeof(c->key), "%s", t);
		return P_OK;
	}
	return parse_motion(k, i, c);

operand:
	c->c2a = i;
	n2 = parse_count(k, &i);
	c->c2b = i;
	if (n2)
		c->count = (c->count ? c->count : 1) * n2;
	if (i >= k->n)
		return P_MORE;
	t = k->k[i];
	if (is(t, "<Esc>"))
		return P_CANCEL;
	/* dd, yy, >>, guu and gugu act on whole lines */
	if (is(t, c->op + strlen(c->op) - 1)) {
		c->doubled = 1;
		return P_OK;
	}
	if (c->op[0] == 'g' && is(t, "g")) {
		if (i + 1 >= k->n)
			return P_MORE;
		if (is(k->k[i + 1], c->op + 1)) {
			c->doubled = 1;
			return P_OK;
		}
	}
	if (among(t, "i a"))
		return need_arg(k, i, c);
	return parse_motion(k, i, c);
}

/* ---- running commands ---- */

static void enter_visual(struct editor *e, int mode)
{
	e->vstart = e->cur;
	e->mode = mode;
}

/* scroll by lines, moving the cursor along when jump is set */
static void scroll_lines(struct editor *e, int n, int move)
{
	int last = ed_lines(e) - 1, line = ed_line_of(e, e->cur);
	size_t col = e->want;

	e->top += n;
	if (e->top > last)
		e->top = last;
	if (e->top < 0)
		e->top = 0;
	if (move)
		line += n;
	else if (line < e->top)
		line = e->top;
	else if (line >= e->top + e->vis)
		line = e->top + e->vis - 1;
	line = line < 0 ? 0 : line > last ? last : line;
	e->cur = ed_at_col(e, ed_line_pos(e, line), col);
}

static int scroll_key(struct editor *e, const char *k, int count)
{
	int half = count ? count : e->vis > 1 ? e->vis / 2 : 1;
	int page = e->vis > 3 ? e->vis - 2 : 1;

	if (is(k, "<C-d>") || is(k, "<C-u>"))
		scroll_lines(e, k[3] == 'd' ? half : -half, 1);
	else if (among(k, "<C-f> <PageDown> <C-b> <PageUp>"))
		scroll_lines(e, (is(k, "<C-f>") || is(k, "<PageDown>") ?
				 page : -page) * (count ? count : 1), 1);
	else if (is(k, "<C-e>") || is(k, "<C-y>"))
		scroll_lines(e, (k[3] == 'e' ? 1 : -1) * (count ? count : 1),
			     0);
	else
		return 0;
	return 1;
}

static int play_macro(int name, int count)
{
	struct keys k = { 0 }, all = { 0 };
	const char *s;
	int type, i, j;

	if (name == '@')
		name = last_macro;
	if (!name) {
		vim_msg(1, "no macro to run");
		return 0;
	}
	last_macro = name;
	/* an empty register does nothing, as in a recursive recording */
	if (!(s = vim_register(name, &type)))
		return 1;
	decode(&k, s);
	if (k.n && count > MAXKEYS / k.n)
		count = MAXKEYS / k.n;
	for (i = 0; i < count; i++)
		for (j = 0; j < k.n; j++)
			keys_add(&all, k.k[j]);
	/* the macro runs before whatever was still waiting */
	for (i = ahead_pos; i < ahead.n; i++)
		keys_add(&all, ahead.k[i]);
	free(ahead.k);
	free(k.k);
	ahead = all;
	ahead_pos = 0;
	return 1;
}

static void stop_recording(void)
{
	/* the "q" that stops was recorded before it arrived here */
	if (rec_buf.len && rec_buf.p[rec_buf.len - 1] == 'q')
		rec_buf.p[--rec_buf.len] = 0;
	buf_add(&rec_buf, "", 0);
	reg_named(rec_reg, rec_buf.p, R_CHAR);
	rec_reg = 0;
}

static int dot_repeat(struct editor *e, int count)
{
	struct keys k = { 0 };
	struct change_cmd d = { 0 };
	char num[16], *p, one[2] = { 0, 0 };
	int i, ok = 1;

	if (!dot.k.n)
		return 0;
	d = dot;
	memset(&d.k, 0, sizeof(d.k));
	keys_copy(&d.k, &dot.k, 0);
	if (count)
		d.count = count;
	if (d.reg) {
		/* numbered registers advance so that "1p.. walks them */
		if (d.reg >= '1' && d.reg < '9')
			dot.reg = ++d.reg;
		one[0] = d.reg;
		keys_add(&k, "\"");
		keys_add(&k, one);
	}
	if (d.count) {
		snprintf(num, sizeof(num), "%d", d.count);
		for (p = num; *p; p++) {
			one[0] = *p;
			keys_add(&k, one);
		}
	}
	for (i = 0; i < d.k.n; i++)
		keys_add(&k, d.k.k[i]);
	if (d.vmode) {
		int line = ed_line_of(e, e->cur) + d.vlines;
		size_t col = ed_col(e, e->cur);
		size_t ls = ed_line_pos(e, line);

		enter_visual(e, d.vmode);
		e->cur = ed_at_col(e, ls, d.vlines ? d.vcols :
				   col + d.vcols);
	}
	replaying++;
	for (i = 0; i < k.n && ok; i++)
		ok = process(e, k.k[i]) != ED_FAIL;
	replaying--;
	if (count)
		dot.count = count;
	free(k.k);
	free(d.k.k);
	return ok;
}

/* remember a change so that "." can repeat it */
static void dot_start(struct editor *e, const struct cmd *c)
{
	int i;

	next_dot.k.n = 0;
	next_dot.reg = c->reg;
	next_dot.count = c->count;
	next_dot.vmode = 0;
	for (i = c->start; i < e->pend.n; i++)
		if (i < c->c2a || i >= c->c2b)
			keys_add(&next_dot.k, e->pend.k[i]);
}

static void dot_shape(struct editor *e)
{
	int l1 = ed_line_of(e, e->vstart), l2 = ed_line_of(e, e->cur);
	size_t c1 = ed_col(e, e->vstart), c2 = ed_col(e, e->cur);
	int backward = e->cur < e->vstart;

	next_dot.vmode = e->mode;
	next_dot.vlines = abs(l2 - l1);
	if (l1 == l2 || e->mode == ED_VBLOCK)
		next_dot.vcols = c1 > c2 ? c1 - c2 : c2 - c1;
	else
		next_dot.vcols = backward ? c1 : c2;
}

static void dot_commit(void)
{
	struct keys old = dot.k;

	dot = next_dot;
	next_dot.k = old;
	next_dot.k.n = 0;
	dot_open = 0;
}

static int run_ex(struct editor *e, const char *line)
{
	if (*line) {
		free(history[1][31]);
		history[1][31] = NULL;
		memmove(history[1] + 1, history[1], 31 * sizeof(char *));
		history[1][0] = xstrdup(line);
		if (nhistory[1] < 32)
			nhistory[1]++;
	}
	return ex_execute(e, line);
}

static int visual_command(struct editor *e, struct cmd *c)
{
	const char *k = c->op[0] ? c->op : c->key;
	int count = c->count ? c->count : 1, first, last, linewise, l;
	size_t a, b, left, right, x, (*part)[2];
	struct motion m;
	struct range r;

	if (is(k, "<Esc>")) {
		leave_visual(e);
		return ED_OK;
	}
	if (among(k, "v V <C-v>")) {
		int mode = k[0] == 'v' ? ED_VISUAL :
		    k[0] == 'V' ? ED_VLINE : ED_VBLOCK;

		if (e->mode == mode)
			leave_visual(e);
		else
			e->mode = mode;
		return ED_OK;
	}
	if (among(k, "o O")) {
		x = e->cur;
		e->cur = e->vstart;
		e->vstart = x;
		e->want = ed_col(e, e->cur);
		return ED_OK;
	}
	if (is(k, "gv")) {
		int mode = e->lastvis;

		a = e->mark[26];
		b = e->mark[27];
		if (a == NOMARK || !mode)
			return ED_FAIL;
		leave_visual(e);
		e->mode = mode;
		e->vstart = a;
		e->cur = b;
		return ED_OK;
	}
	if (is(k, "i") || is(k, "a")) {
		if (!text_object(e, k[0] == 'a', c->arg, count, &r))
			return ED_FAIL;
		e->vstart = r.a;
		e->cur = r.b > r.a ? ed_prev(e, r.b) : r.a;
		if (r.type == M_LINE) {
			e->mode = ED_VLINE;
			e->cur = ed_line_start(e, r.b);
		} else if (e->mode == ED_VLINE) {
			e->mode = ED_VISUAL;
		}
		return ED_OK;
	}
	if (scroll_key(e, k, c->count))
		return ED_OK;
	if (among(k, "q @ m z Z"))
		return ED_UNHANDLED;
	if (!c->op[0] && !among(k, "d x <Del> X D y Y c s S R C p P J gJ ~ u U "
				"> < I A r :")) {
		if (!motion(e, c, c->count, 0, &m))
			return ED_FAIL;
		if (is_jump(c->key))
			e->mark[28] = e->cur;
		e->cur = m.to;
		if (among(c->key, "$ <End>"))
			e->want = WANT_EOL;
		else if (!among(c->key, "j k <Down> <Up>"))
			e->want = ed_col(e, e->cur);
		return ED_OK;
	}
	if (e->readonly && !among(k, "y Y :"))
		return ED_FAIL;
	ed_selection(e, &a, &b);
	/* whole lines keep the column only of a cursor above the anchor */
	if (e->mode == ED_VLINE)
		a = e->cur < e->vstart ? e->cur :
		    ed_line_start(e, e->vstart);
	block_bounds(e, &first, &last, &left, &right);
	linewise = e->mode == ED_VLINE || among(k, "X D Y S R C");
	if (!replaying && !among(k, "y Y :")) {
		dot_shape(e);
		dot_open = 1;
	}
	vim_checkpoint(e);
	if (e->mode == ED_VBLOCK && among(k, "d x <Del> y c s I A D X C")) {
		char *s;
		int eol = e->want == WANT_EOL || is(k, "D") || is(k, "C");

		if (eol)
			e->want = WANT_EOL;
		x = ed_at_col(e, ed_line_pos(e, first), left);
		if (!among(k, "I A")) {
			s = ed_selected_text(e);
			vim_set_register(c->reg, s, R_BLOCK, is(k, "y"));
			free(s);
		}
		if (among(k, "d x <Del> D X c s C"))
			block_delete(e, first, last);
		leave_visual(e);
		e->cur = x;
		if (among(k, "c s C I A")) {
			size_t col = is(k, "A") ? right + 1 : left;
			size_t ls;

			ls = ed_line_pos(e, first);
			if (is(k, "A")) {
				if (!eol)
					while (ed_col(e, ed_line_end(e, ls)) <
					       col)
						ed_splice(e,
							  ed_line_end(e, ls), 0,
							  " ", 1);
				e->cur = eol ? ed_line_end(e, ls) :
				    ed_at_col(e, ls, col);
			}
			start_insert(e, 1, 0);
			e->bi = 1;
			e->bi_first = first;
			e->bi_last = last;
			e->bi_col = col;
			e->bi_pos = e->cur;
			e->bi_home = ed_at_col(e, ls, left);
			e->bi_append = is(k, "A");
			e->bi_eol = eol && is(k, "A");
		}
		return ED_OK;
	}
	if (is(k, "I") || is(k, "A")) {
		leave_visual(e);
		e->cur = k[0] == 'I' ? ed_first_nonblank(e, a) : b;
		start_insert(e, 1, 0);
		return ED_OK;
	}
	if (is(k, ":")) {
		leave_visual(e);
		return ED_OK;
	}
	if (is(k, "r")) {
		const char *s = key_char(c->arg);

		if (!s)
			return ED_FAIL;
		part = line_parts(e, first, last);
		for (l = last - first; l >= 0; l--) {
			size_t p = part[l][0], q = part[l][1];
			struct buf out = { 0 };

			if (p == NOMARK)
				continue;
			if (q > ed_line_end(e, p))
				q = ed_line_end(e, p);
			for (x = p; x < q; x = ed_next(e, x))
				buf_puts(&out, s);
			buf_add(&out, "", 0);
			ed_splice(e, p, q - p, out.p, out.len);
			buf_free(&out);
		}
		free(part);
		leave_visual(e);
		e->cur = a;
		return ED_OK;
	}
	if (among(k, "u U ~ gu gU g~")) {
		int how = k[strlen(k) - 1];

		part = line_parts(e, first, last);
		for (l = last - first; l >= 0; l--)
			if (part[l][0] != NOMARK)
				recase(e, part[l][0], part[l][1] > ed_len(e) ?
				       ed_len(e) : part[l][1], how);
		free(part);
		leave_visual(e);
		e->cur = a;
		return ED_OK;
	}
	x = e->mode == ED_VBLOCK ?
	    ed_at_col(e, ed_line_pos(e, first), left) : NOMARK;
	leave_visual(e);
	e->cur = a;
	if (is(k, ">") || is(k, "<")) {
		if (x == NOMARK) {
			indent(e, first, last, k[0] == '>' ? 1 : -1, count);
			return ED_OK;
		}
		for (l = last; l >= first; l--) {
			size_t ls = ed_line_pos(e, l), q;
			int w = SHIFTWIDTH * count;

			if (ed_col(e, ed_line_end(e, ls)) <= left)
				continue;
			q = ed_at_col(e, ls, left);
			if (k[0] == '>') {
				while (w--)
					ed_splice(e, q, 0, " ", 1);
			} else {
				size_t z = q;

				while (w-- && ch(e, z) == ' ')
					z++;
				ed_splice(e, q, z - q, NULL, 0);
			}
		}
		e->cur = x;
		return ED_OK;
	}
	if (is(k, "J") || is(k, "gJ")) {
		if (first == ed_lines(e) - 1) {
			e->cur = a;
			return ED_FAIL;
		}
		e->cur = ed_line_pos(e, first);
		join(e, last > first ? last - first + 1 : 2, k[0] == 'J');
		return ED_OK;
	}
	if (is(k, "gq") || is(k, "gw")) {
		ex_reflow(e, first, last);
		return ED_OK;
	}
	if (linewise) {
		a = ed_line_pos(e, first);
		b = ed_line_end(e, ed_line_pos(e, last));
	} else if (b > ed_len(e)) {
		b = ed_len(e);
	}
	if (is(k, "p") || is(k, "P")) {
		int type = R_CHAR;
		const char *s = vim_register(c->reg, &type);
		char *text, *gone;
		struct buf in = { 0 };

		if (!s)
			return ED_FAIL;
		text = xstrdup(s);
		/* one line of text replaces the block on every line */
		if (x != NOMARK && type == R_CHAR && !strchr(text, '\n')) {
			gone = xstrdup("");
			e->mode = ED_VBLOCK;
			e->vstart = e->mark[26];
			e->cur = e->mark[27];
			part = line_parts(e, first, last);
			for (l = last - first; l >= 0; l--)
				if (part[l][0] != NOMARK)
					ed_splice(e, part[l][0],
						  part[l][1] - part[l][0],
						  text, strlen(text));
			free(part);
			leave_visual(e);
			e->cur = x;
			free(text);
			free(gone);
			return ED_OK;
		}
		gone = ed_slice(e, a, b);
		for (l = 0; l < count; l++)
			buf_puts(&in, text);
		buf_add(&in, "", 0);
		if (type == R_LINE && in.len && in.p[in.len - 1] == '\n')
			in.p[--in.len] = 0;
		if (type == R_LINE && !linewise) {
			char *t = xasprintf("\n%s\n", in.p);

			ed_splice(e, a, b - a, t, strlen(t));
			e->cur = a + 1;
			free(t);
		} else {
			ed_splice(e, a, b - a, in.p, in.len);
			e->cur = linewise || type == R_LINE ?
			    ed_first_nonblank(e, a) :
			    in.len ? ed_prev(e, a + in.len) : a;
		}
		/* "P" keeps the register, "p" takes the replaced text */
		if (k[0] == 'p') {
			char *t = linewise ? xasprintf("%s\n", gone) :
			    xstrdup(gone);

			vim_set_register(0, t, linewise ? R_LINE : R_CHAR, 0);
			free(t);
		}
		free(text);
		free(gone);
		buf_free(&in);
		return ED_OK;
	}
	e->cur = a;
	return operate(e, among(k, "y Y") ? "y" :
		       among(k, "c s S R C") ? "c" : "d", a, b,
		       linewise ? M_LINE : M_EXCL, c->reg, 1) ? ED_OK : ED_FAIL;
}

/* run a complete normal mode command */
static int normal_command(struct editor *e, struct cmd *c)
{
	const char *k = c->key;
	int count = c->count ? c->count : 1, i;
	size_t p = e->cur, ls = ed_line_start(e, p), le = ed_line_end(e, p);
	struct motion m;
	struct range r;
	const char *a;

	if (c->op[0]) {
		if (c->doubled) {
			int last = ed_line_of(e, p) + count - 1;

			if (count > 1 && ed_line_of(e, p) == ed_lines(e) - 1)
				return ED_FAIL;
			if (last >= ed_lines(e))
				last = ed_lines(e) - 1;
			if (count == 1 && !is(c->op, "y") &&
			    ed_first_nonblank(e, p) < p)
				e->cur = ed_first_nonblank(e, p);
			r.a = ls;
			r.b = ed_line_end(e, ed_line_pos(e, last));
			r.type = M_LINE;
			if (is(c->op, "c") || among(c->op, "> <"))
				count = 1;
		} else if (is(k, "i") || is(k, "a")) {
			if (!text_object(e, k[0] == 'a', c->arg, count, &r))
				return ED_FAIL;
			if (r.type == M_LINE)
				e->cur = r.a;
		} else {
			size_t from = e->cur;

			if (!motion(e, c, c->count, 1, &m)) {
				/* "cl" works even on an empty line */
				if (!is(c->op, "c") || !among(k, "h l <Space>"))
					return ED_FAIL;
				m.to = from;
				m.kind = M_EXCL;
			}
			motion_range(e, &m, &r);
			if (is(c->op, "d") && r.type == M_INCL &&
			    r.a <= ed_first_nonblank(e, r.a) &&
			    ed_line_start(e, r.b) > r.a) {
				size_t q = r.b, end = ed_line_end(e, r.b);

				while (q < end && !vim_class(e, q, 1))
					q++;
				if (q == end) {
					r.type = M_LINE;
					r.a = ed_line_start(e, r.a);
					r.b = end;
				}
			}
			/* whole lines leave the cursor at the earlier end */
			if (r.type == M_LINE &&
			    !(is(k, "_") && is(c->op, "y")))
				e->cur = m.to < from ? m.to : from;
		}
		vim_checkpoint(e);
		big_delete = !c->doubled &&
		    among(k, "% ( ) { } / ? n N `");
		i = operate(e, c->op, r.a, r.b, r.type, c->reg, 1);
		big_delete = 0;
		return i ? ED_OK : ED_FAIL;
	}
	if (is(k, "<Esc>"))
		return ED_ESCAPE;
	if (is(k, "<CR>") && e->single)
		return ED_ENTER;
	if (among(k, "v V <C-v>")) {
		enter_visual(e, k[0] == 'v' ? ED_VISUAL :
			     k[0] == 'V' ? ED_VLINE : ED_VBLOCK);
		return ED_OK;
	}
	if (is(k, "gv")) {
		if (e->mark[26] == NOMARK || !e->lastvis)
			return ED_FAIL;
		e->mode = e->lastvis;
		e->vstart = e->mark[26];
		e->cur = e->mark[27];
		return ED_OK;
	}
	if (is(k, "m")) {
		a = key_char(c->arg);
		i = a ? mark_index(a[0]) : -1;
		if (i < 0)
			return ED_FAIL;
		e->mark[i] = p;
		return ED_OK;
	}
	if (is(k, "q")) {
		if (rec_reg) {
			stop_recording();
		} else {
			a = key_char(c->arg);
			if (!a || (!isalnum((unsigned char)a[0]) &&
				   a[0] != '"'))
				return ED_FAIL;
			rec_reg = a[0];
			buf_reset(&rec_buf);
		}
		return ED_OK;
	}
	if (is(k, "@")) {
		a = key_char(c->arg);
		if (a && a[0] == ':') {
			if (!history[1][0])
				return ED_FAIL;
			return ex_execute(e, history[1][0]);
		}
		return a && play_macro(a[0], count) ? ED_OK : ED_FAIL;
	}
	if (is(k, "z")) {
		int line = ed_line_of(e, p);

		if (among(c->arg, "z ."))
			e->top = line - e->vis / 2;
		else if (among(c->arg, "t <CR>"))
			e->top = line;
		else if (among(c->arg, "b -"))
			e->top = line - e->vis + 1;
		if (e->top < 0)
			e->top = 0;
		return ED_OK;
	}
	if (is(k, "Z")) {
		if (!among(c->arg, "Z Q"))
			return ED_FAIL;
		buf_reset(&vim.command);
		buf_puts(&vim.command, c->arg[0] == 'Z' ? "x" : "q!");
		return ED_EX;
	}
	if (scroll_key(e, k, c->count))
		return ED_OK;
	if (is(k, "u") || is(k, "<C-r>")) {
		if (e->readonly)
			return ED_FAIL;
		for (i = 0; i < count; i++)
			if (!(k[0] == 'u' ? ed_undo(e) : ed_redo(e)))
				break;
		if (!i) {
			vim_msg(1, k[0] == 'u' ? "already at oldest change" :
				"already at newest change");
			return ED_FAIL;
		}
		return ED_OK;
	}
	if (is(k, "."))
		return dot_repeat(e, c->count) ? ED_OK : ED_FAIL;
	if (is(k, "&"))
		return ex_execute(e, "&");

	if (!among(k, "x X s S C D Y J gJ p P gp gP o O i I gI a A R r ~ <C-a> "
		   "<C-x> <Del> <Insert>")) {
		if (!motion(e, c, c->count, 0, &m))
			return ED_FAIL;
		if (is_jump(k))
			vim_jump(e, m.to);
		else
			e->cur = m.to;
		if (among(k, "$ <End>"))
			e->want = WANT_EOL;
		else if (!among(k, "j k <Down> <Up> gj gk"))
			e->want = ed_col(e, e->cur);
		return ED_OK;
	}
	if (is(k, "Y")) {
		strcpy(c->op, "y");
		c->doubled = 1;
		return normal_command(e, c);
	}
	if (e->readonly)
		return ED_FAIL;
	vim_checkpoint(e);
	if (is(k, "i") || is(k, "<Insert>")) {
		start_insert(e, count, 0);
	} else if (is(k, "a")) {
		if (p < le)
			e->cur = ed_next(e, p);
		start_insert(e, count, 0);
	} else if (is(k, "I") || is(k, "gI")) {
		e->cur = k[0] == 'I' ? ed_first_nonblank(e, p) : ls;
		start_insert(e, count, 0);
	} else if (is(k, "A")) {
		e->cur = le;
		start_insert(e, count, 0);
	} else if (is(k, "R")) {
		start_insert(e, count, 0);
		e->mode = ED_REPLACE;
	} else if (is(k, "o") || is(k, "O")) {
		if (e->single)
			return ED_FAIL;
		size_t at = k[0] == 'o' ? le : ls;

		ed_splice(e, at, 0, "\n", 1);
		e->cur = at + (k[0] == 'o');
		start_insert(e, count, k[0]);
	} else if (among(k, "x <Del> s")) {
		size_t q = p;

		for (i = 0; i < count && q < le; i++)
			q = ed_next(e, q);
		/* nothing to delete is no error, so macros go on */
		if (q > p || k[0] == 's')
			operate(e, k[0] == 's' ? "c" : "d", p, q, M_EXCL,
				c->reg, 1);
	} else if (is(k, "X")) {
		size_t q = p;

		for (i = 0; i < count && q > ls; i++)
			q = ed_prev(e, q);
		if (q < p)
			operate(e, "d", q, p, M_EXCL, c->reg, 1);
	} else if (among(k, "S D C")) {
		int last = ed_line_of(e, p) + count - 1;
		size_t end;

		if (count > 1 && ed_line_of(e, p) == ed_lines(e) - 1)
			return ED_FAIL;
		if (last >= ed_lines(e))
			last = ed_lines(e) - 1;
		end = ed_line_end(e, ed_line_pos(e, last));
		if (k[0] == 'S')
			operate(e, "c", ls, end, M_LINE, c->reg, 1);
		else if (k[0] == 'D' && last > ed_line_of(e, p) &&
			 p <= ed_first_nonblank(e, p))
			operate(e, "d", ls, end, M_LINE, c->reg, 1);
		else
			operate(e, k[0] == 'D' ? "d" : "c", p, end, M_EXCL,
				c->reg, 1);
	} else if (is(k, "J") || is(k, "gJ")) {
		int left = ed_lines(e) - ed_line_of(e, p);

		if (count < 2)
			count = 2;
		if (e->single || (count > left && count <= 2))
			return ED_FAIL;
		if (count > left)
			count = left;
		if (count < 2)
			e->cur = ls;
		else
			join(e, count, k[0] == 'J');
	} else if (among(k, "p P gp gP")) {
		return paste(e, c->reg, count, k[strlen(k) - 1] == 'P',
			     k[0] == 'g') ? ED_OK : ED_FAIL;
	} else if (is(k, "r")) {
		const char *with = key_char(c->arg);
		struct buf out = { 0 };
		size_t q = p;

		for (i = 0; i < count; i++) {
			if (q >= le)
				return ED_FAIL;
			q = ed_next(e, q);
		}
		if (is(c->arg, "<CR>")) {
			ed_splice(e, p, q - p, "\n", 1);
			e->cur = p + 1;
			return ED_OK;
		}
		if (!with)
			return ED_FAIL;
		for (i = 0; i < count; i++)
			buf_puts(&out, with);
		ed_splice(e, p, q - p, out.p, out.len);
		e->cur = p + out.len - strlen(with);
		buf_free(&out);
	} else if (is(k, "~")) {
		size_t q = p;

		for (i = 0; i < count && q < le; i++)
			q = ed_next(e, q);
		recase(e, p, q, '~');
		e->cur = q;
	} else {
		if (!add_number(e, k[3] == 'a' ? count : -count))
			return ED_FAIL;
	}
	return ED_OK;
}

/* edit a "/", "?" or ":" line that is being typed */
static int prompt_edit(struct editor *e, int at, const char *k)
{
	int kind = is(e->pend.k[at], ":");

	if (is(k, "<Esc>")) {
		e->pend.n = 0;
	} else if (is(k, "<BS>")) {
		if (--e->pend.n == at)
			e->pend.n = 0;
	} else if (is(k, "<C-u>")) {
		e->pend.n = at + 1;
	} else if (is(k, "<C-w>")) {
		while (e->pend.n > at + 1 &&
		       is(e->pend.k[e->pend.n - 1], "<Space>"))
			e->pend.n--;
		while (e->pend.n > at + 1 &&
		       !is(e->pend.k[e->pend.n - 1], "<Space>"))
			e->pend.n--;
	} else if (kind && (is(k, "<Up>") || is(k, "<Down>"))) {
		/* recall earlier commands */
		hist_at += k[1] == 'U' ? 1 : -1;
		if (hist_at >= nhistory[1])
			hist_at = nhistory[1] - 1;
		e->pend.n = at + 1;
		if (hist_at < 0)
			hist_at = -1;
		else
			decode(&e->pend, history[1][hist_at]);
	} else {
		return 0;
	}
	return 1;
}

static int normal_mode(struct editor *e, const char *k)
{
	static struct cmd c;
	struct keys shown;
	int visual = is_visual(e), st, r, i, change;
	unsigned seq = e->seq;

	/* editing keys apply to an open prompt, not to the command */
	if (e->pend.n && parse(&e->pend, visual, &c) == P_MORE &&
	    c.prompt >= 0 && prompt_edit(e, c.prompt, k))
		return ED_OK;
	keys_add(&e->pend, k);
	st = parse(&e->pend, visual, &c);
	if (st == P_MORE) {
		if (visual && is(k, ":") && c.prompt == e->pend.n - 1) {
			/* commands from visual mode act on its lines */
			decode(&e->pend, "'<,'>");
			hist_at = -1;
		}
		return ED_OK;
	}
	if (st == P_CANCEL) {
		e->pend.n = 0;
		return ED_OK;
	}
	if (st == P_BAD) {
		e->pend.n = 0;
		return ED_FAIL;
	}
	if (is(c.key, ":")) {
		char *line = xstrdup(c.text.p ? c.text.p : "");

		e->pend.n = 0;
		hist_at = -1;
		if (visual)
			leave_visual(e);
		r = run_ex(e, line);
		free(line);
		if (e->mode == ED_NORMAL)
			vim_clamp(e);
		return r;
	}
	change = !visual && !replaying &&
	    !among(c.key, "u <C-r> . @ q : Z");
	if (change || (visual && !replaying))
		dot_start(e, &c);
	/* the command may run others, so its keys are set aside */
	shown = e->pend;
	memset(&e->pend, 0, sizeof(e->pend));
	r = visual ? visual_command(e, &c) : normal_command(e, &c);
	if (!e->pend.k) {
		e->pend = shown;
		e->pend.n = 0;
	} else {
		free(shown.k);
	}
	if (visual)
		change = !replaying && dot_open;
	if (change) {
		if (e->mode == ED_INSERT || e->mode == ED_REPLACE) {
			dot_open = 1;
			dot_seq = e->seq;
		} else if (e->seq != seq || among(c.key, "x X") ||
			   (r == ED_OK && c.op[0] && !is(c.op, "y"))) {
			dot_commit();
		} else {
			dot_open = 0;
		}
	}
	if ((e->seq != seq || (visual && !is_visual(e))) &&
	    e->mode == ED_NORMAL) {
		vim_clamp(e);
		e->want = ed_col(e, e->cur);
	}
	/* back to inserting after the one command that <C-o> allows */
	if (e->ctrl_o && !is_visual(e)) {
		i = e->ctrl_o;
		e->ctrl_o = 0;
		if (e->mode == ED_NORMAL)
			e->mode = i;
	}
	if (e->mode == ED_NORMAL)
		vim_clamp(e);
	return r;
}

static int process(struct editor *e, const char *k)
{
	int r;

	if (e->mode == ED_INSERT || e->mode == ED_REPLACE) {
		if (!replaying && dot_open)
			keys_add(&next_dot.k, k);
		r = insert_mode(e, k);
		if (!replaying && dot_open && e->mode == ED_NORMAL &&
		    !e->ctrl_o)
			dot_commit();
		return r;
	}
	return normal_mode(e, k);
}

/* run keys as typed, as ":normal" does, and finish unfinished commands */
int vim_run(struct editor *e, const char *text)
{
	struct keys k = { 0 }, saved = ahead;
	int pos = ahead_pos, steps = 0, i, ok = 1;
	char next[KEYLEN];

	memset(&ahead, 0, sizeof(ahead));
	ahead_pos = 0;
	decode(&k, text);
	replaying++;
	for (i = 0; i < k.n && ok; i++) {
		ok = process(e, normalize(k.k[i])) != ED_FAIL;
		while (ok && steps++ < MAXKEYS && vim_next(next))
			ok = process(e, normalize(next)) != ED_FAIL;
	}
	if (e->mode == ED_INSERT || e->mode == ED_REPLACE)
		process(e, "<Esc>");
	if (is_visual(e))
		leave_visual(e);
	e->pend.n = 0;
	replaying--;
	free(k.k);
	free(ahead.k);
	ahead = saved;
	ahead_pos = pos;
	return ok;
}

/* ---- the interface ---- */

/*
 * feed one key token to the editor, such as "j" or "<CR>". Without vim the
 * editor is a plain text field. Returns one of the ED_ results.
 */
int ed_key(struct editor *e, const char *key, const char *text, int use_vim)
{
	size_t a, b;
	const char *k = normalize(key);
	char typed[KEYLEN];
	int r;

	/* composed input can be a character the token does not carry */
	if (text && key[0] != '<' && strlen(text) < KEYLEN &&
	    (unsigned char)text[0] >= 0x20) {
		snprintf(typed, sizeof(typed), "%s", text);
		k = normalize(typed);
	}
	if (e->typeover) {
		e->typeover = 0;
		if (ed_selection(e, &a, &b)) {
			e->mode = ED_INSERT;
			if (is(k, "<BS>") || is(k, "<Del>")) {
				ed_checkpoint(e);
				ed_remove(e, a, b);
				return ED_OK;
			}
			if (key_char(k)) {
				ed_checkpoint(e);
				ed_remove(e, a, b);
			}
		}
	}
	if (!use_vim) {
		e->mode = ED_INSERT;
		if (is(k, "<Esc>"))
			return ED_ESCAPE;
		r = cursor_key(e, k);
		return r == ED_OK ? r : insert_edit(e, k);
	}
	r = process(e, k);
	if (r == ED_FAIL)
		vim_abort();
	return r;
}

/* whether the next key belongs to an unfinished command */
int ed_pending(struct editor *e)
{
	return e->pend.n > 0 || e->ctrl_o;
}

static const char *join_keys(struct editor *e, int from)
{
	static struct buf b;
	int i;

	buf_reset(&b);
	for (i = from; i < e->pend.n; i++) {
		const char *c = key_char(e->pend.k[i]);

		buf_puts(&b, c ? c : e->pend.k[i]);
	}
	buf_add(&b, "", 0);
	return b.p;
}

/* scroll the view, keeping the cursor on a visible line */
void ed_scroll(struct editor *e, int lines)
{
	scroll_lines(e, lines, 0);
	vim_clamp(e);
}

/* run a ":" command line without typing it */
int ed_command(struct editor *e, const char *line)
{
	return ex_execute(e, line);
}

/* the keys of an unfinished command, for the status bar */
const char *ed_showcmd(struct editor *e)
{
	return e->pend.n ? join_keys(e, 0) : "";
}

/* the "/", "?" or ":" line being typed, or NULL */
const char *ed_prompt(struct editor *e)
{
	static struct cmd c;

	if (!e->pend.n || e->mode == ED_INSERT || e->mode == ED_REPLACE ||
	    parse(&e->pend, is_visual(e), &c) != P_MORE || c.prompt < 0)
		return NULL;
	return join_keys(e, c.prompt);
}

int vim_recording(void)
{
	return rec_reg;
}

/* end a recording from outside the editor */
void vim_stop_recording(void)
{
	if (rec_reg)
		stop_recording();
}

/* add a typed key to the macro being recorded */
void vim_record(const char *key)
{
	if (rec_reg)
		encode(&rec_buf, key);
}

/* the next key a macro wants typed */
int vim_next(char *key)
{
	if (ahead_pos >= ahead.n) {
		ahead.n = ahead_pos = 0;
		ahead_steps = 0;
		return 0;
	}
	if (++ahead_steps > MAXKEYS) {
		vim_msg(1, "macro stopped after %d keys", MAXKEYS);
		vim_abort();
		return 0;
	}
	snprintf(key, KEYLEN, "%s", ahead.k[ahead_pos++]);
	return 1;
}

/* stop a running macro, as vim does when a command fails */
void vim_abort(void)
{
	ahead.n = ahead_pos = 0;
	ahead_steps = 0;
}

/* forget registers, searches and repeats */
void vim_reset(void)
{
	int i;

	for (i = 0; i < 128; i++) {
		free(regs[i].s);
		regs[i].s = NULL;
		regs[i].type = R_CHAR;
	}
	free(seen);
	seen = NULL;
	free(vim.pattern);
	vim.pattern = NULL;
	free(vim.sub_pat);
	free(vim.sub_rep);
	free(vim.sub_flags);
	vim.sub_pat = vim.sub_rep = vim.sub_flags = NULL;
	buf_reset(&vim.msg);
	dot.k.n = 0;
	dot_open = rec_reg = last_macro = 0;
	find_cmd = 0;
	vim_abort();
}

/* group the changes of a longer command into one undo step */
void vim_hold(int on)
{
	hold += on ? 1 : -1;
}
