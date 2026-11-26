/* select.c - selecting text in the message view with mouse or vim keys */
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* one line of a document as laid out on screen */
struct tline {
	struct win *w;
	size_t off;
	int len, font, size;
	Clay_BoundingBox box;
};

static struct tline *lines;
static int nlines, cap;
/* the window and text position where a mouse drag started */
static struct win *drag;
static size_t drag_off;

/* the window that shows the document a laid out string is a slice of */
static struct win *owner(const char *s)
{
	uintptr_t p = (uintptr_t)s;
	int i;

	for (i = 0; i < app.nleaves; i++) {
		struct win *w = app.leaves[i]->w;

		if ((w->type == W_MSG || w->type == W_TEXT) && w->doc &&
		    p >= (uintptr_t)w->doc &&
		    p < (uintptr_t)w->doc + w->doclen)
			return w;
	}
	return NULL;
}

/* remember where each line of each document ended up */
void sel_layout(Clay_RenderCommandArray *cmds)
{
	int i;

	nlines = 0;
	for (i = 0; i < app.nleaves; i++) {
		Clay_ElementData d = Clay_GetElementData(wid("body", i, 0));

		if (d.found)
			app.leaves[i]->w->body = d.boundingBox;
	}
	for (i = 0; i < cmds->length; i++) {
		Clay_RenderCommand *c = Clay_RenderCommandArray_Get(cmds, i);
		Clay_TextRenderData *t = &c->renderData.text;
		struct win *w;

		if (c->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT ||
		    !(w = owner(t->stringContents.chars)))
			continue;
		if (nlines == cap) {
			cap = cap * 2 + 256;
			lines = xrealloc(lines, cap * sizeof(*lines));
		}
		lines[nlines].w = w;
		lines[nlines].off = t->stringContents.chars - w->doc;
		lines[nlines].len = t->stringContents.length;
		lines[nlines].font = t->fontId;
		lines[nlines].size = t->fontSize;
		lines[nlines++].box = c->boundingBox;
	}
}

static float width(const struct tline *l, size_t from, size_t to)
{
	return font_width(l->font, l->size, l->w->doc + from, to - from);
}

static size_t char_len(const char *s)
{
	uint32_t cp;

	return utf8_decode(s, &cp);
}

/* where the character at off is on screen; returns its line or NULL */
static const struct tline *locate(struct win *w, size_t off,
				  Clay_BoundingBox *at)
{
	const struct tline *l = NULL, *next = NULL;
	size_t i;
	int n = 0;

	for (i = 0; i < (size_t)nlines; i++) {
		if (lines[i].w != w)
			continue;
		if (lines[i].off > off && l) {
			next = &lines[i];
			break;
		}
		l = &lines[i];
		if (l->off > off)
			break;
	}
	if (!l)
		return NULL;
	at->height = l->box.height;
	at->x = l->box.x;
	at->y = l->box.y;
	at->width = 0;
	if (off >= l->off && off < l->off + l->len) {
		at->x += width(l, l->off, off);
		at->width = width(l, off, off + char_len(w->doc + off));
	} else if (off >= l->off) {
		/* past the line: count the empty lines in between */
		for (i = l->off + l->len; i < off; i++)
			n += w->doc[i] == '\n';
		if (n)
			at->y += n * at->height;
		else
			at->x += width(l, l->off, l->off + l->len);
		if (next && at->y >= next->box.y)
			at->y = next->box.y - at->height;
	}
	if (at->width < 2)
		at->width = font_width(l->font, l->size, " ", 1);
	return l;
}

/* draw the selection behind a line of text that is about to be drawn */
void sel_highlight(Clay_RenderCommand *c)
{
	Clay_TextRenderData *t = &c->renderData.text;
	struct win *w = owner(t->stringContents.chars);
	size_t a, b, from, to;
	float x0, x1;

	if (!w || !w->view.b || !ed_selection(&w->view, &a, &b))
		return;
	from = t->stringContents.chars - w->doc;
	to = from + t->stringContents.length;
	a = a > from ? a : from;
	b = b < to ? b : to;
	if (a >= b)
		return;
	x0 = font_width(t->fontId, t->fontSize, w->doc + from, a - from);
	x1 = x0 + font_width(t->fontId, t->fontSize, w->doc + a, b - a);
	r_rect(c->boundingBox.x + x0, c->boundingBox.y, x1 - x0,
	       c->boundingBox.height, cfg.color[C_SEL_BG], 0, 0);
}

/* draw the block cursor of every window with a selection */
void sel_cursor(void)
{
	int i;

	if (app.pick || app.ctx_open || app.menu_open)
		return;
	for (i = 0; i < app.nleaves; i++) {
		struct win *w = app.leaves[i]->w;
		const struct tline *l;
		Clay_BoundingBox at;
		size_t off;

		if (!w->view.b || app.leaves[i] != app.focus ||
		    (w->type != W_MSG && w->type != W_TEXT))
			continue;
		off = w->view.cur;
		if (!(l = locate(w, off, &at)))
			continue;
		if (off >= w->hlen)
			r_scissor(w->body.x, w->body.y, w->body.width,
				  w->body.height);
		r_rect(at.x, at.y, at.width, at.height, cfg.color[C_CURSOR],
		       0, 0);
		if (off >= l->off && off < l->off + l->len)
			font_draw(l->font, l->size, at.x, at.y, w->doc + off,
				  char_len(w->doc + off), cfg.color[C_BG]);
		r_scissor(0, 0, -1, -1);
	}
}

/* scroll so that a cursor that has just moved is visible */
int sel_follow(void)
{
	int i, moved = 0;

	for (i = 0; i < app.nleaves; i++) {
		struct win *w = app.leaves[i]->w;
		Clay_BoundingBox at;
		float d = 0;

		if (!w->follow)
			continue;
		w->follow = 0;
		if (!w->view.b || !locate(w, w->view.cur, &at))
			continue;
		if (w->view.cur < w->hlen)
			d = -w->scroll;
		else if (at.y < w->body.y)
			d = at.y - w->body.y;
		else if (at.y + at.height > w->body.y + w->body.height)
			d = at.y + at.height - w->body.y - w->body.height;
		if (d == 0)
			continue;
		w->scroll += d;
		if (w->scroll > w->maxscroll)
			w->scroll = w->maxscroll;
		if (w->scroll < 0)
			w->scroll = 0;
		moved = 1;
	}
	return moved;
}

/* headers are always shown, body lines only inside the scrolled area */
static int visible(const struct tline *l)
{
	return l->off < l->w->hlen ||
	    (l->box.y + l->box.height > l->w->body.y &&
	     l->box.y < l->w->body.y + l->w->body.height);
}

/*
 * the text position under a point. While dragging past the top or bottom
 * of the body the next hidden line is picked, which scrolls to it.
 */
static int hit(struct win *w, float x, float y, int dragging, size_t *off)
{
	const struct tline *best = NULL;
	int above = dragging && w->scroll > 0 && y < w->body.y;
	int below = dragging && w->scroll < w->maxscroll &&
	    y >= w->body.y + w->body.height;
	float bd = 0, px;
	size_t p, end;
	int i;

	for (i = 0; i < nlines; i++) {
		const struct tline *l = &lines[i];
		float d;

		if (l->w != w)
			continue;
		if (above) {
			if (l->off >= w->hlen && l->box.y < w->body.y)
				best = l;
			continue;
		}
		if (below) {
			if (l->box.y + l->box.height >
			    w->body.y + w->body.height) {
				best = l;
				break;
			}
			continue;
		}
		if (!visible(l))
			continue;
		d = y < l->box.y ? l->box.y - y :
		    y >= l->box.y + l->box.height ?
		    y - l->box.y - l->box.height + 1 : 0;
		if (!best || d < bd) {
			best = l;
			bd = d;
		}
	}
	if (!best)
		return 0;
	px = best->box.x;
	end = best->off + best->len;
	for (p = best->off; p < end; p += char_len(w->doc + p)) {
		px += width(best, p, p + char_len(w->doc + p));
		if (x < px || p + char_len(w->doc + p) >= end)
			break;
	}
	*off = p;
	return 1;
}

void sel_clear(struct win *w)
{
	if (w->view.b)
		ed_free(&w->view);
}

static void start(struct win *w, size_t anchor, size_t cur, int mode)
{
	sel_clear(w);
	ed_init(&w->view, w->doc, 0);
	w->view.readonly = 1;
	w->view.mode = mode;
	w->view.vstart = anchor;
	w->view.cur = cur;
	app.dirty = 1;
}

/* enter visual mode at the first line that is visible in the body */
void sel_begin(struct win *w, int mode)
{
	size_t off = w->hlen && w->doclen > w->hlen + 2 ? w->hlen + 2 : 0;
	int i;

	if (!w->doclen)
		return;
	for (i = 0; i < nlines; i++)
		if (lines[i].w == w && lines[i].off >= w->hlen &&
		    lines[i].box.y >= w->body.y - 1) {
			off = lines[i].off;
			break;
		}
	start(w, off, off, mode);
	w->view.want = ed_col(&w->view, off);
	w->follow = 1;
}

void sel_all(struct win *w)
{
	size_t end = w->doclen;

	while (end && w->doc[end - 1] == '\n')
		end--;
	if (!end)
		return;
	for (end--; end && (w->doc[end] & 0xc0) == 0x80; end--)
		;
	start(w, 0, end, ED_VISUAL);
}

/* the selected text of a message window, or NULL */
char *sel_text(struct win *w)
{
	return w->view.b ? ed_selected_text(&w->view) : NULL;
}

/* send a key that moves the cursor to the read-only editor */
static void move_key(struct win *w, const char *tok, const char *text)
{
	const char *msg;
	int error;

	ed_key(&w->view, tok, text, 1);
	if ((msg = vim_message(&error)))
		ui_status(error, "%s", msg);
	if (w->view.mode != ED_VISUAL && w->view.mode != ED_VLINE &&
	    w->view.mode != ED_VBLOCK)
		sel_clear(w);
	w->follow = 1;
}

/*
 * handle a key while text is selected: vim motions and text objects move
 * the selection, y copies. The editor is read only, so the text cannot
 * change. Returns 0 for keys that keep their usual meaning.
 */
int sel_key(struct win *w, const char *tok, const char *text)
{
	static const char *motions[] = {
		"h", "j", "k", "l", "w", "b", "e", "W", "B", "E", "0", "^",
		"$", "G", "g", "{", "}", "(", ")", "%", "f", "F", "t", "T",
		";", ",", "i", "a", "/", "?", "n", "N", "*", "#", "'", "`",
		"H", "M", "L", "+", "-", "_", "o", "<Left>", "<Right>",
		"<Up>", "<Down>", "<Home>", "<End>", "1", "2", "3", "4", "5",
		"6", "7", "8", "9", NULL
	};
	struct editor *e = &w->view;
	int i;

	if (w->type != W_MSG && w->type != W_TEXT)
		return 0;
	/* the rest of a command such as "fx" or "/text" */
	if (ed_pending(e)) {
		move_key(w, tok, text);
		return 1;
	}
	if (!strcmp(tok, "<Esc>") || !strcmp(tok, "q")) {
		sel_clear(w);
		return 1;
	}
	if (!strcmp(tok, "y")) {
		ui_command("copy");
		sel_clear(w);
		return 1;
	}
	if (!strcmp(tok, "v") || !strcmp(tok, "V")) {
		move_key(w, tok, text);
		return 1;
	}
	for (i = 0; motions[i]; i++)
		if (!strcmp(tok, motions[i])) {
			move_key(w, tok, text);
			return 1;
		}
	return 0;
}

/* a button press in a message window: drop the selection, arm a drag */
void sel_press(struct win *w, float x, float y)
{
	drag = NULL;
	if (!w || (w->type != W_MSG && w->type != W_TEXT))
		return;
	sel_clear(w);
	if (hit(w, x, y, 0, &drag_off))
		drag = w;
}

void sel_drag(float x, float y)
{
	size_t off;
	int i;

	if (!drag)
		return;
	/* the window may have been closed from the keyboard meanwhile */
	for (i = 0; i < app.nleaves; i++)
		if (app.leaves[i]->w == drag)
			break;
	if (i == app.nleaves || !hit(drag, x, y, 1, &off)) {
		drag = NULL;
		return;
	}
	if (!drag->view.b) {
		if (off == drag_off)
			return;
		start(drag, drag_off, off, ED_VISUAL);
	}
	drag->view.cur = off;
	drag->follow = 1;
	app.dirty = 1;
}

void sel_release(void)
{
	drag = NULL;
}
