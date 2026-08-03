# POSIX filesystem client + FUSE3

AIOS exposes a **C-portable POSIX ABI** (`aios_posix.h`) that maps a hierarchical filesystem onto objects. A FUSE3 helper (`aios-fuse`) mounts that ABI on Linux/macOS when `libfuse3` is available.

Root directory is always **inode 1**.

## Object layout

Volume name from mount config (`volume`, default `default`):

```text
posix/{vol}/super                 # next_ino, stripe defaults, uuid (CAS: aios.posix.cas)
posix/{vol}/ino/{id}              # inode JSON meta
posix/{vol}/dir/{id}/meta|log|snap  # directory dentry changelog
posix/{vol}/data/{id}/c/{chunk}   # file data chunk (chunk = offset / stripe_unit)
```

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

## Consistency notes (intentional POSIX relaxations)

- **Cross-directory `rename` / replace** is best-effort: link into destination, then unlink source. A crash can leave two names or a dangling name.
- **`fsync`** drops the local inode cache; chunk PUTs are already durable per-object, not a multi-object transaction.
- No multi-object ACID for `rename`/`link` across directories.

## C ABI

```c
aios_posix_fs* aios_posix_mount(const aios_posix_config*, int* err_out);
int aios_posix_lookup(fs, parent, name, &st);
int aios_posix_read/write/truncate/...
```

Errors are **negative errno**. The ABI avoids Boost/STL so a future kernel port can keep the same surface in C.

## FUSE3

```bash
# Build with libfuse3 (pkg-config fuse3)
aios-fuse -o endpoint=127.0.0.1:7480,cluster_key=$KEY,volume=default /mnt/aios
```

Also accepts `AIOS_ENDPOINT` / `AIOS_CLUSTER_KEY`. Optional: `stripe_unit`, `stripe_width`, `app_label`.

## Library

- `libaios_posix` — ABI implementation over `Session`
- `libaios_client` — HTTP session (`put_bytes`, `delete_object`, `list_prefix`, …)
- Header install: `include/aios/aios_posix.h`
