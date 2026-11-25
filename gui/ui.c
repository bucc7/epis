/* ui.c - application state, commands, key bindings and event loop */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gui.h"

struct app app;

enum { PK_FOLDER, PK_MESSAGE, PK_COMMAND, PK_MOVE };
enum { J_SYNC, J_LLM, J_DRAFT, J_AUTOCONF, J_SHELL };

struct job {
	pid_t pid;
	int fd, kind;
	struct buf out;
	struct win *w;
	char *title;
};

static struct job jobs[8];
static int njobs;
static char self[4096];

struct bind {
	const char *ctx, *keys, *cmd;
};

/* default key bindings; [keys] in the configuration overrides them */
static const struct bind defaults[] = {
	{ "global", ":", "cmdline" },
	{ "global", "<leader>fi", "find_inbox" },
	{ "global", "<leader>fm", "find_message" },
	{ "global", "<leader>fc", "find_command" },
	{ "global", "<leader>c", "compose" },
	{ "global", "<leader>s", "sync" },
	{ "global", "<leader>b", "sidebar" },
	{ "global", "<leader>m", "menubar" },
	{ "global", "<leader>a", "summarize" },
	{ "global", "<leader>wv", "split" },
	{ "global", "<leader>ws", "hsplit" },
	{ "global", "<leader>wq", "close" },
	{ "global", "<C-w>v", "split" },
	{ "global", "<C-w>s", "hsplit" },
	{ "global", "<C-w>h", "focus_left" },
	{ "global", "<C-w>j", "focus_down" },
	{ "global", "<C-w>k", "focus_up" },
	{ "global", "<C-w>l", "focus_right" },
	{ "global", "<C-w>w", "focus_next" },
	{ "global", "<C-w>q", "close" },
	{ "global", "<C-w>o", "only" },
	{ "global", "<C-w>=", "equalize" },
	{ "global", "<F10>", "menu" },
	{ "list", "j", "down" },
	{ "list", "k", "up" },
	{ "list", "<Down>", "down" },
	{ "list", "<Up>", "up" },
	{ "list", "gg", "top" },
	{ "list", "G", "bottom" },
	{ "list", "<C-d>", "pagedown" },
	{ "list", "<C-u>", "pageup" },
	{ "list", "<PageDown>", "pagedown" },
	{ "list", "<PageUp>", "pageup" },
	{ "list", "<CR>", "open" },
	{ "list", "l", "open" },
	{ "list", "<S-CR>", "open_split" },
	{ "list", "/", "find_message" },
	{ "list", "q", "close" },
	{ "message", "j", "scroll_down" },
	{ "message", "k", "scroll_up" },
	{ "message", "<Down>", "scroll_down" },
	{ "message", "<Up>", "scroll_up" },
	{ "message", "<C-d>", "pagedown" },
	{ "message", "<C-u>", "pageup" },
	{ "message", "<Space>", "pagedown" },
	{ "message", "gg", "top" },
	{ "message", "G", "bottom" },
	{ "message", "h", "back" },
	{ "message", "q", "back" },
	{ "message", "<Esc>", "back" },
	{ "message", "J", "next" },
	{ "message", "K", "prev" },
	{ "mail", "r", "reply" },
	{ "mail", "R", "reply_all" },
	{ "mail", "f", "forward" },
	{ "mail", "d", "delete" },
	{ "mail", "a", "archive" },
	{ "mail", "m", "move" },
	{ "mail", "s", "flag" },
	{ "mail", "u", "toggle_read" },
	{ "compose", "ZZ", "send" },
	{ "compose", "<C-j>", "next_field" },
	{ "compose", "<C-k>", "prev_field" },
	{ "compose", "<Tab>", "next_field" },
	{ "compose", "<S-Tab>", "prev_field" },
};

#define NDEF (int)(sizeof(defaults) / sizeof(*defaults))

struct cbind {
	char *ctx, *keys, *cmd;
};

static struct cbind *binds;
static int nbinds;

const char *const menu_titles[] = { "File", "Message", "View", "Window", "Help",
	NULL };

static const struct menu_item m_file[] = {
	{ "Compose", "compose" }, { "Synchronize", "sync" },
	{ "Find folder", "find_inbox" }, { "Quit", "quit" }, { NULL, NULL }
};

static const struct menu_item m_msg[] = {
	{ "Reply", "reply" }, { "Reply all", "reply_all" },
	{ "Forward", "forward" }, { "Toggle read", "toggle_read" },
	{ "Toggle flag", "flag" }, { "Move to...", "move" },
	{ "Archive", "archive" }, { "Delete", "delete" },
	{ "Summarize (AI)", "summarize" }, { NULL, NULL }
};

static const struct menu_item m_view[] = {
	{ "Find message", "find_message" }, { "Toggle sidebar", "sidebar" },
	{ "Toggle menu bar", "menubar" }, { "Reload", "reload" },
	{ NULL, NULL }
};

static const struct menu_item m_win[] = {
	{ "Split side by side", "split" }, { "Split stacked", "hsplit" },
	{ "Next window", "focus_next" }, { "Close window", "close" },
	{ "Only this window", "only" }, { NULL, NULL }
};

static const struct menu_item m_help[] = {
	{ "Commands", "find_command" }, { "Key bindings", "help" },
	{ NULL, NULL }
};

const struct menu_item *const menu_items[] = { m_file, m_msg, m_view, m_win,
	m_help };

const struct menu_item ctx_items[] = {
	{ "Open", "open" }, { "Open in new window", "open_split" },
	{ "Reply", "reply" }, { "Reply all", "reply_all" },
	{ "Forward", "forward" }, { "Toggle read", "toggle_read" },
	{ "Toggle flag", "flag" }, { "Move to...", "move" },
	{ "Delete", "delete" }, { "Summarize (AI)", "summarize" },
	{ NULL, NULL }
};

void ui_status(int err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(app.status, sizeof(app.status), fmt, ap);
	va_end(ap);
	app.status_err = err;
	app.dirty = 1;
}

/* canonical form of a key sequence: <leader> expanded, names normalized */
static char *canon(const char *s)
{
	static const char *names[][2] = {
		{ "enter", "CR" }, { "return", "CR" }, { "cr", "CR" },
		{ "esc", "Esc" }, { "escape", "Esc" }, { "space", "Space" },
		{ "tab", "Tab" }, { "bs", "BS" }, { "backspace", "BS" },
		{ "del", "Del" }, { "delete", "Del" }, { "up", "Up" },
		{ "down", "Down" }, { "left", "Left" }, { "right", "Right" },
		{ "home", "Home" }, { "end", "End" }, { "pageup", "PageUp" },
		{ "pagedown", "PageDown" }, { "lt", "lt" },
	};
	struct buf b = { 0 };

	while (*s) {
		const char *e;

		if (*s == '<' && (e = strchr(s, '>')) && e > s + 1) {
			char tok[64], *name;
			size_t i, l = e - s - 1;

			snprintf(tok, sizeof(tok), "%.*s", (int)l, s + 1);
			if (!strcasecmp(tok, "leader")) {
				char *ld = canon(cfg.leader);

				buf_puts(&b, ld);
				free(ld);
				s = e + 1;
				continue;
			}
			buf_putc(&b, '<');
			name = tok;
			while (name[0] && name[1] == '-') {
				buf_putc(&b, toupper((unsigned char)name[0]) ==
					 'M' ? 'A' : toupper((unsigned char)
							     name[0]));
				buf_putc(&b, '-');
				name += 2;
			}
			for (i = 0; i < sizeof(names) / sizeof(*names); i++)
				if (!strcasecmp(name, names[i][0]))
					break;
			if (i < sizeof(names) / sizeof(*names))
				buf_puts(&b, names[i][1]);
			else if (strlen(name) == 1)
				buf_putc(&b, tolower((unsigned char)*name));
			else
				buf_puts(&b, name);
			buf_putc(&b, '>');
			s = e + 1;
		} else if (*s == ' ') {
			buf_puts(&b, "<Space>");
			s++;
		} else {
			buf_putc(&b, *s++);
		}
	}
	buf_add(&b, "", 0);
	return b.p;
}

static void binds_init(void)
{
	int i;

	nbinds = NDEF + cfg.nkeys;
	binds = xcalloc(nbinds, sizeof(*binds));
	for (i = 0; i < NDEF; i++) {
		binds[i].ctx = xstrdup(defaults[i].ctx);
		binds[i].keys = canon(defaults[i].keys);
		binds[i].cmd = xstrdup(defaults[i].cmd);
	}
	for (i = 0; i < cfg.nkeys; i++) {
		binds[NDEF + i].ctx = xstrdup(cfg.keys[i].ctx);
		binds[NDEF + i].keys = canon(cfg.keys[i].keys);
		binds[NDEF + i].cmd = xstrdup(cfg.keys[i].cmd);
	}
}

static int ctx_has(const char *ctx, const char *b)
{
	const char *p = ctx;
	size_t l = strlen(b);

	while ((p = strstr(p, b))) {
		if ((p == ctx || p[-1] == ' ') && (!p[l] || p[l] == ' '))
			return 1;
		p += l;
	}
	return 0;
}

/* 2 for an exact match (cmd set), 1 when seq prefixes a binding, else 0 */
static int bind_match(const char *ctx, const char *seq, const char **cmd)
{
	size_t l = strlen(seq);
	int i, prefix = 0;
	const char *exact = NULL;

	for (i = nbinds - 1; i >= 0; i--) {
		if (!ctx_has(ctx, binds[i].ctx))
			continue;
		if (!strcmp(binds[i].keys, seq)) {
			if (!exact)
				exact = binds[i].cmd;
		} else if (!strncmp(binds[i].keys, seq, l)) {
			prefix = 1;
		}
	}
	if (prefix && !exact)
		return 1;
	if (exact) {
		*cmd = exact;
		return 2;
	}
	return prefix;
}

/* turn a key event into its token, e.g. "j", "<CR>" or "<C-w>" */
static void key_token(struct event *e, char *out, size_t n)
{
	static const char *sp[] = {
		[K_CR] = "CR", [K_ESC] = "Esc", [K_BS] = "BS",
		[K_TAB] = "Tab", [K_DEL] = "Del", [K_UP] = "Up",
		[K_DOWN] = "Down", [K_LEFT] = "Left", [K_RIGHT] = "Right",
		[K_HOME] = "Home", [K_END] = "End", [K_PAGEUP] = "PageUp",
		[K_PAGEDOWN] = "PageDown", [K_SPACE] = "Space",
	};
	char mods[16] = "";
	const char *shift = e->mods & MOD_SHIFT ? "S-" : "";

	if (e->mods & MOD_CTRL)
		strcat(mods, "C-");
	if (e->mods & MOD_ALT)
		strcat(mods, "A-");
	if (e->key >= K_F1) {
		snprintf(out, n, "<%s%sF%u>", mods, shift, e->key - K_F1 + 1);
		return;
	}
	if (e->key != K_NONE) {
		snprintf(out, n, "<%s%s%s>", mods, shift, sp[e->key]);
		return;
	}
	if (mods[0]) {
		char u[8] = "";

		u[utf8_encode(u, e->cp ? e->cp : '?')] = 0;
		snprintf(out, n, "<%s%s>", mods, u);
		return;
	}
	if (e->text[0] == '<')
		snprintf(out, n, "<lt>");
	else
		snprintf(out, n, "%s", e->text);
}

static struct win *focused(void)
{
	return app.focus ? app.focus->w : NULL;
}

static struct node *leaf_new(struct win *w)
{
	struct node *n = xcalloc(1, sizeof(*n));

	n->w = w;
	n->ratio = 0.5f;
	return n;
}

static struct win *win_new(int type)
{
	struct win *w = xcalloc(1, sizeof(*w));

	w->type = type;
	return w;
}

static void chunks_free(struct win *w)
{
	int i;

	for (i = 0; i < w->nchunks; i++)
		free(w->chunks[i].s);
	free(w->chunks);
	w->chunks = NULL;
	w->nchunks = 0;
}

static void win_free(struct win *w)
{
	int i;

	msginfo_free(w->msgs, w->nmsgs);
	free(w->acc);
	free(w->folder);
	free(w->path);
	free(w->hdr);
	free(w->title);
	free(w->reply_path);
	chunks_free(w);
	for (i = 0; i < 4; i++)
		if (w->f[i].b)
			ed_free(&w->f[i]);
	for (i = 0; i < w->nattach; i++)
		free(w->attach[i]);
	free(w->attach);
	free(w);
}

/* split text into runs of quoted and unquoted lines */
static void set_chunks(struct win *w, const char *text)
{
	const char *p = text;

	chunks_free(w);
	while (*p) {
		const char *s = p;
		int q = *p == '>';

		while (*p) {
			const char *nl = strchr(p, '\n');
			const char *next = nl ? nl + 1 : p + strlen(p);

			if ((*p == '>') != q)
				break;
			p = next;
		}
		w->chunks = xrealloc(w->chunks, (w->nchunks + 1) *
				     sizeof(*w->chunks));
		w->chunks[w->nchunks].len = p - s;
		while (w->chunks[w->nchunks].len && s[w->chunks[w->nchunks].len
						       - 1] == '\n')
			w->chunks[w->nchunks].len--;
		w->chunks[w->nchunks].blank = *p &&
		    p - s > w->chunks[w->nchunks].len ?
		    p - s - w->chunks[w->nchunks].len - 1 : 0;
		w->chunks[w->nchunks].s = xstrndup(s, w->chunks[w->nchunks].len);
		w->chunks[w->nchunks].len = strlen(w->chunks[w->nchunks].s);
		if (!w->chunks[w->nchunks].len)
			w->chunks[w->nchunks].s[0] = 0;
		w->chunks[w->nchunks++].quote = q;
	}
	w->scroll = 0;
}

static void list_load(struct win *w, const char *acc, const char *folder)
{
	char *keep = w->nmsgs && w->sel < w->nmsgs ?
	    xstrdup(strrchr(w->msgs[w->sel].path, '/')) : NULL;
	char *dir;
	int i;

	if (acc != w->acc) {
		free(w->acc);
		w->acc = xstrdup(acc);
	}
	if (folder != w->folder) {
		free(w->folder);
		w->folder = xstrdup(folder);
	}
	msginfo_free(w->msgs, w->nmsgs);
	dir = folder_dir(acc, folder);
	w->nmsgs = maildir_scan(dir, &w->msgs, 1);
	sort_by_date(w->msgs, w->nmsgs);
	free(dir);
	w->has_list = 1;
	if (keep) {
		size_t kl = strcspn(keep, ":");

		for (i = 0; i < w->nmsgs; i++)
			if (!strncmp(strrchr(w->msgs[i].path, '/'), keep, kl))
				w->sel = i;
		free(keep);
	}
	if (w->sel >= w->nmsgs)
		w->sel = w->nmsgs ? w->nmsgs - 1 : 0;
	app.dirty = 1;
}

static void reload_lists(struct node *n)
{
	if (!n)
		return;
	if (n->w && n->w->has_list && n->w->acc)
		list_load(n->w, n->w->acc, n->w->folder);
	reload_lists(n->a);
	reload_lists(n->b);
}

static void folders_load(void)
{
	int i, j, n;
	char **f;

	for (i = 0; i < app.nfolders; i++)
		free(app.folders[i]);
	free(app.folders);
	app.folders = NULL;
	app.nfolders = 0;
	for (i = 0; i < cfg.nacc; i++) {
		n = maildir_folders(cfg.acc[i].name, &f);
		for (j = 0; j < n; j++) {
			app.folders = xrealloc(app.folders, (app.nfolders + 1)
					       * sizeof(*app.folders));
			app.folders[app.nfolders++] = xasprintf("%s/%s",
				cfg.acc[i].name, f[j]);
			free(f[j]);
		}
		free(f);
	}
}

/* path of the message the focused window is about */
static const char *cur_path(struct win *w)
{
	if (!w)
		return NULL;
	if (w->type == W_MSG)
		return w->path;
	if (w->type == W_LIST && w->sel < w->nmsgs)
		return w->msgs[w->sel].path;
	return NULL;
}

static struct account *acc_of(struct win *w)
{
	struct account *a = w && w->acc ? account_find(w->acc) : NULL;

	if (!a && w && w->path) {
		char *id = maildir_id(w->path);

		id[strcspn(id, "/")] = 0;
		a = account_find(id);
		free(id);
	}
	return a ? a : account_find(NULL);
}

static void msg_show(struct win *w, const char *path)
{
	struct message m;
	struct buf h = { 0 };
	const char *hs[] = { "From", "To", "Cc", "Date", "Subject" };
	char *raw, *t, *np;
	size_t len;
	int i, f;

	raw = read_file(path, &len);
	if (!raw) {
		ui_status(1, "cannot read message");
		return;
	}
	msg_parse(&m, raw, len);
	for (i = 0; i < 5; i++) {
		char *v = msg_header(&m, hs[i]);

		if (v)
			buf_printf(&h, "%s%s: %s", h.len ? "\n" : "", hs[i], v);
		free(v);
	}
	free(w->hdr);
	w->hdr = buf_take(&h);
	free(w->title);
	w->title = msg_header(&m, "Subject");
	t = msg_text(&m);
	set_chunks(w, t);
	free(t);
	msg_free(&m);
	f = maildir_flags_parse(path);
	np = !(f & F_SEEN) ? maildir_set_flags(path, f | F_SEEN) : NULL;
	free(w->path);
	w->path = np ? np : xstrdup(path);
	if (w->has_list && w->sel < w->nmsgs) {
		free(w->msgs[w->sel].path);
		w->msgs[w->sel].path = xstrdup(w->path);
		w->msgs[w->sel].flags |= F_SEEN;
	}
	w->type = W_MSG;
	app.dirty = 1;
}

/* split the focused leaf, putting w in the new half */
static void split(int dir, struct win *w)
{
	struct node *f = app.focus, *a, *b;

	if (!f) {
		app.root = app.focus = leaf_new(w);
		return;
	}
	if (app.nleaves >= 8) {
		ui_status(1, "too many windows");
		win_free(w);
		return;
	}
	a = leaf_new(f->w);
	b = leaf_new(w);
	a->parent = b->parent = f;
	f->w = NULL;
	f->split = dir;
	f->a = a;
	f->b = b;
	f->ratio = 0.5f;
	app.focus = b;
	app.dirty = 1;
}

static struct win *clone_list(struct win *src)
{
	struct win *w = win_new(W_LIST);

	if (src && src->acc)
		list_load(w, src->acc, src->folder);
	return w;
}

static struct node *first_leaf(struct node *n)
{
	while (n && !n->w)
		n = n->a;
	return n;
}

static void close_focus(int quit_last)
{
	struct node *f = app.focus, *p, *sib, *gp;

	if (!f || !f->parent) {
		if (quit_last)
			app.running = 0;
		return;
	}
	p = f->parent;
	sib = p->a == f ? p->b : p->a;
	win_free(f->w);
	free(f);
	gp = p->parent;
	*p = *sib;
	p->parent = gp;
	if (p->a)
		p->a->parent = p->b->parent = p;
	free(sib);
	app.focus = first_leaf(p);
	app.dirty = 1;
}

static void free_tree(struct node *n, struct node *keep)
{
	if (!n || n == keep)
		return;
	free_tree(n->a, keep);
	free_tree(n->b, keep);
	if (n->w)
		win_free(n->w);
	free(n);
}

static void focus_dir(int dx, int dy)
{
	struct node *best = NULL;
	Clay_BoundingBox c = { 0 }, o;
	float bd = 1e9f;
	int i;

	for (i = 0; i < app.nhits; i++)
		if (app.hits[i].kind == H_WIN &&
		    app.leaves[app.hits[i].a] == app.focus)
			c = app.hits[i].box;
	for (i = 0; i < app.nhits; i++) {
		float d;

		if (app.hits[i].kind != H_WIN ||
		    app.leaves[app.hits[i].a] == app.focus)
			continue;
		o = app.hits[i].box;
		if ((dx > 0 && o.x < c.x + c.width - 1) ||
		    (dx < 0 && o.x + o.width > c.x + 1) ||
		    (dy > 0 && o.y < c.y + c.height - 1) ||
		    (dy < 0 && o.y + o.height > c.y + 1))
			continue;
		d = (o.x + o.width / 2 - c.x - c.width / 2) * (o.x + o.width /
							       2 - c.x -
							       c.width / 2) +
		    (o.y + o.height / 2 - c.y - c.height / 2) * (o.y +
								 o.height / 2 -
								 c.y -
								 c.height / 2);
		if (d < bd) {
			bd = d;
			best = app.leaves[app.hits[i].a];
		}
	}
	if (best)
		app.focus = best;
}

static void compose(struct draft *d, struct account *a, const char *reply,
		    int flag)
{
	struct win *w = win_new(W_COMPOSE);
	int i;

	w->a = a;
	ed_init(&w->f[0], d->to, 1);
	ed_init(&w->f[1], d->cc, 1);
	ed_init(&w->f[2], d->subject, 1);
	if (a->signature) {
		char *b = xasprintf("%s\n\n-- \n%s", d->body ? d->body : "",
				    a->signature);

		ed_init(&w->f[3], b, 0);
		free(b);
	} else {
		ed_init(&w->f[3], d->body, 0);
	}
	w->f[3].cur = 0;
	for (i = 0; i < 4; i++)
		w->f[i].mode = cfg.vim_edit && i == 3 ? ED_NORMAL : ED_INSERT;
	w->field = d->to && *d->to ? 3 : 0;
	if (w->field == 3 && cfg.vim_edit)
		w->f[3].mode = ED_INSERT;
	w->reply_path = reply ? xstrdup(reply) : NULL;
	w->reply_flag = flag;
	if (d->in_reply_to)
		w->title = xasprintf("%s\n%s", d->in_reply_to, d->references ?
				     d->references : "");
	split('v', w);
}

static int load_message(const char *path, struct message *m)
{
	size_t len;
	char *raw = path ? read_file(path, &len) : NULL;

	if (!raw)
		return -1;
	return msg_parse(m, raw, len);
}

static void reply(int kind)
{
	struct win *w = focused();
	const char *p = cur_path(w);
	struct account *a = acc_of(w);
	struct message m;
	struct draft d;

	if (!a) {
		ui_status(1, "no account configured");
		return;
	}
	if (kind == 0) {
		memset(&d, 0, sizeof(d));
		compose(&d, a, NULL, 0);
		return;
	}
	if (load_message(p, &m)) {
		ui_status(1, "no message selected");
		return;
	}
	if (kind == 3)
		draft_forward(&d, a, &m);
	else
		draft_reply(&d, a, &m, kind == 2);
	compose(&d, a, p, kind == 3 ? F_PASSED : F_REPLIED);
	draft_free(&d);
	msg_free(&m);
}

static void draft_from(struct win *w, struct draft *d)
{
	char *refs = w->title ? strchr(w->title, '\n') : NULL;
	int i;

	memset(d, 0, sizeof(*d));
	d->to = ed_text(&w->f[0]);
	d->cc = ed_text(&w->f[1]);
	d->subject = ed_text(&w->f[2]);
	d->body = ed_text(&w->f[3]);
	if (refs) {
		d->in_reply_to = xstrndup(w->title, refs - w->title);
		d->references = xstrdup(refs + 1);
	}
	d->attach = xcalloc(w->nattach + 1, sizeof(char *));
	for (i = 0; i < w->nattach; i++)
		d->attach[i] = xstrdup(w->attach[i]);
	d->nattach = w->nattach;
}

static void send_compose(int save_draft)
{
	struct win *w = focused();
	struct draft d;

	if (!w || w->type != W_COMPOSE) {
		ui_status(1, "not composing a message");
		return;
	}
	draft_from(w, &d);
	if (save_draft) {
		char *dir = folder_dir(w->a->name, w->a->drafts), *data;
		size_t len;

		d.from = xasprintf("%s <%s>", w->a->realname ? w->a->realname :
				   "", w->a->email);
		data = msg_build(&d, &len);
		free(maildir_store(dir, data, len, F_DRAFT | F_SEEN, 0));
		ui_status(0, "draft saved to %s", w->a->drafts);
		free(data);
		free(dir);
		draft_free(&d);
		return;
	}
	ui_status(0, "sending...");
	draw();
	if (msg_send(w->a, &d)) {
		ui_status(1, "sending failed, see stderr");
	} else {
		if (w->reply_path) {
			char *np = maildir_set_flags(w->reply_path,
				maildir_flags_parse(w->reply_path) |
				w->reply_flag);

			free(np);
		}
		ui_status(0, "message sent");
		close_focus(0);
		reload_lists(app.root);
	}
	draft_free(&d);
}

static void job_start(int kind, char *const argv[], struct win *w,
		      const char *title)
{
	int p[2];
	struct job *j;
	pid_t pid;

	if (njobs == 8 || pipe(p))
		return;
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return;
	}
	if (!pid) {
		int null = open("/dev/null", O_RDONLY);

		dup2(null, 0);
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		execv(self, argv);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	j = &jobs[njobs++];
	memset(j, 0, sizeof(*j));
	j->pid = pid;
	j->fd = p[0];
	j->kind = kind;
	j->w = w;
	j->title = title ? xstrdup(title) : NULL;
}

static int win_alive(struct node *n, struct win *w)
{
	return n && (n->w == w || win_alive(n->a, w) || win_alive(n->b, w));
}

static void wizard_fill(const char *out);

static void job_done(struct job *j, int status)
{
	char *out = j->out.p ? j->out.p : "";

	switch (j->kind) {
	case J_SYNC:
		app.syncing = 0;
		folders_load();
		reload_lists(app.root);
		ui_status(status != 0, status ? "sync failed: %.200s" :
			  "sync finished", out);
		break;
	case J_LLM: {
		struct win *w = win_new(W_TEXT);

		w->title = j->title;
		j->title = NULL;
		set_chunks(w, out);
		split('v', w);
		ui_status(status != 0, status ? "AI request failed" : "");
		break;
	}
	case J_DRAFT:
		if (!status && win_alive(app.root, j->w)) {
			j->w->f[3].cur = 0;
			ed_insert(&j->w->f[3], out, strlen(out));
			ed_insert(&j->w->f[3], "\n", 1);
			ui_status(0, "AI draft inserted");
		} else {
			ui_status(1, "AI draft failed");
		}
		break;
	case J_AUTOCONF:
		wizard_fill(out);
		ui_status(0, "server settings detected, check them and save");
		break;
	default:
		if (j->out.len) {
			char *l = j->out.p + j->out.len - 1;

			while (l > j->out.p && *l == '\n')
				*l-- = 0;
			l = strrchr(j->out.p, '\n');
			ui_status(status != 0, "%s", l ? l + 1 : j->out.p);
		} else {
			ui_status(status != 0, status ? "command failed" :
				  "command finished");
		}
		reload_lists(app.root);
	}
	app.dirty = 1;
}

static void jobs_poll(void)
{
	int i;

	for (i = 0; i < njobs; i++) {
		struct job *j = &jobs[i];
		char tmp[4096];
		ssize_t n;
		int st;

		while ((n = read(j->fd, tmp, sizeof(tmp))) > 0)
			buf_add(&j->out, tmp, n);
		if (n == 0 || (n < 0 && errno != EAGAIN)) {
			close(j->fd);
			waitpid(j->pid, &st, 0);
			buf_add(&j->out, "", 0);
			job_done(j, WIFEXITED(st) ? WEXITSTATUS(st) : 1);
			buf_free(&j->out);
			free(j->title);
			jobs[i--] = jobs[--njobs];
		}
	}
}

static void sync_start(void)
{
	char *argv[] = { "epis", "-c", cfg.path, "sync", "-q", NULL };

	if (app.syncing)
		return;
	app.syncing = 1;
	ui_status(0, "synchronizing...");
	job_start(J_SYNC, argv, NULL, NULL);
}

static void llm_msg(int kind, const char *arg)
{
	struct win *w = focused();
	const char *p = cur_path(w);
	char *id;

	if (kind == J_DRAFT) {
		char *argv[] = { "epis", "-c", cfg.path, "llm", "draft", NULL,
			(char *)arg, NULL };

		if (!w || w->type != W_COMPOSE || !w->reply_path) {
			ui_status(1, "ai_draft works in a reply");
			return;
		}
		argv[5] = id = maildir_id(w->reply_path);
		job_start(J_DRAFT, argv, w, NULL);
		free(id);
	} else if (arg) {
		char *argv[] = { "epis", "-c", cfg.path, "llm", "ask",
			(char *)arg, NULL };

		job_start(J_LLM, argv, NULL, "AI answer");
	} else {
		char *argv[] = { "epis", "-c", cfg.path, "llm", "summarize",
			NULL, NULL };

		if (!p) {
			ui_status(1, "no message selected");
			return;
		}
		argv[5] = id = maildir_id(p);
		job_start(J_LLM, argv, NULL, "Summary");
		free(id);
	}
	ui_status(0, "asking the AI...");
}

static void picker_filter(void)
{
	struct picker *pk = app.pick;
	char *q = ed_text(&pk->input);
	int i, j;

	pk->nmatch = 0;
	for (i = 0; i < pk->n; i++) {
		pk->items[i].score = fuzzy_score(q, pk->items[i].label);
		if (pk->items[i].score < 0)
			continue;
		for (j = pk->nmatch; j > 0 &&
		     pk->items[pk->match[j - 1]].score < pk->items[i].score;
		     j--)
			pk->match[j] = pk->match[j - 1];
		pk->match[j] = i;
		pk->nmatch++;
	}
	pk->sel = 0;
	pk->top = 0;
	free(q);
}

static void picker_add(struct picker *pk, char *label, char *data)
{
	pk->items = xrealloc(pk->items, (pk->n + 1) * sizeof(*pk->items));
	pk->items[pk->n].label = label;
	pk->items[pk->n++].data = data;
}

static void picker_close(void)
{
	struct picker *pk = app.pick;
	int i;

	if (!pk)
		return;
	for (i = 0; i < pk->n; i++) {
		free(pk->items[i].label);
		free(pk->items[i].data);
	}
	free(pk->items);
	free(pk->match);
	free(pk->title);
	ed_free(&pk->input);
	free(pk);
	app.pick = NULL;
	app.dirty = 1;
}

static struct picker *picker_new(int kind, const char *title)
{
	struct picker *pk = xcalloc(1, sizeof(*pk));

	picker_close();
	pk->kind = kind;
	pk->title = xstrdup(title);
	ed_init(&pk->input, "", 1);
	app.pick = pk;
	return pk;
}

static void picker_show(struct picker *pk)
{
	pk->match = xcalloc(pk->n + 1, sizeof(int));
	picker_filter();
	app.dirty = 1;
}


static const char *cmd_names[] = {
	"quit", "close", "only", "split", "hsplit", "equalize", "focus_left",
	"focus_right", "focus_up", "focus_down", "focus_next", "open",
	"open_split", "back", "down", "up", "top", "bottom", "pagedown",
	"pageup", "scroll_down", "scroll_up", "next", "prev", "find_inbox",
	"find_message", "find_command", "folder", "sync", "compose", "reply",
	"reply_all", "forward", "delete", "archive", "move", "flag",
	"toggle_read", "send", "draft", "attach", "next_field", "prev_field",
	"sidebar", "menubar", "menu", "summarize", "ask", "ai_draft", "reload",
	"help", "cmdline", "screenshot", "feedkeys", NULL
};

static void find_folder(int kind)
{
	struct picker *pk = picker_new(kind, kind == PK_MOVE ? "Move to" :
				       "Find inbox");
	int i;

	folders_load();
	for (i = 0; i < app.nfolders; i++)
		picker_add(pk, xstrdup(app.folders[i]),
			   xstrdup(app.folders[i]));
	picker_show(pk);
}

static void find_message(void)
{
	struct win *w = focused();
	struct account *a = acc_of(w);
	struct picker *pk;
	char **f;
	int n, i, j;

	if (!a)
		return;
	pk = picker_new(PK_MESSAGE, "Find message");
	n = maildir_folders(a->name, &f);
	for (i = 0; i < n; i++) {
		char *dir = folder_dir(a->name, f[i]);
		struct msginfo *m;
		int c = maildir_scan(dir, &m, 1);

		sort_by_date(m, c);
		for (j = 0; j < c; j++)
			picker_add(pk, xasprintf("%s  -  %s  [%s]",
						 m[j].subject, m[j].from, f[i]),
				   xstrdup(m[j].path));
		msginfo_free(m, c);
		free(dir);
		free(f[i]);
	}
	free(f);
	picker_show(pk);
}

static void find_command(void)
{
	struct picker *pk = picker_new(PK_COMMAND, "Commands");
	int i;

	for (i = 0; cmd_names[i]; i++)
		picker_add(pk, xstrdup(cmd_names[i]), xstrdup(cmd_names[i]));
	for (i = 0; i < cfg.nalias; i++)
		picker_add(pk, xasprintf("%s  (%s)", cfg.alias[i].name,
					 cfg.alias[i].cmd),
			   xstrdup(cfg.alias[i].name));
	picker_show(pk);
}

static void open_folder(const char *af)
{
	struct win *w = focused();
	char *acc = xstrndup(af, strcspn(af, "/"));
	const char *folder = strchr(af, '/');

	if (!folder) {
		free(acc);
		return;
	}
	if (!w) {
		w = win_new(W_LIST);
		app.root = app.focus = leaf_new(w);
	}
	if (w->type != W_LIST && w->type != W_MSG) {
		w = clone_list(NULL);
		split('v', w);
	}
	w->type = W_LIST;
	w->sel = w->top = 0;
	msginfo_free(w->msgs, w->nmsgs);
	w->msgs = NULL;
	w->nmsgs = 0;
	list_load(w, acc, folder + 1);
	free(acc);
}

static void move_to(const char *af)
{
	struct win *w = focused();
	const char *p = cur_path(w);
	char *acc = xstrndup(af, strcspn(af, "/"));
	char *dir = folder_dir(acc, strchr(af, '/') + 1), *np;

	np = p ? maildir_move(p, dir) : NULL;
	ui_status(!np, np ? "moved to %s" : "move failed", af);
	free(np);
	free(acc);
	free(dir);
	if (w && w->type == W_MSG && w->has_list)
		w->type = W_LIST;
	reload_lists(app.root);
}

static void picker_accept(void)
{
	struct picker *pk = app.pick;
	char *data;
	int kind = pk->kind;

	if (!pk->nmatch) {
		picker_close();
		return;
	}
	data = xstrdup(pk->items[pk->match[pk->sel]].data);
	picker_close();
	if (kind == PK_FOLDER)
		open_folder(data);
	else if (kind == PK_MOVE)
		move_to(data);
	else if (kind == PK_COMMAND)
		ui_command(data);
	else if (kind == PK_MESSAGE && focused())
		msg_show(focused(), data);
	free(data);
}

static void msg_flag(int flag, int toggle)
{
	struct win *w = focused();
	const char *p = cur_path(w);
	char *np;
	int f;

	if (!p)
		return;
	f = maildir_flags_parse(p);
	f = toggle && (f & flag) ? f & ~flag : f | flag;
	np = maildir_set_flags(p, f);
	if (np && w->type == W_MSG) {
		free(w->path);
		w->path = xstrdup(np);
	}
	free(np);
	reload_lists(app.root);
}

static void msg_delete(int archive)
{
	struct win *w = focused();
	const char *p = cur_path(w);
	struct account *a = acc_of(w);
	char *np = NULL;

	if (!p || !a)
		return;
	if (archive) {
		char *dir = folder_dir(a->name, a->archive);
		char *cur = xasprintf("%s/cur", dir);

		if (access(cur, F_OK))
			ui_status(1, "no archive folder '%s'", a->archive);
		else
			np = maildir_move(p, dir);
		free(dir);
		free(cur);
	} else {
		np = maildir_trash(a, p);
	}
	free(np);
	if (w->type == W_MSG && w->has_list)
		w->type = W_LIST;
	else if (w->type == W_MSG)
		close_focus(0);
	reload_lists(app.root);
}

static void move_sel(struct win *w, int d)
{
	if (!w || !w->nmsgs)
		return;
	w->sel += d;
	if (w->sel < 0)
		w->sel = 0;
	if (w->sel >= w->nmsgs)
		w->sel = w->nmsgs - 1;
	app.dirty = 1;
}

static void attach(struct win *w, const char *file)
{
	char *p;

	if (!w || w->type != W_COMPOSE || !file || !*file) {
		ui_status(1, "usage: attach FILE (while composing)");
		return;
	}
	p = expand_home(file);
	if (access(p, R_OK)) {
		ui_status(1, "cannot read %s", p);
		free(p);
		return;
	}
	w->attach = xrealloc(w->attach, (w->nattach + 1) * sizeof(char *));
	w->attach[w->nattach++] = p;
}

static void shell(const char *cmdline)
{
	struct win *w = focused();
	const char *p = cur_path(w);
	struct buf b = { 0 };
	char *argv[] = { "/bin/sh", "-c", NULL, NULL };
	char saved[4096];

	for (; *cmdline; cmdline++) {
		if (cmdline[0] == '%' && cmdline[1] == 'f') {
			buf_printf(&b, "'%s'", p ? p : "");
			cmdline++;
		} else {
			buf_putc(&b, *cmdline);
		}
	}
	buf_add(&b, "", 0);
	argv[2] = b.p;
	memcpy(saved, self, sizeof(saved));
	strcpy(self, "/bin/sh");
	job_start(J_SHELL, argv, NULL, NULL);
	memcpy(self, saved, sizeof(saved));
	buf_free(&b);
}

static void help(void)
{
	struct win *w = win_new(W_TEXT);
	struct buf b = { 0 };
	int i;

	for (i = 0; i < nbinds; i++)
		buf_printf(&b, "%-8s %-14s %s\n", binds[i].ctx, binds[i].keys,
			   binds[i].cmd);
	w->title = xstrdup("Key bindings");
	set_chunks(w, b.p);
	buf_free(&b);
	split('v', w);
}


static void scroll(struct win *w, float dy)
{
	w->scroll += dy;
	if (w->scroll > w->maxscroll)
		w->scroll = w->maxscroll;
	if (w->scroll < 0)
		w->scroll = 0;
	app.dirty = 1;
}

static void feedkeys(const char *keys);

/* run one command line such as "move Archive" or "!notmuch new" */
void ui_command(const char *line)
{
	struct win *w = focused();
	char name[64];
	const char *arg;
	int i, page;

	while (*line == ' ' || *line == ':')
		line++;
	if (!*line)
		return;
	if (*line == '!') {
		shell(line + 1);
		return;
	}
	if (strchr(line, ';')) {
		char *dup = xstrdup(line), *s, *save = NULL;

		for (s = strtok_r(dup, ";", &save); s;
		     s = strtok_r(NULL, ";", &save))
			ui_command(s);
		free(dup);
		return;
	}
	snprintf(name, sizeof(name), "%.*s", (int)strcspn(line, " "), line);
	arg = line + strlen(name);
	while (*arg == ' ')
		arg++;
	if (!*arg)
		arg = NULL;
	for (i = 0; i < cfg.nalias; i++)
		if (!strcmp(name, cfg.alias[i].name)) {
			ui_command(cfg.alias[i].cmd);
			return;
		}
	page = w && w->vis > 2 ? w->vis - 2 : 10;
	app.dirty = 1;
	if (!strcmp(name, "quit") || !strcmp(name, "qa") ||
	    !strcmp(name, "q!")) {
		app.running = 0;
	} else if (!strcmp(name, "q") || !strcmp(name, "close")) {
		close_focus(!strcmp(name, "q"));
	} else if (!strcmp(name, "only")) {
		struct node *f = app.focus;

		if (f && f != app.root) {
			struct win *keep = f->w;

			f->w = NULL;
			free_tree(app.root, NULL);
			app.root = app.focus = leaf_new(keep);
		}
	} else if (!strcmp(name, "split") || !strcmp(name, "vsplit") ||
		   !strcmp(name, "hsplit")) {
		split(name[0] == 'h' ? 's' : 'v', clone_list(w));
	} else if (!strcmp(name, "equalize")) {
		struct node *n = app.focus;

		for (; n; n = n->parent)
			n->ratio = 0.5f;
	} else if (!strncmp(name, "focus_", 6)) {
		if (!strcmp(name + 6, "next")) {
			for (i = 0; i < app.nleaves; i++)
				if (app.leaves[i] == app.focus)
					break;
			if (app.nleaves)
				app.focus = app.leaves[(i + 1) % app.nleaves];
		} else {
			focus_dir(name[6] == 'l' ? -1 : name[6] == 'r' ? 1 : 0,
				  name[6] == 'u' ? -1 : name[6] == 'd' ? 1 : 0);
		}
	} else if (!strcmp(name, "open") && w && w->type == W_LIST) {
		if (w->sel < w->nmsgs)
			msg_show(w, w->msgs[w->sel].path);
	} else if (!strcmp(name, "open_split")) {
		const char *p = cur_path(w);

		if (p) {
			struct win *nw = win_new(W_MSG);
			char *pp = xstrdup(p);

			split('v', nw);
			msg_show(nw, pp);
			free(pp);
			reload_lists(app.root);
		}
	} else if (!strcmp(name, "back")) {
		if (w && w->type == W_MSG && w->has_list) {
			w->type = W_LIST;
			reload_lists(app.root);
		} else {
			close_focus(0);
		}
	} else if (!strcmp(name, "down") || !strcmp(name, "up")) {
		move_sel(w, name[0] == 'd' ? 1 : -1);
	} else if (!strcmp(name, "next") || !strcmp(name, "prev")) {
		if (w && w->has_list) {
			move_sel(w, name[0] == 'n' ? 1 : -1);
			if (w->sel < w->nmsgs)
				msg_show(w, w->msgs[w->sel].path);
		}
	} else if (!strcmp(name, "top") || !strcmp(name, "bottom")) {
		if (w && w->type == W_LIST)
			move_sel(w, name[0] == 't' ? -w->nmsgs : w->nmsgs);
		else if (w)
			scroll(w, name[0] == 't' ? -1e9f : 1e9f);
	} else if (!strcmp(name, "pagedown") || !strcmp(name, "pageup")) {
		if (w && w->type == W_LIST)
			move_sel(w, name[4] == 'd' ? page : -page);
		else if (w)
			scroll(w, (name[4] == 'd' ? 1 : -1) * w->vislines *
			       0.8f);
	} else if (!strcmp(name, "scroll_down") || !strcmp(name, "scroll_up")) {
		if (w)
			scroll(w, name[7] == 'd' ? 40 : -40);
	} else if (!strcmp(name, "find_inbox")) {
		find_folder(PK_FOLDER);
	} else if (!strcmp(name, "find_message")) {
		find_message();
	} else if (!strcmp(name, "find_command")) {
		find_command();
	} else if (!strcmp(name, "folder") && arg) {
		if (strchr(arg, '/')) {
			open_folder(arg);
		} else if (acc_of(w)) {
			char *af = xasprintf("%s/%s", acc_of(w)->name, arg);

			open_folder(af);
			free(af);
		}
	} else if (!strcmp(name, "move")) {
		if (!arg) {
			find_folder(PK_MOVE);
		} else {
			char *af = strchr(arg, '/') ? xstrdup(arg) :
			    xasprintf("%s/%s", acc_of(w) ? acc_of(w)->name : "",
				      arg);

			move_to(af);
			free(af);
		}
	} else if (!strcmp(name, "sync")) {
		sync_start();
	} else if (!strcmp(name, "compose")) {
		reply(0);
	} else if (!strcmp(name, "reply")) {
		reply(1);
	} else if (!strcmp(name, "reply_all")) {
		reply(2);
	} else if (!strcmp(name, "forward")) {
		reply(3);
	} else if (!strcmp(name, "delete")) {
		msg_delete(0);
	} else if (!strcmp(name, "archive")) {
		msg_delete(1);
	} else if (!strcmp(name, "flag")) {
		msg_flag(F_FLAGGED, 1);
	} else if (!strcmp(name, "toggle_read")) {
		msg_flag(F_SEEN, 1);
	} else if (!strcmp(name, "send") || !strcmp(name, "wq") ||
		   !strcmp(name, "x")) {
		send_compose(0);
	} else if (!strcmp(name, "draft") || !strcmp(name, "w")) {
		send_compose(1);
	} else if (!strcmp(name, "attach")) {
		attach(w, arg);
	} else if (!strcmp(name, "next_field") || !strcmp(name, "prev_field")) {
		if (w && w->type == W_COMPOSE) {
			w->field = (w->field + (name[0] == 'n' ? 1 : 3)) % 4;
			w->f[w->field].mode = ED_INSERT;
		}
	} else if (!strcmp(name, "sidebar")) {
		app.sidebar = !app.sidebar;
	} else if (app.native_menu && (!strcmp(name, "menubar") ||
					!strcmp(name, "menu"))) {
		ui_status(0, "the menu is in the system menu bar");
	} else if (!strcmp(name, "menubar")) {
		cfg.menubar = !cfg.menubar;
	} else if (!strcmp(name, "menu")) {
		cfg.menubar = 1;
		app.menu_open = app.menu_open ? 0 : 1;
	} else if (!strcmp(name, "summarize")) {
		llm_msg(J_LLM, NULL);
	} else if (!strcmp(name, "ask")) {
		if (arg)
			llm_msg(J_LLM, arg);
	} else if (!strcmp(name, "ai_draft")) {
		llm_msg(J_DRAFT, arg ? arg : "write a short, polite reply");
	} else if (!strcmp(name, "reload")) {
		folders_load();
		reload_lists(app.root);
	} else if (!strcmp(name, "help")) {
		help();
	} else if (!strcmp(name, "screenshot") && arg) {
		char *p = expand_home(arg);

		r_screenshot(p);
		draw();
		ui_status(0, "saved %s", p);
		free(p);
	} else if (!strcmp(name, "feedkeys") && arg) {
		feedkeys(arg);
	} else if (!strcmp(name, "cmdline")) {
		app.cmdline = 1;
		ed_set(&app.cmd, "");
	} else {
		ui_status(1, "unknown command: %s", name);
	}
}

static const char *win_ctx(struct win *w)
{
	if (!w)
		return "global";
	switch (w->type) {
	case W_LIST:
		return "list mail global";
	case W_MSG:
		return "message mail global";
	case W_COMPOSE:
		return "compose global";
	default:
		return "message global";
	}
}

/* run the binding engine; returns 1 when the key was consumed */
static int dispatch(const char *ctx, const char *tok)
{
	char seq[64];
	const char *cmd = NULL;
	int m;

	snprintf(seq, sizeof(seq), "%s%s", app.keys, tok);
	m = bind_match(ctx, seq, &cmd);
	if (m == 1) {
		snprintf(app.keys, sizeof(app.keys), "%s", seq);
		app.dirty = 1;
		return 1;
	}
	app.keys[0] = 0;
	if (m == 2) {
		char *c = xstrdup(cmd);

		ui_command(c);
		free(c);
		return 1;
	}
	return 0;
}

/* feed the pending sequence and tok to the compose editor */
static void compose_key(struct win *w, const char *tok, const char *text)
{
	struct editor *e = &w->f[w->field];
	char pend[64], *p;

	if (e->mode == ED_INSERT || !cfg.vim_edit) {
		if (!strcmp(tok, "<Tab>") || !strcmp(tok, "<S-Tab>")) {
			if (w->field < 3 || !cfg.vim_edit) {
				ui_command(tok[1] == 'S' ? "prev_field" :
					   "next_field");
				return;
			}
		}
		if (ed_key(e, tok, text, cfg.vim_edit) == ED_ENTER)
			ui_command("next_field");
		app.dirty = 1;
		return;
	}
	if (dispatch(win_ctx(w), tok))
		return;
	snprintf(pend, sizeof(pend), "%s", app.keys);
	app.keys[0] = 0;
	for (p = pend; *p;) {
		char t[16];
		size_t l = *p == '<' && strchr(p, '>') ? (size_t)(strchr(p,
			'>') - p + 1) : 1;

		snprintf(t, sizeof(t), "%.*s", (int)l, p);
		ed_key(e, t, t, 1);
		p += l;
	}
	if (ed_key(e, tok, text, 1) == ED_ENTER)
		ui_command("next_field");
	app.dirty = 1;
}

static void wizard_key(const char *tok, const char *text)
{
	struct editor *e = &app.wf[app.wfield];
	int r;

	if (!strcmp(tok, "<Tab>") || !strcmp(tok, "<Down>")) {
		app.wfield = (app.wfield + 1) % 11;
	} else if (!strcmp(tok, "<S-Tab>") || !strcmp(tok, "<Up>")) {
		app.wfield = (app.wfield + 10) % 11;
	} else if (!strcmp(tok, "<Esc>")) {
		app.running = 0;
	} else {
		r = ed_key(e, tok, text, 0);
		if (r == ED_ENTER) {
			if (app.wfield == 0)
				wizard_detect();
			if (app.wfield == 10)
				wizard_save();
			else
				app.wfield++;
		}
	}
	app.dirty = 1;
}

static void key_tok(const char *tok, const char *text);

static void key(struct event *ev)
{
	char tok[32];

	key_token(ev, tok, sizeof(tok));
	if (tok[0])
		key_tok(tok, ev->text);
}

/* replay a key sequence written in binding notation, as vim's feedkeys */
static void feedkeys(const char *keys)
{
	char *c = canon(keys), *p = c;

	while (*p) {
		char t[32], text[32] = "";
		size_t l = *p == '<' && strchr(p, '>') ? (size_t)(strchr(p,
			'>') - p + 1) : 1;

		while (l == 1 && ((unsigned char)p[l] & 0xc0) == 0x80)
			l++;
		snprintf(t, sizeof(t), "%.*s", (int)l, p);
		if (l == 1 || t[0] != '<')
			snprintf(text, sizeof(text), "%s", t);
		else if (!strcmp(t, "<Space>"))
			strcpy(text, " ");
		else if (!strcmp(t, "<lt>"))
			strcpy(text, "<");
		key_tok(t, text);
		p += l;
	}
	free(c);
}

static void key_tok(const char *tok, const char *text)
{
	struct event evs = { 0 }, *ev = &evs;
	struct win *w = focused();
	int r;

	snprintf(ev->text, sizeof(ev->text), "%s", text);
	app.dirty = 1;
	if (app.menu_open || app.ctx_open) {
		app.menu_open = app.ctx_open = 0;
		if (!strcmp(tok, "<Esc>"))
			return;
	}
	if (app.wizard) {
		wizard_key(tok, ev->text);
		return;
	}
	if (app.pick) {
		struct picker *pk = app.pick;

		if (!strcmp(tok, "<Esc>")) {
			picker_close();
		} else if (!strcmp(tok, "<CR>")) {
			picker_accept();
		} else if (!strcmp(tok, "<Down>") || !strcmp(tok, "<C-n>") ||
			   !strcmp(tok, "<C-j>") || !strcmp(tok, "<Tab>")) {
			if (pk->sel + 1 < pk->nmatch)
				pk->sel++;
		} else if (!strcmp(tok, "<Up>") || !strcmp(tok, "<C-p>") ||
			   !strcmp(tok, "<C-k>") || !strcmp(tok, "<S-Tab>")) {
			if (pk->sel > 0)
				pk->sel--;
		} else if (ed_key(&pk->input, tok, ev->text, 0) == ED_OK) {
			picker_filter();
		}
		return;
	}
	if (app.cmdline) {
		if (!strcmp(tok, "<BS>") && !ed_len(&app.cmd)) {
			app.cmdline = 0;
			return;
		}
		r = ed_key(&app.cmd, tok, ev->text, 0);
		if (r == ED_ENTER || r == ED_ESCAPE) {
			char *line = ed_text(&app.cmd);

			app.cmdline = 0;
			if (r == ED_ENTER)
				ui_command(line);
			free(line);
		}
		return;
	}
	if (w && w->type == W_COMPOSE) {
		compose_key(w, tok, ev->text);
		return;
	}
	if (!dispatch(win_ctx(w), tok) && !strcmp(tok, "<Esc>"))
		app.keys[0] = 0;
}

static struct hit *hit_at(float x, float y)
{
	int i;

	for (i = app.nhits - 1; i >= 0; i--) {
		Clay_BoundingBox b = app.hits[i].box;

		if (x >= b.x && y >= b.y && x < b.x + b.width &&
		    y < b.y + b.height)
			return &app.hits[i];
	}
	return NULL;
}

static struct hit *hit_kind_at(float x, float y, int kind)
{
	int i;

	for (i = app.nhits - 1; i >= 0; i--) {
		Clay_BoundingBox b = app.hits[i].box;

		if (app.hits[i].kind == kind && x >= b.x && y >= b.y &&
		    x < b.x + b.width && y < b.y + b.height)
			return &app.hits[i];
	}
	return NULL;
}

static void click(int button)
{
	struct hit *h = hit_at(app.mx, app.my), *wh;
	int had_menu = app.menu_open || app.ctx_open;

	wh = hit_kind_at(app.mx, app.my, H_WIN);
	if (!h)
		return;
	app.dirty = 1;
	if (h->kind != H_MENU)
		app.menu_open = 0;
	if (h->kind != H_CTX_ITEM)
		app.ctx_open = 0;
	switch (h->kind) {
	case H_MENU:
		app.menu_open = app.menu_open == h->a + 1 ? 0 : h->a + 1;
		break;
	case H_MENU_ITEM:
		app.menu_open = 0;
		ui_command(menu_items[h->a][h->b].cmd);
		break;
	case H_CTX_ITEM:
		app.ctx_open = 0;
		ui_command(ctx_items[h->a].cmd);
		break;
	case H_PICK_ITEM:
		if (app.pick) {
			app.pick->sel = h->a;
			picker_accept();
		}
		break;
	case H_FOLDER:
		if (h->a < app.nfolders) {
			char *f = xstrdup(app.folders[h->a]);

			open_folder(f);
			free(f);
		}
		break;
	case H_FIELD:
		if (app.wizard) {
			app.wfield = h->a;
		} else if (wh) {
			app.focus = app.leaves[wh->a];
			if (focused()->type == W_COMPOSE)
				focused()->field = h->a;
		}
		break;
	case H_BUTTON:
		if (h->a == 0)
			wizard_detect();
		else if (h->a == 1)
			wizard_save();
		else if (h->a == 2)
			ui_command("send");
		else if (h->a == 3)
			ui_command("draft");
		break;
	case H_ROW:
		if (had_menu && button == 1)
			break;
		app.focus = app.leaves[h->a];
		focused()->sel = h->b;
		if (button == 3) {
			app.ctx_open = 1;
			app.ctx_x = app.mx;
			app.ctx_y = app.my;
		} else if (button == 1) {
			ui_command("open");
		}
		break;
	case H_WIN:
		app.focus = app.leaves[h->a];
		break;
	}
}

static void wheel(float dy)
{
	struct hit *h = hit_kind_at(app.mx, app.my, H_WIN);
	struct win *w;

	if (app.pick) {
		app.pick->sel += dy > 0 ? 1 : -1;
		if (app.pick->sel < 0)
			app.pick->sel = 0;
		if (app.pick->sel >= app.pick->nmatch)
			app.pick->sel = app.pick->nmatch - 1;
		app.dirty = 1;
		return;
	}
	if (!h)
		return;
	w = app.leaves[h->a]->w;
	if (w->type == W_LIST) {
		w->top += dy > 0 ? 3 : -3;
		if (w->top > w->nmsgs - w->vis)
			w->top = w->nmsgs - w->vis;
		if (w->top < 0)
			w->top = 0;
		app.dirty = 1;
	} else if (w->type == W_COMPOSE) {
		w->f[3].top += dy > 0 ? 3 : -3;
		if (w->f[3].top < 0)
			w->f[3].top = 0;
		app.dirty = 1;
	} else {
		scroll(w, dy * 3);
	}
}

/* run a command chosen from the system menu bar */
static void menu_command(const char *cmd)
{
	if (app.wizard) {
		if (!strcmp(cmd, "quit"))
			app.running = 0;
		return;
	}
	picker_close();
	app.cmdline = app.ctx_open = 0;
	app.keys[0] = 0;
	ui_command(cmd);
	app.dirty = 1;
}

static void event(struct event *e)
{
	switch (e->type) {
	case E_COMMAND:
		menu_command(e->cmd);
		break;
	case E_KEY:
		key(e);
		break;
	case E_MOTION:
		app.mx = e->x / app.scale;
		app.my = e->y / app.scale;
		if (app.menu_open || app.ctx_open)
			app.dirty = 1;
		break;
	case E_BUTTON:
		app.mx = e->x / app.scale;
		app.my = e->y / app.scale;
		if (e->pressed)
			click(e->button);
		break;
	case E_SCROLL:
		wheel(e->dy);
		break;
	case E_RESIZE:
	case E_EXPOSE:
		app.dirty = 1;
		break;
	case E_CLOSE:
		app.running = 0;
		break;
	}
}

static const char *wizard_labels[] = {
	"Email address", "Your name", "IMAP server", "IMAP port",
	"IMAP security", "SMTP server", "SMTP port", "SMTP security",
	"Username", "Password", "Mail directory"
};

const char **wizard_label_list(void)
{
	return wizard_labels;
}

/* start detecting server settings for the email in the wizard */
int wizard_detect(void)
{
	char *email = ed_text(&app.wf[0]);
	char *argv[] = { "epis", "autoconfig", email, NULL };

	if (!strchr(email, '@')) {
		ui_status(1, "enter an email address first");
		free(email);
		return -1;
	}
	if (!ed_len(&app.wf[8]))
		ed_set(&app.wf[8], email);
	ui_status(0, "looking up server settings...");
	job_start(J_AUTOCONF, argv, NULL, NULL);
	free(email);
	return 0;
}

static void wizard_fill(const char *out)
{
	static const char *keys[] = {
		"imap_host", "imap_port", "imap_security", "smtp_host",
		"smtp_port", "smtp_security"
	};
	int i;

	for (i = 0; i < 6; i++) {
		const char *p = strstr(out, keys[i]);

		if (p && (p = strstr(p, "= "))) {
			char *v = xstrndup(p + 2, strcspn(p + 2, "\n"));

			ed_set(&app.wf[i < 3 ? 2 + i : 5 + i - 3], v);
			free(v);
		}
	}
	if (strstr(out, "oauth = "))
		ui_status(0, "this provider needs OAuth2: after saving, set "
			  "oauth_client_id and run 'epis oauth ACCOUNT'");
	app.dirty = 1;
}

/* write the account from the wizard and switch to the mail view */
int wizard_save(void)
{
	struct account a = { 0 };
	char *v[11], *md;
	int i;

	for (i = 0; i < 11; i++)
		v[i] = ed_text(&app.wf[i]);
	if (!strchr(v[0], '@') || !*v[2] || !*v[5]) {
		ui_status(1, "email, IMAP server and SMTP server are required");
		for (i = 0; i < 11; i++)
			free(v[i]);
		return -1;
	}
	a.email = v[0];
	a.realname = *v[1] ? v[1] : NULL;
	a.name = xstrndup(v[0], strcspn(v[0], "@"));
	a.imap_host = v[2];
	a.imap_port = atoi(v[3]) ? atoi(v[3]) : 993;
	a.imap_sec = sec_from_name(v[4]) < 0 ? SEC_TLS : sec_from_name(v[4]);
	a.smtp_host = v[5];
	a.smtp_port = atoi(v[6]) ? atoi(v[6]) : 465;
	a.smtp_sec = sec_from_name(v[7]) < 0 ? SEC_TLS : sec_from_name(v[7]);
	a.user = *v[8] ? v[8] : v[0];
	a.pass = *v[9] ? v[9] : NULL;
	md = expand_home(*v[10] ? v[10] : "~/Mail");
	if (config_write_account(&a, md)) {
		ui_status(1, "cannot write %s", cfg.path);
	} else {
		char *path = xstrdup(cfg.path);

		cfg.nacc = 0;
		config_load(path);
		free(path);
		app.wizard = 0;
		{
			char *af = xasprintf("%s/INBOX", a.name);

			app.root = app.focus = leaf_new(win_new(W_LIST));
			open_folder(af);
			free(af);
		}
		folders_load();
		sync_start();
	}
	free(a.name);
	free(md);
	for (i = 0; i < 11; i++)
		free(v[i]);
	return 0;
}

static void wizard_init(void)
{
	static const char *defs[11] = {
		"", "", "", "993", "tls", "", "465", "tls", "", "", "~/Mail"
	};
	int i;

	app.wizard = 1;
	for (i = 0; i < 11; i++)
		ed_init(&app.wf[i], defs[i], 1);
	ui_status(0, "Welcome to epis! Enter your email and press Enter to "
		  "detect the servers.");
}

void hit_add(Clay_ElementId id, int kind, int a, int b)
{
	if (app.nhits == app.hitcap) {
		app.hitcap = app.hitcap * 2 + 64;
		app.hits = xrealloc(app.hits, app.hitcap * sizeof(*app.hits));
	}
	app.hits[app.nhits].id = id;
	app.hits[app.nhits].kind = kind;
	app.hits[app.nhits].a = a;
	app.hits[app.nhits++].b = b;
}

const char *editor_ctx_mode(void)
{
	struct win *w = focused();

	if (app.wizard)
		return "SETUP";
	if (app.pick)
		return "FIND";
	if (app.cmdline)
		return "COMMAND";
	if (w && w->type == W_COMPOSE) {
		switch (w->f[w->field].mode) {
		case ED_INSERT:
			return "INSERT";
		case ED_VISUAL:
			return "VISUAL";
		case ED_VLINE:
			return "V-LINE";
		}
	}
	return "NORMAL";
}

static void on_signal(int s)
{
	(void)s;
}

/* path of the running executable, used to start background jobs */
static void self_path(char *out, size_t n)
{
#ifdef __APPLE__
	uint32_t size = n;

	if (_NSGetExecutablePath(out, &size))
		out[0] = 0;
#else
	ssize_t len = readlink("/proc/self/exe", out, n - 1);

	out[len > 0 ? len : 0] = 0;
#endif
}

/* redraw if needed, reloading the fonts when the output scale changed */
void ui_refresh(void)
{
	if (!app.running)
		return;
	if (plat_scale() != app.scale) {
		app.scale = plat_scale();
		font_init(app.scale);
		Clay_ResetMeasureTextCache();
		app.dirty = 1;
	}
	if (app.dirty) {
		app.dirty = 0;
		draw();
	}
}

int gui_main(const char *startup)
{
	struct event ev;

	self_path(self, sizeof(self));
	signal(SIGCHLD, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (plat_init("epis", 1200, 800)) {
		warnx("no display available; use the command line interface "
		      "(epis help)");
		return 1;
	}
	app.scale = plat_scale();
	if (font_init(app.scale) || r_init())
		return 1;
	binds_init();
	app.native_menu = plat_menu(menu_titles, menu_items);
	ed_init(&app.cmd, "", 1);
	app.sidebar = 1;
	app.running = 1;
	app.dirty = 1;
	if (!config_exists() || !cfg.nacc) {
		wizard_init();
	} else {
		char *af = xasprintf("%s/INBOX", cfg.acc[0].name);

		app.root = app.focus = leaf_new(win_new(W_LIST));
		folders_load();
		open_folder(af);
		free(af);
	}
	if (startup) {
		char *dup = xstrdup(startup), *c, *save = NULL;

		for (c = strtok_r(dup, ";", &save); c && app.running;
		     c = strtok_r(NULL, ";", &save)) {
			while (plat_next(&ev))
				event(&ev);
			draw();
			if (!strncmp(trim(c), "wait", 4)) {
				while (njobs) {
					poll(NULL, 0, 50);
					jobs_poll();
				}
				continue;
			}
			ui_command(c);
		}
		free(dup);
	}
	while (app.running) {
		int fds[8], i;

		while (plat_next(&ev))
			event(&ev);
		ui_refresh();
		if (!app.running)
			break;
		for (i = 0; i < njobs; i++)
			fds[i] = jobs[i].fd;
		if (plat_wait(fds, njobs))
			break;
		jobs_poll();
	}
	r_quit();
	plat_quit();
	return 0;
}
