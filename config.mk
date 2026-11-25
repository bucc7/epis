# epis build configuration

VERSION = 0.1.0
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

# detected host system, used to pick the defaults below
UNAME = $(shell uname -s)

CC = cc
GLSLC = glslc
PKG_CONFIG = $(PKG_CONFIG_$(UNAME))
PKG_CONFIG_Linux = pkg-config
PKG_CONFIG_Darwin = pkgconf

# graphical client: 1 to build it, 0 for a headless CLI-only binary
GUI = 1

# windowing backend: wayland or x11 on Linux, cocoa on macOS
WINDOW = $(WINDOW_$(UNAME))
WINDOW_Linux = wayland
WINDOW_Darwin = cocoa

CFLAGS = -std=c11 -pedantic -Wall -Wextra -O2
CPPFLAGS = -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
LDFLAGS =
