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
