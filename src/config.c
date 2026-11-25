/* config.c - configuration file parsing */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "epis.h"

struct config cfg;

const char *color_names[C_COUNT] = {
	"bg", "fg", "panel", "border", "accent", "select_bg", "select_fg",
	"unread", "dim", "menu_bg", "menu_fg", "status_bg", "status_fg",
	"error", "flagged", "quote", "cursor", "mode_normal", "mode_insert",
	"mode_visual", "mode_vline", "mode_command", "mode_find",
	"mode_setup"
};

static const uint32_t default_colors[C_COUNT] = {
	0x1e1e2eff, 0xcdd6f4ff, 0x181825ff, 0x313244ff, 0x89b4faff,
	0x45475aff, 0xf5e0dcff, 0xf9e2afff, 0x7f849cff, 0x11111bff,
	0xcdd6f4ff, 0x313244ff, 0xcdd6f4ff, 0xf38ba8ff, 0xfab387ff,
	0xa6e3a1ff, 0xf5e0dcff,
	/* modes; 0 follows another colour, see color_fallbacks() */
	0, 0xa6e3a1ff, 0xcba6f7ff, 0, 0xfab387ff, 0x94e2d5ff, 0
};

static const char *auth_names[] = {
	"auto", "plain", "login", "cram-md5", "scram-sha-1", "scram-sha-256",
	"xoauth2", "oauthbearer", "none"
};

static const char *sec_names[] = { "tls", "starttls", "none" };

int auth_from_name(const char *s)
{
	size_t i;

	for (i = 0; i < sizeof(auth_names) / sizeof(*auth_names); i++)
		if (!strcasecmp(s, auth_names[i]))
			return i;
	return -1;
}

const char *auth_name(int a)
{
	return auth_names[a];
}

int sec_from_name(const char *s)
{
	if (!strcasecmp(s, "ssl"))
		return SEC_TLS;
	if (!strcasecmp(s, "tls"))
		return SEC_TLS;
	if (!strcasecmp(s, "starttls"))
		return SEC_STARTTLS;
	if (!strcasecmp(s, "none") || !strcasecmp(s, "plain"))
		return SEC_NONE;
	return -1;
}

const char *sec_name(int s)
{
	return sec_names[s];
}

/* parse #rrggbb or #rrggbbaa */
int parse_color(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (*s == '#')
		s++;
	v = strtoul(s, &end, 16);
	if (*end || (end - s != 6 && end - s != 8))
		return -1;
	*out = end - s == 6 ? (uint32_t)(v << 8 | 0xff) : (uint32_t)v;
	return 0;
}

static int boolval(const char *v)
{
	return !strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
	    !strcmp(v, "1") || !strcasecmp(v, "on");
}

char *config_default_path(void)
{
	char *d = config_dir(), *p = xasprintf("%s/config", d);

	free(d);
	return p;
}

int config_exists(void)
{
	char *p = cfg.path ? xstrdup(cfg.path) : config_default_path();
	int r = access(p, R_OK) == 0;

	free(p);
	return r;
}

static void set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = xstrdup(v);
}

static void account_defaults(struct account *a, const char *name)
{
	memset(a, 0, sizeof(*a));
	a->name = xstrdup(name);
	a->imap_port = 993;
	a->smtp_port = 465;
	a->sent = xstrdup("Sent");
	a->drafts = xstrdup("Drafts");
	a->trash = xstrdup("Trash");
	a->archive = xstrdup("Archive");
	a->save_sent = 1;
}

static void account_set(struct account *a, const char *k, const char *v,
			const char *file, int line)
{
	if (!strcmp(k, "email"))
		set_str(&a->email, v);
	else if (!strcmp(k, "name"))
		set_str(&a->realname, v);
	else if (!strcmp(k, "imap_host"))
		set_str(&a->imap_host, v);
	else if (!strcmp(k, "imap_port"))
		a->imap_port = atoi(v);
	else if (!strcmp(k, "imap_security"))
		a->imap_sec = sec_from_name(v);
	else if (!strcmp(k, "smtp_host"))
		set_str(&a->smtp_host, v);
	else if (!strcmp(k, "smtp_port"))
		a->smtp_port = atoi(v);
	else if (!strcmp(k, "smtp_security"))
		a->smtp_sec = sec_from_name(v);
	else if (!strcmp(k, "user"))
		set_str(&a->user, v);
	else if (!strcmp(k, "password"))
		set_str(&a->pass, v);
	else if (!strcmp(k, "password_cmd"))
		set_str(&a->pass_cmd, v);
	else if (!strcmp(k, "auth"))
		a->auth = auth_from_name(v);
	else if (!strcmp(k, "oauth"))
		set_str(&a->oauth, v);
	else if (!strcmp(k, "oauth_client_id"))
		set_str(&a->client_id, v);
	else if (!strcmp(k, "oauth_client_secret"))
		set_str(&a->client_secret, v);
	else if (!strcmp(k, "oauth_tenant"))
		set_str(&a->tenant, v);
	else if (!strcmp(k, "sent"))
		set_str(&a->sent, v);
	else if (!strcmp(k, "drafts"))
		set_str(&a->drafts, v);
	else if (!strcmp(k, "trash"))
		set_str(&a->trash, v);
	else if (!strcmp(k, "archive"))
		set_str(&a->archive, v);
	else if (!strcmp(k, "save_sent"))
		a->save_sent = boolval(v);
	else if (!strcmp(k, "signature"))
		set_str(&a->signature, v);
	else
		warnx("%s:%d: unknown account option '%s'", file, line, k);
	if (a->auth < 0 || a->imap_sec < 0 || a->smtp_sec < 0) {
		warnx("%s:%d: invalid value '%s'", file, line, v);
		a->auth = a->auth < 0 ? AUTH_AUTO : a->auth;
		a->imap_sec = a->imap_sec < 0 ? SEC_TLS : a->imap_sec;
		a->smtp_sec = a->smtp_sec < 0 ? SEC_TLS : a->smtp_sec;
	}
}

static void general_set(const char *k, const char *v, const char *file,
			int line)
{
	if (!strcmp(k, "maildir")) {
		free(cfg.maildir);
		cfg.maildir = expand_home(v);
	} else if (!strcmp(k, "font")) {
		free(cfg.font);
		cfg.font = expand_home(v);
	} else if (!strcmp(k, "font_mono")) {
		free(cfg.font_mono);
		cfg.font_mono = expand_home(v);
	} else if (!strcmp(k, "font_size")) {
		cfg.font_size = atoi(v);
	} else if (!strcmp(k, "scale")) {
		cfg.scale = atof(v);
	} else if (!strcmp(k, "vim_edit")) {
		cfg.vim_edit = boolval(v);
	} else if (!strcmp(k, "menubar")) {
		cfg.menubar = boolval(v);
	} else if (!strcmp(k, "clipboard")) {
		cfg.clipboard = boolval(v);
	} else if (!strcmp(k, "leader")) {
		set_str(&cfg.leader, v);
	} else if (!strcmp(k, "editor")) {
		set_str(&cfg.editor_cmd, v);
	} else {
		warnx("%s:%d: unknown option '%s'", file, line, k);
	}
}

static void theme_set(const char *k, const char *v, const char *file,
		      int line);

static int parse_file(const char *path, int theme_only);

static void theme_set(const char *k, const char *v, const char *file, int line)
{
	int i;

	if (!strcmp(k, "include")) {
		char *p = expand_home(v);

		if (parse_file(p, 1))
			warnx("%s:%d: cannot read theme '%s'", file, line, p);
		free(p);
		return;
	}
	if (!strcmp(k, "radius")) {
		cfg.radius = atoi(v);
		return;
	}
	if (!strcmp(k, "padding")) {
		cfg.padding = atoi(v);
		return;
	}
	for (i = 0; i < C_COUNT; i++)
		if (!strcmp(k, color_names[i])) {
			if (parse_color(v, &cfg.color[i]))
				warnx("%s:%d: bad color '%s'", file, line, v);
			return;
		}
	warnx("%s:%d: unknown theme key '%s'", file, line, k);
}

static void llm_set(const char *k, const char *v, const char *file, int line)
{
	if (!strcmp(k, "url"))
		set_str(&cfg.llm_url, v);
	else if (!strcmp(k, "api_key"))
		set_str(&cfg.llm_key, v);
	else if (!strcmp(k, "api_key_cmd"))
		set_str(&cfg.llm_key_cmd, v);
	else if (!strcmp(k, "model"))
		set_str(&cfg.llm_model, v);
	else if (!strcmp(k, "max_tokens"))
		cfg.llm_max_tokens = atoi(v);
	else
		warnx("%s:%d: unknown llm option '%s'", file, line, k);
}

/*
 * parse one configuration file. Sections are [general], [account NAME],
 * [theme], [keys], [commands] and [llm]; theme files only hold theme keys.
 */
static int parse_file(const char *path, int theme_only)
{
	char *data = read_file(path, NULL), *line, *next, *sect;
	struct account *acc = NULL;
	int ln = 0;

	if (!data)
		return -1;
	sect = xstrdup(theme_only ? "theme" : "general");
	for (line = data; line; line = next) {
		char *k, *v, *eq;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		ln++;
		line = trim(line);
		if (!*line || *line == '#' || *line == ';')
			continue;
		if (*line == '[') {
			char *e = strchr(line, ']');

			if (!e) {
				warnx("%s:%d: bad section", path, ln);
				continue;
			}
			*e = 0;
			free(sect);
			sect = xstrdup(trim(line + 1));
			acc = NULL;
			if (startswith(sect, "account ") && !theme_only) {
				cfg.acc = xrealloc(cfg.acc, (cfg.nacc + 1) *
						   sizeof(*cfg.acc));
				acc = &cfg.acc[cfg.nacc++];
				account_defaults(acc, trim(sect + 8));
			}
			continue;
		}
		eq = strchr(line, '=');
		if (!eq) {
			warnx("%s:%d: expected key = value", path, ln);
			continue;
		}
		*eq = 0;
		k = trim(line);
		v = trim(eq + 1);
		if (theme_only && strcmp(sect, "theme"))
			continue;
		if (acc) {
			account_set(acc, k, v, path, ln);
		} else if (!strcmp(sect, "general")) {
			general_set(k, v, path, ln);
		} else if (!strcmp(sect, "theme")) {
			theme_set(k, v, path, ln);
		} else if (!strcmp(sect, "llm")) {
			llm_set(k, v, path, ln);
		} else if (!strcmp(sect, "keys")) {
			char *sp = strchr(k, ' ');
			struct keybind *kb;

			cfg.keys = xrealloc(cfg.keys, (cfg.nkeys + 1) *
					    sizeof(*cfg.keys));
			kb = &cfg.keys[cfg.nkeys++];
			if (sp) {
				*sp = 0;
				kb->ctx = xstrdup(k);
				kb->keys = xstrdup(trim(sp + 1));
			} else {
				kb->ctx = xstrdup("global");
				kb->keys = xstrdup(k);
			}
			kb->cmd = xstrdup(v);
		} else if (!strcmp(sect, "commands")) {
			cfg.alias = xrealloc(cfg.alias, (cfg.nalias + 1) *
					     sizeof(*cfg.alias));
			cfg.alias[cfg.nalias].name = xstrdup(k);
			cfg.alias[cfg.nalias++].cmd = xstrdup(v);
		} else {
			warnx("%s:%d: unknown section [%s]", path, ln, sect);
		}
	}
	free(sect);
	free(data);
	return 0;
}

/* give the colours that were left unset the one they follow */
static void color_fallbacks(void)
{
	static const int follow[][2] = {
		{ C_MODE_NORMAL, C_ACCENT }, { C_MODE_VLINE, C_MODE_VISUAL },
		{ C_MODE_SETUP, C_ACCENT }
	};
	size_t i;

	for (i = 0; i < sizeof(follow) / sizeof(*follow); i++)
		if (!cfg.color[follow[i][0]])
			cfg.color[follow[i][0]] = cfg.color[follow[i][1]];
}

/* load defaults and then the configuration file at path */
int config_load(const char *path)
{
	int i;

	cfg.path = path ? xstrdup(path) : config_default_path();
	cfg.maildir = expand_home("~/Mail");
	cfg.font_size = 15;
	cfg.radius = 6;
	cfg.padding = 8;
	cfg.vim_edit = 1;
	cfg.menubar = 1;
	cfg.clipboard = 1;
	cfg.leader = xstrdup("<Space>");
	cfg.llm_url = xstrdup("https://api.anthropic.com/v1/messages");
	cfg.llm_model = xstrdup("claude-opus-5-5");
	cfg.llm_max_tokens = 16000;
	for (i = 0; i < C_COUNT; i++)
		cfg.color[i] = default_colors[i];
	i = parse_file(cfg.path, 0);
	color_fallbacks();
	if (i)
		return -1;
	for (i = 0; i < cfg.nacc; i++) {
		struct account *a = &cfg.acc[i];

		if (!a->user && a->email)
			a->user = xstrdup(a->email);
		if (!a->email)
			warnx("account %s: missing email", a->name);
	}
	return 0;
}

struct account *account_find(const char *name)
{
	int i;

	if (!name)
		return cfg.nacc ? &cfg.acc[0] : NULL;
	for (i = 0; i < cfg.nacc; i++)
		if (!strcmp(cfg.acc[i].name, name) ||
		    (cfg.acc[i].email && !strcasecmp(cfg.acc[i].email, name)))
			return &cfg.acc[i];
	return NULL;
}

/* resolve the account password, running password_cmd once if needed */
const char *account_password(struct account *a)
{
	if (!a->pass && a->pass_cmd) {
		a->pass = run_cmd(a->pass_cmd);
		if (!a->pass)
			warnx("%s: password_cmd failed", a->name);
	}
	return a->pass;
}

/* append an account section to the config file, creating it if needed */
int config_write_account(const struct account *a, const char *maildir)
{
	struct buf b = { 0 };
	char *dir = config_dir();
	char *old;
	int r;

	mkdirs(dir, 0700);
	free(dir);
	old = read_file(cfg.path, NULL);
	if (old) {
		buf_puts(&b, old);
		free(old);
	} else {
		buf_printf(&b, "# epis configuration\n\n[general]\n"
			   "maildir = %s\nleader = <Space>\nvim_edit = yes\n"
			   "font_size = 15\n", maildir);
	}
	buf_printf(&b, "\n[account %s]\nemail = %s\n", a->name, a->email);
	if (a->realname)
		buf_printf(&b, "name = %s\n", a->realname);
	buf_printf(&b, "imap_host = %s\nimap_port = %d\nimap_security = %s\n"
		   "smtp_host = %s\nsmtp_port = %d\nsmtp_security = %s\n"
		   "user = %s\nauth = %s\n", a->imap_host, a->imap_port,
		   sec_name(a->imap_sec), a->smtp_host, a->smtp_port,
		   sec_name(a->smtp_sec), a->user ? a->user : a->email,
		   auth_name(a->auth));
	if (a->pass_cmd)
		buf_printf(&b, "password_cmd = %s\n", a->pass_cmd);
	else if (a->pass)
		buf_printf(&b, "password = %s\n", a->pass);
	if (a->oauth)
		buf_printf(&b, "oauth = %s\n", a->oauth);
	if (a->client_id)
		buf_printf(&b, "oauth_client_id = %s\n", a->client_id);
	if (a->client_secret)
		buf_printf(&b, "oauth_client_secret = %s\n", a->client_secret);
	r = write_file(cfg.path, b.p, b.len, 0600);
	buf_free(&b);
	return r;
}
