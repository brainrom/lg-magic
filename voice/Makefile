PKGS := libpipewire-0.3 libudev sbc

PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
override CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
override LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
UNITDIR ?= $(PREFIX)/lib/systemd/user

all: lg-magic-voice

lg-magic-voice: lg_magic_voice.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: lg-magic-voice
	install -D -m 0755 lg-magic-voice $(DESTDIR)$(BINDIR)/lg-magic-voice
	install -D -m 0644 lg-magic-voice.service \
		$(DESTDIR)$(UNITDIR)/lg-magic-voice.service

install-bin: lg-magic-voice
	install -D -m 0755 lg-magic-voice $(DESTDIR)$(BINDIR)/lg-magic-voice

clean:
	rm -f lg-magic-voice

.PHONY: all install install-bin clean
