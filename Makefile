VERSION    = 1.1.0

PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
DATADIR   ?= $(PREFIX)/share
DESTDIR   ?=

CC         ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS     ?= -O2 -g

GIO_CFLAGS  := $(shell $(PKG_CONFIG) --cflags gio-2.0 gio-unix-2.0)
GIO_LIBS    := $(shell $(PKG_CONFIG) --libs gio-2.0 gio-unix-2.0)
GTK_CFLAGS  := $(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS    := $(shell $(PKG_CONFIG) --libs gtk4)

RC_CFLAGS = -std=gnu11 -Wall -Wextra -DRC_VERSION='"$(VERSION)"' $(CPPFLAGS) $(CFLAGS)

all: realcopy realcopy-gui

build/%.o: src/%.c src/copy.h | build
	$(CC) $(RC_CFLAGS) $(GTK_CFLAGS) $(GIO_CFLAGS) -c -o $@ $<

build:
	mkdir -p build

realcopy: build/cli.o build/copy.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GIO_LIBS)

realcopy-gui: build/gui.o build/copy.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(GTK_LIBS)

check: realcopy
	TMPDIR=$(CURDIR)/build tests/run.sh ./realcopy

install: all
	install -Dm755 realcopy $(DESTDIR)$(BINDIR)/realcopy
	install -Dm755 realcopy-gui $(DESTDIR)$(BINDIR)/realcopy-gui
	install -Dm644 data/realcopy.desktop $(DESTDIR)$(DATADIR)/applications/realcopy.desktop
	install -Dm644 data/realcopy.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/realcopy.svg

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/realcopy $(DESTDIR)$(BINDIR)/realcopy-gui \
	      $(DESTDIR)$(DATADIR)/applications/realcopy.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/realcopy.svg

# Include paths for clangd and other editor tooling.
compile_flags.txt:
	printf '%s\n' -std=gnu11 $(GTK_CFLAGS) $(GIO_CFLAGS) > $@

clean:
	rm -rf build realcopy realcopy-gui

.PHONY: all check install uninstall clean
