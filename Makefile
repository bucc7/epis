# epis - mail client
.POSIX:

include config.mk

SRC = src/main.c src/util.c src/json.c src/config.c src/net.c src/auth.c \
      src/imap.c src/smtp.c src/maildir.c src/msg.c src/autoconf.c \
      src/llm.c src/fuzzy.c src/cli.c
PKGS = openssl
LIBS = -lresolv
GEN =

GUI_SRC_1 = gui/ui.c gui/view.c gui/edit.c gui/render.c gui/font.c \
            gui/clay.c gui/$(WINDOW).c
GUI_PKGS_1 = vulkan freetype2 xkbcommon
GUI_GEN_1 = gui/ui.vert.inc gui/ui.frag.inc $(GEN_$(WINDOW))
GUI_DEF_1 = -DEPIS_GUI

PKGS_wayland = wayland-client wayland-cursor
PKGS_x11 = xcb xkbcommon-x11
WLPROTO = $$($(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
GEN_wayland = gui/xdg-shell.h gui/xdg-shell.c gui/xdg-decoration.h \
              gui/xdg-decoration.c

ALLSRC = $(SRC) $(GUI_SRC_$(GUI))
ALLPKGS = $(PKGS) $(GUI_PKGS_$(GUI)) $(PKGS_$(WINDOW)_$(GUI))
PKGS_wayland_1 = $(PKGS_wayland)
PKGS_x11_1 = $(PKGS_x11)
OBJ = $(ALLSRC:.c=.o) $(PROTO_OBJ_$(WINDOW)_$(GUI))
PROTO_OBJ_wayland_1 = gui/xdg-shell.o gui/xdg-decoration.o

EPIS_CPPFLAGS = $(CPPFLAGS) $(GUI_DEF_$(GUI)) -DVERSION=\"$(VERSION)\" \
                -Isrc -Ilib -Igui $$($(PKG_CONFIG) --cflags $(ALLPKGS))
EPIS_LIBS = $$($(PKG_CONFIG) --libs $(ALLPKGS)) $(LIBS) -lm

all: epis

.c.o:
	$(CC) $(CFLAGS) $(EPIS_CPPFLAGS) -c $< -o $@

$(OBJ): src/epis.h config.mk $(GUI_GEN_$(GUI))
gui/ui.o gui/view.o gui/edit.o gui/render.o gui/font.o gui/$(WINDOW).o: \
	gui/gui.h

epis: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(EPIS_LIBS)

gui/ui.vert.inc: shaders/ui.vert
	$(GLSLC) -mfmt=c -o $@ shaders/ui.vert
gui/ui.frag.inc: shaders/ui.frag
	$(GLSLC) -mfmt=c -o $@ shaders/ui.frag

gui/xdg-shell.h:
	wayland-scanner client-header $(WLPROTO)/stable/xdg-shell/xdg-shell.xml $@
gui/xdg-shell.c:
	wayland-scanner private-code $(WLPROTO)/stable/xdg-shell/xdg-shell.xml $@
gui/xdg-decoration.h:
	wayland-scanner client-header \
		$(WLPROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@
gui/xdg-decoration.c:
	wayland-scanner private-code \
		$(WLPROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

# protocol and Clay sources are generated or vendored: relax warnings
gui/xdg-shell.o gui/xdg-decoration.o gui/clay.o:
	$(CC) $(CFLAGS) -w $(EPIS_CPPFLAGS) -c $(@:.o=.c) -o $@

clean:
	rm -f epis $(SRC:.c=.o) gui/*.o gui/*.inc gui/xdg-*.[ch]

install: epis
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(MANPREFIX)/man1
	cp -f epis $(DESTDIR)$(PREFIX)/bin/epis
	chmod 755 $(DESTDIR)$(PREFIX)/bin/epis
	cp -f epis.1 $(DESTDIR)$(MANPREFIX)/man1/epis.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/epis.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/epis $(DESTDIR)$(MANPREFIX)/man1/epis.1

format:
	indent -linux -l80 src/*.c src/*.h gui/ui.c gui/view.c gui/edit.c \
		gui/render.c gui/font.c gui/wayland.c gui/x11.c gui/gui.h

.PHONY: all clean install uninstall format
