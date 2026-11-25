/* undo.c - undo for deleting, moving and flagging messages */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gui.h"

#define MAXUNDO 100

/* one message: its path before and after an action */
struct step {
	char *before, *after;
};

struct action {
	/* the flag that was changed, or 0 for a move */
	int flag;
	struct step *step;
	int n;
};

static struct action stack[MAXUNDO], now;
static int depth;

static void action_free(struct action *a)
{
	int i;

	for (i = 0; i < a->n; i++) {
		free(a->step[i].before);
		free(a->step[i].after);
	}
	free(a->step);
	memset(a, 0, sizeof(*a));
}

/* record that a message went from one path to another */
void undo_add(const char *before, const char *after, int flag)
{
	if (!strcmp(before, after))
		return;
	now.flag = flag;
	now.step = xrealloc(now.step, (now.n + 1) * sizeof(*now.step));
	now.step[now.n].before = xstrdup(before);
	now.step[now.n++].after = xstrdup(after);
}

/* close the action made of the steps added since the last one */
void undo_commit(void)
{
	if (!now.n)
		return;
	if (depth == MAXUNDO) {
		action_free(&stack[0]);
		memmove(stack, stack + 1, (MAXUNDO - 1) * sizeof(*stack));
		depth--;
	}
	stack[depth++] = now;
	memset(&now, 0, sizeof(now));
}

/* where a message is now, even if its flags have changed since */
static char *locate(const char *path)
{
	char *id, *found;

	if (!access(path, F_OK))
		return xstrdup(path);
	id = maildir_id(path);
	found = maildir_resolve(id);
	free(id);
	return found;
}

/* put one message back; returns its new path or NULL */
static char *revert(const struct step *s, int flag, const char *cur)
{
	int f;

	if (flag) {
		/* only the changed flag goes back, others are kept */
		f = (maildir_flags_parse(cur) & ~flag) |
		    (maildir_flags_parse(s->before) & flag);
		return maildir_set_flags(cur, f);
	}
	/*
	 * the old name also brings back the UID, so a sync that has not
	 * run yet sees no change at all. Never overwrite a file.
	 */
	if (!access(s->before, F_OK) || rename(cur, s->before))
		return NULL;
	return xstrdup(s->before);
}

/*
 * undo the latest action. moved is told about each file that changed
 * name. Returns 0 when there is nothing to undo, else counts the messages
 * restored and those that could not be, and sets *first to the path of
 * one restored message, which the caller frees.
 */
int undo_last(void (*moved)(const char *from, const char *to), int *done,
	      int *failed, char **first)
{
	struct action a;
	int i;

	*done = *failed = 0;
	*first = NULL;
	if (!depth)
		return 0;
	a = stack[--depth];
	for (i = a.n - 1; i >= 0; i--) {
		char *cur = locate(a.step[i].after);
		char *back = cur ? revert(&a.step[i], a.flag, cur) : NULL;

		if (back) {
			(*done)++;
			moved(cur, back);
			free(*first);
			*first = back;
		} else {
			(*failed)++;
		}
		free(cur);
	}
	action_free(&a);
	return 1;
}
