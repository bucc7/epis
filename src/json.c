/* json.c - minimal JSON parser */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "epis.h"

static const char *ws(const char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	return s;
}

static const char *parse_str(const char *s, char **out)
{
	struct buf b = { 0 };
	uint32_t cp, lo;
	char u[5];

	for (s++; *s && *s != '"'; s++) {
		if (*s != '\\') {
			buf_putc(&b, *s);
			continue;
		}
		switch (*++s) {
		case 'n':
			buf_putc(&b, '\n');
			break;
		case 't':
			buf_putc(&b, '\t');
			break;
		case 'r':
			buf_putc(&b, '\r');
			break;
		case 'b':
			buf_putc(&b, '\b');
			break;
		case 'f':
			buf_putc(&b, '\f');
			break;
		case 'u':
			if (sscanf(s + 1, "%4x", &cp) != 1)
				goto fail;
			s += 4;
			if (cp >= 0xd800 && cp < 0xdc00 && s[1] == '\\' &&
			    s[2] == 'u' && sscanf(s + 3, "%4x", &lo) == 1) {
				cp = 0x10000 + ((cp - 0xd800) << 10) +
				    (lo - 0xdc00);
				s += 6;
			}
			buf_add(&b, u, utf8_encode(u, cp));
			break;
		case 0:
			goto fail;
		default:
			buf_putc(&b, *s);
		}
	}
	if (*s != '"')
		goto fail;
	*out = buf_take(&b);
	return s + 1;
fail:
	buf_free(&b);
	return NULL;
}

static const char *parse_val(const char *s, struct json *j, int depth)
{
	char *end;

	s = ws(s);
	memset(j, 0, sizeof(*j));
	if (depth > 64)
		return NULL;
	if (*s == '{' || *s == '[') {
		int obj = *s == '{';

		j->type = obj ? J_OBJ : J_ARR;
		s = ws(s + 1);
		if (*s == (obj ? '}' : ']'))
			return s + 1;
		for (;;) {
			struct json kid;
			char *key = NULL;

			s = ws(s);
			if (obj) {
				if (*s != '"' || !(s = parse_str(s, &key)))
					return NULL;
				s = ws(s);
				if (*s++ != ':') {
					free(key);
					return NULL;
				}
			}
			s = parse_val(s, &kid, depth + 1);
			if (!s) {
				free(key);
				return NULL;
			}
			kid.key = key;
			j->kids = xrealloc(j->kids, (j->n + 1) * sizeof(kid));
			j->kids[j->n++] = kid;
			s = ws(s);
			if (*s == ',') {
				s++;
				continue;
			}
			if (*s == (obj ? '}' : ']'))
				return s + 1;
			return NULL;
		}
	}
	if (*s == '"') {
		j->type = J_STR;
		return parse_str(s, &j->str);
	}
	if (startswith(s, "true") || startswith(s, "false")) {
		j->type = J_BOOL;
		j->num = *s == 't';
		return s + (*s == 't' ? 4 : 5);
	}
	if (startswith(s, "null")) {
		j->type = J_NULL;
		return s + 4;
	}
	j->type = J_NUM;
	j->num = strtod(s, &end);
	return end == s ? NULL : end;
}

/* parse a JSON document, returns NULL on syntax errors */
struct json *json_parse(const char *s)
{
	struct json *j = xcalloc(1, sizeof(*j));

	if (!s || !parse_val(s, j, 0)) {
		json_free(j);
		return NULL;
	}
	return j;
}

static void json_clear(struct json *j)
{
	int i;

	for (i = 0; i < j->n; i++)
		json_clear(&j->kids[i]);
	free(j->kids);
	free(j->key);
	free(j->str);
}

void json_free(struct json *j)
{
	if (j) {
		json_clear(j);
		free(j);
	}
}

/* look up an object member */
struct json *json_get(struct json *j, const char *key)
{
	int i;

	if (!j || j->type != J_OBJ)
		return NULL;
	for (i = 0; i < j->n; i++)
		if (j->kids[i].key && !strcmp(j->kids[i].key, key))
			return &j->kids[i];
	return NULL;
}

/* look up a string member */
const char *json_str(struct json *j, const char *key)
{
	struct json *v = json_get(j, key);

	return v && v->type == J_STR ? v->str : NULL;
}

/* append s as a quoted JSON string */
void json_quote(struct buf *b, const char *s)
{
	buf_putc(b, '"');
	for (; s && *s; s++) {
		unsigned char c = *s;

		if (c == '"' || c == '\\')
			buf_printf(b, "\\%c", c);
		else if (c == '\n')
			buf_puts(b, "\\n");
		else if (c == '\r')
			buf_puts(b, "\\r");
		else if (c == '\t')
			buf_puts(b, "\\t");
		else if (c < 0x20)
			buf_printf(b, "\\u%04x", c);
		else
			buf_putc(b, c);
	}
	buf_putc(b, '"');
}
