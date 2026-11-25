/* autoconf.c - automatic account configuration discovery */
#include <arpa/nameser.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "epis.h"

/* text of the first <tag> inside [s, e) */
static char *xml_tag(const char *s, const char *e, const char *tag)
{
	char open[64], close[64];
	const char *a, *b;

	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	a = strstr(s, open);
	if (!a || a >= e)
		return NULL;
	a += strlen(open);
	b = strstr(a, close);
	if (!b || b > e)
		return NULL;
	return xstrndup(a, b - a);
}

static char *subst(const char *v, const char *email)
{
	struct buf b = { 0 };
	const char *at = strchr(email, '@');

	while (*v) {
		if (startswith(v, "%EMAILADDRESS%")) {
			buf_puts(&b, email);
			v += 14;
		} else if (startswith(v, "%EMAILLOCALPART%")) {
			buf_add(&b, email, at ? (size_t)(at - email) :
				strlen(email));
			v += 16;
		} else if (startswith(v, "%EMAILDOMAIN%")) {
			buf_puts(&b, at ? at + 1 : "");
			v += 13;
		} else {
			buf_putc(&b, *v++);
		}
	}
	return buf_take(&b);
}

/* fill account fields from the first server block of the given type */
static int server(const char *xml, const char *kind, const char *type,
		  const char *email, char **host, int *port, int *sec,
		  int *auth, char **user)
{
	char pat[64];
	const char *s = xml, *e;

	snprintf(pat, sizeof(pat), "<%s type=\"%s\"", kind, type);
	while ((s = strstr(s, pat))) {
		char *h, *p, *st, *u, *au;
		char close[64];

		snprintf(close, sizeof(close), "</%s>", kind);
		e = strstr(s, close);
		if (!e)
			return -1;
		h = xml_tag(s, e, "hostname");
		p = xml_tag(s, e, "port");
		st = xml_tag(s, e, "socketType");
		u = xml_tag(s, e, "username");
		au = xml_tag(s, e, "authentication");
		if (h && p) {
			*host = h;
			*port = atoi(p);
			*sec = st && !strcasecmp(st, "SSL") ? SEC_TLS :
			    st && !strcasecmp(st, "STARTTLS") ? SEC_STARTTLS :
			    SEC_NONE;
			if (strstr(s, ">OAuth2<") && (strstr(h, "gmail") ||
			    strstr(h, "google") || strstr(h, "office365") ||
			    strstr(h, "outlook")))
				*auth = AUTH_XOAUTH2;
			if (u && !*user)
				*user = subst(u, email);
			free(p);
			free(st);
			free(u);
			free(au);
			return 0;
		}
		free(h);
		free(p);
		free(st);
		free(u);
		free(au);
		s = e;
	}
	return -1;
}

static int try_xml(const char *url, const char *email, struct account *a,
		   int verbose)
{
	struct buf r = { 0 };
	int st, ok = -1, auth = AUTH_AUTO;

	if (verbose)
		fprintf(stderr, "trying %s\n", url);
	st = http_request("GET", url, NULL, NULL, 0, &r);
	if (st == 200 && strstr(r.p, "<clientConfig") &&
	    !server(r.p, "incomingServer", "imap", email, &a->imap_host,
		    &a->imap_port, &a->imap_sec, &auth, &a->user) &&
	    !server(r.p, "outgoingServer", "smtp", email, &a->smtp_host,
		    &a->smtp_port, &a->smtp_sec, &auth, &a->user)) {
		ok = 0;
		if (auth == AUTH_XOAUTH2) {
			a->auth = AUTH_XOAUTH2;
			a->oauth = xstrdup(strstr(a->imap_host, "gmail") ||
					   strstr(a->imap_host, "google") ?
					   "google" : "microsoft");
		}
	}
	buf_free(&r);
	return ok;
}

/* resolve a SRV record, returning the best target and port */
static int srv(const char *name, char **host, int *port)
{
	unsigned char ans[4096];
	char exp[NS_MAXDNAME];
	ns_msg msg;
	ns_rr rr;
	int len, i, best = 65536;

	len = res_query(name, ns_c_in, ns_t_srv, ans, sizeof(ans));
	if (len < 0 || ns_initparse(ans, len, &msg))
		return -1;
	for (i = 0; i < (int)ns_msg_count(msg, ns_s_an); i++) {
		const unsigned char *d;
		int prio;

		if (ns_parserr(&msg, ns_s_an, i, &rr) ||
		    ns_rr_type(rr) != ns_t_srv)
			continue;
		d = ns_rr_rdata(rr);
		prio = ns_get16(d);
		if (prio >= best || dn_expand(ns_msg_base(msg),
					      ns_msg_end(msg), d + 6, exp,
					      sizeof(exp)) < 0)
			continue;
		if (!strcmp(exp, ".") || !*exp)
			continue;
		best = prio;
		free(*host);
		*host = xstrdup(exp);
		*port = ns_get16(d + 4);
	}
	return *host ? 0 : -1;
}

/* domain of the preferred MX host */
static char *mx_domain(const char *domain)
{
	unsigned char ans[4096];
	char exp[NS_MAXDNAME], *res = NULL, *p;
	ns_msg msg;
	ns_rr rr;
	int len, i, best = 65536, dots = 0;

	len = res_query(domain, ns_c_in, ns_t_mx, ans, sizeof(ans));
	if (len < 0 || ns_initparse(ans, len, &msg))
		return NULL;
	for (i = 0; i < (int)ns_msg_count(msg, ns_s_an); i++) {
		const unsigned char *d;

		if (ns_parserr(&msg, ns_s_an, i, &rr) ||
		    ns_rr_type(rr) != ns_t_mx)
			continue;
		d = ns_rr_rdata(rr);
		if ((int)ns_get16(d) >= best || dn_expand(ns_msg_base(msg),
						     ns_msg_end(msg), d + 2,
						     exp, sizeof(exp)) < 0)
			continue;
		best = ns_get16(d);
		free(res);
		res = xstrdup(exp);
	}
	if (!res)
		return NULL;
	for (p = res + strlen(res); p > res; p--)
		if (p[-1] == '.' && ++dots == 2)
			break;
	p = xstrdup(p);
	free(res);
	return p;
}

/*
 * discover server settings for an email address using, in order, the
 * provider's autoconfig file, the Thunderbird ISPDB, the ISPDB entry of
 * the MX provider and DNS SRV records (RFC 6186 / RFC 8314).
 */
int autoconf(const char *email, struct account *a, int verbose)
{
	const char *at = strchr(email, '@');
	char *url, *mx;
	const char *dom;
	int i;

	if (!at)
		return -1;
	dom = at + 1;
	a->imap_sec = a->smtp_sec = SEC_TLS;
	for (i = 0; i < 4; i++) {
		struct buf e = { 0 };

		url_encode(&e, email);
		switch (i) {
		case 0:
			url = xasprintf("https://autoconfig.%s/mail/config-"
					"v1.1.xml?emailaddress=%s", dom, e.p);
			break;
		case 1:
			url = xasprintf("https://%s/.well-known/autoconfig/mail"
					"/config-v1.1.xml?emailaddress=%s",
					dom, e.p);
			break;
		case 2:
			url = xasprintf("https://autoconfig.thunderbird.net/"
					"v1.1/%s", dom);
			break;
		default:
			mx = mx_domain(dom);
			url = mx ? xasprintf("https://autoconfig.thunderbird."
					     "net/v1.1/%s", mx) : NULL;
			free(mx);
		}
		buf_free(&e);
		if (url && !try_xml(url, email, a, verbose)) {
			free(url);
			return 0;
		}
		free(url);
	}
	if (verbose)
		fprintf(stderr, "trying DNS SRV records\n");
	url = xasprintf("_imaps._tcp.%s", dom);
	if (srv(url, &a->imap_host, &a->imap_port)) {
		free(url);
		url = xasprintf("_imap._tcp.%s", dom);
		if (!srv(url, &a->imap_host, &a->imap_port))
			a->imap_sec = SEC_STARTTLS;
	}
	free(url);
	url = xasprintf("_submissions._tcp.%s", dom);
	if (srv(url, &a->smtp_host, &a->smtp_port)) {
		free(url);
		url = xasprintf("_submission._tcp.%s", dom);
		if (!srv(url, &a->smtp_host, &a->smtp_port))
			a->smtp_sec = SEC_STARTTLS;
	}
	free(url);
	if (a->imap_host && a->smtp_host)
		return 0;
	if (verbose)
		fprintf(stderr, "guessing imap.%s and smtp.%s\n", dom, dom);
	if (!a->imap_host) {
		a->imap_host = xasprintf("imap.%s", dom);
		a->imap_port = 993;
	}
	if (!a->smtp_host) {
		a->smtp_host = xasprintf("smtp.%s", dom);
		a->smtp_port = 465;
	}
	return 1;
}
