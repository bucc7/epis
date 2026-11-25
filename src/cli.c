/* cli.c - headless command line interface */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

#include "epis.h"

static int json_out;

static void usage(void)
{
	puts("usage: epis [-c config] [-e commands] [command] [args]\n"
	     "\n"
	     "  (no command)                 start the graphical client,\n"
	     "                               running -e commands first\n"
	     "  setup                        interactive account setup\n"
	     "  accounts                     list configured accounts\n"
	     "  folders [-a acc]             list local folders\n"
	     "  sync [-a acc] [-f folder] [-q]\n"
	     "                               synchronize with IMAP\n"
	     "  list [-a acc] [-f folder] [-n N] [--unread]\n"
	     "                               list messages, newest first\n"
	     "  search [-a acc] [-f folder] [--body] QUERY\n"
	     "  read [--raw] [--peek] ID     print a message\n"
	     "  send [-a acc] --to ADDR [--cc ADDR] [--bcc ADDR]\n"
	     "       [--subject S] [--attach FILE]... [--raw]\n"
	     "                               send stdin as the body\n"
	     "  reply [--all] [--send] ID    reply with stdin above the quote\n"
	     "  forward --to ADDR [--send] ID\n"
	     "  flag ID [+|-]FLAGS           flags: S seen R replied "
	     "F flagged T trashed D draft\n"
	     "  move ID FOLDER\n"
	     "  delete ID                    move to trash\n"
	     "  attachments ID [--save DIR]\n"
	     "  oauth ACCOUNT                authorize an OAuth2 account\n"
	     "  autoconfig EMAIL             discover server settings\n"
	     "  llm summarize ID | llm draft ID INSTRUCTIONS | llm ask PROMPT\n"
	     "\n"
	     "  --json                       machine readable output\n"
	     "  IDs are printed by list and search: account/folder/name");
}

/* return the value of option name at argv[*i] and advance */
static const char *optval(int argc, char **argv, int *i, const char *name)
{
	if (strcmp(argv[*i], name))
		return NULL;
	if (*i + 1 >= argc)
		die("%s needs an argument", name);
	return argv[++*i];
}

static struct account *need_account(const char *name)
{
	struct account *a = account_find(name);

	if (!a)
		die(name ? "no account '%s'" : "no accounts configured, run "
		    "'epis setup'", name);
	return a;
}

static struct account *account_of_id(const char *id)
{
	char *n = xstrndup(id, strcspn(id, "/"));
	struct account *a = account_find(n);

	free(n);
	return a ? a : need_account(NULL);
}

static char *must_resolve(const char *id)
{
	char *p = maildir_resolve(id);

	if (!p)
		die("no message '%s'", id);
	return p;
}

static int load_msg(const char *path, struct message *m)
{
	size_t len;
	char *raw = read_file(path, &len);

	if (!raw)
		die("cannot read %s", path);
	return msg_parse(m, raw, len);
}

static char *read_stdin(void)
{
	struct buf b = { 0 };
	char tmp[4096];
	size_t n;

	if (isatty(0))
		fputs("(reading body from stdin, end with ^D)\n", stderr);
	while ((n = fread(tmp, 1, sizeof(tmp), stdin)) > 0)
		buf_add(&b, tmp, n);
	buf_add(&b, "", 0);
	return b.p;
}

static void print_msginfo(struct msginfo *m, int first)
{
	char *id = maildir_id(m->path), fl[8], date[32];
	struct buf b = { 0 };

	maildir_flags_str(m->flags, fl);
	strftime(date, sizeof(date), "%Y-%m-%d %H:%M", localtime(&m->date));
	if (!json_out) {
		printf("%s  %-4s %s  %-24.24s  %s\n", id, fl, date, m->from,
		       m->subject);
	} else {
		buf_puts(&b, first ? "  {\"id\":" : ", {\"id\":");
		json_quote(&b, id);
		buf_puts(&b, ",\"flags\":");
		json_quote(&b, fl);
		buf_printf(&b, ",\"date\":%ld,\"from\":", (long)m->date);
		json_quote(&b, m->from);
		buf_puts(&b, ",\"to\":");
		json_quote(&b, m->to);
		buf_puts(&b, ",\"subject\":");
		json_quote(&b, m->subject);
		buf_puts(&b, "}\n");
		fputs(b.p, stdout);
	}
	buf_free(&b);
	free(id);
}

/* collect the messages of one folder or every folder of the accounts */
static int gather(const char *acc, const char *folder, struct msginfo **out)
{
	struct msginfo *all = NULL;
	int n = 0, i, j, k;

	for (i = 0; i < cfg.nacc; i++) {
		char **f;
		int nf;

		if (acc && strcmp(acc, cfg.acc[i].name))
			continue;
		nf = maildir_folders(cfg.acc[i].name, &f);
		for (j = 0; j < nf; j++) {
			if (folder ? strcmp(folder, f[j]) : strcmp(f[j],
								   "INBOX")) {
				free(f[j]);
				continue;
			}
			{
				char *d = folder_dir(cfg.acc[i].name, f[j]);
				struct msginfo *m;
				int c = maildir_scan(d, &m, 1);

				all = xrealloc(all, (n + c) * sizeof(*all));
				for (k = 0; k < c; k++)
					all[n++] = m[k];
				free(m);
				free(d);
			}
			free(f[j]);
		}
		free(f);
	}
	sort_by_date(all, n);
	*out = all;
	return n;
}

static int cmd_list(int argc, char **argv, int search)
{
	const char *acc = NULL, *folder = NULL, *q = NULL, *v;
	struct msginfo *m;
	int i, n, limit = search ? 1000 : 50, unread = 0, body = 0, shown = 0;

	for (i = 1; i < argc; i++) {
		if ((v = optval(argc, argv, &i, "-a")))
			acc = v;
		else if ((v = optval(argc, argv, &i, "-f")))
			folder = v;
		else if ((v = optval(argc, argv, &i, "-n")))
			limit = atoi(v);
		else if (!strcmp(argv[i], "--unread"))
			unread = 1;
		else if (!strcmp(argv[i], "--body"))
			body = 1;
		else if (search && !q)
			q = argv[i];
		else
			die("unexpected argument '%s'", argv[i]);
	}
	if (search && !q)
		die("search needs a query");
	n = gather(acc, folder, &m);
	if (json_out)
		puts("[");
	for (i = 0; i < n && shown < limit; i++) {
		if (unread && (m[i].flags & F_SEEN))
			continue;
		if (q && !strcasestr_(m[i].subject, q) &&
		    !strcasestr_(m[i].from, q) && !strcasestr_(m[i].to, q)) {
			struct message msg;
			char *t;
			int hit;

			if (!body)
				continue;
			load_msg(m[i].path, &msg);
			t = msg_text(&msg);
			hit = strcasestr_(t, q) != NULL;
			free(t);
			msg_free(&msg);
			if (!hit)
				continue;
		}
		print_msginfo(&m[i], !shown++);
	}
	if (json_out)
		puts("]");
	msginfo_free(m, n);
	return 0;
}

static void set_flag(char **path, int add, int del)
{
	int f = (maildir_flags_parse(*path) | add) & ~del;
	char *np = maildir_set_flags(*path, f);

	if (np) {
		free(*path);
		*path = np;
	}
}

static int cmd_read(int argc, char **argv)
{
	const char *id = NULL;
	int raw = 0, peek = 0, i;
	const char *hs[] = { "From", "To", "Cc", "Date", "Subject" };
	struct message m;
	char *path, *t;
	struct buf b = { 0 };

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--raw"))
			raw = 1;
		else if (!strcmp(argv[i], "--peek"))
			peek = 1;
		else
			id = argv[i];
	}
	if (!id)
		die("read needs a message id");
	path = must_resolve(id);
	load_msg(path, &m);
	if (raw) {
		fwrite(m.raw, 1, m.rawlen, stdout);
	} else {
		t = msg_text(&m);
		buf_puts(&b, json_out ? "{\"id\":" : "");
		if (json_out)
			json_quote(&b, id);
		for (i = 0; i < 5; i++) {
			char *v = msg_header(&m, hs[i]), lower[16];
			size_t k;

			for (k = 0; hs[i][k]; k++)
				lower[k] = hs[i][k] | 0x20;
			lower[k] = 0;
			if (json_out) {
				buf_printf(&b, ",\"%s\":", lower);
				json_quote(&b, v ? v : "");
			} else if (v) {
				buf_printf(&b, "%s: %s\n", hs[i], v);
			}
			free(v);
		}
		if (json_out) {
			buf_puts(&b, ",\"body\":");
			json_quote(&b, t);
			buf_puts(&b, "}\n");
		} else {
			buf_printf(&b, "\n%s\n", t);
		}
		fputs(b.p, stdout);
		free(t);
	}
	if (!peek)
		set_flag(&path, F_SEEN, 0);
	msg_free(&m);
	buf_free(&b);
	free(path);
	return 0;
}

static int finish_draft(struct account *a, struct draft *d, int send)
{
	size_t len;
	char *data;

	if (send) {
		if (msg_send(a, d))
			return 1;
		fputs("sent\n", stderr);
		return 0;
	}
	data = msg_build(d, &len);
	fwrite(data, 1, len, stdout);
	free(data);
	return 0;
}

static int cmd_send(int argc, char **argv)
{
	struct draft d = { 0 };
	struct account *a = NULL;
	const char *v;
	int i, raw = 0, r;

	for (i = 1; i < argc; i++) {
		if ((v = optval(argc, argv, &i, "-a")))
			a = need_account(v);
		else if ((v = optval(argc, argv, &i, "--to")))
			d.to = xstrdup(v);
		else if ((v = optval(argc, argv, &i, "--cc")))
			d.cc = xstrdup(v);
		else if ((v = optval(argc, argv, &i, "--bcc")))
			d.bcc = xstrdup(v);
		else if ((v = optval(argc, argv, &i, "--subject")))
			d.subject = xstrdup(v);
		else if ((v = optval(argc, argv, &i, "--attach"))) {
			d.attach = xrealloc(d.attach, (d.nattach + 1) *
					    sizeof(*d.attach));
			d.attach[d.nattach++] = xstrdup(v);
		} else if (!strcmp(argv[i], "--raw"))
			raw = 1;
		else
			die("unexpected argument '%s'", argv[i]);
	}
	if (!a)
		a = need_account(NULL);
	if (raw) {
		char *data = read_stdin(), **rc = NULL, **v2;
		const char *hs[] = { "To", "Cc", "Bcc" };
		int n = 0, k, j;
		char *from = hdr_get(data, "From"), *fe;

		for (k = 0; k < 3; k++) {
			char *h = hdr_get(data, hs[k]);
			int c = h ? addr_split(h, &v2) : 0;

			for (j = 0; j < c; j++) {
				rc = xrealloc(rc, (n + 1) * sizeof(*rc));
				rc[n++] = addr_email(v2[j]);
				free(v2[j]);
			}
			if (c)
				free(v2);
			free(h);
		}
		fe = addr_email(from ? from : a->email);
		r = n ? smtp_send(a, fe, rc, n, data, strlen(data)) : -1;
		if (!n)
			warnx("no recipients");
		free(from);
		free(fe);
		free(data);
		for (j = 0; j < n; j++)
			free(rc[j]);
		free(rc);
		return r ? 1 : 0;
	}
	if (!d.to)
		die("send needs --to");
	d.body = read_stdin();
	if (a->signature) {
		char *s = xasprintf("%s\n-- \n%s\n", d.body, a->signature);

		free(d.body);
		d.body = s;
	}
	r = finish_draft(a, &d, 1);
	draft_free(&d);
	return r;
}

static int cmd_reply(int argc, char **argv, int fwd)
{
	const char *id = NULL, *to = NULL, *v;
	int all = 0, send = 0, i, r;
	struct draft d;
	struct message m;
	struct account *a;
	char *path, *text, *body;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--all"))
			all = 1;
		else if (!strcmp(argv[i], "--send"))
			send = 1;
		else if ((v = optval(argc, argv, &i, "--to")))
			to = v;
		else
			id = argv[i];
	}
	if (!id)
		die("missing message id");
	path = must_resolve(id);
	a = account_of_id(id);
	load_msg(path, &m);
	if (fwd) {
		draft_forward(&d, a, &m);
		if (!to)
			die("forward needs --to");
		free(d.to);
		d.to = xstrdup(to);
	} else {
		draft_reply(&d, a, &m, all);
	}
	text = read_stdin();
	body = xasprintf("%s%s", text, d.body);
	free(d.body);
	free(text);
	d.body = body;
	r = finish_draft(a, &d, send);
	if (!r && send)
		set_flag(&path, fwd ? F_PASSED : F_REPLIED, 0);
	draft_free(&d);
	msg_free(&m);
	free(path);
	return r;
}

static int cmd_flag(int argc, char **argv)
{
	char *path;
	const char *s;
	int add = 0, del = 0, sign = 1, f;

	if (argc != 3)
		die("usage: epis flag ID [+|-]FLAGS");
	path = must_resolve(argv[1]);
	for (s = argv[2]; *s; s++) {
		if (*s == '+' || *s == '-') {
			sign = *s == '+';
			continue;
		}
		{
			char tmp[8];

			snprintf(tmp, sizeof(tmp), ":2,%c", *s);
			f = maildir_flags_parse(tmp);
		}
		if (!f)
			die("unknown flag '%c'", *s);
		if (sign)
			add |= f;
		else
			del |= f;
	}
	set_flag(&path, add, del);
	free(path);
	return 0;
}

static int cmd_move(const char *id, const char *folder)
{
	char *path = must_resolve(id), *acc = xstrndup(id, strcspn(id, "/"));
	char *dir = folder_dir(acc, folder), *np, *cur = xasprintf("%s/cur",
								    dir);

	if (access(cur, F_OK))
		die("no folder '%s' in account %s", folder, acc);
	np = maildir_move(path, dir);
	if (!np)
		die("move failed");
	if (json_out)
		printf("{\"id\":\"%s\"}\n", maildir_id(np));
	free(path);
	free(np);
	free(acc);
	free(dir);
	free(cur);
	return 0;
}

static int cmd_delete(const char *id)
{
	char *path = must_resolve(id), *np;

	np = maildir_trash(account_of_id(id), path);
	if (!np)
		die("delete failed");
	free(np);
	free(path);
	return 0;
}

static int cmd_attachments(int argc, char **argv)
{
	const char *id = NULL, *save = NULL, *v;
	struct message m;
	char *path;
	int i;

	for (i = 1; i < argc; i++) {
		if ((v = optval(argc, argv, &i, "--save")))
			save = v;
		else
			id = argv[i];
	}
	if (!id)
		die("missing message id");
	path = must_resolve(id);
	load_msg(path, &m);
	for (i = 0; i < m.nparts; i++) {
		struct part *p = &m.parts[i];
		struct buf b = { 0 };

		if (!p->name)
			continue;
		printf("%s\t%s\t%zu\n", p->name, p->type, p->blen);
		if (save) {
			const char *base = strrchr(p->name, '/');
			char *out = xasprintf("%s/%s", save, base ? base + 1 :
					      p->name);

			part_decode(p, &b);
			if (write_file(out, b.p ? b.p : "", b.len, 0644))
				warnx("cannot write %s", out);
			free(out);
			buf_free(&b);
		}
	}
	msg_free(&m);
	free(path);
	return 0;
}

static char *msg_for_llm(const char *id)
{
	char *path = must_resolve(id), *t, *s;
	char *f, *sub, *date;
	struct message m;

	load_msg(path, &m);
	t = msg_text(&m);
	f = msg_header(&m, "From");
	sub = msg_header(&m, "Subject");
	date = msg_header(&m, "Date");
	s = xasprintf("From: %s\nDate: %s\nSubject: %s\n\n%s", f ? f : "",
		      date ? date : "", sub ? sub : "", t);
	free(t);
	free(f);
	free(sub);
	free(date);
	msg_free(&m);
	free(path);
	return s;
}

static int cmd_llm(int argc, char **argv)
{
	char *p = NULL, *m, *r;

	if (argc < 3)
		die("usage: epis llm summarize ID | draft ID TEXT | ask PROMPT");
	if (!strcmp(argv[1], "summarize")) {
		m = msg_for_llm(argv[2]);
		p = xasprintf("Summarize this email in a few short bullet "
			      "points, then list any action items.\n\n<email>"
			      "\n%s\n</email>", m);
		free(m);
	} else if (!strcmp(argv[1], "draft") && argc >= 4) {
		m = msg_for_llm(argv[2]);
		p = xasprintf("Write the body of a reply to this email. "
			      "Instructions: %s\nOutput only the reply text, "
			      "no subject line.\n\n<email>\n%s\n</email>",
			      argv[3], m);
		free(m);
	} else if (!strcmp(argv[1], "ask")) {
		p = xstrdup(argv[2]);
	} else {
		die("unknown llm command '%s'", argv[1]);
	}
	r = llm_ask("You are an email assistant inside the epis mail client."
		    " Be concise.", p);
	free(p);
	if (!r)
		return 1;
	puts(r);
	free(r);
	return 0;
}

static char *prompt(const char *q, const char *def, int echo)
{
	char line[1024];
	struct termios t, old;

	if (def)
		fprintf(stderr, "%s [%s]: ", q, def);
	else
		fprintf(stderr, "%s: ", q);
	if (!echo && !tcgetattr(0, &old)) {
		t = old;
		t.c_lflag &= ~ECHO;
		tcsetattr(0, TCSANOW, &t);
	}
	if (!fgets(line, sizeof(line), stdin))
		line[0] = 0;
	if (!echo) {
		tcsetattr(0, TCSANOW, &old);
		fputc('\n', stderr);
	}
	trim(line);
	if (!line[0] && def)
		return xstrdup(def);
	return xstrdup(line);
}

static int prompt_int(const char *q, int def)
{
	char d[16], *s;
	int v;

	snprintf(d, sizeof(d), "%d", def);
	s = prompt(q, d, 1);
	v = atoi(s);
	free(s);
	return v;
}

/* guide the user through creating a configuration in the terminal */
int setup_wizard_tty(void)
{
	struct account a = { 0 };
	char *s, *md;

	fputs("epis setup - let's configure your mail account.\n\n", stderr);
	a.email = prompt("Email address", NULL, 1);
	if (!strchr(a.email, '@'))
		die("invalid email address");
	a.realname = prompt("Your name", NULL, 1);
	s = xstrndup(a.email, strcspn(a.email, "@"));
	a.name = prompt("Account name", s, 1);
	free(s);
	fputs("Looking up server settings...\n", stderr);
	autoconf(a.email, &a, 1);
	a.imap_host = prompt("IMAP server", a.imap_host, 1);
	a.imap_port = prompt_int("IMAP port", a.imap_port);
	s = prompt("IMAP security (tls/starttls/none)", sec_name(a.imap_sec),
		   1);
	a.imap_sec = sec_from_name(s) < 0 ? SEC_TLS : sec_from_name(s);
	free(s);
	a.smtp_host = prompt("SMTP server", a.smtp_host, 1);
	a.smtp_port = prompt_int("SMTP port", a.smtp_port);
	s = prompt("SMTP security (tls/starttls/none)", sec_name(a.smtp_sec),
		   1);
	a.smtp_sec = sec_from_name(s) < 0 ? SEC_TLS : sec_from_name(s);
	free(s);
	a.user = prompt("Username", a.user ? a.user : a.email, 1);
	if (a.oauth) {
		fprintf(stderr, "This provider uses OAuth2 (%s). You need an "
			"OAuth client id registered with the provider.\n",
			a.oauth);
		a.client_id = prompt("OAuth client id", NULL, 1);
		s = prompt("OAuth client secret (empty if none)", "", 1);
		a.client_secret = *s ? s : NULL;
		if (!*s)
			free(s);
	} else {
		s = prompt("Password command (e.g. 'pass show mail'), empty to "
			   "type the password", "", 1);
		if (*s)
			a.pass_cmd = s;
		else {
			free(s);
			a.pass = prompt("Password (stored in the config file)",
					NULL, 0);
		}
	}
	md = prompt("Mail directory", cfg.maildir, 1);
	if (config_write_account(&a, md))
		die("cannot write %s", cfg.path);
	fprintf(stderr, "Wrote %s\n", cfg.path);
	free(md);
	return 0;
}

/* dispatch a command line; returns the exit status */
int cli_main(int argc, char **argv)
{
	const char *cmd = argv[0];
	int i, r = 0;

	for (i = 0; i < argc; i++)
		if (!strcmp(argv[i], "--json")) {
			json_out = 1;
			memmove(argv + i, argv + i + 1, (argc - i - 1) *
				sizeof(*argv));
			argc--;
			i--;
		}
	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") ||
	    !strcmp(cmd, "--help")) {
		usage();
	} else if (!strcmp(cmd, "version")) {
		puts("epis " EPIS_VERSION);
	} else if (!strcmp(cmd, "setup")) {
		r = setup_wizard_tty();
	} else if (!strcmp(cmd, "autoconfig")) {
		struct account a = { 0 };

		if (argc < 2)
			die("usage: epis autoconfig EMAIL");
		r = autoconf(argv[1], &a, 1) < 0;
		printf("imap_host = %s\nimap_port = %d\nimap_security = %s\n"
		       "smtp_host = %s\nsmtp_port = %d\nsmtp_security = %s\n",
		       a.imap_host, a.imap_port, sec_name(a.imap_sec),
		       a.smtp_host, a.smtp_port, sec_name(a.smtp_sec));
		if (a.oauth)
			printf("auth = xoauth2\noauth = %s\n", a.oauth);
	} else if (!strcmp(cmd, "accounts")) {
		struct buf b = { 0 };

		for (i = 0; i < cfg.nacc; i++) {
			if (!json_out) {
				printf("%s\t%s\n", cfg.acc[i].name,
				       cfg.acc[i].email ? cfg.acc[i].email : "");
				continue;
			}
			buf_puts(&b, i ? ",{\"name\":" : "{\"name\":");
			json_quote(&b, cfg.acc[i].name);
			buf_puts(&b, ",\"email\":");
			json_quote(&b, cfg.acc[i].email);
			buf_puts(&b, "}");
		}
		if (json_out)
			printf("[%s]\n", b.p ? b.p : "");
		buf_free(&b);
	} else if (!strcmp(cmd, "folders")) {
		const char *acc = argc > 2 && !strcmp(argv[1], "-a") ?
		    argv[2] : NULL;

		for (i = 0; i < cfg.nacc; i++) {
			char **f;
			int n, j;

			if (acc && strcmp(acc, cfg.acc[i].name))
				continue;
			n = maildir_folders(cfg.acc[i].name, &f);
			for (j = 0; j < n; j++) {
				printf("%s/%s\n", cfg.acc[i].name, f[j]);
				free(f[j]);
			}
			free(f);
		}
	} else if (!strcmp(cmd, "sync")) {
		const char *acc = NULL, *folder = NULL, *v;
		int quiet = 0;

		for (i = 1; i < argc; i++) {
			if ((v = optval(argc, argv, &i, "-a")))
				acc = v;
			else if ((v = optval(argc, argv, &i, "-f")))
				folder = v;
			else if (!strcmp(argv[i], "-q"))
				quiet = 1;
		}
		if (acc)
			need_account(acc);
		for (i = 0; i < cfg.nacc; i++)
			if (!acc || !strcmp(acc, cfg.acc[i].name))
				r |= imap_sync_account(&cfg.acc[i], folder,
						       !quiet) ? 1 : 0;
	} else if (!strcmp(cmd, "list")) {
		r = cmd_list(argc, argv, 0);
	} else if (!strcmp(cmd, "search")) {
		r = cmd_list(argc, argv, 1);
	} else if (!strcmp(cmd, "read")) {
		r = cmd_read(argc, argv);
	} else if (!strcmp(cmd, "send")) {
		r = cmd_send(argc, argv);
	} else if (!strcmp(cmd, "reply")) {
		r = cmd_reply(argc, argv, 0);
	} else if (!strcmp(cmd, "forward")) {
		r = cmd_reply(argc, argv, 1);
	} else if (!strcmp(cmd, "flag")) {
		r = cmd_flag(argc, argv);
	} else if (!strcmp(cmd, "move")) {
		if (argc != 3)
			die("usage: epis move ID FOLDER");
		r = cmd_move(argv[1], argv[2]);
	} else if (!strcmp(cmd, "delete")) {
		if (argc != 2)
			die("usage: epis delete ID");
		r = cmd_delete(argv[1]);
	} else if (!strcmp(cmd, "attachments")) {
		r = cmd_attachments(argc, argv);
	} else if (!strcmp(cmd, "oauth")) {
		if (argc != 2)
			die("usage: epis oauth ACCOUNT");
		r = oauth_authorize(need_account(argv[1])) ? 1 : 0;
	} else if (!strcmp(cmd, "llm")) {
		r = cmd_llm(argc, argv);
	} else {
		warnx("unknown command '%s'", cmd);
		usage();
		r = 2;
	}
	return r;
}
