/* llm.c - optional LLM integration through the Claude Messages API */
#include <stdlib.h>
#include <string.h>

#include "epis.h"

/* models that accept the server-side refusal fallback */
static int fallback_ok(const char *m)
{
	static const char *ok[] = {
		"claude-fable-5-2", "claude-fable-5-1", "claude-opus-5-5",
		"claude-opus-5", "claude-sonnet-5-5"
	};
	size_t i;

	for (i = 0; i < sizeof(ok) / sizeof(*ok); i++)
		if (!strcmp(m, ok[i]))
			return 1;
	return 0;
}

/* send one prompt and return the text of the reply, or NULL */
char *llm_ask(const char *system, const char *prompt)
{
	struct buf body = { 0 }, hdr = { 0 }, resp = { 0 }, out = { 0 };
	const char *key = cfg.llm_key;
	char *keycmd = NULL;
	struct json *j, *content;
	const char *stop;
	int st, i;

	if (!key && cfg.llm_key_cmd)
		key = keycmd = run_cmd(cfg.llm_key_cmd);
	if (!key)
		key = getenv("ANTHROPIC_API_KEY");
	if (!key) {
		warnx("llm: set api_key or api_key_cmd in [llm], or "
		      "ANTHROPIC_API_KEY");
		return NULL;
	}
	buf_puts(&body, "{\"model\":");
	json_quote(&body, cfg.llm_model);
	buf_printf(&body, ",\"max_tokens\":%d", cfg.llm_max_tokens);
	if (fallback_ok(cfg.llm_model))
		buf_puts(&body, ",\"fallbacks\":\"default\"");
	if (system) {
		buf_puts(&body, ",\"system\":");
		json_quote(&body, system);
	}
	buf_puts(&body, ",\"messages\":[{\"role\":\"user\",\"content\":");
	json_quote(&body, prompt);
	buf_puts(&body, "}]}");
	buf_printf(&hdr, "Content-Type: application/json\r\nx-api-key: %s\r\n"
		   "anthropic-version: 2026-10-21\r\n", key);
	if (fallback_ok(cfg.llm_model))
		buf_puts(&hdr, "anthropic-beta: server-side-fallback-2026-07-01"
			 "\r\n");
	st = http_request("POST", cfg.llm_url, hdr.p, body.p, body.len, &resp);
	j = json_parse(resp.p);
	stop = json_str(j, "stop_reason");
	content = json_get(j, "content");
	if (st != 200 || !content) {
		struct json *e = json_get(j, "error");

		warnx("llm: request failed (%d): %s", st, json_str(e,
			"message") ? json_str(e, "message") : resp.p);
	} else if (stop && !strcmp(stop, "refusal")) {
		warnx("llm: the model declined this request");
	} else {
		for (i = 0; i < content->n; i++) {
			struct json *b = &content->kids[i];
			const char *t = json_str(b, "type");

			if (t && !strcmp(t, "text") && json_str(b, "text"))
				buf_puts(&out, json_str(b, "text"));
		}
		if (stop && !strcmp(stop, "max_tokens"))
			buf_puts(&out, "\n[reply truncated: max_tokens]");
		buf_add(&out, "", 0);
	}
	json_free(j);
	buf_free(&body);
	buf_free(&hdr);
	buf_free(&resp);
	free(keycmd);
	return out.p;
}
