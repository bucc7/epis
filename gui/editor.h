/* editor.h - gap buffer text editor with vim emulation */
#ifndef EDITOR_H
#define EDITOR_H

#include "epis.h"

enum {
	ED_NORMAL, ED_INSERT, ED_VISUAL, ED_VLINE, ED_VBLOCK, ED_REPLACE
};

/*
 * results of ed_key: ED_FAIL is a command that could not run, which stops
 * a macro; ED_EX leaves a command for the application in vim_command().
 */
enum { ED_UNHANDLED, ED_OK, ED_ESCAPE, ED_ENTER, ED_FAIL, ED_EX };

#define KEYLEN 16
#define NMARKS 29		/* a-z, '<, '> and the jump mark '' */
#define NOMARK ((size_t)-1)

/* a list of key tokens in binding notation, such as "d" or "<Esc>" */
struct keys {
	char (*k)[KEYLEN];
	int n, cap;
};

/* one reversible splice; changes with equal group are undone together */
struct change {
	/* cur is where the cursor was when the change was made */
	size_t pos, ndel, nins, cur;
	char *del, *ins;
	unsigned group;
};

struct editor {
	char *b;
	size_t cap, gs, ge;
	size_t cur, vstart;
	/* the column that vertical motions try to keep */
	size_t want;
	int mode, single, readonly;
	/* set by ed_select_all: the next typed text replaces everything */
	int typeover;
	/* viewport kept by the view: first line, rows shown, wrap columns */
	int top, vis, cols;
	struct keys pend;
	struct change *log;
	int nlog, logcap, applied;
	unsigned group, seq;
	size_t mark[NMARKS];
	int lastvis;
	/* the insert session: how it started and what was typed */
	int icount, iopen, ctrl_o;
	struct keys typed;
	size_t istart;
	struct buf over;	/* text overwritten in replace mode */
	/* a block insert to repeat on the other lines when it ends */
	int bi, bi_first, bi_last, bi_append, bi_eol;
	size_t bi_col, bi_pos, bi_home;
};

/* buffer (edit.c) */
void ed_init(struct editor *e, const char *text, int single);
void ed_free(struct editor *e);
void ed_set(struct editor *e, const char *text);
size_t ed_len(struct editor *e);
char ed_at(struct editor *e, size_t i);
char *ed_text(struct editor *e);
void ed_copy(struct editor *e, size_t a, size_t b, char *out);
char *ed_slice(struct editor *e, size_t a, size_t b);
void ed_splice(struct editor *e, size_t pos, size_t ndel, const char *s,
	       size_t n);
void ed_insert(struct editor *e, const char *s, size_t n);
void ed_remove(struct editor *e, size_t a, size_t b);
void ed_checkpoint(struct editor *e);
int ed_undo(struct editor *e);
int ed_redo(struct editor *e);
size_t ed_next(struct editor *e, size_t p);
size_t ed_prev(struct editor *e, size_t p);
size_t ed_line_start(struct editor *e, size_t p);
size_t ed_line_end(struct editor *e, size_t p);
int ed_line_of(struct editor *e, size_t p);
int ed_lines(struct editor *e);
size_t ed_line_pos(struct editor *e, int line);
size_t ed_col(struct editor *e, size_t p);
size_t ed_at_col(struct editor *e, size_t ls, size_t col);
size_t ed_first_nonblank(struct editor *e, size_t p);
size_t ed_wrap(struct editor *e, size_t p, size_t end, int cols);

/* modal editing (vim.c) */
int ed_key(struct editor *e, const char *key, const char *text, int vim);
int ed_pending(struct editor *e);
const char *ed_showcmd(struct editor *e);
const char *ed_prompt(struct editor *e);
int ed_selection(struct editor *e, size_t *a, size_t *b);
int ed_line_selection(struct editor *e, size_t ls, size_t le, size_t *a,
		      size_t *b);
char *ed_selected_text(struct editor *e);
void ed_cut_selection(struct editor *e);
void ed_paste(struct editor *e, const char *s);
void ed_select_all(struct editor *e);
void ed_scroll(struct editor *e, int lines);
int ed_command(struct editor *e, const char *line);
char *ed_clipboard(void);

/* macros: keys are recorded and replayed by whoever dispatches them */
int vim_recording(void);
void vim_record(const char *key);
void vim_stop_recording(void);
int vim_next(char *key);
void vim_abort(void);
const char *vim_command(void);
const char *vim_message(int *error);
void vim_reset(void);

/* search and ex commands (ex.c) */
int ed_matches(struct editor *e, size_t ls, size_t le, size_t (*out)[2],
	       int max);

/* implemented by the window backend */
void plat_clip_set(const char *s);
char *plat_clip_get(void);

#endif
