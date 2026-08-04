# aiosfs — AlmaLinux 9 kernel modules

Out-of-tree VFS + HTTP client for **AlmaLinux 9 / RHEL 9** (kernel **5.14.x**).

Two modules:

| Module | Role |
|--------|------|
| [`aios_http/`](aios_http/) | In-kernel HTTP/1.1 + AIOS-HMAC-SHA256 client (`get`/`put`/`delete`/`head`/range, 307 redirects) |
| [`aiosfs/`](aiosfs/) | Filesystem type `aios` with two backends |

```text
backend=upcall (default):
  mount -t aios  →  aiosfs.ko  ⇄  /dev/aios_bridge  ⇄  aios-kbridge  →  libaios_posix  →  cluster

backend=http:
  mount -t aios  →  aiosfs.ko  →  aios_http.ko  →  TCP/HTTP  →  cluster
```

## Build (on AlmaLinux 9)

```bash
sudo dnf install -y kernel-devel-$(uname -r) gcc make elfutils-libelf-devel
cd kernel
make            # builds aios_http then aiosfs
sudo make install
# or: sudo insmod aios_http/aios_http.ko && sudo insmod aiosfs/aiosfs.ko
```

Userspace bridge (only needed for `backend=upcall`):

```bash
cmake -S . -B build -DAIOS_WITH_KBRIDGE=ON
cmake --build build --target aios-kbridge -j
```

## Run — in-kernel HTTP (no bridge)

```bash
sudo insmod kernel/aios_http/aios_http.ko
sudo insmod kernel/aiosfs/aiosfs.ko

sudo mkdir -p /mnt/aios
sudo mount -t aios none /mnt/aios \
  -o backend=http,endpoint=127.0.0.1:7480,cluster_key=$KEY,volume=default

ls /mnt/aios
echo hi | sudo tee /mnt/aios/hello.txt

sudo umount /mnt/aios
sudo rmmod aiosfs aios_http
```

Prefer a numeric IPv4 `endpoint=` (hostname resolution needs `CONFIG_DNS_RESOLVER`).

## Run — upcall + aios-kbridge

```bash
sudo insmod kernel/aios_http/aios_http.ko   # still required (aiosfs links its API)
sudo insmod kernel/aiosfs/aiosfs.ko
sudo ./build/aios-kbridge

sudo mount -t aios none /mnt/aios \
  -o backend=upcall,endpoint=127.0.0.1:7480,cluster_key=$KEY,volume=default
```

## Mount options

| Option | Notes |
|--------|--------|
| `endpoint` | `HOST:PORT` (required) |
| `cluster_key` | HMAC key (required) |
| `backend` | `upcall` (default) or `http` |
| `volume`, `app_label`, `stripe_unit`, `stripe_width`, `uid`, `gid` | same as userspace POSIX |

## Layout

| Path | Role |
|------|------|
| [`aios_kabi.h`](aios_kabi.h) | Upcall request/reply ABI |
| [`aios_http/`](aios_http/) | HTTP client module + [`aios_http_api.h`](aios_http/aios_http_api.h) exports |
| [`aiosfs/`](aiosfs/) | VFS module (`main`, `upcall`, `super`, `inode`, `http_backend`, `io`, `pagecache`) |
| [`../tools/aios_kbridge.cpp`](../tools/aios_kbridge.cpp) | Userspace daemon for `backend=upcall` |

## Status / limits (prototype)

- **Page cache**: both backends use buffered I/O (`generic_file_read_iter` / `generic_file_write_iter`) with `address_space_operations` writeback (`readpage` / `writepage` / `writepages`). `fsync` waits for dirty pages then syncs metadata (and upcall `AIOS_OP_FSYNC` when `backend=upcall`).
- **`backend=http`**: mount, lookup, create/mkdir, unlink/rmdir, same-dir rename, read/write, setattr/truncate. Directory mutations rewrite compact snap+meta (compatible with libaios_posix). Cross-directory rename returns `-EOPNOTSUPP` (needs `/txn`; use `backend=upcall`).
- Secure Boot: modules must be signed or SB disabled for `insmod`.
