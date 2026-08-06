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

## Parent pointers and recursive directory stats

Every inode persists `parent_ino` (primary parent directory; `0` for root). Create/mkdir set it; cross-directory rename updates it; hard `link` leaves the primary parent unchanged.

Directories also store lazy rollups `rbytes` / `rfiles` / `rdirs` / `rtime_ns` in inode JSON. Mutators mark the affected directory dirty; a mount-local timer (`aios_posix_config.rstat_interval_ms`, default **60000**, `0` disables) recomputes dirty dirs from `DirTable` children and cascades via `parent_ino`. Final flush on unmount (and via `aios_posix_flush_rstats`).

Exposed as **virtual** xattrs on directories (not writable): `aios.rbytes`, `aios.rfiles`, `aios.rdirs`, `aios.rtime` (decimal; `rtime` is Unix seconds). `listxattr` includes these four for directories; `setxattr` / `removexattr` return `-EPERM`.

## flock

`aios_posix_flock(ino, op)` maps `LOCK_SH` / `LOCK_EX` / `LOCK_UN` (+ optional `LOCK_NB`) onto exclusive AIOS object locks on the inode OID. Shared and exclusive are both exclusive at the cluster layer. Tokens are tracked per mount and released on `LOCK_UN` / unmount / inode delete. Non-blocking contention returns `-EWOULDBLOCK`.

## Subtree layout (meta vs data)

POSIX mounts apply **path-prefix** placement rules (longest match) separately for:

- **meta** — inode JSON and directory tips (`posix/{vol}/ino/…`, `dir/…`)
- **data** — file chunks (`posix/{vol}/data/…`)

Rules live in cluster object `posix/layout_rules` (YAML `posix_layout_rules` seeds when empty). Edit via `aios admin posix-layout show|set` or the Web UI **POSIX layout** tab. Each rule may set `layout` (`replica`/`ec`), `storage_class`, and EC fields; omitted fields use cluster defaults. Optional `volume` scopes a rule.

**Rename across domains:** if the source and destination paths match rules whose meta/data placement differs, `rename` returns **`-EXDEV`** so tools copy instead of moving tips across storage classes/layouts. Same-domain rename (including cross-directory `/txn`) is unchanged.

There is **no dedicated MDS**: directory and inode metadata are regular objects with changelog tips, placed by the same rules as above.

## Cross-directory rename

Uses the cluster **multi-object transaction** API (`POST /txn` … commit):

1. Lock both directories’ `meta` + `log` oids (sorted) so changelog appends cannot interleave.
2. Reload dentries under those locks.
3. Prepare a **compact rewrite** of each directory tip (`snap` = full map, empty `log`, updated `meta`) plus parent inode mtime/`nlink` updates (and optional victim inode delete / nlink drop).
4. `commit` publishes tips in oid-sorted order.

Same-directory rename remains a single changelog `Rename` op. Commit still has the general `/txn` v1 torn-window if publish fails mid-commit (see [`http.md`](http.md)).

## Volume / subtree snapshot

`aios_posix_snapshot(fs, id_out, id_len)` freezes the volume (`super.frozen`), copies live oids to `posix/{vol}/.snap/{id}/` (excluding other snaps), then unfreezes. `aios_posix_snapshot_at(fs, path, …)` limits the copy to a volume-relative subtree (`"/"` = whole volume). Mutating ops return `-EBUSY` while frozen. Used by cluster backup ([`backup.md`](backup.md)).

## Consistency notes (intentional POSIX relaxations)

- **Cross-directory `link`** is still best-effort (not multi-object atomic).
- **`fsync`** drops the local inode cache; chunk PUTs are already durable per-object, not a multi-object transaction.
- Victim file **chunk GC** after a replacing cross-dir rename runs after commit (directory tips are consistent first).

## Caller credentials and mode checks

Gateways (FUSE, S3, XRootD OSS) set a **thread-scoped caller** before ops:

```c
typedef struct aios_posix_cred { uint32_t uid; uint32_t gid; } aios_posix_cred;

void aios_posix_set_caller(aios_posix_fs* fs, uint32_t uid, uint32_t gid);
void aios_posix_clear_caller(aios_posix_fs* fs);  /* revert to mount defaults */
aios_posix_cred aios_posix_get_caller(const aios_posix_fs* fs);
int aios_posix_access(aios_posix_fs* fs, uint64_t ino, int amode);  /* R_OK/W_OK/X_OK/F_OK */
```

- Stored **thread-local**, keyed by `fs*` (multi-mount safe).
- If unset on a thread: fall back to mount `aios_posix_config.uid/gid` (single-principal clients).
- **uid 0** bypasses mode checks (root). Primary **gid only** (no supplementary groups / ACLs yet).
- `create` / `mkdir` own new inodes as the caller.
- Reads/writes/truncates require R/W vs mode; directory mutations need W+X on the parent; sticky bit (`S_ISVTX`) restricts unlink/rename of others’ entries; `chmod` is owner/root, `chown` is root-only.
- Cluster object HMAC auth is unchanged (mount = trusted client). Callers are an application-layer principal for POSIX mode bits.

**FUSE** sets the caller from `fuse_get_context()->uid/gid` on each op. **S3** sets IAM `uid`/`gid`, or `0:0` for the root access key. The S3 volume root is mode `1777` (sticky) so IAM principals can create buckets they own; multipart staging is `0777`. **XRootD** (`libXrdAios` OSS under stock XrdOfs) maps `XrdSecEntity::name` through the local passwd DB (`getpwnam_r`) to uid/gid — see [`xrd_oss.md`](xrd_oss.md).

## C ABI

```c
aios_posix_fs* aios_posix_mount(const aios_posix_config*, int* err_out);
void aios_posix_set_caller / clear_caller / get_caller;
int aios_posix_access(fs, ino, amode);
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

Also accepts `AIOS_ENDPOINT` / `AIOS_CLUSTER_KEY`. Optional: `stripe_unit`, `stripe_width`, `app_label`. Process credentials from the kernel (`fuse_get_context`) drive permission checks.

## Kernel prototype (AlmaLinux 9)

Out-of-tree modules: `aios_http.ko` (in-kernel HTTP/HMAC) and `aiosfs.ko` (`mount -t aios`).

- `backend=http` — VFS uses `aios_http` directly (same object layout as `libaios_posix`, including cross-directory rename via `/txn`).
- `backend=upcall` (default) — upcalls to `aios-kbridge` over `/dev/aios_bridge` ([`kernel/aios_kabi.h`](../kernel/aios_kabi.h)), which calls the `aios_posix_*` ABI.

See [`kernel/README.md`](../kernel/README.md).

## Library

- `libaios_posix` — ABI implementation over `Session`
- `libaios_client` — HTTP session (`put_bytes`, `delete_object`, `list_prefix`, …)
- Header install: `include/aios/aios_posix.h` (+ `aios_kabi.h` when kbridge is enabled)
