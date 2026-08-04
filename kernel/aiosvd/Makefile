# Out-of-tree build for AlmaLinux 9 / RHEL 9 (kernel 5.14.x).
# Build aios_http first so Module.symvers is available.

obj-m += aiosvd.o
aiosvd-y := main.o ctl.o disk.o io.o oids.o clients.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
HTTP_SYMVERS ?= $(PWD)/../aios_http/Module.symvers

all:
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(HTTP_SYMVERS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(HTTP_SYMVERS) modules_install
	depmod -a

.PHONY: all clean install
