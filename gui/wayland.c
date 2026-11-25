/* wayland.c - Wayland window backend */
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "gui.h"
#include "xdg-decoration.h"
#include "xdg-shell.h"

#include <vulkan/vulkan_wayland.h>

#define QLEN 256

static struct {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_seat *seat;
	struct wl_shm *shm;
	struct wl_keyboard *kbd;
	struct wl_pointer *ptr;
	struct xdg_wm_base *wm;
	struct zxdg_decoration_manager_v1 *decoman;
	struct wl_surface *surf, *cursurf;
	struct xdg_surface *xsurf;
	struct xdg_toplevel *top;
	struct wl_cursor_theme *ctheme;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	int w, h, scale, configured;
	int rate, delay;
	uint32_t rkey;
	long long rnext;
	struct event q[QLEN];
	int qh, qt;
	float px, py;
	struct wl_data_device_manager *dataman;
	struct wl_data_device *data;
	struct wl_data_source *source;
	struct wl_data_offer *offer, *fresh, *dnd;
	const char *mime, *fresh_mime;
	char *clip;
	/* serial of the last input, required to set the clipboard */
	uint32_t serial;
} wl;

static void push(struct event *e)
{
	if ((wl.qt + 1) % QLEN == wl.qh)
		return;
	wl.q[wl.qt] = *e;
	wl.qt = (wl.qt + 1) % QLEN;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
	struct event e = { .type = E_RESIZE };

	(void)d;
	xdg_surface_ack_configure(s, serial);
	wl.configured = 1;
	push(&e);
}

static const struct xdg_surface_listener xsurf_listener = { xsurf_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w,
			  int32_t h, struct wl_array *states)
{
	(void)d;
	(void)t;
	(void)states;
	if (w > 0 && h > 0) {
		wl.w = w;
		wl.h = h;
	}
}

static void top_close(void *d, struct xdg_toplevel *t)
{
	struct event e = { .type = E_CLOSE };

	(void)d;
	(void)t;
	push(&e);
}

static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{
	(void)d;
	(void)t;
	(void)w;
	(void)h;
}

static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{
	(void)d;
	(void)t;
	(void)c;
}

static const struct xdg_toplevel_listener top_listener = {
	top_configure, top_close, top_bounds, top_caps
};

static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	(void)o;
}

static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	(void)o;
}

static void surf_scale(void *d, struct wl_surface *s, int32_t factor)
{
	struct event e = { .type = E_RESIZE };

	(void)d;
	if (factor != wl.scale) {
		wl.scale = factor;
		wl_surface_set_buffer_scale(s, factor);
		push(&e);
	}
}

static void surf_transform(void *d, struct wl_surface *s, uint32_t t)
{
	(void)d;
	(void)s;
	(void)t;
}

static const struct wl_surface_listener surf_listener = {
	surf_enter, surf_leave, surf_scale, surf_transform
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd,
		      uint32_t size)
{
	char *map;

	(void)d;
	(void)k;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	xkb_state_unref(wl.state);
	xkb_keymap_unref(wl.keymap);
	wl.keymap = xkb_keymap_new_from_string(wl.xkb, map,
					       XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	munmap(map, size);
	wl.state = wl.keymap ? xkb_state_new(wl.keymap) : NULL;
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s,
		     struct wl_surface *sf, struct wl_array *keys)
{
	(void)d;
	(void)k;
	wl.serial = s;
	(void)sf;
	(void)keys;
}

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s,
		     struct wl_surface *sf)
{
	(void)d;
	(void)k;
	(void)s;
	(void)sf;
	wl.rkey = 0;
}

static void key_event(uint32_t code)
{
	struct event e = { .type = E_KEY };

	if (!wl.state)
		return;
	xkb_event(&e, xkb_state_key_get_one_sym(wl.state, code));
	xkb_state_key_get_utf8(wl.state, code, e.text, sizeof(e.text));
	if (xkb_state_mod_name_is_active(wl.state, XKB_MOD_NAME_SHIFT,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		e.mods |= MOD_SHIFT;
	if (xkb_state_mod_name_is_active(wl.state, XKB_MOD_NAME_CTRL,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		e.mods |= MOD_CTRL;
	if (xkb_state_mod_name_is_active(wl.state, XKB_MOD_NAME_ALT,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		e.mods |= MOD_ALT;
	if (xkb_state_mod_name_is_active(wl.state, XKB_MOD_NAME_LOGO,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		e.mods |= MOD_SUPER;
	e.pressed = 1;
	push(&e);
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial,
		   uint32_t time, uint32_t key, uint32_t st)
{
	uint32_t code = key + 8;

	(void)d;
	(void)k;
	wl.serial = serial;
	(void)time;
	if (st == WL_KEYBOARD_KEY_STATE_PRESSED) {
		key_event(code);
		if (wl.keymap && xkb_keymap_key_repeats(wl.keymap, code) &&
		    wl.rate > 0) {
			wl.rkey = code;
			wl.rnext = now_ms() + wl.delay;
		}
	} else if (code == wl.rkey) {
		wl.rkey = 0;
	}
}

static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep,
		    uint32_t lat, uint32_t lock, uint32_t grp)
{
	(void)d;
	(void)k;
	(void)s;
	if (wl.state)
		xkb_state_update_mask(wl.state, dep, lat, lock, 0, 0, grp);
}

static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate,
		      int32_t delay)
{
	(void)d;
	(void)k;
	wl.rate = rate;
	wl.delay = delay;
}

static const struct wl_keyboard_listener kb_listener = {
	kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat
};

static void pt_enter(void *d, struct wl_pointer *p, uint32_t serial,
		     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	struct wl_cursor *c;
	struct wl_cursor_image *img;

	(void)d;
	(void)s;
	wl.px = wl_fixed_to_double(x) * wl.scale;
	wl.py = wl_fixed_to_double(y) * wl.scale;
	if (!wl.ctheme)
		return;
	c = wl_cursor_theme_get_cursor(wl.ctheme, "left_ptr");
	if (!c)
		c = wl_cursor_theme_get_cursor(wl.ctheme, "default");
	if (!c)
		return;
	img = c->images[0];
	wl_surface_set_buffer_scale(wl.cursurf, wl.scale);
	wl_surface_attach(wl.cursurf, wl_cursor_image_get_buffer(img), 0, 0);
	wl_surface_damage(wl.cursurf, 0, 0, img->width, img->height);
	wl_surface_commit(wl.cursurf);
	wl_pointer_set_cursor(p, serial, wl.cursurf, img->hotspot_x / wl.scale,
			      img->hotspot_y / wl.scale);
}

static void pt_leave(void *d, struct wl_pointer *p, uint32_t s,
		     struct wl_surface *sf)
{
	(void)d;
	(void)p;
	(void)s;
	(void)sf;
}

static void pt_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
		      wl_fixed_t y)
{
	struct event e = { .type = E_MOTION };

	(void)d;
	(void)p;
	(void)t;
	wl.px = e.x = wl_fixed_to_double(x) * wl.scale;
	wl.py = e.y = wl_fixed_to_double(y) * wl.scale;
	push(&e);
}

static void pt_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t,
		      uint32_t button, uint32_t st)
{
	struct event e = { .type = E_BUTTON };

	(void)d;
	(void)p;
	wl.serial = s;
	(void)t;
	e.x = wl.px;
	e.y = wl.py;
	e.button = button == BTN_LEFT ? 1 : button == BTN_RIGHT ? 3 :
	    button == BTN_MIDDLE ? 2 : 0;
	e.pressed = st == WL_POINTER_BUTTON_STATE_PRESSED;
	push(&e);
}

static void pt_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis,
		    wl_fixed_t v)
{
	struct event e = { .type = E_SCROLL };

	(void)d;
	(void)p;
	(void)t;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	e.x = wl.px;
	e.y = wl.py;
	e.dy = wl_fixed_to_double(v);
	push(&e);
}

static void pt_frame(void *d, struct wl_pointer *p)
{
	(void)d;
	(void)p;
}

static void pt_source(void *d, struct wl_pointer *p, uint32_t s)
{
	(void)d;
	(void)p;
	(void)s;
}

static void pt_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
}

static void pt_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{
	(void)d;
	(void)p;
	(void)a;
	(void)v;
}

static const struct wl_pointer_listener pt_listener = {
	.enter = pt_enter, .leave = pt_leave, .motion = pt_motion,
	.button = pt_button, .axis = pt_axis, .frame = pt_frame,
	.axis_source = pt_source, .axis_stop = pt_stop,
	.axis_discrete = pt_discrete
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl.kbd) {
		wl.kbd = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(wl.kbd, &kb_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl.ptr) {
		wl.ptr = wl_seat_get_pointer(s);
		wl_pointer_add_listener(wl.ptr, &pt_listener, NULL);
	}
}

static void seat_name(void *d, struct wl_seat *s, const char *n)
{
	(void)d;
	(void)s;
	(void)n;
}

static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static const char *text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"
};

static void offer_mime(void *d, struct wl_data_offer *o, const char *mime)
{
	int i;

	(void)d;
	if (o != wl.fresh)
		return;
	/* the entries are in order of preference */
	for (i = 0; i < 3; i++) {
		if (wl.fresh_mime == text_mimes[i])
			return;
		if (!strcmp(mime, text_mimes[i])) {
			wl.fresh_mime = text_mimes[i];
			return;
		}
	}
}

static const struct wl_data_offer_listener offer_listener = {
	.offer = offer_mime
};

static void dev_offer(void *d, struct wl_data_device *dev,
		      struct wl_data_offer *o)
{
	(void)d;
	(void)dev;
	wl.fresh = o;
	wl.fresh_mime = NULL;
	wl_data_offer_add_listener(o, &offer_listener, NULL);
}

static void drop_dnd(void)
{
	if (wl.dnd)
		wl_data_offer_destroy(wl.dnd);
	wl.dnd = NULL;
}

/* drag and drop is not accepted, only its offer is released */
static void dev_enter(void *d, struct wl_data_device *dev, uint32_t s,
		      struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y,
		      struct wl_data_offer *o)
{
	(void)d;
	(void)dev;
	(void)s;
	(void)sf;
	(void)x;
	(void)y;
	drop_dnd();
	wl.dnd = o;
}

static void dev_leave(void *d, struct wl_data_device *dev)
{
	(void)d;
	(void)dev;
	drop_dnd();
}

static void dev_motion(void *d, struct wl_data_device *dev, uint32_t t,
		       wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)dev;
	(void)t;
	(void)x;
	(void)y;
}

static void dev_drop(void *d, struct wl_data_device *dev)
{
	(void)d;
	(void)dev;
}

static void dev_selection(void *d, struct wl_data_device *dev,
			  struct wl_data_offer *o)
{
	(void)d;
	(void)dev;
	if (wl.offer && wl.offer != o)
		wl_data_offer_destroy(wl.offer);
	wl.offer = o;
	wl.mime = o && o == wl.fresh ? wl.fresh_mime : NULL;
}

static const struct wl_data_device_listener dev_listener = {
	.data_offer = dev_offer, .enter = dev_enter, .leave = dev_leave,
	.motion = dev_motion, .drop = dev_drop, .selection = dev_selection
};

static void src_target(void *d, struct wl_data_source *s, const char *mime)
{
	(void)d;
	(void)s;
	(void)mime;
}

static void src_send(void *d, struct wl_data_source *s, const char *mime,
		     int32_t fd)
{
	const char *p = wl.clip ? wl.clip : "";
	size_t n = strlen(p);
	ssize_t w;

	(void)d;
	(void)s;
	(void)mime;
	while (n && (w = write(fd, p, n)) > 0) {
		p += w;
		n -= w;
	}
	close(fd);
}

/* another client took over the clipboard */
static void src_cancelled(void *d, struct wl_data_source *s)
{
	(void)d;
	wl_data_source_destroy(s);
	if (s == wl.source)
		wl.source = NULL;
}

static const struct wl_data_source_listener src_listener = {
	.target = src_target, .send = src_send, .cancelled = src_cancelled
};

void plat_clip_set(const char *s)
{
	int i;

	free(wl.clip);
	wl.clip = xstrdup(s);
	if (!wl.data)
		return;
	if (wl.source)
		wl_data_source_destroy(wl.source);
	wl.source = wl_data_device_manager_create_data_source(wl.dataman);
	wl_data_source_add_listener(wl.source, &src_listener, NULL);
	for (i = 0; i < 3; i++)
		wl_data_source_offer(wl.source, text_mimes[i]);
	wl_data_device_set_selection(wl.data, wl.source, wl.serial);
	wl_display_flush(wl.dpy);
}

/* the clipboard text, or NULL; the caller frees it */
char *plat_clip_get(void)
{
	struct buf b = { 0 };
	struct pollfd pf = { 0 };
	char tmp[4096];
	ssize_t n;
	int p[2];

	/* our own text is not read back through the compositor */
	if (wl.source || !wl.data)
		return wl.clip ? xstrdup(wl.clip) : NULL;
	if (!wl.offer || !wl.mime || pipe(p))
		return NULL;
	wl_data_offer_receive(wl.offer, wl.mime, p[1]);
	close(p[1]);
	wl_display_flush(wl.dpy);
	pf.fd = p[0];
	pf.events = POLLIN;
	buf_add(&b, "", 0);
	while (poll(&pf, 1, 1000) > 0 && (n = read(p[0], tmp,
						   sizeof(tmp))) > 0)
		buf_add(&b, tmp, n);
	close(p[0]);
	return b.p;
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name))
		wl.comp = wl_registry_bind(r, name, &wl_compositor_interface,
					   ver < 6 ? ver : 6);
	else if (!strcmp(iface, xdg_wm_base_interface.name))
		wl.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name))
		wl.seat = wl_registry_bind(r, name, &wl_seat_interface,
					   ver < 5 ? ver : 5);
	else if (!strcmp(iface, wl_data_device_manager_interface.name))
		/* version 1 has all the clipboard needs */
		wl.dataman = wl_registry_bind(r, name,
			&wl_data_device_manager_interface, 1);
	else if (!strcmp(iface, wl_shm_interface.name))
		wl.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
		wl.decoman = wl_registry_bind(r, name,
			&zxdg_decoration_manager_v1_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d;
	(void)r;
	(void)name;
}

static const struct wl_registry_listener reg_listener = {
	reg_global, reg_remove
};

/* connect to the compositor and create the toplevel window */
int plat_init(const char *title, int w, int h)
{
	const char *ct = getenv("XCURSOR_SIZE");

	wl.dpy = wl_display_connect(NULL);
	if (!wl.dpy) {
		warnx("cannot connect to the Wayland display");
		return -1;
	}
	wl.w = w;
	wl.h = h;
	wl.scale = 1;
	wl.xkb = xkb_context_new(0);
	wl.reg = wl_display_get_registry(wl.dpy);
	wl_registry_add_listener(wl.reg, &reg_listener, NULL);
	wl_display_roundtrip(wl.dpy);
	if (!wl.comp || !wl.wm) {
		warnx("compositor lacks wl_compositor or xdg_wm_base");
		return -1;
	}
	xdg_wm_base_add_listener(wl.wm, &wm_listener, NULL);
	if (wl.seat)
		wl_seat_add_listener(wl.seat, &seat_listener, NULL);
	if (wl.dataman && wl.seat) {
		wl.data = wl_data_device_manager_get_data_device(wl.dataman,
								 wl.seat);
		wl_data_device_add_listener(wl.data, &dev_listener, NULL);
	}
	if (wl.shm) {
		wl.ctheme = wl_cursor_theme_load(getenv("XCURSOR_THEME"),
						 ct ? atoi(ct) : 24, wl.shm);
		wl.cursurf = wl_compositor_create_surface(wl.comp);
	}
	wl.surf = wl_compositor_create_surface(wl.comp);
	wl_surface_add_listener(wl.surf, &surf_listener, NULL);
	wl.xsurf = xdg_wm_base_get_xdg_surface(wl.wm, wl.surf);
	xdg_surface_add_listener(wl.xsurf, &xsurf_listener, NULL);
	wl.top = xdg_surface_get_toplevel(wl.xsurf);
	xdg_toplevel_add_listener(wl.top, &top_listener, NULL);
	xdg_toplevel_set_title(wl.top, title);
	xdg_toplevel_set_app_id(wl.top, "epis");
	if (wl.decoman) {
		struct zxdg_toplevel_decoration_v1 *deco =
		    zxdg_decoration_manager_v1_get_toplevel_decoration(
			wl.decoman, wl.top);

		zxdg_toplevel_decoration_v1_set_mode(deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(wl.surf);
	while (!wl.configured)
		if (wl_display_dispatch(wl.dpy) < 0)
			return -1;
	return 0;
}

int plat_next(struct event *e)
{
	if (wl.qh == wl.qt) {
		long long t = now_ms();

		if (wl.rkey && t >= wl.rnext) {
			key_event(wl.rkey);
			wl.rnext = t + 1000 / wl.rate;
		}
		wl_display_dispatch_pending(wl.dpy);
		if (wl.qh == wl.qt)
			return 0;
	}
	*e = wl.q[wl.qh];
	wl.qh = (wl.qh + 1) % QLEN;
	return 1;
}

/* milliseconds until the next key repeat, or -1 */
static int repeat_timeout(void)
{
	long long d;

	if (!wl.rkey)
		return -1;
	d = wl.rnext - now_ms();
	return d < 0 ? 0 : (int)d;
}

/* sleep until there is input or one of the file descriptors is readable */
int plat_wait(const int *fds, int nfds)
{
	struct pollfd p[9];
	int i;

	wl_display_flush(wl.dpy);
	p[0].fd = wl_display_get_fd(wl.dpy);
	p[0].events = POLLIN;
	for (i = 0; i < nfds && i < 8; i++) {
		p[i + 1].fd = fds[i];
		p[i + 1].events = POLLIN;
	}
	if (poll(p, i + 1, repeat_timeout()) < 0)
		return errno == EINTR ? 0 : -1;
	if ((p[0].revents & POLLIN) && wl_display_dispatch(wl.dpy) < 0)
		return -1;
	return 0;
}

/* Wayland has no global menu bar, the menu is drawn in the window */
int plat_menu(const char *const *titles, const struct menu_item *const *items)
{
	(void)titles;
	(void)items;
	return 0;
}

void plat_size(int *w, int *h)
{
	*w = wl.w * wl.scale;
	*h = wl.h * wl.scale;
}

float plat_scale(void)
{
	return wl.scale * (cfg.scale > 0 ? cfg.scale : 1);
}

const char *plat_vk_ext(void)
{
	return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
}

int plat_vk_surface(VkInstance inst, VkSurfaceKHR *s)
{
	VkWaylandSurfaceCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };

	ci.display = wl.dpy;
	ci.surface = wl.surf;
	return vkCreateWaylandSurfaceKHR(inst, &ci, NULL, s) == VK_SUCCESS ?
	    0 : -1;
}

void plat_quit(void)
{
	if (wl.ctheme)
		wl_cursor_theme_destroy(wl.ctheme);
	xkb_state_unref(wl.state);
	xkb_keymap_unref(wl.keymap);
	xkb_context_unref(wl.xkb);
	free(wl.clip);
	wl_display_disconnect(wl.dpy);
}
