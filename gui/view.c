/* view.c - Clay layout of the interface and render command translation */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define FRAME (1 << 20)

static char frame[FRAME];
static size_t used;
static Clay_Context *clay;
static float lw, lh;

const char **wizard_label_list(void);

Clay_Color col(int c)
{
	uint32_t v = cfg.color[c];

	return (Clay_Color){ v >> 24 & 0xff, v >> 16 & 0xff, v >> 8 & 0xff,
		v & 0xff };
}

static uint32_t rgba(Clay_Color c)
{
	return (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 |
	    (uint32_t)c.a;
}

/* a Clay string referencing s, which must outlive the frame */
Clay_String cstr(const char *s)
{
	return (Clay_String){ false, (int32_t)strlen(s), s };
}

static Clay_String astr(const char *s, size_t n)
{
	char *p;

	if (used + n + 1 > FRAME)
		return cstr("");
	p = frame + used;
	memcpy(p, s, n);
	p[n] = 0;
	used += n + 1;
	return (Clay_String){ false, (int32_t)n, p };
}

/* a formatted string allocated for the current frame */
Clay_String fstr(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(frame + used, FRAME - used, fmt, ap);
	va_end(ap);
	if (n < 0 || used + n + 1 > FRAME)
		return cstr("");
	used += n + 1;
	return (Clay_String){ false, n, frame + used - n - 1 };
}

static Clay_Dimensions measure(Clay_StringSlice t, Clay_TextElementConfig *c,
			       void *ud)
{
	(void)ud;
	return (Clay_Dimensions){ font_width(c->fontId, c->fontSize, t.chars,
					     t.length),
		font_lineh(c->fontId, c->fontSize) };
}

static void clay_error(Clay_ErrorData e)
{
	warnx("layout: %.*s", (int)e.errorText.length, e.errorText.chars);
}

static Clay_TextElementConfig *txt(int font, int c, int wrap)
{
	return CLAY_TEXT_CONFIG({ .fontId = font, .fontSize = cfg.font_size,
				  .textColor = col(c), .wrapMode = wrap });
}

static float rowh(void)
{
	return font_lineh(FONT_UI, cfg.font_size) + 8;
}

Clay_ElementId wid(const char *base, int leaf, int i)
{
	return Clay__HashString(cstr(base), leaf * 100000 + i, 0);
}

/* s truncated with an ellipsis to fit in maxw logical pixels */
static Clay_String fit(const char *s, int font, float maxw)
{
	size_t n = strlen(s);
	float ell = font_width(font, cfg.font_size, "\xe2\x80\xa6", 3);
	char *t;
	Clay_String r;

	if (maxw <= 0 || font_width(font, cfg.font_size, s, n) <= maxw)
		return astr(s, n);
	while (n > 0 && font_width(font, cfg.font_size, s, n) + ell > maxw) {
		n--;
		while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
			n--;
	}
	t = xasprintf("%.*s\xe2\x80\xa6", (int)n, s);
	r = astr(t, strlen(t));
	free(t);
	return r;
}

static float elem_w(Clay_ElementId id)
{
	Clay_ElementData d = Clay_GetElementData(id);

	return d.found ? d.boundingBox.width : 0;
}

static float elem_h(Clay_ElementId id)
{
	Clay_ElementData d = Clay_GetElementData(id);

	return d.found ? d.boundingBox.height : 0;
}

/* most search matches highlighted on one screen row */
#define MATCHES 24

static int compare_pos(const void *a, const void *b)
{
	size_t x = *(const size_t *)a, y = *(const size_t *)b;

	return x < y ? -1 : x > y;
}

/*
 * render the part [rs, re) of the line [ls, le] with the cursor, the
 * selection and search matches highlighted. The last row of a line also
 * shows its end.
 */
static void ed_row(struct editor *e, size_t rs, size_t re, size_t ls,
		   size_t le, int focus, int font, int search)
{
	size_t cuts[2 * MATCHES + 6], found[MATCHES][2], sa = 0, sb = 0, a, b;
	int n = 0, nfound = 0, i, k, last = re == le;
	int sel = focus && ed_line_selection(e, ls, le, &sa, &sb);
	int bar = focus && e->mode == ED_INSERT;
	int here = focus && e->cur >= rs && (e->cur < re ||
					     (last && e->cur == re));
	char *buf = xmalloc(re - rs + 1);

	if (search)
		nfound = ed_matches(e, ls, le, found, MATCHES);
	cuts[n++] = rs;
	cuts[n++] = re;
	if (sel) {
		cuts[n++] = sa;
		cuts[n++] = sb;
	}
	for (i = 0; i < nfound; i++) {
		cuts[n++] = found[i][0];
		cuts[n++] = found[i][1];
	}
	if (here) {
		cuts[n++] = e->cur;
		cuts[n++] = e->cur < re ? ed_next(e, e->cur) : re;
	}
	for (i = 0; i < n; i++)
		cuts[i] = cuts[i] < rs ? rs : cuts[i] > re ? re : cuts[i];
	qsort(cuts, n, sizeof(*cuts), compare_pos);
	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_FIT(0) } } }) {
		if (rs == re && !here && !(sel && sb > le))
			CLAY_TEXT(CLAY_STRING(" "), txt(font, C_FG, 2));
		for (i = 0; i < n; i++) {
			int cursor, marked = 0, hit = 0;
			Clay_Color bg = { 0 };

			a = cuts[i];
			b = i + 1 < n ? cuts[i + 1] : a;
			if (here && bar && a == e->cur &&
			    (!i || cuts[i - 1] != a))
				CLAY({ .layout = { .sizing = {
					CLAY_SIZING_FIXED(2),
					CLAY_SIZING_FIXED(font_lineh(font,
						cfg.font_size)) } },
				       .backgroundColor = col(C_CURSOR) }) {
				}
			if (a >= b)
				continue;
			cursor = here && !bar && a == e->cur;
			marked = sel && a >= sa && a < sb;
			for (k = 0; k < nfound; k++)
				hit |= a >= found[k][0] && a < found[k][1];
			if (cursor)
				bg = col(C_CURSOR);
			else if (marked)
				bg = col(C_SEL_BG);
			else if (hit)
				bg = col(C_SEARCH);
			ed_copy(e, a, b, buf);
			CLAY({ .backgroundColor = bg }) {
				CLAY_TEXT(astr(buf, b - a),
					  txt(font, cursor || (hit && !marked) ?
					      C_BG : C_FG, 2));
			}
		}
		/* the cursor or the selected line break after the text */
		if (here && !bar && e->cur == le) {
			CLAY({ .backgroundColor = col(C_CURSOR) }) {
				CLAY_TEXT(CLAY_STRING(" "), txt(font, C_BG, 2));
			}
		} else if (last && sel && sb > le) {
			CLAY({ .backgroundColor = col(C_SEL_BG) }) {
				CLAY_TEXT(CLAY_STRING(" "), txt(font, C_FG, 2));
			}
		}
	}
	free(buf);
}

static void single_field(struct editor *e, int focus)
{
	ed_row(e, 0, ed_len(e), 0, ed_len(e), focus, FONT_UI, 0);
}

/* the screen rows a line takes, and the one the cursor is on */
static int line_rows(struct editor *e, size_t ls, int *cursor_row)
{
	size_t le = ed_line_end(e, ls), p = ls, next;
	int n = 0;

	do {
		next = ed_wrap(e, p, le, e->cols);
		if (cursor_row && e->cur >= p &&
		    (e->cur < next || next == le))
			*cursor_row = n;
		n++;
		p = next;
	} while (p < le);
	return n;
}

/*
 * keep the cursor in sight; returns how many rows of the top line are
 * scrolled away, which only happens for a line taller than the view
 */
static int scroll_editor(struct editor *e, int height)
{
	int cur = ed_line_of(e, e->cur), crow = 0, used, l;
	size_t cls = ed_line_start(e, e->cur), p;

	if (cur < e->top)
		e->top = cur;
	line_rows(e, cls, &crow);
	/* far jumps need not measure every line in between */
	if (cur - e->top > height)
		e->top = cur - height;
	used = crow + 1;
	for (l = e->top, p = ed_line_pos(e, l); l < cur; l++) {
		used += line_rows(e, p, NULL);
		p = ed_line_end(e, p) + 1;
	}
	for (p = ed_line_pos(e, e->top); used > height && e->top < cur;
	     e->top++) {
		used -= line_rows(e, p, NULL);
		p = ed_line_end(e, p) + 1;
	}
	return e->top == cur && crow >= height ? crow - height + 1 : 0;
}

/* the number shown beside a line: absolute, relative or both */
static Clay_String line_number(int line, int cursor, int width)
{
	if (!cfg.relativenumber)
		return fstr("%*d", width, line + 1);
	if (line != cursor)
		return fstr("%*d", width, abs(line - cursor));
	/* like vim, the current line number stands out to the left */
	return cfg.number ? fstr("%-*d", width, line + 1) :
	    fstr("%*d", width, 0);
}

/* the multi line editor with wrapping and optional line numbers */
static void editor_body(struct editor *e, Clay_ElementId body, int focus,
			int height)
{
	float charw = font_width(FONT_MONO, cfg.font_size, "M", 1);
	float rowheight = font_lineh(FONT_MONO, cfg.font_size);
	float inner = elem_w(body) - 4 * cfg.padding, gutter = 0;
	int numbers = cfg.number || cfg.relativenumber;
	int cursor = ed_line_of(e, e->cur), line, digits, n, skip, rows = 0;
	int shown = 0;
	size_t p;

	for (digits = 1, n = ed_lines(e); n >= 10; n /= 10)
		digits++;
	if (digits < 3)
		digits = 3;
	if (numbers)
		gutter = (digits + 1) * charw;
	e->cols = inner > 0 ? (int)((inner - gutter - 2) / charw) : 80;
	skip = scroll_editor(e, height);
	line = e->top;
	for (p = ed_line_pos(e, line); rows <= height && p <= ed_len(e);
	     line++) {
		size_t le = ed_line_end(e, p), rs = p, re;
		int first = 1;

		do {
			re = ed_wrap(e, rs, le, e->cols);
			if (skip) {
				skip--;
			} else if (rows++ <= height) {
				CLAY({ .layout = { .sizing = {
					CLAY_SIZING_GROW(0),
					CLAY_SIZING_FIXED(rowheight) } } }) {
					if (numbers)
						CLAY({ .layout = { .sizing = {
							CLAY_SIZING_FIXED(
								gutter),
							CLAY_SIZING_FIT(0) } }
						     }) {
							if (first)
								CLAY_TEXT(
								line_number(line,
									cursor,
									digits),
								txt(FONT_MONO,
								    line ==
								    cursor ?
								    C_ACCENT :
								    C_DIM, 2));
						}
					ed_row(e, rs, re, p, le, focus,
					       FONT_MONO, 1);
				}
			}
			first = 0;
			rs = re;
		} while (rs < le);
		shown += rows <= height;
		p = le + 1;
	}
	/* H, M, L and half pages work on the lines that fit */
	e->vis = rows < height ? height : shown > 0 ? shown : 1;
}

static void list_view(struct win *w, int li, int focus)
{
	Clay_ElementId rows = wid("rows", li, 0);
	float rh = rowh(), h = elem_h(rows), rw = elem_w(rows);
	float datew = font_width(FONT_UI, cfg.font_size, "00 Mmm 0000", 11);
	int i, unread = 0;

	w->vis = h > 0 ? (int)(h / rh) : 20;
	if (w->vis < 1)
		w->vis = 1;
	if (w->sel < w->top)
		w->top = w->sel;
	if (w->sel >= w->top + w->vis)
		w->top = w->sel - w->vis + 1;
	if (w->top > w->nmsgs - w->vis)
		w->top = w->nmsgs - w->vis;
	if (w->top < 0)
		w->top = 0;
	for (i = 0; i < w->nmsgs; i++)
		unread += !(w->msgs[i].flags & F_SEEN);
	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_FIT(0) },
			   .padding = CLAY_PADDING_ALL(cfg.padding) },
	       .backgroundColor = col(C_PANEL) }) {
		CLAY_TEXT(fstr("%s / %s   %d messages, %d unread",
			       w->acc ? w->acc : "", w->folder ? w->folder : "",
			       w->nmsgs, unread), txt(FONT_BOLD, C_ACCENT, 2));
	}
	CLAY({ .id = rows, .layout = { .sizing = { CLAY_SIZING_GROW(0),
						   CLAY_SIZING_GROW(0) },
				       .layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .clip = { .vertical = true, .horizontal = true } }) {
		if (!w->nmsgs)
			CLAY({ .layout = { .padding = CLAY_PADDING_ALL(16) } }) {
				CLAY_TEXT(CLAY_STRING("No messages. Press "
						      "<leader>s to sync."),
					  txt(FONT_UI, C_DIM, 2));
			}
		for (i = w->top; i < w->nmsgs && i < w->top + w->vis; i++) {
			struct msginfo *m = &w->msgs[i];
			Clay_ElementId id = wid("row", li, i);
			int sel = i == w->sel, un = !(m->flags & F_SEEN);
			int fg = sel && focus ? C_SEL_FG : un ? C_UNREAD :
			    C_FG;
			int font = un ? FONT_BOLD : FONT_UI;
			char date[32], *from = addr_display(m->from);
			time_t now = time(NULL);

			strftime(date, sizeof(date), now - m->date < 86400 ?
				 "%H:%M" : "%d %b %Y", localtime(&m->date));
			hit_add(id, H_ROW, li, i);
			CLAY({ .id = id, .layout = {
				.sizing = { CLAY_SIZING_GROW(0),
					    CLAY_SIZING_FIXED(rh) },
				.padding = { cfg.padding, cfg.padding, 4, 4 },
				.childGap = 12,
				.childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
			       .backgroundColor = sel ? (focus ? col(C_SEL_BG)
							 : col(C_BORDER)) :
			       (Clay_Color){ 0 } }) {
				CLAY({ .layout = { .sizing = {
					CLAY_SIZING_FIXED(28),
					CLAY_SIZING_FIXED(10) },
					.childGap = 3 } }) {
					if (un)
						CLAY({ .layout = { .sizing = {
							CLAY_SIZING_FIXED(8),
							CLAY_SIZING_FIXED(8) } },
						       .backgroundColor =
						       col(C_ACCENT),
						       .cornerRadius =
						       CLAY_CORNER_RADIUS(4) }) {
						}
					if (m->flags & F_FLAGGED)
						CLAY({ .layout = { .sizing = {
							CLAY_SIZING_FIXED(8),
							CLAY_SIZING_FIXED(8) } },
						       .backgroundColor =
						       col(C_FLAGGED),
						       .cornerRadius =
						       CLAY_CORNER_RADIUS(1) }) {
						}
					if (m->flags & F_REPLIED)
						CLAY({ .layout = { .sizing = {
							CLAY_SIZING_FIXED(8),
							CLAY_SIZING_FIXED(8) } },
						       .cornerRadius =
						       CLAY_CORNER_RADIUS(4),
						       .border = { col(C_DIM),
							CLAY_BORDER_OUTSIDE(1) }
						     }) {
						}
				}
				CLAY({ .layout = { .sizing = {
					CLAY_SIZING_FIXED(200),
					CLAY_SIZING_FIT(0) } } }) {
					CLAY_TEXT(fit(from, font, 196),
						  txt(font, fg, 2));
				}
				CLAY({ .layout = { .sizing = {
					CLAY_SIZING_GROW(0),
					CLAY_SIZING_FIT(0) } } }) {
					CLAY_TEXT(fit(m->subject, font, rw -
						      datew - 28 - 200 - 36 -
						      2 * cfg.padding),
						  txt(font, fg, 2));
				}
				CLAY_TEXT(fstr("%s", date), txt(FONT_UI, sel &&
						focus ? C_SEL_FG : C_DIM, 2));
			}
			free(from);
		}
	}
}

static void text_view(struct win *w, int li)
{
	Clay_ElementId body = wid("body", li, 0), content = wid("content", li,
								0);
	float bh = elem_h(body), ch = elem_h(content);
	int i;

	w->maxscroll = ch > bh ? ch - bh : 0;
	if (w->scroll > w->maxscroll)
		w->scroll = w->maxscroll;
	w->vislines = bh;
	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_FIT(0) },
			   .padding = CLAY_PADDING_ALL(cfg.padding * 1.5),
			   .childGap = 6,
			   .layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .backgroundColor = col(C_PANEL) }) {
		if (w->title)
			CLAY_TEXT(cstr(w->title), CLAY_TEXT_CONFIG({
				.fontId = FONT_BOLD,
				.fontSize = cfg.font_size + 3,
				.textColor = col(C_ACCENT) }));
		if (w->hlen && w->type == W_MSG)
			CLAY_TEXT(((Clay_String){ false, (int32_t)w->hlen,
						  w->doc }),
				  txt(FONT_UI, C_DIM, 1));
	}
	CLAY({ .id = body, .layout = { .sizing = { CLAY_SIZING_GROW(0),
						   CLAY_SIZING_GROW(0) } },
	       .clip = { .vertical = true,
			 .childOffset = { 0, -w->scroll } } }) {
		CLAY({ .id = content, .layout = {
			.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
			.padding = CLAY_PADDING_ALL(cfg.padding * 2),
			.layoutDirection = CLAY_TOP_TO_BOTTOM } }) {
			for (i = 0; i < w->nchunks; i++) {
				CLAY_TEXT(((Clay_String){ false,
						w->chunks[i].len,
						w->chunks[i].s }),
					  txt(FONT_MONO, w->chunks[i].quote ?
					      C_QUOTE : C_FG, 0));
				if (w->chunks[i].blank)
					CLAY({ .layout = { .sizing = {
						CLAY_SIZING_GROW(0),
						CLAY_SIZING_FIXED(
						w->chunks[i].blank * font_lineh(
						FONT_MONO, cfg.font_size)) } } }) {
					}
			}
		}
	}
}

static void field_row(struct win *w, int li, int i, const char *label,
		      int focus)
{
	Clay_ElementId id = wid("field", li, i);

	hit_add(id, H_FIELD, i, li);
	CLAY({ .id = id, .layout = { .sizing = { CLAY_SIZING_GROW(0),
						 CLAY_SIZING_FIT(0) },
				     .padding = { cfg.padding, cfg.padding, 4,
					     4 },
				     .childGap = 8 },
	       .backgroundColor = focus && w->field == i ? col(C_BORDER) :
	       (Clay_Color){ 0 } }) {
		CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(80),
					       CLAY_SIZING_FIT(0) } } }) {
			CLAY_TEXT(cstr(label), txt(FONT_BOLD, C_DIM, 2));
		}
		single_field(&w->f[i], focus && w->field == i);
	}
}

static void button(int id, const char *label, int li)
{
	Clay_ElementId eid = wid("btn", li, id);

	hit_add(eid, H_BUTTON, id, li);
	CLAY({ .id = eid, .layout = { .padding = { 14, 14, 6, 6 } },
	       .backgroundColor = Clay_Hovered() ? col(C_SEL_BG) :
	       col(C_ACCENT),
	       .cornerRadius = CLAY_CORNER_RADIUS(cfg.radius) }) {
		CLAY_TEXT(cstr(label), txt(FONT_BOLD, Clay_Hovered() ? C_SEL_FG
					   : C_BG, 2));
	}
}

static void compose_view(struct win *w, int li, int focus)
{
	static const char *labels[] = { "To", "Cc", "Subject" };
	struct editor *e = &w->f[3];
	Clay_ElementId body = wid("cbody", li, 0);
	float lh1 = font_lineh(FONT_MONO, cfg.font_size);
	int i;

	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_FIT(0) },
			   .padding = CLAY_PADDING_ALL(cfg.padding),
			   .childGap = 2,
			   .layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .backgroundColor = col(C_PANEL) }) {
		CLAY_TEXT(fstr("New message from %s", w->a->email),
			  txt(FONT_BOLD, C_ACCENT, 2));
		for (i = 0; i < 3; i++)
			field_row(w, li, i, labels[i], focus);
		for (i = 0; i < w->nattach; i++)
			CLAY_TEXT(fstr("attachment: %s", w->attach[i]),
				  txt(FONT_UI, C_DIM, 2));
		CLAY({ .layout = { .childGap = 8,
				   .padding = { cfg.padding, 0, 6, 2 } } }) {
			button(2, "Send", li);
			button(3, "Save draft", li);
		}
	}
	w->vislines = (int)((elem_h(body) - 4 * cfg.padding) / lh1);
	if (w->vislines < 1)
		w->vislines = 1;
	hit_add(body, H_FIELD, 3, li);
	CLAY({ .id = body, .layout = { .sizing = { CLAY_SIZING_GROW(0),
						   CLAY_SIZING_GROW(0) },
				       .padding = CLAY_PADDING_ALL(cfg.padding *
							2),
				       .layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .clip = { .vertical = true, .horizontal = true } }) {
		editor_body(e, body, focus && w->field == 3, w->vislines);
	}
}

static void node_view(struct node *n)
{
	if (!n)
		return;
	if (n->w) {
		int li = app.nleaves, focus = n == app.focus;
		Clay_ElementId id = wid("win", li, 0);

		if (app.nleaves < 64)
			app.leaves[app.nleaves++] = n;
		hit_add(id, H_WIN, li, 0);
		CLAY({ .id = id, .layout = {
			.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
			.layoutDirection = CLAY_TOP_TO_BOTTOM },
		       .backgroundColor = col(C_BG),
		       .cornerRadius = CLAY_CORNER_RADIUS(cfg.radius),
		       .border = { focus && app.nleaves > 1 ? col(C_ACCENT) :
				   col(C_BORDER), CLAY_BORDER_OUTSIDE(1) } }) {
			switch (n->w->type) {
			case W_LIST:
				list_view(n->w, li, focus);
				break;
			case W_COMPOSE:
				compose_view(n->w, li, focus);
				break;
			default:
				text_view(n->w, li);
			}
		}
		return;
	}
	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_GROW(0) },
			   .childGap = 4,
			   .layoutDirection = n->split == 'v' ?
			   CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM } }) {
		Clay_Sizing sa, sb;

		if (n->split == 'v') {
			sa = (Clay_Sizing){ CLAY_SIZING_PERCENT(n->ratio),
				CLAY_SIZING_GROW(0) };
			sb = (Clay_Sizing){ CLAY_SIZING_PERCENT(1 - n->ratio),
				CLAY_SIZING_GROW(0) };
		} else {
			sa = (Clay_Sizing){ CLAY_SIZING_GROW(0),
				CLAY_SIZING_PERCENT(n->ratio) };
			sb = (Clay_Sizing){ CLAY_SIZING_GROW(0),
				CLAY_SIZING_PERCENT(1 - n->ratio) };
		}
		CLAY({ .layout = { .sizing = sa } }) {
			node_view(n->a);
		}
		CLAY({ .layout = { .sizing = sb } }) {
			node_view(n->b);
		}
	}
}

static void menubar(void)
{
	int i, j;

	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_FIT(0) },
			   .padding = { 4, 4, 2, 2 }, .childGap = 2 },
	       .backgroundColor = col(C_MENU_BG) }) {
		for (i = 0; menu_titles[i]; i++) {
			Clay_ElementId id = CLAY_IDI("menu", i);

			hit_add(id, H_MENU, i, 0);
			CLAY({ .id = id, .layout = { .padding = { 10, 10, 4,
								 4 } },
			       .backgroundColor = app.menu_open == i + 1 ||
			       Clay_Hovered() ? col(C_SEL_BG) :
			       (Clay_Color){ 0 },
			       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
				CLAY_TEXT(cstr(menu_titles[i]),
					  txt(FONT_UI, C_MENU_FG, 2));
				if (app.menu_open != i + 1)
					continue;
				CLAY({ .floating = {
					.attachTo = CLAY_ATTACH_TO_PARENT,
					.attachPoints = {
						CLAY_ATTACH_POINT_LEFT_TOP,
						CLAY_ATTACH_POINT_LEFT_BOTTOM },
					.zIndex = 50 },
				       .layout = { .sizing = {
						CLAY_SIZING_FIT(200),
						CLAY_SIZING_FIT(0) },
					.padding = CLAY_PADDING_ALL(4),
					.layoutDirection = CLAY_TOP_TO_BOTTOM },
				       .backgroundColor = col(C_MENU_BG),
				       .cornerRadius = CLAY_CORNER_RADIUS(
					cfg.radius),
				       .border = { col(C_BORDER),
						   CLAY_BORDER_OUTSIDE(1) } }) {
					for (j = 0; menu_items[i][j].label;
					     j++) {
						Clay_ElementId iid =
						    CLAY_IDI("mitem",
							     i * 100 + j);

						hit_add(iid, H_MENU_ITEM, i, j);
						CLAY({ .id = iid, .layout = {
							.sizing = {
							CLAY_SIZING_GROW(0),
							CLAY_SIZING_FIT(0) },
							.padding = { 10, 10, 5,
								     5 } },
						       .backgroundColor =
						       Clay_Hovered() ?
						       col(C_SEL_BG) :
						       (Clay_Color){ 0 },
						       .cornerRadius =
						       CLAY_CORNER_RADIUS(4) }) {
							CLAY_TEXT(cstr(
							menu_items[i][j].label),
							txt(FONT_UI,
							    C_MENU_FG, 2));
						}
					}
				}
			}
		}
	}
}

static void sidebar(void)
{
	struct win *w = app.focus ? app.focus->w : NULL;
	char cur[512] = "";
	int i;

	if (w && w->acc)
		snprintf(cur, sizeof(cur), "%s/%s", w->acc, w->folder);
	CLAY({ .id = CLAY_ID("sidebar"), .layout = {
		.sizing = { CLAY_SIZING_FIXED(210), CLAY_SIZING_GROW(0) },
		.padding = CLAY_PADDING_ALL(cfg.padding),
		.childGap = 2, .layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .backgroundColor = col(C_PANEL),
	       .clip = { .vertical = true } }) {
		const char *last = "";

		for (i = 0; i < app.nfolders; i++) {
			const char *f = app.folders[i];
			size_t al = strcspn(f, "/");
			Clay_ElementId id = CLAY_IDI("folder", i);

			if (strncmp(last, f, al) || last[al] != '/')
				CLAY({ .layout = { .padding = { 4, 4, 8,
								 4 } } }) {
					CLAY_TEXT(astr(f, al), txt(FONT_BOLD,
								   C_DIM, 2));
				}
			last = f;
			hit_add(id, H_FOLDER, i, 0);
			CLAY({ .id = id, .layout = {
				.sizing = { CLAY_SIZING_GROW(0),
					    CLAY_SIZING_FIT(0) },
				.padding = { 12, 8, 4, 4 } },
			       .backgroundColor = !strcmp(cur, f) ?
			       col(C_SEL_BG) : Clay_Hovered() ? col(C_BORDER) :
			       (Clay_Color){ 0 },
			       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
				CLAY_TEXT(cstr(f + al + 1), txt(FONT_UI, C_FG,
								2));
			}
		}
	}
}

static void statusbar(void)
{
	struct win *w = app.focus ? app.focus->w : NULL;
	int color;
	const char *mode = editor_ctx_mode(&color);
	struct editor *e = ui_vim_editor();
	const char *prompt = e ? ed_prompt(e) : NULL;

	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_FIT(0) },
			   .padding = { 6, 8, 3, 3 }, .childGap = 10,
			   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
	       .backgroundColor = col(C_STATUS_BG) }) {
		CLAY({ .layout = { .padding = { 8, 8, 1, 1 } },
		       .backgroundColor = col(color),
		       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
			CLAY_TEXT(cstr(mode), txt(FONT_BOLD, C_BG, 2));
		}
		if (vim_recording())
			CLAY_TEXT(fstr("recording @%c", vim_recording()),
				  txt(FONT_BOLD, C_ERROR, 2));
		if (app.keys[0])
			CLAY_TEXT(cstr(app.keys), txt(FONT_MONO, C_FLAGGED, 2));
		if (e && !prompt && *ed_showcmd(e))
			CLAY_TEXT(fstr("%s", ed_showcmd(e)),
				  txt(FONT_MONO, C_FLAGGED, 2));
		CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
					       CLAY_SIZING_FIT(0) } } }) {
			if (app.cmdline) {
				CLAY_TEXT(CLAY_STRING(":"), txt(FONT_MONO,
								C_STATUS_FG, 2));
				ed_row(&app.cmd, 0, ed_len(&app.cmd), 0,
				       ed_len(&app.cmd), 1, FONT_MONO, 0);
			} else if (prompt) {
				CLAY_TEXT(fstr("%s", prompt),
					  txt(FONT_MONO, C_STATUS_FG, 2));
				CLAY({ .layout = { .sizing = {
					CLAY_SIZING_FIXED(2),
					CLAY_SIZING_FIXED(font_lineh(FONT_MONO,
						cfg.font_size)) } },
				       .backgroundColor = col(C_CURSOR) }) {
				}
			} else {
				CLAY_TEXT(fit(app.status, FONT_UI, lw - 400),
					  txt(FONT_UI,
					app.status_err ? C_ERROR : C_STATUS_FG,
					2));
			}
		}
		if (app.syncing)
			CLAY_TEXT(CLAY_STRING("syncing..."), txt(FONT_UI,
								 C_ACCENT, 2));
		if (w && w->acc)
			CLAY_TEXT(fstr("%s/%s", w->acc, w->folder),
				  txt(FONT_UI, C_DIM, 2));
	}
}

static void picker_view(void)
{
	struct picker *pk = app.pick;
	float rh = rowh(), ph = lh * 0.6f;
	int vis = (int)((ph - rh * 2 - 24) / rh), i;

	if (vis < 3)
		vis = 3;
	if (pk->sel < pk->top)
		pk->top = pk->sel;
	if (pk->sel >= pk->top + vis)
		pk->top = pk->sel - vis + 1;
	CLAY({ .id = CLAY_ID("picker"), .floating = {
		.attachTo = CLAY_ATTACH_TO_ROOT,
		.offset = { lw * 0.2f, lh * 0.15f }, .zIndex = 100 },
	       .layout = { .sizing = { CLAY_SIZING_FIXED(lw * 0.6f),
				       CLAY_SIZING_FIT(0, ph) },
			   .padding = CLAY_PADDING_ALL(10), .childGap = 6,
			   .layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .backgroundColor = col(C_MENU_BG),
	       .cornerRadius = CLAY_CORNER_RADIUS(cfg.radius * 2),
	       .border = { col(C_ACCENT), CLAY_BORDER_OUTSIDE(1) } }) {
		CLAY_TEXT(fstr("%s  (%d/%d)", pk->title, pk->nmatch, pk->n),
			  txt(FONT_BOLD, C_ACCENT, 2));
		CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
					       CLAY_SIZING_FIT(0) },
				   .padding = { 8, 8, 5, 5 } },
		       .backgroundColor = col(C_BG),
		       .cornerRadius = CLAY_CORNER_RADIUS(cfg.radius) }) {
			CLAY_TEXT(CLAY_STRING("> "), txt(FONT_MONO, C_ACCENT,
							 2));
			single_field(&pk->input, 1);
		}
		for (i = pk->top; i < pk->nmatch && i < pk->top + vis; i++) {
			Clay_ElementId id = CLAY_IDI("pick", i);

			hit_add(id, H_PICK_ITEM, i, 0);
			CLAY({ .id = id, .layout = {
				.sizing = { CLAY_SIZING_GROW(0),
					    CLAY_SIZING_FIXED(rh) },
				.padding = { 8, 8, 4, 4 } },
			       .backgroundColor = i == pk->sel ?
			       col(C_SEL_BG) : (Clay_Color){ 0 },
			       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
				CLAY_TEXT(fit(pk->items[pk->match[i]].label,
					      FONT_UI, lw * 0.6f - 40),
					  txt(FONT_UI, i == pk->sel ? C_SEL_FG :
					      C_MENU_FG, 2));
			}
		}
	}
}

static void ctx_menu(void)
{
	int i;

	CLAY({ .floating = { .attachTo = CLAY_ATTACH_TO_ROOT,
			     .offset = { app.ctx_x, app.ctx_y },
			     .zIndex = 90 },
	       .layout = { .sizing = { CLAY_SIZING_FIT(210),
				       CLAY_SIZING_FIT(0) },
			   .padding = CLAY_PADDING_ALL(4),
			   .layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .backgroundColor = col(C_MENU_BG),
	       .cornerRadius = CLAY_CORNER_RADIUS(cfg.radius),
	       .border = { col(C_BORDER), CLAY_BORDER_OUTSIDE(1) } }) {
		for (i = 0; ctx_items[i].label; i++) {
			Clay_ElementId id = CLAY_IDI("ctx", i);

			hit_add(id, H_CTX_ITEM, i, 0);
			CLAY({ .id = id, .layout = {
				.sizing = { CLAY_SIZING_GROW(0),
					    CLAY_SIZING_FIT(0) },
				.padding = { 10, 10, 5, 5 } },
			       .backgroundColor = Clay_Hovered() ?
			       col(C_SEL_BG) : (Clay_Color){ 0 },
			       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
				CLAY_TEXT(cstr(ctx_items[i].label),
					  txt(FONT_UI, C_MENU_FG, 2));
			}
		}
	}
}

static void wizard_view(void)
{
	const char **labels = wizard_label_list();
	int i;

	CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
				       CLAY_SIZING_GROW(0) },
			   .childAlignment = { CLAY_ALIGN_X_CENTER,
					       CLAY_ALIGN_Y_CENTER } } }) {
		CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(560),
					       CLAY_SIZING_FIT(0) },
				   .padding = CLAY_PADDING_ALL(24),
				   .childGap = 6,
				   .layoutDirection = CLAY_TOP_TO_BOTTOM },
		       .backgroundColor = col(C_PANEL),
		       .cornerRadius = CLAY_CORNER_RADIUS(cfg.radius * 2),
		       .border = { col(C_BORDER), CLAY_BORDER_OUTSIDE(1) } }) {
			CLAY_TEXT(CLAY_STRING("Set up your mail account"),
				  CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD,
						     .fontSize = cfg.font_size
						     + 6,
						     .textColor = col(C_ACCENT)
						   }));
			CLAY_TEXT(CLAY_STRING("Tab moves between fields. Enter "
					      "on the email detects the server "
					      "settings."), txt(FONT_UI, C_DIM,
								0));
			for (i = 0; i < 11; i++) {
				Clay_ElementId id = CLAY_IDI("wf", i);

				hit_add(id, H_FIELD, i, 0);
				CLAY({ .id = id, .layout = {
					.sizing = { CLAY_SIZING_GROW(0),
						    CLAY_SIZING_FIT(0) },
					.padding = { 8, 8, 5, 5 },
					.childGap = 8 },
				       .backgroundColor = app.wfield == i ?
				       col(C_BORDER) : col(C_BG),
				       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
					CLAY({ .layout = { .sizing = {
						CLAY_SIZING_FIXED(140),
						CLAY_SIZING_FIT(0) } } }) {
						CLAY_TEXT(cstr(labels[i]),
							  txt(FONT_UI, C_DIM,
							      2));
					}
					if (i == 9 && app.wfield != 9) {
						char dots[64];
						size_t n = ed_len(&app.wf[9]);

						n = n > 63 ? 63 : n;
						memset(dots, '*', n);
						CLAY_TEXT(astr(dots, n),
							  txt(FONT_UI, C_FG,
							      2));
					} else {
						single_field(&app.wf[i],
							     app.wfield == i);
					}
				}
			}
			CLAY({ .layout = { .childGap = 10,
					   .padding = { 0, 0, 10, 0 } } }) {
				button(0, "Detect settings", 0);
				button(1, "Save and start", 0);
			}
		}
	}
}

/* build the whole interface for the current state */
void ui_layout(Clay_RenderCommandArray *out)
{
	int w, h, i;

	plat_size(&w, &h);
	lw = w / app.scale;
	lh = h / app.scale;
	if (!clay) {
		uint32_t sz = Clay_MinMemorySize();

		clay = Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(
			sz, xmalloc(sz)), (Clay_Dimensions){ lw, lh },
			(Clay_ErrorHandler){ clay_error, 0 });
		Clay_SetMeasureTextFunction(measure, NULL);
		/* every line is needed to place a cursor scrolled away */
		Clay_SetCullingEnabled(false);
	}
	used = 0;
	app.nhits = 0;
	app.nleaves = 0;
	Clay_SetLayoutDimensions((Clay_Dimensions){ lw, lh });
	Clay_SetPointerState((Clay_Vector2){ app.mx, app.my }, false);
	Clay_BeginLayout();
	CLAY({ .id = CLAY_ID("root"), .layout = {
		.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
		.layoutDirection = CLAY_TOP_TO_BOTTOM },
	       .backgroundColor = col(C_BG) }) {
		if (cfg.menubar && !app.wizard && !app.native_menu)
			menubar();
		CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0),
					       CLAY_SIZING_GROW(0) },
				   .padding = CLAY_PADDING_ALL(4),
				   .childGap = 4 } }) {
			if (app.wizard) {
				wizard_view();
			} else {
				if (app.sidebar)
					sidebar();
				node_view(app.root);
			}
		}
		statusbar();
		if (app.ctx_open)
			ctx_menu();
		if (app.pick)
			picker_view();
	}
	*out = Clay_EndLayout();
	for (i = 0; i < app.nhits; i++) {
		Clay_ElementData d = Clay_GetElementData(app.hits[i].id);

		app.hits[i].box = d.found ? d.boundingBox :
		    (Clay_BoundingBox){ 0 };
	}
}

/* queue the render commands of a laid out frame */
static void render(Clay_RenderCommandArray *cmds)
{
	int i;

	for (i = 0; i < cmds->length; i++) {
		Clay_RenderCommand *c = Clay_RenderCommandArray_Get(cmds, i);
		Clay_BoundingBox b = c->boundingBox;

		switch (c->commandType) {
		case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
			r_rect(b.x, b.y, b.width, b.height,
			       rgba(c->renderData.rectangle.backgroundColor),
			       c->renderData.rectangle.cornerRadius.topLeft, 0);
			break;
		case CLAY_RENDER_COMMAND_TYPE_BORDER: {
			Clay_BorderRenderData *d = &c->renderData.border;
			uint32_t bc = rgba(d->color);

			if (d->width.left == d->width.right &&
			    d->width.top == d->width.bottom &&
			    d->width.left == d->width.top) {
				r_rect(b.x, b.y, b.width, b.height, bc,
				       d->cornerRadius.topLeft, d->width.left);
				break;
			}
			r_rect(b.x, b.y, d->width.left, b.height, bc, 0, 0);
			r_rect(b.x + b.width - d->width.right, b.y,
			       d->width.right, b.height, bc, 0, 0);
			r_rect(b.x, b.y, b.width, d->width.top, bc, 0, 0);
			r_rect(b.x, b.y + b.height - d->width.bottom, b.width,
			       d->width.bottom, bc, 0, 0);
			break;
		}
		case CLAY_RENDER_COMMAND_TYPE_TEXT: {
			Clay_TextRenderData *t = &c->renderData.text;

			sel_highlight(c);
			font_draw(t->fontId, t->fontSize, b.x, b.y,
				  t->stringContents.chars,
				  t->stringContents.length, rgba(t->textColor));
			break;
		}
		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
			r_scissor(b.x, b.y, b.width, b.height);
			break;
		case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
			r_scissor(0, 0, -1, -1);
			break;
		default:
			break;
		}
	}
}

/* lay out the interface and draw it */
void draw(void)
{
	Clay_RenderCommandArray cmds;

	ui_layout(&cmds);
	sel_layout(&cmds);
	/* scroll to the selection cursor, then lay out again */
	if (sel_follow()) {
		ui_layout(&cmds);
		sel_layout(&cmds);
	}
	if (r_begin(cfg.color[C_BG]))
		return;
	font_flush();
	render(&cmds);
	/* the atlas filled up: start over with only what is visible */
	if (font_flush()) {
		scene_clear();
		render(&cmds);
	}
	sel_cursor();
	r_end();
}
