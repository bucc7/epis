/* smtp.c - SMTP submission client */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "epis.h"

/* read a possibly multi-line reply, returns the status code */
static int reply(struct conn *c, struct buf *all)
{
	struct buf line = { 0 };
	int code = -1;

	if (all)
		buf_reset(all);
	while (!conn_getline(c, &line)) {
		code = atoi(line.p);
		if (all && line.len > 4) {
			buf_add(all, line.p + 4, line.len - 4);
			buf_putc(all, ' ');
		}
		if (line.len < 4 || line.p[3] != '-')
			break;
	}
	buf_free(&line);
	return code;
}

static int ehlo(struct conn *c, struct buf *caps)
{
	char host[256] = "localhost";

	gethostname(host, sizeof(host) - 1);
	conn_printf(c, "EHLO %s\r\n", host);
	return reply(c, caps) == 250 ? 0 : -1;
}

/* find the space separated mechanism list of the AUTH capability */
static const char *auth_caps(const char *caps)
{
	const char *p = strcasestr_(caps, "AUTH ");

	return p ? p + 4 : "";
}

static int smtp_auth(struct conn *c, struct account *a, const char *caps)
{
	struct sasl s = { 0 };
	struct buf out = { 0 }, rb = { 0 };
	char *tok = NULL;
	int code, r = -1;

	s.user = a->user;
	s.pass = a->oauth ? (tok = oauth_access_token(a)) :
	    account_password(a);
	if (!s.pass)
		return -1;
	s.mech = sasl_pick(a->auth, auth_caps(caps), a->oauth != NULL);
	if (s.mech == AUTH_NONE)
		s.mech = AUTH_PLAIN;
	conn_printf(c, "AUTH %s\r\n", sasl_name(s.mech));
	while ((code = reply(c, &rb)) == 334) {
		if (rb.len)
			rb.p[--rb.len] = 0;
		if (sasl_step(&s, rb.p, &out)) {
			conn_printf(c, "*\r\n");
			continue;
		}
		conn_printf(c, "%s\r\n", out.p);
	}
	if (code == 235)
		r = 0;
	else
		warnx("%s: SMTP authentication failed: %d %s", a->name, code,
		      rb.p ? rb.p : "");
	free(tok);
	buf_free(&out);
	buf_free(&rb);
	sasl_free(&s);
	return r;
}

/* send data with dot stuffing and CRLF line endings */
static void send_data(struct conn *c, const char *d, size_t len)
{
	struct buf b = { 0 };
	size_t i;
	int bol = 1;

	for (i = 0; i < len; i++) {
		if (bol && d[i] == '.')
			buf_putc(&b, '.');
		if (d[i] == '\n' && (i == 0 || d[i - 1] != '\r'))
			buf_putc(&b, '\r');
		buf_putc(&b, d[i]);
		bol = d[i] == '\n';
	}
	if (!bol)
		buf_puts(&b, "\r\n");
	buf_puts(&b, ".\r\n");
	conn_write(c, b.p, b.len);
	buf_free(&b);
}

/* submit a message to the account's SMTP server */
int smtp_send(struct account *a, const char *from, char **rcpt, int nrcpt,
	      const char *data, size_t len)
{
	struct buf caps = { 0 }, rb = { 0 };
	struct conn *c;
	int i, r = -1;

	if (!a->smtp_host) {
		warnx("%s: smtp_host not set", a->name);
		return -1;
	}
	c = conn_open(a->smtp_host, a->smtp_port, a->smtp_sec == SEC_TLS);
	if (!c)
		return -1;
	if (reply(c, NULL) != 220 || ehlo(c, &caps))
		goto out;
	if (a->smtp_sec == SEC_STARTTLS) {
		conn_printf(c, "STARTTLS\r\n");
		if (reply(c, NULL) != 220 || conn_starttls(c, a->smtp_host) ||
		    ehlo(c, &caps))
			goto out;
	}
	if (a->auth != AUTH_NONE && strcasestr_(caps.p, "AUTH") &&
	    smtp_auth(c, a, caps.p))
		goto out;
	conn_printf(c, "MAIL FROM:<%s>\r\n", from);
	if (reply(c, &rb) != 250) {
		warnx("MAIL FROM rejected: %s", rb.p);
		goto out;
	}
	for (i = 0; i < nrcpt; i++) {
		conn_printf(c, "RCPT TO:<%s>\r\n", rcpt[i]);
		if (reply(c, &rb) / 100 != 2) {
			warnx("recipient %s rejected: %s", rcpt[i], rb.p);
			goto out;
		}
	}
	conn_printf(c, "DATA\r\n");
	if (reply(c, NULL) != 354)
		goto out;
	send_data(c, data, len);
	if (reply(c, &rb) == 250)
		r = 0;
	else
		warnx("message rejected: %s", rb.p);
	conn_printf(c, "QUIT\r\n");
out:
	conn_close(c);
	buf_free(&caps);
	buf_free(&rb);
	return r;
}
