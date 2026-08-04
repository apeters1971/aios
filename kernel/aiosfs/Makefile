# Out-of-tree build for AlmaLinux 9 / RHEL 9 (kernel 5.14.x).
#   sudo dnf install kernel-devel gcc make elfutils-libelf-devel
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

obj-m += aiosfs.o
aiosfs-y := main.o upcall.o super.o inode.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
