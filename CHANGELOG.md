# Changelog

All notable changes to AIOS are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project has not yet cut a tagged
release, so everything sits under *Unreleased*.

Finding IDs (`OBJ-*`, `HTTP-*`, `STO-*`, `POS-*`, `KRN-*`) refer to the 2026-09-02 code review
(`docs/dev/CODE_REVIEW.md` is the earlier 2026-08 pass; the September findings live in the
review canvas and in the `tests/test_review2_*.cpp` regression tests). Items are listed by area
as the *problem that was found*; an ID appears here when a fix for it has landed or is in the
current fix cycle — check the regression test of the same name before relying on it.

## [Unreleased]

### Changed — `aios-store-bench` measures the engine the way the daemon uses it

`--threads N` (workers on disjoint objects; shards lock independently), `--no-fsync`
(`data_fsync=false`, SQLite `synchronous=OFF`) to separate engine cost from the durability write,
and `--op put|range|append|all`: random `--io-size` range writes and reads inside per-thread
objects, and appends to per-thread logs via `put_range` at the tail — the paths FUSE and STL
traffic actually take. Per-op timing with p50/p99 instead of phase totals.

### Changed — every client signs the body digest

The kernel module `aios_http`, the `aios` CLI and `aios-bench` used to sign `UNSIGNED-PAYLOAD`, so
the HMAC covered the request line and headers but not the bytes: an on-path party could swap a
PUT's payload under a valid signature (the CRC32C only catches accidents). They now send the body's
SHA-256 in `x-aios-content-sha256` like `aios::Session` already did, and the server verifies the
received bytes against it. Kernel: one `sha256` shash per client, one pass per request (the digest
of the empty body for bodiless requests). CLI: one extra read pass over the file before the upload.
Bench: signed by default so the numbers include the client-side hash; `--unsigned-payload` /
`"unsigned_payload": true` measures the wire alone. The process smoke test runs its daemon with
`http_require_signed_payload: true`, so a client that regresses to `UNSIGNED-PAYLOAD` fails CI.

Node-to-node RPC: requests already bound their raw trailer to the signed envelope (`sha256`,
required by `ObjectService::handle`); GET replies did not — a replica's ranged/full read (used
for client reads, recovery and EC repair) returned the bytes with a CRC32C only. The reply
envelope now carries the trailer's `sha256` too, and `parse_object_reply` (`verify_reply_trailer`)
rejects a signed reply that omits it or whose bytes do not hash to it (`code: auth`).

### Added — TLS on the HTTP API

`http_tls_cert` / `http_tls_key` (+ `http_tls_chain`, `http_tls_ca`) make the HTTP listener speak
TLS natively — object API, `/admin/*`, `/metrics` and `POST /auth/ticket` together, cluster-wide.
Nodes call each other's admin API with the same scheme (verifying against `http_tls_ca`, else their
own chain), redirects are issued as `https://`, and `GET /map` / `/admin/status` report `http_tls`.
On the client side an `https://` endpoint turns TLS on in `aios::Session` (`SessionConfig::tls`,
`tls_ca`, `tls_insecure`), `libaios_posix` (`aios_posix_config::tls_ca`, `AIOS_POSIX_F_TLS_INSECURE`),
the bench client, and the tools (`aios`, `aios-bench`, `aios-posix-fsck`: `--tls-ca` / `--tls-insecure`;
`aios-fuse`, `aios-fusell`: `-o tls_ca=…,tls_insecure`). Hostnames are verified (DNS via SAN/CN, IP
literals via IP SAN); the daemon's own loopback consumers (S3 gateway mount, admin-UI bench) connect
insecurely. `TlsStream` gained a client mode and moved into `aios_core`. The kernel modules stay plain
HTTP. Tests: `tests/test_http_tls.cpp`.

### Added — `aios-posix-fsck`

Offline consistency check and repair of a `libaios_posix` volume from its objects
(`aios::posix::fsck_volume`, `src/posix/posix_fsck.{hpp,cpp}`). Finds dangling dentries, orphan
inodes, orphan and stray chunks, stale directory objects, `nlink` and `parent_ino` mismatches, a
superblock allocator behind the highest inode, doubly linked directories and directory-log garbage;
`--repair` fixes the first eight with CAS-guarded writes and skips anything younger than `--min-age`
(a lease-deferred create or an in-flight write looks like damage for a moment). Exit codes follow
`fsck(8)`. See [`proto/posix_fuse.md`](proto/posix_fuse.md#fsck-aios-posix-fsck).

### Added — FUSE mounts exercised in CI

`tests/fuse_smoke.sh` (shared setup in `tests/fuse_env.sh`) brings up a single-node `aiosd`, mounts
`aios-fuse` and `aios-fusell` through the runner's `/dev/fuse`, and checks the POSIX behaviour the
in-process suite cannot reach through the VFS (page cache and partial rewrites, truncate, rename,
hard links, symlinks, unlink-while-open, xattrs, `chmod`/`utimens`, lease batching of many creates,
`fsyncdir`, and a second mount taking the directory lease back). The `linux` workflow job runs it
after `ctest`; `tests/fuse_pjdfstest.sh` runs pjdfstest groups against the same mount as an advisory
step with the TAP archive uploaded. Known gap surfaced by the script: `aios-fusell` does not keep the
data of an unlinked-while-open file (no libfuse `.fuse_hidden` rename on the low-level path).

### Changed — leases are scoped to the granting primary

An object lease (`POST /o/{oid}/lock`) used to be silently forgotten when the cluster map moved
the object to another primary or the primary restarted: the new primary did not know the token and
waved writes under it through, so a holder that had batched directory changes could land them on
top of a successor's. Tokens now carry the issuing `LockTable` instance id; a mutation or renew
under a token from another primary, from a previous incarnation, or on an object whose lease this
primary fenced because the map moved it (`LockTable::fence_if` on `update_cluster_map`) is refused
with **409** `lock_expired`. Acquire / renew return `epoch` and `primary`. `libaios_posix` and the
kernel client already treat `lock_expired` as a lost lease (re-sync, replay under a fresh lease);
`Review2Posix.DirLeaseLostToMapChangeIsReplayedUnderAFreshLease` covers the round trip.

### Added — consensus-backed cluster map (`monitors`)

The map epoch was a content hash of each node's own gossip view; two partitions could disagree
about who is primary and both serve writes. With `monitors` (3–5 TCP++ addresses) set, those
nodes run a small Raft-style register (`cluster/map_monitor`): elected leader, Pre-Vote so a
partitioned or restarted voter cannot unseat a healthy leader, monotonic epochs committed by a
majority and persisted (`map_state_file`), leader-granted primary leases (`map_lease_ms`, bounded
by the leader's own remaining lease), and an epoch that becomes *active* only once every node in
the map acknowledged it (or a lease period passed). Storage nodes gossip only to monitors and pull
the committed map. `ObjectService` gates every primary path on that lease (`503 no_map_lease` /
`map_transition`, retried by `Session` for `map_transition_wait_ms`), replicas reject RPCs older
than the highest committed epoch they have seen, and stale write grants are fenced.
`GET /map` reports `consensus`, `lease_valid`, `active_epoch`; the status file carries
`map_consensus`. Without `monitors` the gossip-derived map is kept, with a startup warning when
peers are configured.

### Added — client I/O path (`io_path: client`)

Replication and erasure coding can run as a **client data plane** while the object primary still coordinates seq, locks, preconditions, and tip publish. Cluster `io_path: server` (default) keeps today’s primary fan-out. `io_path: client` enables `POST /o/{oid}/prepare`, `PUT /o/{oid}/install`, `POST /o/{oid}/publish` (and abort) authenticated with an HMAC write grant; `libaios_client` `Session` follows `GET /map` (`io_path: auto`) or `SessionConfig::io_path`. Ordinary PUT / S3 / kernel HTTP still fan out on the primary. Range and append stay server-side.

### Added — sparse-range prefetch ioctl (`AIOS_IOC_PREFETCHV`)

One UAPI ([`kernel/aiosfs_uapi.h`](kernel/aiosfs_uapi.h)) lets an application pass up to 256 inline file ranges in a single `ioctl` so FUSE and kernel aiosfs see the complete vector before any backend fetch. `aios-fuse` / `aios-fusell` handle it as a restricted ioctl and `aios_posix_prefetchv` GETs the unique stripe chunks into the POSIX chunk cache; aiosfs `.unlocked_ioctl` maps the same struct onto unique chunks and populates the page cache (`AIOS_OP_PREFETCHV` on the upcall path). `libXrdAios` `ReadV` uses the same prefetch (ranges clipped to the file, then each iovec filled with `Read`) so sparse XRootD vector I/O is one backend batch instead of one GET per range. Ranges past EOF, empty lengths, unknown flags, and a total over 256 MiB are rejected before I/O.

### Added — `aios-fusell` (libfuse3 low-level)

Inode-based FUSE mount of `libaios_posix`, next to high-level `aios-fuse`. Same `-o` keys
(`endpoint`, `cluster_key`, `volume`, `nolease`, …) and the same directory-lease / writeback
behaviour; the kernel talks inodes instead of paths (`fuse_session_new`, `fuse_lowlevel_ops`).
`aios_posix_link_ino` is the hard-link primitive that path-based `aios_posix_link` now shares.

### Added — ticket authentication (cephx / Kerberos style)

- **Principals** replace handing `cluster_key` to clients. `aios admin principal
  create|list|rotate|delete` manages a cluster-wide keyring (object `auth/principals`, AES-256-GCM
  sealed under a key derived from `cluster_key`, refused over HTTP for every caller). Each principal
  has a role (`client` / `admin` / `node`) and optional oid-prefix caps. Works on every node, admin
  UI not required.
- **`POST /auth/ticket`**: one round trip, HMAC-only on the client. The principal key never
  travels; both sides derive a per-session key from two nonces, the server proves it knows the key
  (mutual auth), and the client gets an opaque AES-GCM ticket any node can open locally. Requests
  then use the existing `AIOS-HMAC-SHA256` header with `Credential=<ticket>` and the session key.
  Tickets default to 8 h (`http_ticket_lifetime_ms`), renew at half-life, and a captured ticket is
  useless without its session key. Grant attempts share the admin-login throttle and the replay
  cache.
- Roles and caps are enforced in the HTTP front end: `client` may not reach `/admin/*` or
  `/metrics` (`403 forbidden_role`), and with caps set may only touch matching oids and list inside
  them (`403 forbidden_cap`). Auth failures now carry a stable `code`.
- `http_shared_key_clients: loopback` refuses `cluster_key`-signed HTTP requests from non-loopback
  peers, so only principals can talk to the cluster from outside while the daemon's own gateway
  still works.
- Clients: `SessionConfig::principal` / `principal_key` (lazy grant, half-life renewal, one retry
  on `ticket_expired`); `aios --principal NAME --key HEX` (or `AIOS_PRINCIPAL` /
  `AIOS_PRINCIPAL_KEY`); kernel `aios_http` gains `aios_http_client_set_principal` /
  `aios_http_pool_set_principal` and `aiosfs` the `principal=,key=` mount options (http backend).
  The kernel auth header buffer moved off the stack to hold the ticket. `aiosvd` still uses the
  shared key (its map ioctl ABI has no principal field yet).
- Tests: `tests/test_ticket_auth.cpp` (seal/open, tamper, expiry, grant protocol, verifier policy,
  keyring at rest, end-to-end roles/caps/rotate/delete/replay/renewal, kernel canonical-string
  pin); the process smoke test now creates a principal and drives the CLI with it across a daemon
  restart.

### Added — native HTTPS on the S3 listener

- `s3_tls_cert` / `s3_tls_key` (+ optional `s3_tls_chain`) turn `s3_listen` into an HTTPS
  endpoint (TLS 1.2+, OpenSSL on the same blocking socket so `http_idle_timeout_ms` still bounds
  every read, including the handshake). SigV4 cannot provide confidentiality and AWS clients
  cannot use AIOS tickets, so this is the way to put the S3 gateway on a non-private network
  without a proxy. Plain HTTP when both are unset; half-configured or mismatched files fail
  startup. New `src/http/tls_stream.{hpp,cpp}` (`TlsServerContext`, `TlsStream`) replaces the raw
  fd calls in the S3 session so the HTTP listener can adopt it later.
- `S3Server::stop()` no longer blocks forever when `start()` failed before `listen()` (there was
  nothing to cancel, but it waited for an io_context that had never run).
- Tests: `tests/test_s3_tls.cpp` (context loading errors, startup refusal, SigV4 PUT/GET over TLS
  with a generated certificate, plaintext-on-TLS-port and TLS-on-plain-port both fail cleanly).

### Added — directory leases in `libaios_posix` / `aios-fuse`; POSIX layer round-trip cuts

The FUSE mount (and everything else on `libaios_posix`: the S3 gateway, XRootD OSS,
`aios-kbridge`) now uses the same directory-lease protocol as the kernel client, so both kinds of
mount interoperate on one volume and break each other's leases.

- **Leases (`DirLeaseManager`, `src/posix/posix_lease.cpp`).** The first namespace change in a
  directory takes the server lock on its meta object (TTL 30 s, renewed every 1 s, released after
  10 s idle, at most 64 per mount). Under the lease `create`/`mkdir`/`symlink`/`link`/`unlink`
  (and `rmdir`'s parent-side removal) check against and update the lease's in-memory table, update
  the parent inode in-core and queue a changelog record; `lookup`/`readdir` on a leased directory
  are served from that table without a round trip. A flusher thread (woken per op, so cross-mount
  visibility lags by about one round trip) PUTs unpublished child inodes, then appends the queue
  in batches (one `append` + one CAS `PUT` of meta per batch, compaction under log/snap locks
  when garbage from a peer's refused append is detected or the log exceeds 1 MiB) and writes the
  parent's mtime/nlink once per batch. After the lease is held, `create`/`mkdir`/`symlink` are
  local (no child inode PUT on the calling thread) instead of about twelve round trips (three lock
  acquires, reload, append, meta PUT, parent PUT, three releases). The first mutation of a
  directory still takes the lock. `break_requested` seen at a renew flushes and releases within
  the grace period; a lost lease replays the queue with the synchronous protocol and keeps a sticky
  error. Same-directory `rename` is one queued record too (the replaced inode is dropped
  synchronously, as for `unlink`), so create+rename never gives the lease up; cross-directory
  `rename` and `rmdir` of a leased directory flush and release first, then run the existing `/txn`
  protocol.
- **ABI.** `aios_posix_fsyncdir(fs, dir_ino)` and `aios_posix_sync(fs)` wait for the queue and
  return the sticky error; unmount flushes and releases every lease.
  `aios_posix_config.flags` with `AIOS_POSIX_F_NOLEASE` (also `AIOS_POSIX_NOLEASE=1` in the
  environment, `-o nolease` for `aios-fuse`) keeps the old per-operation commit.
- **`aios-fuse`.** Implements `opendir`/`fsyncdir` (so `fsync(dirfd)` commits the queue), reports
  our inode numbers to the kernel (`use_ino`, `readdir_ino`: hard links share `st_ino`) and sets
  `nullpath_ok` so read/write/flush/fsync/release/readdir skip libfuse's path resolution.
- **Fewer round trips elsewhere.** Inode numbers are reserved from the super object in batches of
  64 (one CAS per batch instead of per create). Layout-rule matching no longer walks the parent
  chain (one directory load per level, twice per create) when no rules are configured, and caches
  the path per inode when they are. `readdir` fetches the uncached child inodes of a batch with
  up to 8 parallel GETs instead of serially (d_type needs the mode; the S3 gateway relies on it).
- `Session::lock_renew` reports `break_requested`.
- Tests: `Review2Posix.DirLeaseHeldWhileActiveAndReleasedOnUnmount`,
  `Review2Posix.DirLeaseBatchesManyCreates` (200 creates: ~1.7 s → ~60 ms against a local daemon),
  `Review2Posix.DirLeaseUnlinkRmdirAndRenameUnderLease`,
  `Review2Posix.DirLeaseUnlinkOfUnpublishedCreateLeavesNoInode`,
  `Review2Posix.PeerBreaksDirLeaseAndBothMountsConverge`, `Review2Posix.NoLeaseFlagCommitsSynchronously`.

### Added — directory leases: asynchronous namespace operations in `aiosfs`

The per-object lock API is now a lease with fencing and a break protocol, and the kernel client
uses it to delegate whole directories to itself.

- **Server (`LockTable`, `/o/{oid}/lock*`).** An expired or released token is *fenced*: a
  mutation carrying it fails with `409 lock_expired` (retained `kFenceRetainMs`) instead of being
  accepted as unlocked. New `POST /o/{oid}/lock/break` (`x-aios-lock-grace-ms`, default 5000,
  capped at the 300 s max TTL) shortens the holder's lease to the grace period and sets `break_requested`, which
  `lock/renew` and `GET lock` now report; a renew after a break cannot extend past the deadline.
  Lease state stays primary-local (not replicated), as before.
- **`libaios` / `libaios_posix`.** `Session::lock_break` and `Session::lock_acquire_wait`
  (break + wait with backoff). `DirTable::append_ops` and `HeldLocks::acquire_sorted` use them,
  so a userspace client blocked by a kernel mount's lease gets the directory back within the
  grace period instead of failing with `EBUSY`.
- **`aios_http`.** `aios_http_lock_renew` (returns `break_requested`) and `aios_http_lock_break`;
  `409`/`404` on renew map to `-ESTALE`.
- **`aiosfs` (http backend).** A directory this mount modifies is leased (server lock on its
  meta object, TTL 30 s, renewed every 3 s, released after 10 s idle, at most 32 per mount).
  Under the lease `create`/`mkdir`/`symlink`/`link`/`unlink`/`rename` (same directory) update
  the cached table and the in-core parent inode and queue an `AOPk` record; a `wb_wq` worker
  appends the queue in batches (one `POST …/append` + one CAS `PUT` of meta, compaction when
  the log grows or garbage is detected) and writes the parent's mtime/nlink once per batch. A
  `create` thus costs one synchronous round trip (the child inode PUT) instead of four. Cache
  misses under a lease overlay the queue on the server tip. `fsync(dir)`, `syncfs` and `umount`
  wait for the queue; `fsync`/`syncfs` return the sticky error of a record that could not be
  committed. A lost lease (expiry, `-ESTALE` on renew, break past the grace) replays the queue
  with the synchronous protocol; `break_requested` is honoured at the next renew by flushing and
  releasing. Cross-directory rename and `rmdir` of a leased directory flush and release first
  and then run the locking protocol. `nolease` mount option disables all of this.
- Tests: `LocksWatches.ReleasedOrExpiredTokenIsFenced`, `LocksWatches.BreakShortensLeaseAndIsVisibleToHolder`,
  `Append.SessionLockAcquireWaitBreaksAndFencesLease`.

### Changed — `aiosfs` in-kernel HTTP backend: caching, concurrency, fewer round trips

Findings `K-1`…`K-19` of the 2026-09-02 kernel client review. Wire format and server API are
unchanged; a mount made with the previous module reads the same objects.

- **Data path off the global lock.** Read, readahead, writeback, O_DIRECT, punch and metadata
  revalidation take a connection from the `aios_http` pool (`pool=N` mount option, default 8)
  instead of serializing on the single namespace connection; `http_mu` now only covers
  create/unlink/rename/mkdir. Per-inode metadata updates (size, mtime, xattrs) serialize on a new
  `meta_mu` in the inode aux instead.
- **Partial chunk writes are ranged PUTs.** `write`, writeback and `fallocate(PUNCH_HOLE)` send
  `Content-Range` PUTs for the touched bytes; only when the server rejects it (EC / compressed
  layouts, `400` → `-EOPNOTSUPP`) does the client fall back to GET-modify-PUT. `truncate` down
  trims the last partial chunk the same way.
- **Readahead.** New `.readahead` aop issues one ranged GET per contiguous run per chunk instead of
  a full-chunk GET per page; `readpage` only fetches the page it needs.
- **Pipelined writeback.** `writepages` collects dirty pages into rounds grouped by chunk and
  keeps `AIOS_HTTP_WB_INFLIGHT` rounds in flight across pool workers; the size/mtime update is
  written once at the end instead of once per page.
- **Parallel O_DIRECT.** Chunk-aligned segments of a DIO request are dispatched to the pool in
  parallel and reassembled in order.
- **Metadata caching.** `stat`, `getxattr`, `listxattr` and `readdir` `d_type` are served from
  the in-core inode while it is within `actimeo=` (milliseconds, default 250 ms, was hard-wired); revalidation is a
  `HEAD` with the cached CAS tag and re-`GET`s only on change. `HEAD` responses no longer stall
  the client on `Content-Length` (`aios_http/client.c`).
- **Directory updates append to the changelog** (`POST /o/{oid}/append` under the meta lock,
  same `AOPk` records as `libaios_posix`) instead of rewriting the full table on every
  create/unlink/rename, and compact when the log passes `AIOS_HTTP_LOG_COMPACT_BYTES` or an
  append lands past the committed `log_bytes`. Parent `mtime`/`nlink` updates retry on CAS
  conflicts (`touch_parent`). `unlink` no longer re-GETs the child to check for a directory —
  the VFS already did.
- **Inode numbers** are allocated in batches of `AIOSFS_INO_BATCH` from the volume super object
  under `ino_mu`, with exponential backoff on CAS conflicts, instead of one CAS round trip per
  create.
- **Deferred deletion.** Chunk objects of unlinked files are deleted by workqueue items striped
  across pool connections after `evict`; `truncate` deletes dropped chunks in parallel and only
  after `truncate_setsize` has drained in-flight writeback, and sizes the drop range from the
  largest size this client has seen rather than the (lazily flushed) size on the server, so a
  shrink followed by a re-extension reads zeros instead of stale chunk bytes.
- Mount options `pool=` and `actimeo=` (`kernel/README.md`).

### Security model (documented, not yet changed)

- README gained a *Security model and limitations* section: plaintext transport, one shared
  `cluster_key` acting as node/client/admin/S3-root secret, body integrity only with a content
  hash, 60 s replay window on HTTP, no key rotation. Deploy on a private network behind a
  TLS-terminating proxy.

### Object data plane (`src/object`, `src/net`, `src/util`)

- OBJ-1 — Unauthenticated peer could terminate `aiosd` with a non-string `sig`
  (uncaught `nlohmann` type error on the RPC worker pool).
- OBJ-2 — Wire-supplied `fs_path` was used verbatim as a filesystem path (arbitrary
  write/read by any cluster-key holder).
- OBJ-3 — `UnlockForRpc` was a no-op at recursive-lock depth ≥ 2, holding the service lock across
  peer RPC in txn/purge/abort paths.
- OBJ-4 — Repair resurrected deleted objects (delete markers invisible to `stat`).
- OBJ-5 — Blocking RPC server had no timeouts; idle keep-alive connections pinned workers forever.
- OBJ-6 — `handle_stage_data` wrote through an fd it did not own.
- OBJ-7 — Repair built a `/tmp` path from the raw oid (traversal; slash-oids never repaired).
- OBJ-8 — Pipelined PUT peeked its seq without reserving it; abort could delete another writer's
  unpublished version.
- OBJ-9 — `map_` read without `mu_` in every RPC prologue.
- OBJ-10 — Local multi-store LIST dropped entries; `limit=0` returned the whole store.
- OBJ-11 — Replica stage sessions (fd + tmp file) leaked on failed/aborted remote streams.
- OBJ-12 — Memory amplification: 256 MiB fan-out buffer per PUT, full-body replication per
  range write/append, unbounded pub/sub catch-up.
- OBJ-13 — Object bodies no longer covered by the HMAC; replica install did not re-verify CRC.
- OBJ-14 — Lock-order inversion, unjoined thread vectors, `StageBegin` without acting-set check.

### HTTP / S3 / admin (`src/http`, `src/bench`, `web/admin`)

- HTTP-1 — Unauthenticated bodies > 256 KiB were staged to disk before auth; temp files leaked on
  `/admin/login`.
- HTTP-2 — S3 sessions: one detached thread per connection, no socket timeouts, whole body
  buffered before the signature check.
- HTTP-3 — Uncaught exceptions on HTTP worker / long-poll threads (non-UTF-8 oid in LIST output
  terminated the daemon).
- HTTP-4 — HTTP HMAC had no replay protection within the 60 s skew window.
- HTTP-5 — S3 never verified the body against `x-amz-content-sha256`; `aws-chunked` not decoded.
- HTTP-6 — Admin bench runner accepted a caller-supplied `endpoint` (SSRF with cluster-key-signed
  requests).
- HTTP-7 — Unbounded detached threads for `/watch` and `/pubsub` long polls.
- HTTP-8 — S3 multipart uploads not owner-scoped; `uploadId` from `mt19937_64`.
- HTTP-9 — SigV4 canonical query string collapsed duplicate keys.
- HTTP-10 — Admin CSRF protection rested on `SameSite=Strict` alone; no login rate limiting.
- HTTP-11 — Admin SPA injected `node_id` / `addr` / `access_key_id` into `innerHTML` without `esc()`.
- HTTP-12 — `url_decode` turned `+` into a space in path segments.

### Store / EC / archive (`src/store`, `src/ec`, `src/object/archive_*`, `src/util`)

- STO-1 — Tape drain took binary, root and URI from client-settable object attributes
  (file read/write and `execvp` of an attacker-named path).
- STO-2 — Archive / recall / drain installed stubs with no expected-tip check.
- STO-3 — `LocalStores::sync_paths` destroyed an `ObjectStore` other threads still held.
- STO-4 — Compressed object with a huge `full_size` attribute made every GET zero-fill up to
  64 GiB (U1 leftover).
- STO-5 — Backup / archive background jobs threw on client-authored metadata (`std::terminate`).
- STO-6 — Pipelined-PUT versions were not power-loss durable (missing directory fsync).
- STO-7 — RPC replay cache (4096 entries) far too small for the 60 s window.
- STO-8 — Bag decoding trusted `count` and `offset+length` from tape/S3 headers.
- STO-9 — Cached SQLite statements left stepped between operations.
- STO-10 — Primary staging name `upload-<sha(oid)>-<ms>` opened with `O_TRUNC`, no `O_EXCL`.
- STO-11 — `.aios` marker rewritten in place; `place()` copied the whole ring per call.

### POSIX / FUSE / client (`src/posix`, `src/client`, `src/xrd`)

- POS-1 — S3 gateway and XRootD never flushed the deferred inode PUT (objects read as size 0 from
  other nodes and after restart).
- POS-2 — Concurrent writers to one inode lost size (P3 regression inside one process).
- POS-3 — Inode cache had no TTL / invalidation and was unbounded.
- POS-4 — STL changelog: a reserved-but-never-appended op id stalled readers; compaction then
  destroyed later records.
- POS-5 — Keep-alive retry in `Session` replayed non-idempotent requests.
- POS-6 — `store_inode` reapply path dropped a pending deferred size.
- POS-7 — Chunk cache could be regressed to an older version by a concurrent reader.
- POS-8 — `Session` redirect allowlist mutated without synchronisation.
- POS-9 — 250 ms directory cache turned unlink/rmdir races into double-frees and quota drift.
- POS-10 — `ensure_not_frozen` ran outside the exception barrier in `link` / `rename2`.
- POS-11 — Server required `UNSIGNED-PAYLOAD` for streamed PUTs (user data never covered by the
  HTTP signature).
- POS-12 — `release` dropped another fd's `flock`; one-level rename cycle check; sub-chunk write
  retry budget; mutex lease never renewed.

### Kernel modules (`kernel/`)

- KRN-1 — `aiosfs` HTTP backend did not compile (`extract_xattrs_object` used before its
  `static` definition).
- KRN-2 — 128 KiB automatic array in `parse_xattrs_object` (guaranteed stack overflow on any
  xattr operation); further > 2 KiB frames in the same file.
- KRN-3 — aiosvd COW clones: zero writes / WRITE_ZEROES / DISCARD deleted the child object and
  resurrected the parent's data.
- KRN-4 — `fsync()` returned success with all but 64 pages of dirty data unwritten.
- KRN-5 — Chunk read-modify-write unserialised across the three writeback paths.
- KRN-6 — `attach_iinfo` from `getattr` ran with no lock (double `kfree` / UAF).
- KRN-7 — Upcall `aios_dev_read` dereferenced `req` after dropping `conn->lock`.
- KRN-8 — `aios-kbridge` wrote header and payload in two `write()`s (kernel requires one); the
  upcall backend could not complete a mount.
- KRN-9 — `unlink` destroyed the data of still-open files.
- KRN-10 — `writepages` error path ended writeback without redirtying.
- KRN-11 — `last_synced_size` never reset by truncate.
- KRN-12 — Assorted: evict pushed local size as absolute truncate, non-atomic single-dir
  mutations, aiosvd rename while writable, stress script, unterminated ioctl strings.

### Build, CI and repository

- `LICENSE`: GPL-3.0-or-later for the project; `kernel/` modules are GPL-2.0-or-later
  (`MODULE_LICENSE("GPL")`) because they link against the GPL-2.0-only kernel.
- `ProcessSmoke.*`: the first process-level tests. They spawn the built `aiosd` and `aios`
  binaries, bring a single-node daemon up on free loopback ports, and check `--version`, usage
  errors, PUT/GET/LIST/DELETE over the wire, a wrong-key 401, a CLI round trip, the status file,
  a clean `SIGTERM` exit and object durability across a daemon restart.
- GitHub Actions workflow: Linux (gcc) and macOS (AppleClang) with `-Werror`, ASan+UBSan
  (required) and TSan (advisory) matrix, AlmaLinux 9 kernel-module build (blocking).
- `CMakePresets.json` with `default`, `ci`, `asan-ubsan`, `tsan` configure/build/test/workflow
  presets; `AIOS_WERROR` and `AIOS_SANITIZE` options (`cmake/AiosBuildOptions.cmake`).
- FetchContent dependencies pinned to commit hashes (yaml-cpp 0.8.0, nlohmann/json v3.11.3,
  googletest v1.15.2) and marked `SYSTEM` so their warnings do not break `-Werror`.
- Generated `aios_version.hpp` (`AIOS_VERSION_STRING`, `AIOS_VERSION_FULL` with `git describe`).
- XRootD discovery no longer looks in `../xrootd` next to the checkout; `XRootD_ROOT` /
  `XROOTD_ROOT`, `find_package(XRootD CONFIG)` and pkg-config are honoured and a missing XRootD is
  reported explicitly.
- Homebrew paths moved from `CMakeLists.txt` into `cmake/macos-homebrew.cmake`.
- `GNUInstallDirs`; the compiled-in admin SPA fallback is the install location
  (`${datadir}/aios/admin`) instead of the source tree; `libaios_core.a` and the `client/*.hpp`
  headers are installed so `libaios_client.a` / `libaios_posix.a` are linkable.
- `SessionWireC6.SocketTimeoutSurfacesAsHttpError` made deterministic (asserts on the error, wide
  timing bounds).
- README: requirements (googletest is fetched too; optional dependencies table), *Security model
  and limitations*, *Testing & CI*; new `CONTRIBUTING.md`, `.clang-format`, `.editorconfig`,
  `.gitattributes`; review artefacts moved to `docs/dev/`; admin icon shrunk from 1254×1254
  (2.6 MB) to 256×256.
