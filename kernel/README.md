# aiosfs — AlmaLinux 9 kernel prototype

First out-of-tree VFS prototype for **AlmaLinux 9 / RHEL 9** (kernel **5.14.x**).

The kernel module does **not** speak HTTP. It registers filesystem type `aios` and a misc device `/dev/aios_bridge`. VFS operations are upcalled to userspace **`aios-kbridge`**, which calls `libaios_posix` (same ABI as FUSE).

```text
mount -t aios  →  aiosfs.ko  ⇄  /dev/aios_bridge  ⇄  aios-kbridge  →  libaios_posix  →  cluster
```

## Build (on AlmaLinux 9)

```bash
# Kernel module
sudo dnf install -y kernel-devel-$(uname -r) gcc make elfutils-libelf-devel
cd kernel/aiosfs
make
sudo make install   # or: sudo insmod ./aiosfs.ko

# Userspace bridge (from repo root, on the same host)
cmake -S . -B build -DAIOS_WITH_KBRIDGE=ON
cmake --build build --target aios-kbridge -j
```

## Run

```bash
# 1) cluster up, then start the bridge (must be running before mount)
sudo ./build/aios-kbridge

# 2) mount
sudo mkdir -p /mnt/aios
sudo mount -t aios none /mnt/aios \
  -o endpoint=127.0.0.1:7480,cluster_key=$KEY,volume=default

# 3) use as a normal filesystem
ls /mnt/aios
echo hi | sudo tee /mnt/aios/hello.txt

sudo umount /mnt/aios
sudo rmmod aiosfs
```

Mount options: `endpoint`, `cluster_key` (required), `volume`, `app_label`, `stripe_unit`, `stripe_width`, `uid`, `gid`.

## Layout

| Path | Role |
|------|------|
| [`aios_kabi.h`](aios_kabi.h) | Shared request/reply opcodes + structs |
| [`aiosfs/`](aiosfs/) | Out-of-tree module (`main`, `upcall`, `super`, `inode`) |
| [`../tools/aios_kbridge.cpp`](../tools/aios_kbridge.cpp) | Userspace daemon |

## Status / limits (prototype)

- Synchronous upcalls on the VFS path (no page cache writeback yet).
- One daemon open on `/dev/aios_bridge` at a time.
- Secure Boot: module must be signed or SB disabled for `insmod`.
- Not a pure in-kernel HTTP client — that remains future work; this proves the VFS + `aios_posix` ABI seam on el9.
