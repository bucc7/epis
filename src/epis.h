/* epis - shared declarations */
#ifndef EPIS_H
#define EPIS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define EPIS_VERSION "0.1.0"

/* growable byte buffer, always NUL terminated */
struct buf {
	char *p;
	size_t len, cap;
};

void buf_add(struct buf *b, const void *p, size_t n);
void buf_puts(struct buf *b, const char *s);
void buf_putc(struct buf *b, int c);
void buf_printf(struct buf *b, const char *fmt, ...);
void buf_reset(struct buf *b);
void buf_free(struct buf *b);
char *buf_take(struct buf *b);

/* util.c */
void die(const char *fmt, ...);
void warnx(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...);
char *trim(char *s);
int startswith(const char *s, const char *pre);
int strcaseprefix(const char *s, const char *pre);
const char *strcasestr_(const char *h, const char *n);
char *expand_home(const char *path);
int mkdirs(const char *path, int mode);
char *read_file(const char *path, size_t *len);
int write_file(const char *path, const char *data, size_t len, int mode);
char *run_cmd(const char *cmd);
void b64_encode(struct buf *out, const void *in, size_t n);
int b64_decode(struct buf *out, const char *in, size_t n);
void url_encode(struct buf *out, const char *s);
void random_hex(char *out, size_t nbytes);
int utf8_decode(const char *s, uint32_t *cp);
int utf8_encode(char *out, uint32_t cp);
char *data_dir(void);
char *config_dir(void);

/* json.c - minimal JSON reader and writer helpers */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
struct json {
	int type;
	char *key;
	char *str;
	double num;
	struct json *kids;
	int n;
};
struct json *json_parse(const char *s);
void json_free(struct json *j);
struct json *json_get(struct json *j, const char *key);
const char *json_str(struct json *j, const char *key);
void json_quote(struct buf *b, const char *s);

/* config.c */
enum { SEC_TLS, SEC_STARTTLS, SEC_NONE };
enum {
	AUTH_AUTO, AUTH_PLAIN, AUTH_LOGIN, AUTH_CRAM_MD5, AUTH_SCRAM_SHA1,
	AUTH_SCRAM_SHA256, AUTH_XOAUTH2, AUTH_OAUTHBEARER, AUTH_NONE
};

struct account {
	char *name, *email, *realname;
	char *imap_host, *smtp_host;
	int imap_port, smtp_port, imap_sec, smtp_sec;
	char *user, *pass, *pass_cmd;
	int auth;
	char *oauth, *client_id, *client_secret, *tenant;
	char *sent, *drafts, *trash, *archive;
	int save_sent;
	char *signature;
};

enum {
	C_BG, C_FG, C_PANEL, C_BORDER, C_ACCENT, C_SEL_BG, C_SEL_FG,
	C_UNREAD, C_DIM, C_MENU_BG, C_MENU_FG, C_STATUS_BG, C_STATUS_FG,
	C_ERROR, C_FLAGGED, C_QUOTE, C_CURSOR, C_COUNT
};

struct keybind {
	char *ctx, *keys, *cmd;
};

struct alias {
	char *name, *cmd;
};

struct config {
	char *path;
	char *maildir;
	char *font, *font_mono;
	int font_size, radius, padding;
	float scale;
	int vim_edit, menubar;
	char *leader;
	char *editor_cmd;
	uint32_t color[C_COUNT];
	struct account *acc;
	int nacc;
	struct keybind *keys;
	int nkeys;
	struct alias *alias;
	int nalias;
	char *llm_url, *llm_key, *llm_key_cmd, *llm_model;
	int llm_max_tokens;
};

extern struct config cfg;
extern const char *color_names[C_COUNT];

int config_load(const char *path);
int config_exists(void);
char *config_default_path(void);
struct account *account_find(const char *name);
const char *account_password(struct account *a);
int config_write_account(const struct account *a, const char *maildir);
int parse_color(const char *s, uint32_t *out);
int auth_from_name(const char *s);
const char *auth_name(int auth);
int sec_from_name(const char *s);
const char *sec_name(int s);

/* net.c - TCP/TLS connections and a tiny HTTP client */
struct conn {
	int fd;
	void *ssl;
	char rbuf[8192];
	size_t rpos, rlen;
	int trace;
};

struct conn *conn_open(const char *host, int port, int tls);
int conn_starttls(struct conn *c, const char *host);
int conn_write(struct conn *c, const void *p, size_t n);
int conn_printf(struct conn *c, const char *fmt, ...);
int conn_getline(struct conn *c, struct buf *line);
int conn_read(struct conn *c, size_t n, struct buf *out);
void conn_close(struct conn *c);

int http_request(const char *method, const char *url, const char *headers,
		 const char *body, size_t blen, struct buf *resp);

/* auth.c - SASL mechanisms and OAuth2 */
struct sasl {
	int mech;
	int step;
	const char *user, *pass;
	char nonce[64];
	struct buf state;
};

const char *sasl_name(int mech);
int sasl_pick(int want, const char *caps, int oauth);
int sasl_step(struct sasl *s, const char *chal64, struct buf *out64);
void sasl_free(struct sasl *s);
char *oauth_access_token(struct account *a);
int oauth_authorize(struct account *a);

/* imap.c */
struct imap;
struct folder {
	char *name;
	char delim;
	int noselect;
};

struct imap *imap_connect(struct account *a);
void imap_close(struct imap *im);
int imap_list(struct imap *im, struct folder **out);
int imap_sync_folder(struct imap *im, struct account *a, const char *fname,
		     char delim, int verbose);
int imap_sync_account(struct account *a, const char *only, int verbose);

/* smtp.c */
int smtp_send(struct account *a, const char *from, char **rcpt, int nrcpt,
	      const char *data, size_t len);

/* maildir.c */
enum {
	F_SEEN = 1, F_REPLIED = 2, F_FLAGGED = 4, F_TRASHED = 8,
	F_DRAFT = 16, F_PASSED = 32
};

struct msginfo {
	char *path;
	char *from, *to, *subject, *msgid;
	time_t date;
	int flags;
	unsigned uid;
};

char *folder_dir(const char *acc, const char *folder);
char *folder_from_imap(const char *name, char delim);
int maildir_make(const char *dir);
int maildir_scan(const char *dir, struct msginfo **out, int headers);
void msginfo_free(struct msginfo *m, int n);
char *maildir_store(const char *dir, const char *data, size_t len, int flags,
		    unsigned uid);
int maildir_flags_parse(const char *path);
void maildir_flags_str(int flags, char *out);
char *maildir_set_flags(const char *path, int flags);
char *maildir_move(const char *path, const char *dstdir);
unsigned maildir_uid(const char *path);
char *maildir_id(const char *path);
char *maildir_resolve(const char *id);
int maildir_folders(const char *acc, char ***out);
void sort_by_date(struct msginfo *m, int n);

/* msg.c - RFC 5322 / MIME */
struct part {
	char *type, *name, *charset, *encoding, *disposition;
	const char *body;
	size_t blen;
};

struct message {
	char *raw;
	size_t rawlen;
	struct buf hdr;
	struct part *parts;
	int nparts;
};

int msg_parse(struct message *m, char *raw, size_t len);
void msg_free(struct message *m);
char *hdr_get(const char *hdrs, const char *name);
char *hdr_decode(const char *s);
char *msg_header(struct message *m, const char *name);
char *msg_text(struct message *m);
int part_decode(struct part *p, struct buf *out);
void hdr_encode(struct buf *b, const char *name, const char *val);
time_t parse_date(const char *s);
char *addr_email(const char *s);
char *addr_display(const char *s);
int addr_split(const char *s, char ***out);

struct draft {
	char *from, *to, *cc, *bcc, *subject, *body;
	char *in_reply_to, *references;
	char **attach;
	int nattach;
};

char *msg_build(struct draft *d, size_t *len);
void draft_reply(struct draft *d, struct account *a, struct message *m,
		 int all);
void draft_forward(struct draft *d, struct account *a, struct message *m);
void draft_free(struct draft *d);
int msg_send(struct account *a, struct draft *d);

/* autoconf.c */
int autoconf(const char *email, struct account *a, int verbose);

/* llm.c */
char *llm_ask(const char *system, const char *prompt);

/* fuzzy.c */
int fuzzy_score(const char *pat, const char *str);

/* cli.c */
int cli_main(int argc, char **argv);
int setup_wizard_tty(void);

/* gui */
int gui_main(void);

#endif
