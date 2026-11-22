PKGS := libudev libsystemd sbc
MODULE_PKGS := libpipewire-0.3 libsystemd

PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
HARDEN_CFLAGS ?= -fstack-protector-strong -D_FORTIFY_SOURCE=2
HARDEN_LDFLAGS ?= -Wl,-z,relro,-z,now
override CFLAGS += $(HARDEN_CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PKGS))
override LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

MODULE_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(MODULE_PKGS))
MODULE_LIBS := $(shell $(PKG_CONFIG) --libs $(MODULE_PKGS))

MODULE := libpipewire-module-lgmagic.so

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSTEMD_UNIT_DIR ?= $(PREFIX)/lib/systemd/system
FIFO_DROPIN_DIR ?= $(SYSTEMD_UNIT_DIR)/lgmagicd.service.d
PIPEWIRE_CONF_DIR ?= /etc/pipewire/pipewire.conf.d
WIREPLUMBER_CONF_DIR ?= /etc/wireplumber/wireplumber.conf.d
PIPEWIRE_MODULEDIR := $(shell $(PKG_CONFIG) --variable=moduledir libpipewire-0.3)

MODULEDIR ?= $(if $(PIPEWIRE_MODULEDIR),$(PIPEWIRE_MODULEDIR),/usr/lib/pipewire-0.3)
UDEV_RULES_DIR ?= /etc/udev/rules.d
SYSUSERS_DIR ?= /usr/lib/sysusers.d
DBUS_POLICY_DIR ?= /etc/dbus-1/system.d

all: lgmagicd $(MODULE)

lgmagicd: lgmagicd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(HARDEN_LDFLAGS) $(LDLIBS)

$(MODULE): lg-magic-voice-module.c
	$(CC) $(CFLAGS) $(MODULE_CFLAGS) -fPIC -shared -o $@ $< \
		$(LDFLAGS) $(HARDEN_LDFLAGS) $(MODULE_LIBS)

lgmagicd.service: lgmagicd.service.in force
	sed 's|@BINDIR@|$(BINDIR)|g' $< > $@

lgmagicd-fifo.conf: lgmagicd-fifo.conf.in force
	sed 's|@BINDIR@|$(BINDIR)|g' $< > $@

force:

install: install-core install-plugin

install-core: lgmagicd lgmagicd.service
	install -D -m 0755 lgmagicd $(DESTDIR)$(BINDIR)/lgmagicd
	install -D -m 0644 lgmagicd.service \
		$(DESTDIR)$(SYSTEMD_UNIT_DIR)/lgmagicd.service
	install -D -m 0644 52-lgmagic-hidraw.rules \
		$(DESTDIR)$(UDEV_RULES_DIR)/52-lgmagic-hidraw.rules
	install -D -m 0644 lgmagic.conf \
		$(DESTDIR)$(SYSUSERS_DIR)/lgmagic.conf
	install -D -m 0644 org.lgmagic.conf \
		$(DESTDIR)$(DBUS_POLICY_DIR)/org.lgmagic.conf

install-plugin: $(MODULE) 60-lg-magic-voice.conf
	install -D -m 0755 $(MODULE) \
		$(DESTDIR)$(MODULEDIR)/$(MODULE)
	install -D -m 0644 60-lg-magic-voice.conf \
		$(DESTDIR)$(WIREPLUMBER_CONF_DIR)/60-lg-magic-voice.conf

install-fifo: install-core lgmagicd-fifo.conf
	install -D -m 0644 50-lg-magic-voice.conf \
		$(DESTDIR)$(PIPEWIRE_CONF_DIR)/50-lg-magic-voice.conf
	install -D -m 0644 lgmagicd-fifo.conf \
		$(DESTDIR)$(FIFO_DROPIN_DIR)/50-lg-magic-fifo.conf

install-all: install install-fifo

clean:
	rm -f lgmagicd $(MODULE) lgmagicd.service lgmagicd-fifo.conf

.PHONY: all install install-core install-plugin install-fifo install-all \
	clean force
