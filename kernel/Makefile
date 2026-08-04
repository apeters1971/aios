# Build out-of-tree modules (AlmaLinux 9 / RHEL 9).
#   sudo dnf install -y kernel-devel-$(uname -r) gcc make elfutils-libelf-devel
#   make -C kernel
#
# DKMS (optional):
#   sudo dnf install -y dkms
#   sudo make -C kernel dkms-install

KDIR ?= /lib/modules/$(shell uname -r)/build
DKMS_NAME ?= aios-kernel
DKMS_VERSION ?= 0.4.0

.PHONY: all clean install dkms-add dkms-install dkms-remove

all:
	$(MAKE) -C aios_http
	$(MAKE) -C aiosfs
	$(MAKE) -C aiosvd

clean:
	$(MAKE) -C aios_http clean
	$(MAKE) -C aiosfs clean
	$(MAKE) -C aiosvd clean

install:
	$(MAKE) -C aios_http install
	$(MAKE) -C aiosfs install
	$(MAKE) -C aiosvd install

# Invoked by dkms.conf MAKE[0] with KDIR set to the target kernel build tree.
dkms_build:
	$(MAKE) -C aios_http KDIR=$(KDIR)
	$(MAKE) -C aiosfs KDIR=$(KDIR)
	$(MAKE) -C aiosvd KDIR=$(KDIR)

dkms-add:
	dkms add $(CURDIR)

dkms-install: dkms-add
	dkms install $(DKMS_NAME)/$(DKMS_VERSION) -k $(shell uname -r)

dkms-remove:
	-dkms remove $(DKMS_NAME)/$(DKMS_VERSION) --all
