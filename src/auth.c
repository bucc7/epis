/* auth.c - SASL mechanisms and OAuth2 authorization */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "epis.h"

#ifdef __APPLE__
#define BROWSER "open"
#else
#define BROWSER "xdg-open"
#endif

static const char *mech_names[] = {
	"", "PLAIN", "LOGIN", "CRAM-MD5", "SCRAM-SHA-1", "SCRAM-SHA-256",
	"XOAUTH2", "OAUTHBEARER", ""
};

const char *sasl_name(int mech)
{
	return mech_names[mech];
}

static int has_mech(const char *caps, const char *m)
{
	const char *p = caps;
	size_t l = strlen(m);

	while ((p = strcasestr_(p, m))) {
		if ((p == caps || p[-1] == ' ' || p[-1] == '=') &&
		    (!p[l] || p[l] == ' '))
			return 1;
		p += l;
	}
	return 0;
}

/*
 * choose a mechanism from a space separated capability list. Returns
 * AUTH_NONE when no SASL mechanism fits, meaning plain IMAP LOGIN.
 */
int sasl_pick(int want, const char *caps, int oauth)
{
	static const int pref[] = {
		AUTH_SCRAM_SHA256, AUTH_SCRAM_SHA1, AUTH_CRAM_MD5, AUTH_PLAIN,
		AUTH_LOGIN
	};
	size_t i;

	if (want != AUTH_AUTO)
		return want;
	if (oauth)
		return has_mech(caps, "OAUTHBEARER") &&
		    !has_mech(caps, "XOAUTH2") ? AUTH_OAUTHBEARER :
		    AUTH_XOAUTH2;
	for (i = 0; i < sizeof(pref) / sizeof(*pref); i++)
		if (has_mech(caps, mech_names[pref[i]]))
			return pref[i];
	return AUTH_NONE;
}

static void hmac(const EVP_MD *md, const void *k, size_t kl, const void *d,
		 size_t dl, unsigned char *out, unsigned *ol)
{
	HMAC(md, k, kl, d, dl, out, ol);
}

/* compute the SCRAM client-final message and remember the server key */
static int scram_final(struct sasl *s, const char *first, struct buf *out)
{
	const EVP_MD *md = s->mech == AUTH_SCRAM_SHA1 ? EVP_sha1() :
	    EVP_sha256();
	unsigned char salted[64], ckey[64], skey[64], stored[64], sig[64];
	unsigned dl = EVP_MD_size(md), l, i;
	const char *r, *salt, *it;
	struct buf sb = { 0 }, msg = { 0 }, nr = { 0 }, proof = { 0 };
	int iter;

	r = strstr(first, "r=");
	salt = strstr(first, "s=");
	it = strstr(first, "i=");
	if (!r || !salt || !it || strncmp(r + 2, s->nonce, strlen(s->nonce)))
		return -1;
	buf_add(&nr, r + 2, strcspn(r + 2, ","));
	b64_decode(&sb, salt + 2, strcspn(salt + 2, ","));
	iter = atoi(it + 2);
	PKCS5_PBKDF2_HMAC(s->pass, strlen(s->pass), (unsigned char *)sb.p,
			  sb.len, iter, md, dl, salted);
	hmac(md, salted, dl, "Client Key", 10, ckey, &l);
	EVP_Digest(ckey, dl, stored, &l, md, NULL);
	buf_printf(&msg, "n=%s,r=%s,%s,c=biws,r=%s", s->user, s->nonce,
		   first, nr.p);
	hmac(md, stored, dl, msg.p, msg.len, sig, &l);
	for (i = 0; i < dl; i++)
		ckey[i] ^= sig[i];
	hmac(md, salted, dl, "Server Key", 10, skey, &l);
	hmac(md, skey, dl, msg.p, msg.len, sig, &l);
	buf_reset(&s->state);
	b64_encode(&s->state, sig, dl);
	b64_encode(&proof, ckey, dl);
	buf_printf(out, "c=biws,r=%s,p=%s", nr.p, proof.p);
	buf_free(&sb);
	buf_free(&msg);
	buf_free(&nr);
	buf_free(&proof);
	return 0;
}

/*
 * produce the base64 response to a base64 server challenge. The first call
 * (step 0) yields the initial response for mechanisms that have one.
 */
int sasl_step(struct sasl *s, const char *chal64, struct buf *out64)
{
	struct buf chal = { 0 }, r = { 0 };
	unsigned char mac[EVP_MAX_MD_SIZE];
	unsigned ml, i;
	int ret = 0;

	b64_decode(&chal, chal64 ? chal64 : "", chal64 ? strlen(chal64) : 0);
	buf_add(&r, "", 0);
	switch (s->mech) {
	case AUTH_PLAIN:
		buf_putc(&r, 0);
		buf_puts(&r, s->user);
		buf_putc(&r, 0);
		buf_puts(&r, s->pass);
		break;
	case AUTH_LOGIN:
		buf_puts(&r, s->step == 0 ? s->user : s->pass);
		break;
	case AUTH_CRAM_MD5:
		hmac(EVP_md5(), s->pass, strlen(s->pass), chal.p, chal.len,
		     mac, &ml);
		buf_printf(&r, "%s ", s->user);
		for (i = 0; i < ml; i++)
			buf_printf(&r, "%02x", mac[i]);
		break;
	case AUTH_SCRAM_SHA1:
	case AUTH_SCRAM_SHA256:
		if (s->step == 0) {
			random_hex(s->nonce, 16);
			buf_printf(&r, "n,,n=%s,r=%s", s->user, s->nonce);
		} else if (s->step == 1) {
			ret = scram_final(s, chal.p, &r);
		} else {
			const char *v = strstr(chal.p, "v=");

			if (!v || strcmp(v + 2, s->state.p)) {
				warnx("SCRAM: server signature mismatch");
				ret = -1;
			}
		}
		break;
	case AUTH_XOAUTH2:
		if (s->step == 0)
			buf_printf(&r, "user=%s\1auth=Bearer %s\1\1", s->user,
				   s->pass);
		else
			warnx("XOAUTH2: %s", chal.p);
		break;
	case AUTH_OAUTHBEARER:
		if (s->step == 0)
			buf_printf(&r, "n,a=%s,\1auth=Bearer %s\1\1", s->user,
				   s->pass);
		else
			buf_puts(&r, "\1");
		break;
	default:
		ret = -1;
	}
	s->step++;
	buf_reset(out64);
	b64_encode(out64, r.p, r.len);
	buf_free(&chal);
	buf_free(&r);
	return ret;
}

void sasl_free(struct sasl *s)
{
	buf_free(&s->state);
}

/* OAuth2 endpoints of the supported providers */
struct provider {
	const char *name, *auth, *token, *scope;
};

static const struct provider providers[] = {
	{ "google", "https://accounts.google.com/o/oauth2/v2/auth",
	  "https://oauth2.googleapis.com/token", "https://mail.google.com/" },
	{ "microsoft",
	  "https://login.microsoftonline.com/%s/oauth2/v2.0/authorize",
	  "https://login.microsoftonline.com/%s/oauth2/v2.0/token",
	  "https://outlook.office.com/IMAP.AccessAsUser.All "
	  "https://outlook.office.com/SMTP.Send offline_access" },
};

static const struct provider *provider_find(const char *name)
{
	size_t i;

	for (i = 0; name && i < sizeof(providers) / sizeof(*providers); i++)
		if (!strcmp(providers[i].name, name))
			return &providers[i];
	return NULL;
}

static char *endpoint(struct account *a, const char *fmt)
{
	return xasprintf(fmt, a->tenant ? a->tenant : "common");
}

static char *token_path(struct account *a)
{
	char *d = data_dir(), *p = xasprintf("%s/%s.oauth", d, a->name);

	free(d);
	return p;
}

static void token_save(struct account *a, const char *refresh,
		       const char *access, long expires)
{
	struct buf b = { 0 };
	char *p = token_path(a);

	buf_puts(&b, "{\"refresh_token\":");
	json_quote(&b, refresh);
	buf_puts(&b, ",\"access_token\":");
	json_quote(&b, access);
	buf_printf(&b, ",\"expires\":%ld}\n", expires);
	if (write_file(p, b.p, b.len, 0600))
		warnx("cannot write %s", p);
	free(p);
	buf_free(&b);
}

/* POST form to the token endpoint and store the resulting tokens */
static char *token_request(struct account *a, const struct provider *pv,
			   struct buf *form, const char *old_refresh)
{
	struct buf resp = { 0 };
	struct json *j;
	char *url = endpoint(a, pv->token), *tok = NULL;
	const char *at, *rt;
	int st;

	buf_puts(form, "&client_id=");
	url_encode(form, a->client_id);
	if (!strcmp(pv->name, "microsoft")) {
		buf_puts(form, "&scope=");
		url_encode(form, pv->scope);
	}
	if (a->client_secret) {
		buf_puts(form, "&client_secret=");
		url_encode(form, a->client_secret);
	}
	st = http_request("POST", url, "Content-Type: application/"
			  "x-www-form-urlencoded\r\n", form->p, form->len,
			  &resp);
	j = json_parse(resp.p);
	at = json_str(j, "access_token");
	rt = json_str(j, "refresh_token");
	if (st != 200 || !at) {
		warnx("%s: token request failed (%d): %s", a->name, st, resp.p);
	} else {
		struct json *e = json_get(j, "expires_in");

		token_save(a, rt ? rt : old_refresh, at, (long)time(NULL) +
			   (e ? (long)e->num : 3600));
		tok = xstrdup(at);
	}
	json_free(j);
	buf_free(&resp);
	free(url);
	return tok;
}

/* return a valid access token, refreshing it when it has expired */
char *oauth_access_token(struct account *a)
{
	const struct provider *pv = provider_find(a->oauth);
	char *p = token_path(a), *data = read_file(p, NULL), *tok = NULL;
	struct json *j = json_parse(data);
	struct json *exp = json_get(j, "expires");
	const char *rt = json_str(j, "refresh_token");
	struct buf form = { 0 };

	free(p);
	free(data);
	if (!pv || !a->client_id) {
		warnx("%s: oauth needs oauth = google|microsoft and "
		      "oauth_client_id", a->name);
	} else if (!rt) {
		warnx("%s: not authorized, run 'epis oauth %s'", a->name,
		      a->name);
	} else if (exp && exp->num > time(NULL) + 60 &&
		   json_str(j, "access_token")) {
		tok = xstrdup(json_str(j, "access_token"));
	} else {
		buf_puts(&form, "grant_type=refresh_token&refresh_token=");
		url_encode(&form, rt);
		tok = token_request(a, pv, &form, rt);
	}
	buf_free(&form);
	json_free(j);
	return tok;
}

/* extract a query parameter value from an HTTP request line */
static char *query_param(const char *req, const char *key)
{
	char pat[64];
	const char *p;
	struct buf b = { 0 };

	snprintf(pat, sizeof(pat), "%s=", key);
	for (p = req; (p = strstr(p, pat)); p++)
		if (p[-1] == '?' || p[-1] == '&')
			break;
	if (!p)
		return NULL;
	for (p += strlen(pat); *p && *p != '&' && *p != ' '; p++) {
		unsigned c;

		if (*p == '%' && sscanf(p + 1, "%2x", &c) == 1) {
			buf_putc(&b, c);
			p += 2;
		} else {
			buf_putc(&b, *p == '+' ? ' ' : *p);
		}
	}
	return buf_take(&b);
}

/*
 * run the interactive authorization code flow with PKCE using a loopback
 * redirect, then store the refresh token.
 */
int oauth_authorize(struct account *a)
{
	const struct provider *pv = provider_find(a->oauth);
	struct sockaddr_in sa = { 0 };
	socklen_t sl = sizeof(sa);
	unsigned char hash[SHA256_DIGEST_LENGTH];
	char verifier[65], state[33], req[4096], *redirect, *url, *code,
	    *rstate, *tok, *cmd;
	struct buf u = { 0 }, ch = { 0 }, form = { 0 };
	int fd, cfd, n, one = 1;
	size_t i;

	if (!pv || !a->client_id) {
		warnx("%s: set oauth = google|microsoft and oauth_client_id",
		      a->name);
		return -1;
	}
	random_hex(verifier, 32);
	random_hex(state, 16);
	SHA256((unsigned char *)verifier, 64, hash);
	b64_encode(&ch, hash, sizeof(hash));
	for (i = 0; i < ch.len; i++)
		ch.p[i] = ch.p[i] == '+' ? '-' : ch.p[i] == '/' ? '_' : ch.p[i];
	while (ch.len && ch.p[ch.len - 1] == '=')
		ch.p[--ch.len] = 0;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(fd, 1) ||
	    getsockname(fd, (struct sockaddr *)&sa, &sl)) {
		warnx("cannot listen on loopback");
		close(fd);
		return -1;
	}
	redirect = xasprintf("http://127.0.0.1:%d/", ntohs(sa.sin_port));
	url = endpoint(a, pv->auth);
	buf_printf(&u, "%s?response_type=code&access_type=offline&prompt="
		   "consent&code_challenge_method=S256&code_challenge=%s"
		   "&state=%s&client_id=", url, ch.p, state);
	url_encode(&u, a->client_id);
	buf_puts(&u, "&redirect_uri=");
	url_encode(&u, redirect);
	buf_puts(&u, "&scope=");
	url_encode(&u, pv->scope);
	buf_puts(&u, "&login_hint=");
	url_encode(&u, a->email ? a->email : "");
	printf("Open this URL in your browser to authorize epis:\n\n%s\n\n",
	       u.p);
	fflush(stdout);
	cmd = xasprintf(BROWSER " '%s' >/dev/null 2>&1 &", u.p);
	if (system(cmd))
		warnx("could not start a browser");
	free(cmd);

	cfd = accept(fd, NULL, NULL);
	n = cfd >= 0 ? read(cfd, req, sizeof(req) - 1) : -1;
	req[n > 0 ? n : 0] = 0;
	code = query_param(req, "code");
	rstate = query_param(req, "state");
	if (cfd >= 0) {
		const char *page = code ? "Authorized. You can close this tab."
		    : "Authorization failed.";
		char *resp = xasprintf("HTTP/1.1 200 OK\r\nContent-Type: "
				       "text/plain\r\nContent-Length: %zu\r\n"
				       "Connection: close\r\n\r\n%s",
				       strlen(page), page);

		if (write(cfd, resp, strlen(resp)) < 0)
			warnx("cannot answer the browser");
		free(resp);
		close(cfd);
	}
	close(fd);
	tok = NULL;
	if (code && rstate && !strcmp(rstate, state)) {
		buf_puts(&form, "grant_type=authorization_code&code=");
		url_encode(&form, code);
		buf_puts(&form, "&redirect_uri=");
		url_encode(&form, redirect);
		buf_printf(&form, "&code_verifier=%s", verifier);
		tok = token_request(a, pv, &form, NULL);
	} else {
		warnx("authorization failed");
	}
	free(code);
	free(rstate);
	free(redirect);
	free(url);
	buf_free(&u);
	buf_free(&ch);
	buf_free(&form);
	if (!tok)
		return -1;
	free(tok);
	return 0;
}
