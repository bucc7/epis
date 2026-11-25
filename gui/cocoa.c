/* cocoa.c - Cocoa window backend and system menu bar for macOS */
#include <ctype.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>

#include "gui.h"
#include "objc.h"

#define QLEN 256

/* NSEventModifierFlags */
#define NS_SHIFT (1UL << 17)
#define NS_CONTROL (1UL << 18)
#define NS_OPTION (1UL << 19)
#define NS_COMMAND (1UL << 20)

#define NS_APP_DEFINED 15
/* trackpad distance in points that counts as one wheel step */
#define SCROLL_STEP 40.0

extern id const NSDefaultRunLoopMode;

static struct {
	id app, win, view, layer, delegate;
	const struct menu_item *const *menu;
	struct event q[QLEN];
	int qh, qt, waiting;
	double scroll;
} mac;

/* post an empty event so that a blocked plat_wait returns */
static void wake(void)
{
	CGPoint zero = { 0, 0 };
	id ev;

	ev = MSG(id, id, SEL, nsuint, CGPoint, nsuint, double, nsint, id,
		 short, nsint, nsint)(cls("NSEvent"),
		sel("otherEventWithType:location:modifierFlags:timestamp:"
		    "windowNumber:context:subtype:data1:data2:"),
		NS_APP_DEFINED, zero, 0, 0, 0, NULL, 0, 0, 0);
	MSG(void, id, SEL, id, BOOL)(mac.app, sel("postEvent:atStart:"), ev,
				     0);
}

static void push(struct event *e)
{
	if ((mac.qt + 1) % QLEN == mac.qh)
		return;
	mac.q[mac.qt] = *e;
	mac.qt = (mac.qt + 1) % QLEN;
	if (mac.waiting) {
		mac.waiting = 0;
		wake();
	}
}

static void push_type(int type)
{
	struct event e = { .type = type };

	push(&e);
}

static CGRect view_bounds(void)
{
#ifdef __x86_64__
	CGRect r;

	((void (*)(CGRect *, id, SEL))(void (*)(void))objc_msgSend_stret)
	    (&r, mac.view, sel("bounds"));
	return r;
#else
	return MSG(CGRect, id, SEL)(mac.view, sel("bounds"));
#endif
}

static double backing_scale(void)
{
	return MSG(double, id, SEL)(mac.win, sel("backingScaleFactor"));
}

/* translate the function key code points that AppKit reports */
static uint32_t special_key(uint32_t c)
{
	static const struct {
		uint32_t c, key;
	} sp[] = {
		{ '\r', K_CR }, { 0x03, K_CR }, { 0x1b, K_ESC },
		{ 0x7f, K_BS }, { '\t', K_TAB }, { 0x19, K_TAB },
		{ ' ', K_SPACE }, { 0xf700, K_UP }, { 0xf701, K_DOWN },
		{ 0xf702, K_LEFT }, { 0xf703, K_RIGHT }, { 0xf728, K_DEL },
		{ 0xf729, K_HOME }, { 0xf72b, K_END }, { 0xf72c, K_PAGEUP },
		{ 0xf72d, K_PAGEDOWN },
	};
	size_t i;

	for (i = 0; i < sizeof(sp) / sizeof(*sp); i++)
		if (c == sp[i].c)
			return sp[i].key;
	if (c >= 0xf704 && c <= 0xf70f)
		return K_F1 + (c - 0xf704);
	return K_NONE;
}

/*
 * Option composes characters on many layouts (@ [ ] { } on European
 * ones), so it only counts as Alt with Control or with a special key.
 */
static void key_down(id self, SEL cmd, id ev)
{
	struct event e = { .type = E_KEY, .pressed = 1 };
	nsuint f = MSG(nsuint, id, SEL)(ev, sel("modifierFlags"));
	const char *chars = utf8(get(ev, "characters"));
	const char *base = utf8(get(ev, "charactersIgnoringModifiers"));
	uint32_t c = 0, cp = 0;

	(void)self;
	(void)cmd;
	if (f & NS_COMMAND)
		return;
	if (*base)
		utf8_decode(base, &cp);
	if (*chars)
		utf8_decode(chars, &c);
	e.key = special_key(c == 0x03 ? c : cp);
	e.cp = cp < 0x80 ? (uint32_t)tolower((int)cp) : cp;
	if (f & NS_SHIFT)
		e.mods |= MOD_SHIFT;
	if (f & NS_CONTROL)
		e.mods |= MOD_CTRL;
	if ((f & NS_OPTION) && (e.key != K_NONE || (f & NS_CONTROL)))
		e.mods |= MOD_ALT;
	/* function keys come as private use code points, not as text */
	if (!(c >= 0xf700 && c <= 0xf8ff) && strlen(chars) < sizeof(e.text))
		strcpy(e.text, chars);
	push(&e);
}

/* position of a mouse event in pixels from the top left of the view */
static void mouse_pos(id ev, struct event *e)
{
	CGPoint p = MSG(CGPoint, id, SEL)(ev, sel("locationInWindow"));
	double s = backing_scale();

	p = MSG(CGPoint, id, SEL, CGPoint, id)(mac.view,
		sel("convertPoint:fromView:"), p, NULL);
	e->x = p.x * s;
	e->y = p.y * s;
}

static void mouse_button(id ev, int button, int pressed)
{
	struct event e = { .type = E_BUTTON };

	mouse_pos(ev, &e);
	e.button = button;
	e.pressed = pressed;
	push(&e);
}

/* Control-click is a right click */
static int left_button(id ev)
{
	return MSG(nsuint, id, SEL)(ev, sel("modifierFlags")) & NS_CONTROL ?
	    3 : 1;
}

static void mouse_down(id self, SEL cmd, id ev)
{
	(void)self;
	(void)cmd;
	mouse_button(ev, left_button(ev), 1);
}

static void mouse_up(id self, SEL cmd, id ev)
{
	(void)self;
	(void)cmd;
	mouse_button(ev, left_button(ev), 0);
}

static void right_down(id self, SEL cmd, id ev)
{
	(void)self;
	(void)cmd;
	mouse_button(ev, 3, 1);
}

static void right_up(id self, SEL cmd, id ev)
{
	(void)self;
	(void)cmd;
	mouse_button(ev, 3, 0);
}

static void mouse_moved(id self, SEL cmd, id ev)
{
	struct event e = { .type = E_MOTION };

	(void)self;
	(void)cmd;
	mouse_pos(ev, &e);
	push(&e);
}

static void scroll_wheel(id self, SEL cmd, id ev)
{
	struct event e = { .type = E_SCROLL };
	double d = -MSG(double, id, SEL)(ev, sel("scrollingDeltaY"));

	(void)self;
	(void)cmd;
	if (MSG(BOOL, id, SEL)(ev, sel("hasPreciseScrollingDeltas"))) {
		mac.scroll += d;
		if (mac.scroll > -SCROLL_STEP && mac.scroll < SCROLL_STEP)
			return;
		d = mac.scroll;
		mac.scroll = 0;
	}
	if (d == 0)
		return;
	mouse_pos(ev, &e);
	e.dy = d < 0 ? -15 : 15;
	push(&e);
}

static BOOL yes(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return 1;
}

static BOOL should_close(id self, SEL cmd, id sender)
{
	(void)self;
	(void)cmd;
	(void)sender;
	push_type(E_CLOSE);
	return 0;
}

/* quitting from the Dock or at logout goes through the event loop */
static nsuint should_terminate(id self, SEL cmd, id sender)
{
	(void)self;
	(void)cmd;
	(void)sender;
	push_type(E_CLOSE);
	return 0;
}

/*
 * AppKit runs its own loop during a live resize, so the frame is drawn
 * from this callback.
 */
static void resized(id self, SEL cmd, id note)
{
	(void)self;
	(void)cmd;
	(void)note;
	MSG(void, id, SEL, double)(mac.layer, sel("setContentsScale:"),
				   backing_scale());
	app.dirty = 1;
	push_type(E_RESIZE);
	ui_refresh();
}

static void exposed(id self, SEL cmd, id note)
{
	(void)self;
	(void)cmd;
	(void)note;
	push_type(E_EXPOSE);
}

/* the tag of a menu item is 100 * menu + item, or -1 for Quit */
static void menu_action(id self, SEL cmd, id sender)
{
	struct event e = { .type = E_COMMAND };
	nsint tag = MSG(nsint, id, SEL)(sender, sel("tag"));

	(void)self;
	(void)cmd;
	e.cmd = tag < 0 ? "quit" : mac.menu[tag / 100][tag % 100].cmd;
	push(&e);
}

static void method(Class c, const char *name, void (*fn)(void),
		   const char *types)
{
	class_addMethod(c, sel(name), (IMP)fn, types);
}

#define METHOD(c, name, fn, types) method(c, name, (void (*)(void))fn, types)

/* define the NSView and delegate subclasses at run time */
static void make_classes(void)
{
	Class v = objc_allocateClassPair((Class)cls("NSView"), "EpisView", 0);
	Class d = objc_allocateClassPair((Class)cls("NSObject"),
					 "EpisDelegate", 0);

	METHOD(v, "acceptsFirstResponder", yes, "B@:");
	METHOD(v, "isFlipped", yes, "B@:");
	METHOD(v, "keyDown:", key_down, "v@:@");
	METHOD(v, "mouseDown:", mouse_down, "v@:@");
	METHOD(v, "mouseUp:", mouse_up, "v@:@");
	METHOD(v, "rightMouseDown:", right_down, "v@:@");
	METHOD(v, "rightMouseUp:", right_up, "v@:@");
	METHOD(v, "mouseMoved:", mouse_moved, "v@:@");
	METHOD(v, "mouseDragged:", mouse_moved, "v@:@");
	METHOD(v, "scrollWheel:", scroll_wheel, "v@:@");
	objc_registerClassPair(v);
	METHOD(d, "windowShouldClose:", should_close, "B@:@");
	METHOD(d, "applicationShouldTerminate:", should_terminate, "Q@:@");
	METHOD(d, "windowDidResize:", resized, "v@:@");
	METHOD(d, "windowDidChangeBackingProperties:", resized, "v@:@");
	METHOD(d, "windowDidChangeOcclusionState:", exposed, "v@:@");
	METHOD(d, "windowDidExpose:", exposed, "v@:@");
	METHOD(d, "menuAction:", menu_action, "v@:@");
	objc_registerClassPair(d);
}

/* create the application and its window with a Metal layer */
int plat_init(const char *title, int w, int h)
{
	id pool = new_object("NSAutoreleasePool");
	CGRect frame = { { 0, 0 }, { 0, 0 } };

	frame.size.width = w;
	frame.size.height = h;
	mac.app = get(cls("NSApplication"), "sharedApplication");
	if (!get(cls("NSScreen"), "mainScreen")) {
		call(pool, "drain");
		warnx("cannot connect to the window server");
		return -1;
	}
	set_int(mac.app, "setActivationPolicy:", 0);
	make_classes();
	mac.delegate = new_object("EpisDelegate");
	set_id(mac.app, "setDelegate:", mac.delegate);
	set_bool(cls("NSWindow"), "setAllowsAutomaticWindowTabbing:", 0);
	/* titled, closable, miniaturizable and resizable; buffered */
	mac.win = MSG(id, id, SEL, CGRect, nsuint, nsuint, BOOL)(
		get(cls("NSWindow"), "alloc"),
		sel("initWithContentRect:styleMask:backing:defer:"), frame,
		1 | 2 | 4 | 8, 2, 0);
	set_bool(mac.win, "setReleasedWhenClosed:", 0);
	set_id(mac.win, "setTitle:", nsstr(title));
	mac.view = MSG(id, id, SEL, CGRect)(get(cls("EpisView"), "alloc"),
		sel("initWithFrame:"), frame);
	mac.layer = get(get(cls("CAMetalLayer"), "layer"), "retain");
	set_id(mac.view, "setLayer:", mac.layer);
	set_bool(mac.view, "setWantsLayer:", 1);
	set_id(mac.win, "setContentView:", mac.view);
	set_id(mac.win, "setDelegate:", mac.delegate);
	set_id(mac.win, "makeFirstResponder:", mac.view);
	set_bool(mac.win, "setAcceptsMouseMovedEvents:", 1);
	MSG(void, id, SEL, double)(mac.layer, sel("setContentsScale:"),
				   backing_scale());
	call(mac.win, "center");
	call(mac.app, "finishLaunching");
	set_id(mac.win, "makeKeyAndOrderFront:", NULL);
	set_bool(mac.app, "activateIgnoringOtherApps:", 1);
	call(pool, "drain");
	return 0;
}

static id menu_new(const char *title)
{
	return MSG(id, id, SEL, id)(get(cls("NSMenu"), "alloc"),
				    sel("initWithTitle:"), nsstr(title));
}

/* append an item; action NULL leaves it without one */
static id menu_add(id menu, const char *title, const char *action,
		   const char *key)
{
	return MSG(id, id, SEL, id, SEL, id)(menu,
		sel("addItemWithTitle:action:keyEquivalent:"), nsstr(title),
		action ? sel(action) : NULL, nsstr(key));
}

static void menu_command_item(id menu, const char *title, const char *key,
			      nsint tag)
{
	id item = menu_add(menu, title, "menuAction:", key);

	set_id(item, "setTarget:", mac.delegate);
	set_int(item, "setTag:", tag);
}

static id submenu(id bar, const char *title)
{
	id item = menu_add(bar, title, NULL, ""), menu = menu_new(title);

	set_id(item, "setSubmenu:", menu);
	return menu;
}

/* install the menus in the system menu bar instead of in the window */
int plat_menu(const char *const *titles, const struct menu_item *const *items)
{
	id pool = new_object("NSAutoreleasePool");
	id bar = menu_new(""), menu;
	int i, j;

	mac.menu = items;
	menu = submenu(bar, "epis");
	menu_add(menu, "Hide epis", "hide:", "h");
	menu_add(menu, "Show All", "unhideAllApplications:", "");
	set_id(menu, "addItem:", get(cls("NSMenuItem"), "separatorItem"));
	menu_command_item(menu, "Quit epis", "q", -1);
	for (i = 0; titles[i]; i++) {
		menu = submenu(bar, titles[i]);
		for (j = 0; items[i][j].label; j++) {
			const char *c = items[i][j].cmd;

			/* Quit and the in-window menu bar do not apply */
			if (!strcmp(c, "quit") || !strcmp(c, "menubar"))
				continue;
			menu_command_item(menu, items[i][j].label, "",
					  i * 100 + j);
		}
		if (!strcmp(titles[i], "Window")) {
			menu_add(menu, "Minimize", "performMiniaturize:", "m");
			set_id(mac.app, "setWindowsMenu:", menu);
		}
	}
	set_id(mac.app, "setMainMenu:", bar);
	call(pool, "drain");
	return 1;
}

/* hand the next application event to AppKit, waiting until date */
static int pump(const char *date)
{
	id ev = MSG(id, id, SEL, nsuint, id, id, BOOL)(mac.app,
		sel("nextEventMatchingMask:untilDate:inMode:dequeue:"),
		~0UL, get(cls("NSDate"), date), NSDefaultRunLoopMode, 1);

	if (!ev)
		return 0;
	set_id(mac.app, "sendEvent:", ev);
	return 1;
}

int plat_next(struct event *e)
{
	id pool = new_object("NSAutoreleasePool");

	while (mac.qh == mac.qt && pump("distantPast"))
		;
	call(pool, "drain");
	if (mac.qh == mac.qt)
		return 0;
	*e = mac.q[mac.qh];
	mac.qh = (mac.qh + 1) % QLEN;
	return 1;
}

static void fd_ready(CFFileDescriptorRef f, CFOptionFlags flags, void *info)
{
	(void)f;
	(void)flags;
	(void)info;
	wake();
}

/*
 * sleep until there is input or one of the file descriptors is readable,
 * watching the descriptors from the run loop
 */
int plat_wait(const int *fds, int nfds)
{
	id pool = new_object("NSAutoreleasePool");
	CFFileDescriptorRef ref[8];
	CFRunLoopSourceRef src[8];
	int i, n = nfds < 8 ? nfds : 8;

	for (i = 0; i < n; i++) {
		ref[i] = CFFileDescriptorCreate(NULL, fds[i], 0, fd_ready,
						NULL);
		CFFileDescriptorEnableCallBacks(ref[i],
						kCFFileDescriptorReadCallBack);
		src[i] = CFFileDescriptorCreateRunLoopSource(NULL, ref[i], 0);
		CFRunLoopAddSource(CFRunLoopGetCurrent(), src[i],
				   kCFRunLoopDefaultMode);
	}
	if (mac.qh == mac.qt) {
		mac.waiting = 1;
		pump("distantFuture");
		mac.waiting = 0;
	}
	for (i = 0; i < n; i++) {
		CFFileDescriptorInvalidate(ref[i]);
		CFRelease(src[i]);
		CFRelease(ref[i]);
	}
	call(pool, "drain");
	return 0;
}

/* size of the view in pixels */
void plat_size(int *w, int *h)
{
	CGRect r = view_bounds();
	double s = backing_scale();

	*w = r.size.width * s;
	*h = r.size.height * s;
}

float plat_scale(void)
{
	return backing_scale() * (cfg.scale > 0 ? cfg.scale : 1);
}

void *plat_metal_layer(void)
{
	return mac.layer;
}

void plat_quit(void)
{
	set_id(mac.win, "setDelegate:", NULL);
	call(mac.win, "close");
}
