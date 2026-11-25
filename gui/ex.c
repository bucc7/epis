/* ex.c - searching, ":" commands and paragraph formatting */
#include <ctype.h>
#include <locale.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

#include "vim.h"

#ifdef __APPLE__
#define WORD_START "[[:<:]]"
#define WORD_END "[[:>:]]"
#else
#define WORD_START "\\<"
#define WORD_END "\\>"
#endif

#define NGROUPS 10

struct rx {
	regex_t re;
	/* the pattern starts with "\<" */
	int word;
};

/*
 * patterns match characters rather than bytes: use a UTF-8 locale around
 * the regex calls only, as the rest of the program expects the C locale
 */
static locale_t utf8_locale(void)
{
	static locale_t l;

	if (!l && !(l = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0)))
		l = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
	return l;
}

static int run(const regex_t *re, const char *s, size_t from, size_t len,
	       regmatch_t *m)
{
	locale_t old = utf8_locale() ? uselocale(utf8_locale()) : (locale_t)0;
	int r;

	m[0].rm_so = from;
	m[0].rm_eo = len;
	r = regexec(re, s, NGROUPS, m, REG_STARTEND |
		    (from ? REG_NOTBOL : 0));
	if (old)
		uselocale(old);
	return !r;
}

static int word_byte(unsigned char c)
{
	return isalnum(c) || c == '_' || c >= 0x80;
}

/*
 * match from an offset into a line. The BSD library does not look before
 * the offset, so it would find a word start in the middle of a word.
 */
static int match(const struct rx *x, const char *s, size_t from, size_t len,
		 regmatch_t *m)
{
	while (from <= len && run(&x->re, s, from, len, m)) {
		size_t p = m[0].rm_so;

		if (!x->word || !p || p >= len || !word_byte(s[p - 1]) ||
		    !word_byte(s[p]))
			return 1;
		from = p + 1;
	}
	return 0;
}

/*
 * translate a vim "magic" pattern to an extended regular expression.
 * Case is ignored unless the pattern has capitals, or as \c and \C say.
 */
static int compile(const char *pat, int icase, struct rx *x)
{
	struct buf b = { 0 };
	const char *p;
	int forced = -1, upper = 0, brace = 0, r;
	locale_t old;

	for (p = pat; *p; p++) {
		if (*p == '[') {
			const char *q = p + 1;

			if (*q == '^')
				q++;
			if (*q == ']')
				q++;
			while (*q && *q != ']')
				q++;
			if (*q) {
				buf_add(&b, p, q - p + 1);
				p = q;
				continue;
			}
			buf_puts(&b, "\\[");
		} else if (*p == '\\' && p[1]) {
			char c = *++p;

			if (strchr("()|+?", c))
				buf_putc(&b, c);
			else if (c == '=')
				buf_putc(&b, '?');
			else if (c == '{') {
				buf_putc(&b, '{');
				brace = 1;
			} else if (c == '}') {
				buf_putc(&b, '}');
				brace = 0;
			} else if (c == '<')
				buf_puts(&b, WORD_START);
			else if (c == '>')
				buf_puts(&b, WORD_END);
			else if (c == 'd')
				buf_puts(&b, "[0-9]");
			else if (c == 'D')
				buf_puts(&b, "[^0-9]");
			else if (c == 'w')
				buf_puts(&b, "[A-Za-z0-9_]");
			else if (c == 'W')
				buf_puts(&b, "[^A-Za-z0-9_]");
			else if (c == 's')
				buf_puts(&b, "[ \t]");
			else if (c == 'S')
				buf_puts(&b, "[^ \t]");
			else if (c == 'a')
				buf_puts(&b, "[A-Za-z]");
			else if (c == 't')
				buf_putc(&b, '\t');
			else if (c == 'c' || c == 'C')
				forced = c == 'c';
			else if (c == '/')
				buf_putc(&b, '/');
			else {
				buf_putc(&b, '\\');
				buf_putc(&b, c);
			}
		} else if (*p == '}' && brace) {
			buf_putc(&b, '}');
			brace = 0;
		} else {
			if (strchr("()|+?{}\\", *p))
				buf_putc(&b, '\\');
			buf_putc(&b, *p);
			upper |= isupper((unsigned char)*p) != 0;
		}
	}
	buf_add(&b, "", 0);
	if (forced >= 0)
		icase = forced;
	else if (icase < 0)
		icase = !upper;
	old = utf8_locale() ? uselocale(utf8_locale()) : (locale_t)0;
	x->word = startswith(b.p, WORD_START);
	r = regcomp(&x->re, b.p, REG_EXTENDED | (icase ? REG_ICASE : 0));
	if (old)
		uselocale(old);
	buf_free(&b);
	return r ? -1 : 0;
}

/* the compiled form of the last search pattern, or NULL */
static struct rx *search_re(void)
{
	static struct rx re;
	static char *src;
	static int ok;

	if (!vim.pattern)
		return NULL;
	if (!src || strcmp(src, vim.pattern)) {
		if (ok)
			regfree(&re.re);
		free(src);
		src = xstrdup(vim.pattern);
		ok = !compile(src, -1, &re);
	}
	return ok ? &re : NULL;
}

/* remember a pattern for n, N, :s// and highlighting */
int ex_set_pattern(const char *pat, int backward)
{
	char *c = xstrdup(pat);

	free(vim.pattern);
	vim.pattern = c;
	vim.backward = backward;
	vim.nohl = 0;
	return search_re() ? 0 : -1;
}

/* the next position after m, so that empty matches advance */
static size_t past(const char *s, size_t len, const regmatch_t *m)
{
	size_t p = m->rm_eo;

	if (m->rm_eo > m->rm_so)
		return p;
	for (p++; p < len && ((unsigned char)s[p] & 0xc0) == 0x80; p++)
		;
	return p;
}

/*
 * find the next match of the search pattern from a position, wrapping
 * around the ends of the text
 */
int ex_search(struct editor *e, size_t from, int backward, size_t *at)
{
	struct rx *re = search_re();
	size_t first = NOMARK, last = NOMARK, next = NOMARK, prev = NOMARK;
	char *text;
	size_t len, ls, le;

	if (!re) {
		vim_msg(1, "invalid pattern: %s", vim.pattern ? vim.pattern :
			"");
		return 0;
	}
	text = ed_text(e);
	len = ed_len(e);
	for (ls = 0; ls <= len; ls = le + 1) {
		char *nl = memchr(text + ls, '\n', len - ls);
		size_t off = 0, n;
		regmatch_t m[NGROUPS];

		le = nl ? (size_t)(nl - text) : len;
		n = le - ls;
		while (off <= n && match(re, text + ls, off, n, m)) {
			size_t p = ls + m[0].rm_so;

			/* only an empty line has a match past its last byte */
			if ((size_t)m[0].rm_so == n && n)
				break;
			if (first == NOMARK)
				first = p;
			last = p;
			if (p > from && next == NOMARK)
				next = p;
			if (p < from)
				prev = p;
			off = past(text + ls, n, &m[0]);
		}
	}
	free(text);
	if (first == NOMARK) {
		vim_msg(1, "pattern not found: %s", vim.pattern);
		return 0;
	}
	if (backward) {
		if (prev == NOMARK)
			vim_msg(0, "search hit TOP, continuing at BOTTOM");
		*at = prev != NOMARK ? prev : last;
	} else {
		if (next == NOMARK)
			vim_msg(0, "search hit BOTTOM, continuing at TOP");
		*at = next != NOMARK ? next : first;
	}
	return 1;
}

/* byte ranges of the highlighted search matches in the line [ls, le] */
int ed_matches(struct editor *e, size_t ls, size_t le, size_t (*out)[2],
	       int max)
{
	struct rx *re;
	regmatch_t m[NGROUPS];
	size_t off = 0, len = le - ls;
	char *s;
	int n = 0;

	if (!cfg.hlsearch || vim.nohl || !(re = search_re()))
		return 0;
	s = ed_slice(e, ls, le);
	while (n < max && off < len && match(re, s, off, len, m)) {
		if (m[0].rm_eo > m[0].rm_so) {
			out[n][0] = ls + m[0].rm_so;
			out[n++][1] = ls + m[0].rm_eo;
		}
		off = past(s, len, &m[0]);
	}
	free(s);
	return n;
}

/* append a replacement, expanding &, \1 and \r */
static void expand(struct buf *out, const char *rep, const char *line,
		   const regmatch_t *m)
{
	for (; *rep; rep++) {
		int g = -1;

		if (*rep == '&') {
			g = 0;
		} else if (*rep == '\\' && rep[1]) {
			char c = *++rep;

			if (isdigit((unsigned char)c))
				g = c - '0';
			else
				buf_putc(out, c == 'r' || c == 'n' ? '\n' :
					 c == 't' ? '\t' : c);
		} else {
			buf_putc(out, *rep);
		}
		if (g >= 0 && m[g].rm_so >= 0)
			buf_add(out, line + m[g].rm_so,
				m[g].rm_eo - m[g].rm_so);
	}
}

static int substitute(struct editor *e, int first, int last, const char *pat,
		      const char *rep, const char *flags)
{
	int global = strchr(flags, 'g') != NULL;
	int icase = strchr(flags, 'i') ? 1 : strchr(flags, 'I') ? 0 : -1;
	int count = 0, lines = 0, l;
	size_t cursor = NOMARK;
	struct rx re;

	if (compile(pat, icase, &re)) {
		vim_msg(1, "invalid pattern: %s", pat);
		return ED_FAIL;
	}
	for (l = first; l <= last && l < ed_lines(e); l++) {
		size_t ls = ed_line_pos(e, l), le = ed_line_end(e, ls);
		size_t len = le - ls, off = 0, done = 0, k;
		char *s = ed_slice(e, ls, le);
		struct buf out = { 0 };
		regmatch_t m[NGROUPS];
		int hit = 0, added = 0;

		while (off <= len && match(&re, s, off, len, m)) {
			if ((size_t)m[0].rm_so == len && len && hit)
				break;
			buf_add(&out, s + done, m[0].rm_so - done);
			expand(&out, rep, s, m);
			done = m[0].rm_eo;
			hit++;
			if (!global)
				break;
			off = past(s, len, &m[0]);
		}
		if (hit) {
			buf_add(&out, s + done, len - done);
			ed_splice(e, ls, len, out.p, out.len);
			for (k = 0; k < out.len; k++)
				added += out.p[k] == '\n';
			l += added;
			last += added;
			count += hit;
			lines++;
			cursor = ed_line_pos(e, l);
		}
		buf_free(&out);
		free(s);
	}
	regfree(&re.re);
	if (!count) {
		vim_msg(1, "pattern not found: %s", pat);
		return ED_FAIL;
	}
	e->cur = ed_first_nonblank(e, cursor);
	if (count > 1)
		vim_msg(0, "%d substitutions on %d lines", count, lines);
	return ED_OK;
}

/* the part of s up to an unescaped delimiter, which *s is left after */
static char *section(const char **s, char delim)
{
	struct buf b = { 0 };
	const char *p = *s;

	for (; *p && *p != delim; p++) {
		if (*p == '\\' && p[1] == delim)
			p++;
		else if (*p == '\\' && p[1])
			buf_putc(&b, *p++);
		buf_putc(&b, *p);
	}
	*s = *p ? p + 1 : p;
	return buf_take(&b);
}

static int substitute_command(struct editor *e, int first, int last,
			      const char *arg, int again)
{
	char delim = *arg;
	char *pat, *rep, *flags;
	int r;

	if (again || !delim || isalnum((unsigned char)delim) || delim == ' ') {
		if (!vim.sub_pat) {
			vim_msg(1, "no previous substitute");
			return ED_FAIL;
		}
		return substitute(e, first, last, vim.sub_pat, vim.sub_rep,
				  *arg ? arg : "");
	}
	arg++;
	pat = section(&arg, delim);
	rep = section(&arg, delim);
	flags = xstrdup(arg);
	if (!*pat) {
		free(pat);
		pat = xstrdup(vim.pattern ? vim.pattern : "");
	}
	free(vim.sub_pat);
	free(vim.sub_rep);
	free(vim.sub_flags);
	vim.sub_pat = pat;
	vim.sub_rep = rep;
	vim.sub_flags = flags;
	ex_set_pattern(pat, 0);
	vim.nohl = 1;
	r = substitute(e, first, last, pat, rep, flags);
	return r;
}

/* one line address; returns 0 when there is none at *s */
static int address(struct editor *e, const char **s, int *line)
{
	const char *p = *s;
	int n;

	while (*p == ' ')
		p++;
	if (isdigit((unsigned char)*p)) {
		*line = (int)strtol(p, (char **)&p, 10) - 1;
	} else if (*p == '.') {
		*line = ed_line_of(e, e->cur);
		p++;
	} else if (*p == '$') {
		*line = ed_lines(e) - 1;
		p++;
	} else if (*p == '\'' && p[1]) {
		int i = p[1] >= 'a' && p[1] <= 'z' ? p[1] - 'a' :
		    p[1] == '<' ? 26 : p[1] == '>' ? 27 : -1;

		if (i < 0 || e->mark[i] == NOMARK)
			return 0;
		*line = ed_line_of(e, e->mark[i]);
		p += 2;
	} else if (*p == '+' || *p == '-') {
		*line = ed_line_of(e, e->cur);
	} else {
		return 0;
	}
	while (*p == '+' || *p == '-') {
		int sign = *p++ == '+' ? 1 : -1;

		n = isdigit((unsigned char)*p) ?
		    (int)strtol(p, (char **)&p, 10) : 1;
		*line += sign * n;
	}
	*s = p;
	return 1;
}

/* whether name abbreviates full and is at least min long */
static int abbrev(const char *name, const char *full, size_t min)
{
	size_t l = strlen(name);

	return l >= min && l <= strlen(full) && !strncmp(name, full, l);
}

static char *lines_text(struct editor *e, int first, int last)
{
	size_t a = ed_line_pos(e, first);
	char *s = ed_slice(e, a, ed_line_end(e, ed_line_pos(e, last)));
	char *r = xasprintf("%s\n", s);

	free(s);
	return r;
}

static void remove_lines(struct editor *e, int first, int last)
{
	size_t a = ed_line_pos(e, first);
	size_t b = ed_line_end(e, ed_line_pos(e, last));

	if (b < ed_len(e))
		b++;
	else if (a)
		a--;
	ed_splice(e, a, b - a, NULL, 0);
	e->cur = ed_first_nonblank(e, a > ed_len(e) ? ed_len(e) : a);
}

/* insert whole lines after a line; -1 inserts at the top */
static void add_lines(struct editor *e, int after, const char *text)
{
	size_t n = strlen(text);

	if (after >= ed_lines(e) - 1) {
		ed_splice(e, ed_len(e), 0, "\n", 1);
		ed_splice(e, ed_len(e), 0, text, n ? n - 1 : 0);
		e->cur = ed_line_pos(e, ed_lines(e) - 1);
	} else {
		size_t at = after < 0 ? 0 : ed_line_pos(e, after + 1);
		size_t last = 0, i;

		ed_splice(e, at, 0, text, n);
		for (i = 0; i + 1 < n; i++)
			if (text[i] == '\n')
				last = i + 1;
		e->cur = at + last;
	}
	e->cur = ed_first_nonblank(e, e->cur);
}

static int copy_move(struct editor *e, int first, int last, const char *arg,
		     int move)
{
	int to, n = last - first + 1;
	char *s;

	if (!address(e, &arg, &to) && strcmp(arg, "0")) {
		vim_msg(1, "address required");
		return ED_FAIL;
	}
	if (to >= ed_lines(e) || (move && to >= first && to < last)) {
		vim_msg(1, "invalid address");
		return ED_FAIL;
	}
	if (move && (to == last || to == first - 1))
		return ED_OK;
	s = lines_text(e, first, last);
	if (move) {
		remove_lines(e, first, last);
		if (to > last)
			to -= n;
	}
	add_lines(e, to, s);
	free(s);
	return ED_OK;
}

static int line_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void sort_lines(struct editor *e, int first, int last, int reverse,
		       int unique)
{
	int n = last - first + 1, i;
	char **v = xcalloc(n, sizeof(*v));
	struct buf out = { 0 };
	size_t a, b;

	for (i = 0; i < n; i++) {
		a = ed_line_pos(e, first + i);
		v[i] = ed_slice(e, a, ed_line_end(e, a));
	}
	qsort(v, n, sizeof(*v), line_cmp);
	for (i = 0; i < n; i++) {
		char *s = v[reverse ? n - 1 - i : i];

		if (unique && i && !strcmp(s, v[reverse ? n - i : i - 1]))
			continue;
		if (out.len || i)
			buf_putc(&out, '\n');
		buf_puts(&out, s);
	}
	buf_add(&out, "", 0);
	a = ed_line_pos(e, first);
	b = ed_line_end(e, ed_line_pos(e, last));
	ed_splice(e, a, b - a, out.p, out.len);
	e->cur = ed_first_nonblank(e, a);
	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
	buf_free(&out);
}

/* ":set" for the options that can change while running */
static int set_options(const char *arg)
{
	static const struct {
		const char *name, *alias;
		int *value;
	} opts[] = {
		{ "number", "nu", &cfg.number },
		{ "relativenumber", "rnu", &cfg.relativenumber },
		{ "hlsearch", "hls", &cfg.hlsearch },
		{ "clipboard", "cb", &cfg.clipboard },
	};
	char *dup = xstrdup(arg), *w, *save = NULL;
	int r = ED_OK;

	for (w = strtok_r(dup, " ", &save); w && r == ED_OK;
	     w = strtok_r(NULL, " ", &save)) {
		int value = 1, toggle = 0, query = 0, found = 0;
		size_t i, l;
		char *name = w;

		if (!strncmp(w, "textwidth=", 10) || !strncmp(w, "tw=", 3)) {
			cfg.textwidth = atoi(strchr(w, '=') + 1);
			continue;
		}
		l = strlen(w);
		if (l && (w[l - 1] == '!' || w[l - 1] == '?')) {
			toggle = w[l - 1] == '!';
			query = w[l - 1] == '?';
			w[l - 1] = 0;
		}
		if (!strncmp(name, "inv", 3)) {
			toggle = 1;
			name += 3;
		} else if (!strncmp(name, "no", 2)) {
			value = 0;
			name += 2;
		}
		for (i = 0; i < sizeof(opts) / sizeof(*opts) && !found; i++)
			if (!strcmp(name, opts[i].name) ||
			    !strcmp(name, opts[i].alias)) {
				found = 1;
				if (query)
					vim_msg(0, "%s%s", *opts[i].value ?
						"" : "no", opts[i].name);
				else
					*opts[i].value = toggle ?
					    !*opts[i].value : value;
			}
		if (!found) {
			vim_msg(1, "unknown option: %s", name);
			r = ED_FAIL;
		}
	}
	free(dup);
	return r;
}

/* ":g/pat/cmd" runs cmd on every matching line, ":v" on the others */
static int global_command(struct editor *e, int first, int last,
			  const char *arg, int invert)
{
	char delim = *arg;
	char *pat, *s;
	struct rx re;
	regmatch_t m[NGROUPS];
	int *hit, n = 0, i, delta = 0, r = ED_OK;
	static int nested;

	if (!delim || isalnum((unsigned char)delim) || nested) {
		vim_msg(1, "usage: g/pattern/command");
		return ED_FAIL;
	}
	arg++;
	pat = section(&arg, delim);
	if (*pat)
		ex_set_pattern(pat, 0);
	if (compile(*pat ? pat : vim.pattern ? vim.pattern : "", -1, &re)) {
		vim_msg(1, "invalid pattern");
		free(pat);
		return ED_FAIL;
	}
	free(pat);
	vim.nohl = 1;
	hit = xcalloc(last - first + 1, sizeof(*hit));
	for (i = first; i <= last; i++) {
		size_t ls = ed_line_pos(e, i), le = ed_line_end(e, ls);

		s = ed_slice(e, ls, le);
		if (match(&re, s, 0, le - ls, m) != invert)
			hit[n++] = i;
		free(s);
	}
	regfree(&re.re);
	nested = 1;
	for (i = 0; i < n; i++) {
		int line = hit[i] + delta, before = ed_lines(e);

		if (line < 0 || line >= before)
			continue;
		e->cur = ed_line_pos(e, line);
		r = ex_execute(e, *arg ? arg : "d");
		delta += ed_lines(e) - before;
		if (r == ED_EX)
			break;
	}
	nested = 0;
	free(hit);
	if (!n) {
		vim_msg(1, "pattern not found");
		return ED_FAIL;
	}
	return r == ED_EX ? ED_FAIL : ED_OK;
}

/* leave a command that the editor does not know to the application */
static int pass(const char *line)
{
	buf_reset(&vim.command);
	buf_puts(&vim.command, line);
	return ED_EX;
}

/* run a ":" command line */
int ex_execute(struct editor *e, const char *line)
{
	const char *s = line, *arg;
	int first, last, range = 0, i, r = ED_OK, bang = 0;
	int cur = ed_line_of(e, e->cur), n = ed_lines(e);
	char name[32] = "";
	size_t l = 0;

	while (*s == ' ' || *s == ':')
		s++;
	first = last = cur;
	if (*s == '%') {
		first = 0;
		last = n - 1;
		range = 1;
		s++;
	} else if (address(e, &s, &first)) {
		range = 1;
		last = first;
		if (*s == ',' || *s == ';') {
			s++;
			if (!address(e, &s, &last))
				last = cur;
		}
	}
	while (*s == ' ')
		s++;
	for (i = 0; i < cfg.nalias; i++) {
		size_t a = strlen(cfg.alias[i].name);

		if (!range && !strncmp(s, cfg.alias[i].name, a) &&
		    (!s[a] || s[a] == ' '))
			return pass(s);
	}
	if (isalpha((unsigned char)*s)) {
		while (isalpha((unsigned char)s[l]) && l + 1 < sizeof(name))
			l++;
	} else if (*s && strchr("&<>!", *s)) {
		l = 1;
	}
	memcpy(name, s, l);
	arg = s + l;
	if (*arg == '!' && *name != '!') {
		bang = 1;
		arg++;
	}
	while (*arg == ' ')
		arg++;
	if (range && last < first) {
		int t = first;

		first = last;
		last = t;
	}
	if (range && (first < -1 || last >= n)) {
		vim_msg(1, "invalid range");
		return ED_FAIL;
	}
	if (first < 0)
		first = 0;

	if (!*name) {
		if (*arg) {
			vim_msg(1, "unknown command: %s", line);
			return ED_FAIL;
		}
		if (range)
			vim_jump(e, ed_first_nonblank(e, ed_line_pos(e, last <
								     0 ? 0 :
								     last)));
		return ED_OK;
	}
	if (abbrev(name, "nohlsearch", 3)) {
		vim.nohl = 1;
		return ED_OK;
	}
	if (abbrev(name, "set", 2))
		return set_options(arg);
	if (!strcmp(name, "!"))
		return pass(s);
	if (e->readonly)
		return range ? ED_FAIL : pass(s);
	if (!strcmp(name, "&") || abbrev(name, "substitute", 1)) {
		vim_checkpoint(e);
		return substitute_command(e, first, last, arg, *name == '&');
	}
	if (abbrev(name, "global", 1) || abbrev(name, "vglobal", 1)) {
		vim_checkpoint(e);
		vim_hold(1);
		r = global_command(e, range ? first : 0, range ? last : n - 1,
				   arg, *name == 'v' || bang);
		vim_hold(-1);
		return r;
	}
	if (abbrev(name, "delete", 1) || abbrev(name, "yank", 1)) {
		int reg = isalpha((unsigned char)*arg) || *arg == '"' ||
		    *arg == '+' || *arg == '_' ? *arg : 0;
		char *t = lines_text(e, first, last);

		vim_set_register(reg, t, R_LINE, *name == 'y');
		free(t);
		if (*name == 'd') {
			vim_checkpoint(e);
			remove_lines(e, first, last);
		}
		return ED_OK;
	}
	if (abbrev(name, "put", 2)) {
		int type;
		const char *t = vim_register(*arg, &type);
		char *copy;

		if (!t)
			return ED_FAIL;
		copy = xasprintf("%s%s", t, t[strlen(t) - 1] == '\n' ? "" :
				 "\n");
		vim_checkpoint(e);
		add_lines(e, bang ? first - 1 : last, copy);
		free(copy);
		return ED_OK;
	}
	if (abbrev(name, "join", 1)) {
		int count = last > first ? last - first + 1 : 2;

		vim_checkpoint(e);
		e->cur = ed_line_pos(e, first);
		while (--count && ed_line_end(e, e->cur) < ed_len(e)) {
			char *keys = "J";
			size_t keep = ed_line_start(e, e->cur);

			vim_hold(1);
			vim_run(e, keys);
			vim_hold(-1);
			e->cur = keep;
		}
		e->cur = ed_first_nonblank(e, e->cur);
		return ED_OK;
	}
	if (!strcmp(name, ">") || !strcmp(name, "<")) {
		char *keys = xasprintf("%d%s%s", last - first + 1, name, name);

		vim_checkpoint(e);
		e->cur = ed_line_pos(e, first);
		vim_hold(1);
		vim_run(e, keys);
		vim_hold(-1);
		free(keys);
		e->cur = ed_first_nonblank(e, ed_line_pos(e, last));
		vim_clamp(e);
		return ED_OK;
	}
	if (abbrev(name, "copy", 2) || !strcmp(name, "t")) {
		vim_checkpoint(e);
		return copy_move(e, first, last, arg, 0);
	}
	/* without a line address "move" is the mail command */
	if (abbrev(name, "move", 1) &&
	    (range || (*arg && strchr("0123456789.$'+-", *arg)))) {
		vim_checkpoint(e);
		return copy_move(e, first, last, arg, 1);
	}
	if (abbrev(name, "sort", 3)) {
		vim_checkpoint(e);
		sort_lines(e, range ? first : 0, range ? last : n - 1, bang,
			   strchr(arg, 'u') != NULL);
		return ED_OK;
	}
	if (abbrev(name, "normal", 4)) {
		char *keys = xstrdup(arg);
		int count = range ? last - first + 1 : 1, delta = 0;

		vim_checkpoint(e);
		vim_hold(1);
		for (i = 0; i < count; i++) {
			int before = ed_lines(e);
			int at = first + i + delta;

			if (range) {
				if (at < 0 || at >= before)
					break;
				e->cur = ed_line_pos(e, at);
			}
			e->mode = ED_NORMAL;
			vim_run(e, keys);
			delta += ed_lines(e) - before;
		}
		vim_hold(-1);
		free(keys);
		return ED_OK;
	}
	if (range) {
		vim_msg(1, "unknown command: %s", name);
		return ED_FAIL;
	}
	return pass(s);
}

static int characters(const char *s, size_t n)
{
	int c = 0;
	size_t i;

	for (i = 0; i < n; i++)
		c += ((unsigned char)s[i] & 0xc0) != 0x80;
	return c;
}

/* the quoting and indentation a line starts with */
static size_t prefix_length(const char *s)
{
	size_t i = strspn(s, " \t");

	while (s[i] == '>' || s[i] == '|') {
		i++;
		i += strspn(s + i, " \t");
	}
	return i;
}

/*
 * refill the paragraphs in lines first to last to the text width, as gq
 * does. Lines only join when they are quoted the same way, and empty
 * lines and the signature separator stay where they are.
 */
void ex_reflow(struct editor *e, int first, int last)
{
	int width = cfg.textwidth > 0 ? cfg.textwidth : 72;
	size_t a = ed_line_pos(e, first);
	size_t b = ed_line_end(e, ed_line_pos(e, last));
	char *text = ed_slice(e, a, b), *line = text, *next;
	struct buf out = { 0 };
	char *quote = NULL;
	int col = 0;
	size_t end;

	for (; line; line = next) {
		size_t pl, ql;
		char *w, *save = NULL;
		int keep;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		pl = prefix_length(line);
		keep = !line[pl] || !strcmp(line, "-- ") ||
		    !strcmp(line, "--");
		/* a change of quoting ends the paragraph */
		for (ql = pl; ql && strchr(" \t", line[ql - 1]); ql--)
			;
		if (quote && (keep || strlen(quote) < ql ||
			      strncmp(quote, line, ql) ||
			      quote[ql + strspn(quote + ql, " \t")])) {
			free(quote);
			quote = NULL;
		}
		if (keep) {
			if (out.len || line != text)
				buf_putc(&out, '\n');
			buf_puts(&out, line);
			continue;
		}
		if (!quote) {
			quote = xstrndup(line, pl);
			if (out.len || line != text)
				buf_putc(&out, '\n');
			buf_puts(&out, quote);
			col = characters(quote, strlen(quote));
		}
		for (w = strtok_r(line + pl, " \t", &save); w;
		     w = strtok_r(NULL, " \t", &save)) {
			int len = characters(w, strlen(w));
			int fresh = col == characters(quote, strlen(quote));

			if (!fresh && col + 1 + len > width) {
				buf_putc(&out, '\n');
				buf_puts(&out, quote);
				col = characters(quote, strlen(quote));
				fresh = 1;
			}
			if (!fresh) {
				buf_putc(&out, ' ');
				col++;
			}
			buf_puts(&out, w);
			col += len;
		}
	}
	buf_add(&out, "", 0);
	ed_splice(e, a, b - a, out.p, out.len);
	end = a + out.len;
	e->cur = ed_first_nonblank(e, end);
	free(quote);
	free(text);
	buf_free(&out);
}
