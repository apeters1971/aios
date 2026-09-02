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

- GitHub Actions workflow: Linux (gcc) and macOS (AppleClang) with `-Werror`, ASan+UBSan
  (required) and TSan (advisory) matrix, AlmaLinux 9 kernel-module build (advisory until KRN-1).
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
