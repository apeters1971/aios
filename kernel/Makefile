# Build both out-of-tree modules (AlmaLinux 9 / RHEL 9).
#   sudo dnf install -y kernel-devel-$(uname -r) gcc make elfutils-libelf-devel
#   make -C kernel

.PHONY: all clean install

all:
	$(MAKE) -C aios_http
	$(MAKE) -C aiosfs

clean:
	$(MAKE) -C aios_http clean
	$(MAKE) -C aiosfs clean

install:
	$(MAKE) -C aios_http install
	$(MAKE) -C aiosfs install
