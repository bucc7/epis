/* vim.h - declarations shared by the editor modules */
#ifndef VIM_H
#define VIM_H

#include "editor.h"

/* kinds of motion: what an operator does with the range */
enum { M_EXCL, M_INCL, M_LINE };
/* kinds of register contents */
enum { R_CHAR, R_LINE, R_BLOCK };

#define SHIFTWIDTH 4
#define WANT_EOL ((size_t)-1)

/* shared state of all editors, as vim shares registers and searches */
struct vimstate {
	struct buf msg, command;
	int msg_err;
	/* last search pattern and direction */
	char *pattern;
	int backward, nohl;
	char *sub_pat, *sub_rep, *sub_flags;
};

extern struct vimstate vim;

void vim_msg(int error, const char *fmt, ...);
void vim_set_register(int name, const char *s, int type, int yank);
const char *vim_register(int name, int *type);
int vim_run(struct editor *e, const char *keys);
int vim_class(struct editor *e, size_t p, int big);
void vim_clamp(struct editor *e);
void vim_jump(struct editor *e, size_t to);
void vim_hold(int on);
void vim_checkpoint(struct editor *e);

/* ex.c */
int ex_search(struct editor *e, size_t from, int backward, size_t *at);
int ex_set_pattern(const char *pat, int backward);
int ex_execute(struct editor *e, const char *line);
void ex_reflow(struct editor *e, int first, int last);

#endif
