# POSIX filesystem client + FUSE3

AIOS exposes a **C-portable POSIX ABI** (`aios_posix.h`) that maps a hierarchical filesystem onto objects. A FUSE3 helper (`aios-fuse`) mounts that ABI on Linux/macOS when `libfuse3` is available.

Root directory is always **inode 1**.

## Object layout

Volume name from mount config (`volume`, default `default`):

```text
posix/{vol}/super                 # next_ino, stripe defaults, uuid (CAS: aios.posix.cas)
posix/{vol}/ino/{id}              # inode JSON meta (incl. base64 xattrs map)
posix/{vol}/dir/{id}/meta|log|snap  # directory dentry changelog
posix/{vol}/data/{id}/c/{chunk}   # file data chunk (chunk = offset / stripe_unit)
```

Inode locks for `flock` use the AIOS HTTP lock on `posix/{vol}/ino/{id}`.

## Striping

- Default **stripe_unit** = 1 MiB, **stripe_width** = 4 (max concurrent chunk PUTs).
- Each chunk is a separate object → parallel HTTP and natural CH distribution.
- Small writes RMW the chunk object; inode `size`/`mtime` updated with CAS.

## Directories

Append-only dentry changelog (same wire framing as STL `AOPk` records):

| Op | Meaning |
|----|---------|
| Link(name, ino) | create/lookup bind |
| Unlink(name) | remove dentry |
| Rename(old, new) | same-directory rename (single batch) |

Snapshot = JSON `{"entries":{name:ino,...}}`; auto-compact ~1 MiB.

## Hard links

- Regular files only (`link` on a directory returns `-EPERM`).
- Same- or cross-directory: bump `nlink`, then append a `Link` dentry. Cross-directory is best-effort (not multi-object atomic).
- Last `unlink` deletes inode meta and chunk objects.

## Extended attributes

Stored in inode JSON as `xattrs: { name: base64(value) }` (opaque bytes). ABI: `aios_posix_setxattr` / `getxattr` / `listxattr` / `removexattr` with `AIOS_POSIX_XATTR_CREATE` / `REPLACE`. Limits: name ≤ 255, value ≤ 64 KiB, ≤ 128 attrs per inode.

## flock

`aios_posix_flock(ino, op)` maps `LOCK_SH` / `LOCK_EX` / `LOCK_UN` (+ optional `LOCK_NB`) onto exclusive AIOS object locks on the inode OID. Shared and exclusive are both exclusive at the cluster layer. Tokens are tracked per mount and released on `LOCK_UN` / unmount / inode delete. Non-blocking contention returns `-EWOULDBLOCK`.

## Cross-directory rename

Uses the cluster **multi-object transaction** API (`POST /txn` … commit):

1. Lock both directories’ `meta` + `log` oids (sorted) so changelog appends cannot interleave.
2. Reload dentries under those locks.
3. Prepare a **compact rewrite** of each directory tip (`snap` = full map, empty `log`, updated `meta`) plus parent inode mtime/`nlink` updates (and optional victim inode delete / nlink drop).
4. `commit` publishes tips in oid-sorted order.

Same-directory rename remains a single changelog `Rename` op. Commit still has the general `/txn` v1 torn-window if publish fails mid-commit (see [`http.md`](http.md)).

## Consistency notes (intentional POSIX relaxations)

- **Cross-directory `link`** is still best-effort (not multi-object atomic).
- **`fsync`** drops the local inode cache; chunk PUTs are already durable per-object, not a multi-object transaction.
- Victim file **chunk GC** after a replacing cross-dir rename runs after commit (directory tips are consistent first).

## C ABI

```c
aios_posix_fs* aios_posix_mount(const aios_posix_config*, int* err_out);
int aios_posix_lookup(fs, parent, name, &st);
int aios_posix_link / setxattr / getxattr / listxattr / removexattr / flock;
int aios_posix_read/write/truncate/...
```

Errors are **negative errno**. The ABI avoids Boost/STL so a future kernel port can keep the same surface in C.

## FUSE3

```bash
# Build with libfuse3 (pkg-config fuse3)
aios-fuse -o endpoint=127.0.0.1:7480,cluster_key=$KEY,volume=default /mnt/aios
```

Also accepts `AIOS_ENDPOINT` / `AIOS_CLUSTER_KEY`. Optional: `stripe_unit`, `stripe_width`, `app_label`.

## Kernel prototype (AlmaLinux 9)

Out-of-tree modules: `aios_http.ko` (in-kernel HTTP/HMAC) and `aiosfs.ko` (`mount -t aios`).

- `backend=http` — VFS uses `aios_http` directly (same object layout as `libaios_posix`).
- `backend=upcall` (default) — upcalls to `aios-kbridge` over `/dev/aios_bridge` ([`kernel/aios_kabi.h`](../kernel/aios_kabi.h)), which calls the `aios_posix_*` ABI.

See [`kernel/README.md`](../kernel/README.md).

## Library

- `libaios_posix` — ABI implementation over `Session`
- `libaios_client` — HTTP session (`put_bytes`, `delete_object`, `list_prefix`, …)
- Header install: `include/aios/aios_posix.h` (+ `aios_kabi.h` when kbridge is enabled)
