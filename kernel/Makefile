obj-m += lg_magic.o

lg_magic-objs := lg_magic_main.o lg_magic_airmouse.o

KDIR ?= /lib/modules/$(KVER)/build
KVER ?= $(shell uname -r)
PWD  := $(shell pwd)

CFLAGS_lg_magic_airmouse.o += $(CC_FLAGS_FPU)
ccflags-remove-y += -mgeneral-regs-only

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
