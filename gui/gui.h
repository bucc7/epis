/* gui.h - graphical client declarations */
#ifndef GUI_H
#define GUI_H

#ifndef __APPLE__
#include <vulkan/vulkan.h>
#endif

#include "clay.h"
#include "epis.h"

/* platform (wayland.c, x11.c or cocoa.c) */
enum {
	E_NONE, E_KEY, E_MOTION, E_BUTTON, E_SCROLL, E_RESIZE, E_CLOSE,
	E_EXPOSE, E_COMMAND
};

enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_SUPER = 8 };

/* keys that bindings refer to by name; K_F1 to K_F1 + 11 are F1 to F12 */
enum {
	K_NONE, K_CR, K_ESC, K_BS, K_TAB, K_DEL, K_UP, K_DOWN, K_LEFT,
	K_RIGHT, K_HOME, K_END, K_PAGEUP, K_PAGEDOWN, K_SPACE, K_F1
};

/* cp is the unshifted-by-ctrl character of a key, used for <C-x> */
struct event {
	int type;
	uint32_t key, cp, mods;
	char text[16];
	const char *cmd;
	float x, y, dy;
	int button, pressed;
};

struct menu_item {
	const char *label, *cmd;
};

int plat_init(const char *title, int w, int h);
int plat_wait(const int *fds, int nfds);
int plat_next(struct event *ev);
int plat_menu(const char *const *titles,
	      const struct menu_item *const *items);
void plat_size(int *w, int *h);
float plat_scale(void);
void plat_quit(void);
#ifdef __APPLE__
void *plat_metal_layer(void);
#else
const char *plat_vk_ext(void);
int plat_vk_surface(VkInstance inst, VkSurfaceKHR *s);
void xkb_event(struct event *ev, uint32_t sym);
#endif

/* renderer: shared batching (render.c), drawing (vulkan.c or metal.c) */
/* side of the square glyph atlas, which only holds glyphs that were drawn */
#define ATLAS 2048

/* one instanced quad, laid out as the shaders expect it */
struct inst {
	float rect[4], uv[4], color[4], params[4];
};

/* a run of instances sharing a scissor rectangle in pixels */
struct batch {
	uint32_t first, count;
	int x, y, w, h;
};

struct scene {
	struct inst *data;
	size_t n, cap;
	struct batch *b, sc;
	size_t nb, bcap;
	int w, h;
	float scale;
	uint32_t clear;
	char *shot;
};

extern struct scene scene;

void scene_reset(int w, int h, uint32_t clear);
void scene_clear(void);
void scene_unpack(uint32_t col, float *rgba);
void scene_shot(const uint8_t *pix, int bgr);
int r_init(void);
void r_quit(void);
int r_begin(uint32_t clear);
void r_rect(float x, float y, float w, float h, uint32_t col, float radius,
	    float border);
void r_glyph(float x, float y, float w, float h, const float uv[4],
	     uint32_t col);
void r_scissor(float x, float y, float w, float h);
void r_end(void);
void r_atlas(int x, int y, int w, int h, const uint8_t *pix, int pitch);
void r_screenshot(const char *path);

/* fonts (font.c) */
enum { FONT_UI, FONT_MONO, FONT_BOLD };
int font_init(float scale);
float font_width(int font, int size, const char *s, int len);
float font_lineh(int font, int size);
void font_draw(int font, int size, float x, float y, const char *s, int len,
	       uint32_t col);
int font_flush(void);

/* text editor with vim emulation (edit.c) */
enum { ED_NORMAL, ED_INSERT, ED_VISUAL, ED_VLINE };
enum { ED_UNHANDLED, ED_OK, ED_ESCAPE, ED_ENTER };

struct editor {
	char *b;
	size_t cap, gs, ge;
	size_t cur, vstart, want;
	int mode, single, top;
	char pend[16];
	int count;
	struct {
		char *text;
		size_t cur;
	} undo[64];
	int nundo;
};

void ed_init(struct editor *e, const char *text, int single);
void ed_free(struct editor *e);
void ed_set(struct editor *e, const char *text);
size_t ed_len(struct editor *e);
char ed_at(struct editor *e, size_t i);
char *ed_text(struct editor *e);
void ed_insert(struct editor *e, const char *s, size_t n);
int ed_key(struct editor *e, const char *key, const char *text, int vim);
size_t ed_line_start(struct editor *e, size_t p);
size_t ed_line_end(struct editor *e, size_t p);
int ed_line_of(struct editor *e, size_t p);
void ed_copy(struct editor *e, size_t a, size_t b, char *out);

/* application (ui.c) */
enum { W_LIST, W_MSG, W_COMPOSE, W_TEXT, W_WIZARD };

struct chunk {
	char *s;
	/* blank counts the empty lines that follow the run */
	int len, quote, blank;
};

struct win {
	int type;
	int has_list;
	char *acc, *folder;
	struct msginfo *msgs;
	int nmsgs, sel, top, vis;
	char *path, *hdr, *title;
	struct chunk *chunks;
	int nchunks;
	float scroll, maxscroll;
	struct account *a;
	struct editor f[4];
	int field;
	char *reply_path;
	int reply_flag;
	char **attach;
	int nattach;
	int vislines;
};

struct node {
	int split;
	float ratio;
	struct node *a, *b, *parent;
	struct win *w;
};

struct item {
	char *label, *data;
	int score;
};

struct picker {
	int kind;
	char *title;
	struct editor input;
	struct item *items;
	int n, *match, nmatch, sel, top;
};

struct hit {
	Clay_ElementId id;
	int kind, a, b;
	Clay_BoundingBox box;
};

enum {
	H_ROW, H_WIN, H_MENU, H_MENU_ITEM, H_CTX_ITEM, H_PICK_ITEM, H_FOLDER,
	H_FIELD, H_BUTTON
};

struct app {
	struct node *root, *focus;
	int running, dirty;
	int sidebar;
	char **folders;
	int nfolders;
	struct picker *pick;
	int cmdline;
	struct editor cmd;
	char status[512];
	int status_err;
	char keys[64];
	int menu_open;
	int ctx_open, ctx_win, ctx_row;
	float ctx_x, ctx_y, mx, my;
	struct hit *hits;
	int nhits, hitcap;
	int wizard;
	struct editor wf[11];
	int wfield;
	int syncing;
	struct node *leaves[64];
	int nleaves;
	float scale;
	int native_menu;
};

extern struct app app;
extern const char *const menu_titles[];
extern const struct menu_item *const menu_items[];
extern const struct menu_item ctx_items[];

void ui_status(int err, const char *fmt, ...);
void ui_command(const char *line);
void ui_layout(Clay_RenderCommandArray *out);
void draw(void);
void ui_refresh(void);
void hit_add(Clay_ElementId id, int kind, int a, int b);
Clay_String cstr(const char *s);
Clay_String fstr(const char *fmt, ...);
Clay_Color col(int c);
const char *editor_ctx_mode(void);
int wizard_save(void);
int wizard_detect(void);

#endif
