# AIOS kernel modules (AlmaLinux 9)

Out-of-tree modules for **AlmaLinux 9 / RHEL 9** (kernel **5.14.x**).

| Module | Role |
|--------|------|
| [`aios_http/`](aios_http/) | In-kernel HTTP/1.1 + AIOS-HMAC-SHA256 (+ locks / `/txn`) |
| [`aiosfs/`](aiosfs/) | POSIX filesystem type `aios` (`backend=upcall` or `backend=http`) |
| [`aiosvd/`](aiosvd/) | **AIOS Volume Device** — block device over object stripes (`/dev/aiosvdN`) |

```text
Filesystem:
  backend=upcall:  aiosfs  ⇄  /dev/aios_bridge  ⇄  aios-kbridge  →  libaios_posix  →  cluster
  backend=http:    aiosfs  →  aios_http  →  cluster

Block device:
  aios-vd map …  →  /dev/aiosvd_ctl  →  aiosvd.ko  →  aios_http  →  cluster
                   →  /dev/aiosvdN
```

## Build (on AlmaLinux 9)

```bash
sudo dnf install -y kernel-devel-$(uname -r) gcc make elfutils-libelf-devel
cd kernel
make            # aios_http, aiosfs, aiosvd
sudo make install
```

### DKMS

```bash
sudo dnf install -y dkms
cd kernel
sudo make dkms-install    # adds + builds aios-kernel/0.4.0 for $(uname -r)
# remove: sudo make dkms-remove
```

[`dkms.conf`](dkms.conf) builds `aios_http`, `aiosfs`, and `aiosvd` into `/extra`. **Secure Boot** still requires module signing (or SB disabled); DKMS does not sign modules by itself.

Userspace helpers (from repo root):

```bash
cmake -S . -B build -DAIOS_WITH_KBRIDGE=ON
cmake --build build --target aios-kbridge aios-vd -j
```

## Filesystem — in-kernel HTTP

```bash
sudo insmod aios_http/aios_http.ko
sudo insmod aiosfs/aiosfs.ko
sudo mount -t aios none /mnt/aios \
  -o backend=http,endpoint=127.0.0.1:7480,cluster_key=$KEY,volume=default
```

Prefer a numeric IPv4 `endpoint=` (hostname resolution needs `CONFIG_DNS_RESOLVER`).

## Filesystem — upcall + aios-kbridge

```bash
sudo insmod aios_http/aios_http.ko
sudo insmod aiosfs/aiosfs.ko
sudo ./build/aios-kbridge
sudo mount -t aios none /mnt/aios \
  -o backend=upcall,endpoint=127.0.0.1:7480,cluster_key=$KEY,volume=default
```

Mount options: `endpoint`, `cluster_key`, `backend=upcall|http`, `volume`, `app_label`, `stripe_unit`, `stripe_width`, `uid`, `gid`.

## Block device — aiosvd

`aiosvd` presents a Linux block device whose sectors are striped across AIOS objects (same idea as a remote block volume, without borrowing Ceph’s naming).

**Object layout**

| OID | Contents |
|-----|----------|
| `vd/{pool}/{name}/header` | JSON: `size`, `obj_order`, optional `parent_pool`/`parent_name`, `key_id` |
| `vd/{pool}/{name}/data.{objno}` | Fixed-size data stripe (sparse; missing ⇒ zeros, or parent COW) |

Object size = `1 << obj_order` (default 4 MiB). Lightweight clones store a child header with a parent ref; first write to a shared object materializes a child copy (COW).

**I/O path**

- blk-mq with dual workqueues (`req_wq` + `stripe_wq`); `io_opt` / readahead tuned toward object size
- Shared `aios_http` client pool (keep-alive TCP, ~30s timeouts, reconnect on error)
- 16-entry per-device object cache; FLUSH/FUA/unmap/resize flush dirty entries
- Partial writes prefer `Content-Range` PUT (`aios_http_put_range`); full-object RMW only for COW
- Discard / WRITE_ZEROES → delete or zero-out object slices (thin provisioning)
- QoS: `--queue-depth` / `--max-clients` cap tag-set depth and HTTP pool size
- `key_id` is stored/logged only (no in-kernel AES)
- Stats via `aios-vd info` / `list`, and sysfs `…/aiosvd/stats` (includes timeouts/reconnects/cache_hits)

**Map a volume**

```bash
sudo insmod aios_http/aios_http.ko
sudo insmod aiosvd/aiosvd.ko

# create header + map → /dev/aiosvd0
sudo ./build/aios-vd map \
  --endpoint 127.0.0.1:7480 --key "$KEY" \
  --pool default --name disk1 --size 1G --create --excl

sudo mkfs.xfs /dev/aiosvd0
sudo mount /dev/aiosvd0 /mnt/vd

sudo ./build/aios-vd info 0
sudo ./build/aios-vd list
# grow (or shrink; shrink deletes trailing data.* objects best-effort)
sudo ./build/aios-vd resize 0 --size 2G

# lightweight COW clone → /dev/aiosvd1
sudo ./build/aios-vd clone 0 --pool default --name disk1-snap

sudo umount /mnt/vd
sudo ./build/aios-vd unmap 1
sudo ./build/aios-vd unmap 0
sudo rmmod aiosvd aios_http
```

Re-map an existing volume (header already in the cluster) by omitting `--create` / `--size`.
Flags: `--readonly`, `--excl` (with `--create`), `--key-id`, `--queue-depth`, `--max-clients`.
Also: `rename DEV --pool P --name N`.

Stress helper: [`../tools/aios_vd_stress.sh`](../tools/aios_vd_stress.sh) (map → fio randrw → discard → resize → unmap).

Control device: `/dev/aiosvd_ctl` ([`aiosvd_uapi.h`](aiosvd_uapi.h) ioctls: map/unmap/info/list/resize/clone/rename). Userspace tool: [`../tools/aios_vd.cpp`](../tools/aios_vd.cpp).

## Layout

| Path | Role |
|------|------|
| [`aios_kabi.h`](aios_kabi.h) | Upcall request/reply ABI (incl. xattr ops 18–21) |
| [`aiosvd_uapi.h`](aiosvd_uapi.h) | aiosvd map/unmap/info/list/resize/clone/rename ioctl ABI |
| [`aios_http/`](aios_http/) | Shared HTTP client (keep-alive, timeouts, `put_range`, pool) |
| [`aiosfs/`](aiosfs/) | VFS module |
| [`aiosvd/`](aiosvd/) | Block device module (blk-mq) |
| [`dkms.conf`](dkms.conf) | DKMS package `aios-kernel` |
| [`../tools/aios_kbridge.cpp`](../tools/aios_kbridge.cpp) | Userspace daemon for `backend=upcall` |
| [`../tools/aios_vd.cpp`](../tools/aios_vd.cpp) | aiosvd CLI |
| [`../tools/aios_vd_stress.sh`](../tools/aios_vd_stress.sh) | aiosvd fio/discard/resize stress |

## Status / limits (prototype)

- **aios_http**: keep-alive TCP, ~30s send/recv timeouts, reconnect-on-error; `Content-Range` PUT; shared `aios_http_pool` used by aiosfs + aiosvd.
- **aiosfs page cache**: buffered I/O + `writepage`/`writepages`; HTTP writeback groups dirty pages by stripe chunk and flushes in parallel via the pool; `O_DIRECT` via `IOCB_DIRECT`; `fsync` waits for writeback.
- **aiosfs xattrs**: `user.*` / `trusted.*` via 5.14 `s_xattr` handlers; HTTP stores base64 values in inode meta JSON (`xattrs`); upcall uses `AIOS_OP_*XATTR` → `aios_posix_*xattr`.
- **aiosfs locks**: advisory POSIX/`flock` via kernel `locks_lock_file_wait` — **node-local only**, not cluster-wide.
- **aiosfs densening**: HTTP hardlinks (`link(2)`); `fallocate` punch-hole + `KEEP_SIZE` best-effort on HTTP (else `-EOPNOTSUPP`); prealloc not implemented.
- **aiosfs `backend=http`**: POSIX subset including cross-directory rename via `/txn` and hardlinks; mount holds one pool client for meta, leaves the rest for parallel writeback.
- **aiosvd**: workqueue + keep-alive pool + range PUT + 16-entry object cache; discard/WRITE_ZEROES; COW clones; readonly/excl/QoS map flags; `key_id` hook only (no AES); max 32 volumes.
- Secure Boot: modules must be signed or SB disabled for `insmod` / DKMS-loaded modules.
- No true HTTP/1.1 pipelining on one socket; no cluster-wide `fcntl` locks; no in-kernel volume encryption.
