# epis build configuration

VERSION = 0.1.0
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

CC = cc
GLSLC = glslc
PKG_CONFIG = pkg-config

# graphical client: 1 to build it, 0 for a headless CLI-only binary
GUI = 1

# Linux windowing backend: wayland or x11
WINDOW = wayland

CFLAGS = -std=c11 -pedantic -Wall -Wextra -O2
CPPFLAGS = -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS =
