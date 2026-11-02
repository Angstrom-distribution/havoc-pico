VERSION = "0.7.0-git"

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

CFLAGS ?= -g -O2
EMOJI_DIR ?= /usr/share/wvkbd/emoji
# pixfont-none.o leaves the bitmap fonts (about 120 KB of data) out
PIXFONT_OBJ ?= pixfont-data.o
# PICOWL_PALETTE_XML names picowl-palette-v1.xml, the protocol of picowl's C8
# output (no file here: it comes with picowl). Without it havoc has no C8 path.
PICOWL_PALETTE_XML ?=
ifneq ($(PICOWL_PALETTE_XML),)
PALETTE_GEN = picowl-palette-v1.h picowl-palette-v1.c
PALETTE_OBJ = picowl-palette-v1.o
PALETTE_DEFS = -DHAVE_PICOWL_PALETTE
endif
# Pictures in the terminal: sixel (DCS q) and a subset of the kitty graphics
# protocol (APC G). HAVOC_PNG adds PNG (f=100) and zlib (o=z) to kitty with
# libpng. Each is also off at run time until [graphics] turns it on.
# graphics.o and kplace.o, the glue to the window, are added with that code.
HAVOC_SIXEL ?= 0
HAVOC_KITTY ?= 0
HAVOC_PNG ?= 0
GFX_DEFS = -DHAVOC_SIXEL=$(HAVOC_SIXEL) -DHAVOC_KITTY=$(HAVOC_KITTY) -DHAVOC_PNG=$(HAVOC_PNG)
GFX_LIBS =
GFX_OBJ =
ifeq ($(HAVOC_SIXEL),1)
GFX_OBJ += sixel.o
endif
ifeq ($(HAVOC_KITTY),1)
GFX_OBJ += kitty.o base64.o kitty-shm.o
endif
ifeq ($(HAVOC_PNG),1)
ifneq ($(HAVOC_KITTY),1)
$(error HAVOC_PNG=1 needs HAVOC_KITTY=1)
endif
GFX_OBJ += kitty-png.o
GFX_LIBS = libpng zlib
endif
CDEFS = -DVERSION='$(VERSION)' -D_XOPEN_SOURCE=700 -DEMOJI_DIR='"$(EMOJI_DIR)"' $(PALETTE_DEFS) $(GFX_DEFS)

WAYLAND_PROTOCOLS_DIR != $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols

LIBRARIES = wayland-client wayland-cursor xkbcommon $(GFX_LIBS)
PKG_CFLAGS != $(PKG_CONFIG) --cflags $(LIBRARIES)
PKG_LIBS != $(PKG_CONFIG) --libs $(LIBRARIES)

LIBS = -lm -lutil $(PKG_LIBS)

XML = \
	xdg-shell.xml \
	xdg-decoration-unstable-v1.xml \
	primary-selection-unstable-v1.xml \
	viewporter.xml \
	fractional-scale-v1.xml \
	text-input-unstable-v3.xml

GEN = \
	xdg-shell.h \
	xdg-shell.c \
	xdg-decoration-unstable-v1.h \
	xdg-decoration-unstable-v1.c \
	primary-selection-unstable-v1.h \
	primary-selection-unstable-v1.c \
	viewporter.h \
	viewporter.c \
	fractional-scale-v1.h \
	fractional-scale-v1.c \
	text-input-unstable-v3.h \
	text-input-unstable-v3.c \
	$(PALETTE_GEN)

OBJ = \
	main.o \
	glyph.o \
	emoji.o \
	pixfont.o \
	c8.o \
	$(PIXFONT_OBJ) \
	xdg-shell.o \
	xdg-decoration-unstable-v1.o \
	primary-selection-unstable-v1.o \
	viewporter.o \
	fractional-scale-v1.o \
	text-input-unstable-v3.o \
	$(PALETTE_OBJ) \
	$(GFX_OBJ) \
	tsm/wcwidth.o \
	tsm/shl-htable.o \
	tsm/tsm-render.o \
	tsm/tsm-screen.o \
	tsm/tsm-selection.o \
	tsm/tsm-unicode.o \
	tsm/tsm-vte-charsets.o \
	tsm/tsm-vte.o

.SUFFIXES:
.SUFFIXES: .xml .h .c .o

havoc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

.c.o:
	$(CC) $(PKG_CFLAGS) $(CFLAGS) $(CDEFS) -c $< -o $@

.xml.c:
	$(WAYLAND_SCANNER) private-code < $< > $@

.xml.h:
	$(WAYLAND_SCANNER) client-header < $< > $@

xdg-shell.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/$@ $@

xdg-decoration-unstable-v1.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/$@ $@

primary-selection-unstable-v1.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/unstable/primary-selection/$@ $@

viewporter.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/stable/viewporter/$@ $@

fractional-scale-v1.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/staging/fractional-scale/$@ $@

text-input-unstable-v3.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/unstable/text-input/$@ $@

picowl-palette-v1.xml:
	cp $(PICOWL_PALETTE_XML) $@

install: havoc
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 havoc $(DESTDIR)$(BINDIR)/havoc

uninstall:
	rm $(DESTDIR)$(BINDIR)/havoc

clean:
	rm -f havoc $(XML) $(GEN) $(OBJ) sixel.o kitty.o base64.o kitty-shm.o kitty-png.o picowl-palette-v1.xml picowl-palette-v1.h picowl-palette-v1.c picowl-palette-v1.o

.PHONY: install uninstall clean
