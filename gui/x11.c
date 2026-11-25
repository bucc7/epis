/* x11.c - X11 (XCB) window backend */
#define VK_USE_PLATFORM_XCB_KHR
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include "gui.h"

#include <vulkan/vulkan_xcb.h>

static struct {
	xcb_connection_t *c;
	xcb_window_t win;
	xcb_atom_t del;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	int w, h;
	xcb_generic_event_t *pending;
} x;

static xcb_atom_t atom(const char *name)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(x.c,
		xcb_intern_atom(x.c, 0, strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_NONE;

	free(r);
	return a;
}

static void load_keymap(void)
{
	int32_t dev = xkb_x11_get_core_keyboard_device_id(x.c);

	xkb_state_unref(x.state);
	xkb_keymap_unref(x.keymap);
	x.keymap = xkb_x11_keymap_new_from_device(x.xkb, x.c, dev, 0);
	x.state = x.keymap ? xkb_x11_state_new_from_device(x.keymap, x.c, dev)
	    : NULL;
}

/* open the display and map the window */
int plat_init(const char *title, int w, int h)
{
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals[2];
	xcb_screen_t *scr;
	xcb_atom_t proto;

	x.c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(x.c)) {
		warnx("cannot connect to the X server");
		return -1;
	}
	scr = xcb_setup_roots_iterator(xcb_get_setup(x.c)).data;
	x.w = w;
	x.h = h;
	x.win = xcb_generate_id(x.c);
	vals[0] = scr->black_pixel;
	vals[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	    XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS |
	    XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION;
	xcb_create_window(x.c, XCB_COPY_FROM_PARENT, x.win, scr->root, 0, 0, w,
			  h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->root_visual, mask, vals);
	xcb_change_property(x.c, XCB_PROP_MODE_REPLACE, x.win,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
			    strlen(title), title);
	xcb_change_property(x.c, XCB_PROP_MODE_REPLACE, x.win,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 10,
			    "epis\0epis\0");
	proto = atom("WM_PROTOCOLS");
	x.del = atom("WM_DELETE_WINDOW");
	xcb_change_property(x.c, XCB_PROP_MODE_REPLACE, x.win, proto,
			    XCB_ATOM_ATOM, 32, 1, &x.del);
	xkb_x11_setup_xkb_extension(x.c, XKB_X11_MIN_MAJOR_XKB_VERSION,
				    XKB_X11_MIN_MINOR_XKB_VERSION, 0, NULL,
				    NULL, NULL, NULL);
	x.xkb = xkb_context_new(0);
	load_keymap();
	xcb_map_window(x.c, x.win);
	xcb_flush(x.c);
	return 0;
}

static int translate(xcb_generic_event_t *g, struct event *e)
{
	memset(e, 0, sizeof(*e));
	switch (g->response_type & 0x7f) {
	case XCB_EXPOSE:
		e->type = E_EXPOSE;
		return 1;
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *c = (void *)g;

		if (c->width == x.w && c->height == x.h)
			return 0;
		x.w = c->width;
		x.h = c->height;
		e->type = E_RESIZE;
		return 1;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *c = (void *)g;

		if (c->data.data32[0] != x.del)
			return 0;
		e->type = E_CLOSE;
		return 1;
	}
	case XCB_MAPPING_NOTIFY:
		load_keymap();
		return 0;
	case XCB_KEY_PRESS: {
		xcb_key_press_event_t *k = (void *)g;

		if (!x.state)
			return 0;
		xkb_state_update_mask(x.state, k->state & 0xff, 0, 0, 0, 0,
				      (k->state >> 13) & 3);
		e->type = E_KEY;
		e->pressed = 1;
		xkb_event(e, xkb_state_key_get_one_sym(x.state, k->detail));
		xkb_state_key_get_utf8(x.state, k->detail, e->text,
				       sizeof(e->text));
		e->mods = (k->state & XCB_MOD_MASK_SHIFT ? MOD_SHIFT : 0) |
		    (k->state & XCB_MOD_MASK_CONTROL ? MOD_CTRL : 0) |
		    (k->state & XCB_MOD_MASK_1 ? MOD_ALT : 0) |
		    (k->state & XCB_MOD_MASK_4 ? MOD_SUPER : 0);
		return 1;
	}
	case XCB_BUTTON_PRESS:
	case XCB_BUTTON_RELEASE: {
		xcb_button_press_event_t *b = (void *)g;

		e->x = b->event_x;
		e->y = b->event_y;
		if (b->detail == 4 || b->detail == 5) {
			if ((g->response_type & 0x7f) != XCB_BUTTON_PRESS)
				return 0;
			e->type = E_SCROLL;
			e->dy = b->detail == 4 ? -15 : 15;
			return 1;
		}
		e->type = E_BUTTON;
		e->button = b->detail;
		e->pressed = (g->response_type & 0x7f) == XCB_BUTTON_PRESS;
		return 1;
	}
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *m = (void *)g;

		e->type = E_MOTION;
		e->x = m->event_x;
		e->y = m->event_y;
		return 1;
	}
	}
	return 0;
}

int plat_next(struct event *e)
{
	xcb_generic_event_t *g;

	while ((g = xcb_poll_for_event(x.c))) {
		int ok = translate(g, e);

		free(g);
		if (ok)
			return 1;
	}
	return 0;
}

/* sleep until there is input or one of the file descriptors is readable */
int plat_wait(const int *fds, int nfds)
{
	struct pollfd p[9];
	int i;

	xcb_flush(x.c);
	p[0].fd = xcb_get_file_descriptor(x.c);
	p[0].events = POLLIN;
	for (i = 0; i < nfds && i < 8; i++) {
		p[i + 1].fd = fds[i];
		p[i + 1].events = POLLIN;
	}
	if (poll(p, i + 1, -1) < 0)
		return errno == EINTR ? 0 : -1;
	return xcb_connection_has_error(x.c) ? -1 : 0;
}

/* X11 has no global menu bar, the menu is drawn in the window */
int plat_menu(const char *const *titles, const struct menu_item *const *items)
{
	(void)titles;
	(void)items;
	return 0;
}

void plat_size(int *w, int *h)
{
	*w = x.w;
	*h = x.h;
}

float plat_scale(void)
{
	return cfg.scale > 0 ? cfg.scale : 1;
}

const char *plat_vk_ext(void)
{
	return VK_KHR_XCB_SURFACE_EXTENSION_NAME;
}

int plat_vk_surface(VkInstance inst, VkSurfaceKHR *s)
{
	VkXcbSurfaceCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR };

	ci.connection = x.c;
	ci.window = x.win;
	return vkCreateXcbSurfaceKHR(inst, &ci, NULL, s) == VK_SUCCESS ? 0 : -1;
}

void plat_quit(void)
{
	xkb_state_unref(x.state);
	xkb_keymap_unref(x.keymap);
	xkb_context_unref(x.xkb);
	xcb_disconnect(x.c);
}
