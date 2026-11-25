/* fuzzy.c - subsequence fuzzy matching */
#include <ctype.h>
#include <string.h>

#include "epis.h"

/*
 * score str against the pattern: -1 when pat is not a case-insensitive
 * subsequence of str, otherwise higher for contiguous runs, word starts
 * and early matches.
 */
int fuzzy_score(const char *pat, const char *str)
{
	int score = 0, run = 0, pos = 0;
	const char *s = str;

	if (!*pat)
		return 0;
	for (; *pat; pat++) {
		int found = 0;

		if (*pat == ' ')
			continue;
		for (; *s; s++, pos++) {
			if (tolower((unsigned char)*s) ==
			    tolower((unsigned char)*pat)) {
				int start = s == str || !isalnum(
					(unsigned char)s[-1]);

				run = run ? run + 1 : 1;
				score += 10 + run * 5 + (start ? 15 : 0) -
				    (pos > 50 ? 5 : pos / 10);
				s++;
				pos++;
				found = 1;
				break;
			}
			run = 0;
		}
		if (!found)
			return -1;
	}
	return score;
}
