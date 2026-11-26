/* list.c - vim counts, operators and visual mode in the message list */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* the command being typed: its count and a "d" waiting for a motion */
static struct win *owner;
static int count, pending, pending_count;

void list_reset(void)
{
	count = pending = pending_count = 0;
}

/* the keys of an unfinished command, for the status bar */
const char *list_showcmd(struct win *w)
{
	static char s[48];
	int n = 0;

	if (w != owner || w->type != W_LIST)
		return "";
	s[0] = 0;
	if (pending && pending_count)
		n = snprintf(s, sizeof(s), "%d", pending_count);
	if (pending)
		n += snprintf(s + n, sizeof(s) - n, "d");
	if (count)
		snprintf(s + n, sizeof(s) - n, "%d", count);
	return s;
}

/* the selected rows; without a selection just the cursor row */
void list_range(const struct win *w, int *lo, int *hi)
{
	int a = w->visual ? w->anchor : w->sel;

	*lo = a < w->sel ? a : w->sel;
	*hi = a < w->sel ? w->sel : a;
}

static int clamp(const struct win *w, int row)
{
	if (row >= w->nmsgs)
		row = w->nmsgs - 1;
	return row < 0 ? 0 : row;
}

/* select the rows from anchor to cursor */
void list_select(struct win *w, int anchor, int cursor)
{
	if (!w->nmsgs)
		return;
	w->visual = 1;
	w->anchor = clamp(w, anchor);
	w->sel = clamp(w, cursor);
	app.dirty = 1;
}

/* the part of a maildir file name that stays when its flags change */
static const char *name_of(const char *path, size_t *len)
{
	const char *s = strrchr(path, '/');

	s = s ? s + 1 : path;
	*len = strcspn(s, ":");
	return s;
}

int list_same(const char *a, const char *b)
{
	size_t la, lb;
	const char *x = name_of(a, &la), *y = name_of(b, &lb);

	return la == lb && !memcmp(x, y, la);
}

static int find(const struct win *w, const char *path)
{
	int i;

	for (i = 0; i < w->nmsgs; i++)
		if (list_same(w->msgs[i].path, path))
			return i;
	return -1;
}

/* remember which messages the cursor and the selection are on */
void list_save(const struct win *w, struct list_mark *m)
{
	int lo, hi, i;

	memset(m, 0, sizeof(*m));
	if (w->sel >= w->nmsgs)
		return;
	m->cursor = xstrdup(w->msgs[w->sel].path);
	if (!w->visual || w->anchor >= w->nmsgs)
		return;
	list_range(w, &lo, &hi);
	m->anchor = xstrdup(w->msgs[w->anchor].path);
	m->n = hi - lo + 1;
	m->selected = xcalloc(m->n, sizeof(*m->selected));
	for (i = 0; i < m->n; i++)
		m->selected[i] = xstrdup(w->msgs[lo + i].path);
}

/*
 * put the cursor and the selection back on their messages after the list
 * was read again. A selection that would now cover different messages,
 * say because mail arrived inside it, is dropped: acting on it could
 * delete mail that was never selected.
 */
void list_restore(struct win *w, struct list_mark *m)
{
	int cursor = m->cursor ? find(w, m->cursor) : -1;
	int anchor = m->anchor ? find(w, m->anchor) : -1;
	int keep = 0, i, j, lo, hi;

	if (cursor >= 0)
		w->sel = cursor;
	w->sel = clamp(w, w->sel);
	/*
	 * a half typed command was aimed at the list as it was: "dj" must
	 * not take a message that has just arrived below the cursor
	 */
	if (w == owner)
		list_reset();
	if (w->visual && cursor >= 0 && anchor >= 0) {
		w->anchor = anchor;
		list_range(w, &lo, &hi);
		keep = hi - lo + 1 == m->n;
		for (i = lo; keep && i <= hi; i++) {
			for (j = 0; j < m->n; j++)
				if (list_same(w->msgs[i].path, m->selected[j]))
					break;
			keep = j < m->n;
		}
	}
	if (w->visual && !keep) {
		w->visual = 0;
		list_reset();
		ui_status(1, "selection cleared: the mailbox changed");
	}
	for (i = 0; i < m->n; i++)
		free(m->selected[i]);
	free(m->selected);
	free(m->cursor);
	free(m->anchor);
}

static int is_motion(const char *cmd)
{
	return !strcmp(cmd, "down") || !strcmp(cmd, "up") ||
	    !strcmp(cmd, "top") || !strcmp(cmd, "bottom") ||
	    !strcmp(cmd, "pagedown") || !strcmp(cmd, "pageup");
}

/* the commands that act on every selected message */
int list_is_action(const char *cmd)
{
	return !strcmp(cmd, "delete") || !strcmp(cmd, "d") ||
	    !strcmp(cmd, "archive") || !strcmp(cmd, "move") ||
	    !strcmp(cmd, "flag") || !strcmp(cmd, "toggle_read") ||
	    !strcmp(cmd, "pipe");
}

static void run(const char *cmd, int n)
{
	app.count = n;
	ui_command(cmd);
	app.count = 0;
}

/*
 * run a command bound to a key in a message list, applying the typed
 * count and completing a waiting "d"
 */
void list_run(struct win *w, const char *cmd)
{
	int n = count, total, from;

	total = pending && pending_count ? pending_count * (n ? n : 1) : n;
	count = 0;
	if (pending) {
		pending = pending_count = 0;
		app.dirty = 1;
		if (!strcmp(cmd, "delete_motion")) {
			/* "dd" deletes this message and the next ones */
			list_select(w, w->sel,
				    w->sel + (total ? total : 1) - 1);
			run("delete", 0);
		} else if (is_motion(cmd)) {
			from = w->sel;
			run(cmd, total);
			if (w->sel == from && (!strcmp(cmd, "down") ||
					       !strcmp(cmd, "up")))
				return;
			list_select(w, from, w->sel);
			run("delete", 0);
		}
		/* any other command only cancels the "d" */
		return;
	}
	if (!strcmp(cmd, "delete_motion")) {
		if (w->visual) {
			run("delete", 0);
		} else if (w->nmsgs) {
			pending = 1;
			pending_count = n;
			app.dirty = 1;
		}
		return;
	}
	/* "3s" acts on three messages from the cursor */
	if (n > 1 && !w->visual && list_is_action(cmd))
		list_select(w, w->sel, w->sel + n - 1);
	run(cmd, n);
}

/*
 * handle the keys that are not bindings: counts, <Esc> and the extra keys
 * of visual mode. bound says whether the key has a binding of its own.
 * Returns 1 when the key was used.
 */
int list_key(struct win *w, const char *tok, int bound)
{
	if (w != owner) {
		list_reset();
		owner = w;
	}
	if (!app.keys[0] && !bound && isdigit((unsigned char)tok[0]) &&
	    !tok[1] && (count || tok[0] != '0')) {
		if (count < 10000)
			count = count * 10 + tok[0] - '0';
		app.dirty = 1;
		return 1;
	}
	if (!strcmp(tok, "<Esc>") && (count || pending || w->visual ||
				      app.keys[0])) {
		list_reset();
		app.keys[0] = 0;
		w->visual = 0;
		app.dirty = 1;
		return 1;
	}
	if (!w->visual || app.keys[0])
		return 0;
	if (!strcmp(tok, "o")) {
		int t = w->anchor;

		w->anchor = w->sel;
		w->sel = t;
		app.dirty = 1;
		return 1;
	}
	if (!strcmp(tok, "x")) {
		list_run(w, "delete");
		return 1;
	}
	return 0;
}

/* a row number or one of "." and "$", with an optional offset */
static int address(const struct win *w, const char **s, int *row)
{
	const char *p = *s;

	if (isdigit((unsigned char)*p))
		*row = (int)strtol(p, (char **)&p, 10) - 1;
	else if (*p == '.')
		*row = w->sel, p++;
	else if (*p == '$')
		*row = w->nmsgs - 1, p++;
	else if (*p == '+' || *p == '-')
		*row = w->sel;
	else
		return 0;
	while (*p == '+' || *p == '-') {
		int sign = *p++ == '+' ? 1 : -1;

		*row += sign * (isdigit((unsigned char)*p) ?
				(int)strtol(p, (char **)&p, 10) : 1);
	}
	*s = p;
	return 1;
}

/*
 * apply the line range at the start of a command, such as "%", "2,5" or
 * "'<,'>", by selecting those messages. *line is left at the command.
 * Returns -1 on an error, 1 if there was nothing but a row to go to.
 */
int list_command_range(struct win *w, const char **line)
{
	const char *p = *line;
	int first, last;

	if (!strncmp(p, "'<,'>", 5)) {
		if (!w->visual) {
			ui_status(1, "no messages are selected");
			return -1;
		}
		list_range(w, &first, &last);
		p += 5;
	} else if (*p == '%') {
		first = 0;
		last = w->nmsgs - 1;
		p++;
	} else if (address(w, &p, &first)) {
		last = first;
		if (*p == ',') {
			p++;
			if (!address(w, &p, &last)) {
				ui_status(1, "invalid range");
				return -1;
			}
		}
	} else {
		return 0;
	}
	while (*p == ' ')
		p++;
	if (first > last) {
		int t = first;

		first = last;
		last = t;
	}
	if (!*p) {
		/* a bare row number moves there, keeping a selection */
		w->sel = clamp(w, last);
		app.dirty = 1;
		return 1;
	}
	if (!w->nmsgs || first < 0 || last >= w->nmsgs) {
		ui_status(1, "invalid range");
		return -1;
	}
	list_select(w, first, last);
	*line = p;
	return 0;
}
