/* objc.h - calling Cocoa and Metal from C through the Objective-C runtime */
#ifndef OBJC_H
#define OBJC_H

#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>

typedef unsigned long nsuint;
typedef long nsint;

/* objc_msgSend cast to the signature of the method being called */
#define MSG(ret, ...) ((ret (*)(__VA_ARGS__))(void (*)(void))objc_msgSend)

static inline SEL sel(const char *name)
{
	return sel_registerName(name);
}

static inline id cls(const char *name)
{
	return (id)objc_getClass(name);
}

/* send a message without arguments that returns an object */
static inline id get(id o, const char *s)
{
	return MSG(id, id, SEL)(o, sel(s));
}

/* send a message without arguments or result */
static inline void call(id o, const char *s)
{
	MSG(void, id, SEL)(o, sel(s));
}

static inline void set_id(id o, const char *s, id v)
{
	MSG(void, id, SEL, id)(o, sel(s), v);
}

static inline void set_int(id o, const char *s, nsint v)
{
	MSG(void, id, SEL, nsint)(o, sel(s), v);
}

static inline void set_bool(id o, const char *s, BOOL v)
{
	MSG(void, id, SEL, BOOL)(o, sel(s), v);
}

static inline id new_object(const char *name)
{
	return get(get(cls(name), "alloc"), "init");
}

static inline id nsstr(const char *s)
{
	return MSG(id, id, SEL, const char *)(cls("NSString"),
		sel("stringWithUTF8String:"), s);
}

static inline const char *utf8(id s)
{
	return s ? MSG(const char *, id, SEL)(s, sel("UTF8String")) : "";
}

#endif
