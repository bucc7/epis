/* xkb.c - keysym translation shared by the Linux backends */
#include <xkbcommon/xkbcommon.h>

#include "gui.h"

/* fill the key and cp members of a key event from an XKB keysym */
void xkb_event(struct event *e, uint32_t sym)
{
	static const struct {
		uint32_t sym, key;
	} sp[] = {
		{ XKB_KEY_Return, K_CR }, { XKB_KEY_KP_Enter, K_CR },
		{ XKB_KEY_Escape, K_ESC }, { XKB_KEY_BackSpace, K_BS },
		{ XKB_KEY_Tab, K_TAB }, { XKB_KEY_ISO_Left_Tab, K_TAB },
		{ XKB_KEY_Delete, K_DEL }, { XKB_KEY_Up, K_UP },
		{ XKB_KEY_Down, K_DOWN }, { XKB_KEY_Left, K_LEFT },
		{ XKB_KEY_Right, K_RIGHT }, { XKB_KEY_Home, K_HOME },
		{ XKB_KEY_End, K_END }, { XKB_KEY_Prior, K_PAGEUP },
		{ XKB_KEY_Next, K_PAGEDOWN }, { XKB_KEY_space, K_SPACE },
	};
	size_t i;

	e->key = K_NONE;
	e->cp = xkb_keysym_to_utf32(xkb_keysym_to_lower(sym));
	for (i = 0; i < sizeof(sp) / sizeof(*sp); i++)
		if (sym == sp[i].sym)
			e->key = sp[i].key;
	if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
		e->key = K_F1 + (sym - XKB_KEY_F1);
}
