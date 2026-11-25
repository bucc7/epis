/* pipe.c - handing messages to a shell command */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* starts each message when several are piped; "git am" splits on it */
#define MBOX_FROM "From epis@localhost Thu Jan  1 00:00:00 1970\n"
#define HISTORY 20

static char *history[HISTORY];
static int nhistory;

/* the n and m of a "[PATCH n/m]" subject; -1 without them */
static int patch_number(const char *subject, int *of)
{
	const char *p = subject ? strchr(subject, '[') : NULL, *end;
	int n;

	if (!p || !(end = strchr(p, ']')))
		return -1;
	for (; p < end; p++)
		if (*p == '/' && isdigit((unsigned char)p[-1]) &&
		    isdigit((unsigned char)p[1])) {
			*of = atoi(p + 1);
			for (n = 0; isdigit((unsigned char)p[-n - 1]); n++)
				;
			return atoi(p - n);
		}
	return -1;
}

static int by_number(const void *a, const void *b)
{
	int unused;

	return patch_number(((const struct msginfo *)a)->subject, &unused) -
	    patch_number(((const struct msginfo *)b)->subject, &unused);
}

static int by_age(const void *a, const void *b)
{
	const struct msginfo *x = a, *y = b;

	if (x->date != y->date)
		return x->date < y->date ? -1 : 1;
	if (x->uid != y->uid)
		return x->uid < y->uid ? -1 : 1;
	return strcmp(x->path, y->path);
}

/*
 * the order in which to pipe messages, which a list shows newest first: a
 * patch series goes by its numbers, anything else from oldest to newest
 */
static void order(struct msginfo *m, int n)
{
	int i, j, of, first_of = 0, num, series = 1;

	for (i = 0; i < n && series; i++) {
		num = patch_number(m[i].subject, &of);
		if (!i)
			first_of = of;
		/* a cover letter is number 0, and so comes first */
		series = num >= 0 && of == first_of;
		for (j = 0; j < i && series; j++)
			series = by_number(&m[i], &m[j]) != 0;
	}
	qsort(m, n, sizeof(*m), series ? by_number : by_age);
}

/*
 * a temporary file holding the raw messages with Unix line endings, ready
 * to be a command's input. One message is passed as it is; several form
 * an mbox. The array is put in the order used. Returns NULL on failure.
 */
FILE *pipe_input(struct msginfo *m, int n)
{
	FILE *f = tmpfile();
	int i;

	if (!f)
		return NULL;
	if (n > 1)
		order(m, n);
	for (i = 0; i < n; i++) {
		size_t len, k;
		char *raw = read_file(m[i].path, &len);

		if (!raw) {
			fclose(f);
			return NULL;
		}
		if (n > 1)
			fputs(MBOX_FROM, f);
		for (k = 0; k < len; k++)
			if (raw[k] != '\r' || k + 1 == len ||
			    raw[k + 1] != '\n')
				fputc(raw[k], f);
		if (n > 1) {
			if (len && raw[len - 1] != '\n')
				fputc('\n', f);
			fputc('\n', f);
		}
		free(raw);
	}
	if (fflush(f) || ferror(f)) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	return f;
}

/* command output made fit to show: no terminal escapes, no final blanks */
char *pipe_output(const char *s)
{
	struct buf b = { 0 };

	for (; *s; s++) {
		if (*s == 0x1b) {
			/* skip a control sequence up to its final letter */
			if (s[1] == '[')
				for (s++; s[1] && !(s[1] >= '@' && s[1] <= '~');
				     s++)
					;
			if (s[1])
				s++;
		} else if (*s == '\r') {
			if (s[1] != '\n')
				buf_putc(&b, '\n');
		} else {
			buf_putc(&b, *s);
		}
	}
	while (b.len && strchr(" \t\n", b.p[b.len - 1]))
		b.p[--b.len] = 0;
	return buf_take(&b);
}

/* where the command starts in a ":" line that pipes, or NULL */
const char *pipe_argument(const char *line)
{
	/* a range of rows may come first */
	while (*line && strchr("0123456789%.$,+-'<> :", *line))
		line++;
	if (*line == '|')
		return line + 1;
	if (!strncmp(line, "pipe", 4) && (!line[4] || line[4] == ' '))
		return line + 4 + (line[4] == ' ');
	return NULL;
}

void pipe_remember(const char *cmd)
{
	int i;

	for (i = 0; i < nhistory && strcmp(history[i], cmd); i++)
		;
	if (i == nhistory && nhistory < HISTORY)
		nhistory++;
	else if (i == HISTORY)
		i--;
	if (i < nhistory)
		free(history[i]);
	memmove(history + 1, history, i * sizeof(*history));
	history[0] = xstrdup(cmd);
}

/* an earlier command, 0 being the latest, or NULL */
const char *pipe_recall(int i)
{
	return i >= 0 && i < nhistory ? history[i] : NULL;
}
