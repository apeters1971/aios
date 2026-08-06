# Code review — first full-codebase pass

Snapshot generated **2026-08-06**. Covers the five areas that no earlier review
had touched: `src/posix`, `src/ec` + `src/store`, `src/client`, `src/xrd` /
`src/cuobject` / `src/metrics` / `src/util`, and the parts of `kernel/aiosfs`
outside the earlier workqueue pass. Roughly 47k lines of non-test source.

**53 findings: 12 critical, 17 high, 20 medium, 4 low.** All listed findings were
addressed in follow-up commits after this snapshot was written (2026-08-06). Five
were re-checked against the source by hand and are marked ✓ below; the rest carry
the reviewer's own confidence rating. Keep this document as the audit trail; do
not treat open wording in the finding bodies as current status.

An interactive version of this document lives in `CODE_REVIEW.canvas.tsx`. To
open it, copy that file into
`~/.cursor/projects/Users-apeters-Software-aios/canvases/` and open it from
there — Cursor only renders canvases from that directory.

---

## Two results that change what you can assume today

**`kernel/aiosfs`'s HTTP backend does not compile** (K1), and could not have
worked if it did (K2). That backend has therefore never been built in its
current state — including the workqueue changes committed in `47eb7c4`.

**Any client holding the cluster key can terminate `aiosd` on demand** (U1), or
poison a single object so that every later GET re-crashes the daemon. This was
reproduced live against a running instance during the review.

## One result worth recording as good news

**The erasure coding maths is correct.** The stripe arithmetic, last-stripe
padding and the scatter/gather were traced by hand for `k=4, m=2` at sizes 0, 1,
5 and 100 with no off-by-one. `gen_decode_matrix` is a faithful port of ISA-L's
reference implementation, including the survivor-selection loop and the
inverse-matrix product for lost parity shards; matrix-inversion failure is
surfaced rather than swallowed, and the erasure guard correctly caps at `m`. XOR
is a correct systematic code for `m == 1` and the factory rejects `m > 1`. Every
EC bug below is at the edges of the codecs, not in the algebra.

---

## Index

| ID | Sev | Area | Finding |
|---|---|---|---|
| P1 | crit | posix | Quota ledger reads its CAS from the wrong attribute ✓ |
| P2 | crit | posix | Chunk writes are unsynchronized RMW with an unconditional PUT |
| P3 | crit | posix | `store_inode` answers a CAS conflict by rewriting its stale record |
| P4 | crit | posix | Directory compaction erases the log with no lock and no CAS |
| P5 | high | posix | Exceptions can escape FUSE callbacks and terminate the mount |
| P6 | high | posix | Cross-dir rename over an existing file leaks its chunks and quota |
| P7 | high | posix | Unlocked shared state; layout lookup returns a dangling pointer |
| P8 | high | posix | `readdir` gives every entry in a batch the same offset cookie |
| P9 | high | posix | QoS burst equals rate, so a large I/O can never be admitted |
| P10 | high | posix | `create`/`mkdir` check-then-act race orphans an inode |
| P11 | med | posix | Names over 255 bytes accepted on create, truncated in `readdir` |
| P12 | med | posix | Same-dir rename is not atomic and destroys the destination first |
| E1 | crit | ec | Third EC read path selects shards by position, no CRC check ✓ |
| E2 | crit | ec | `shard_len` truncated to `int` for ISA-L → all-zero parity ✓ |
| E3 | high | ec | `decode()` never checks shard length against `full_size` |
| E4 | high | store | Staging/tmp names keyed on seq only, not on the object |
| E5 | high | store | No serialization of sqlite transactions across threads |
| E6 | high | ec | `shard_len == 0` doubles as the "unset" sentinel |
| E7 | high | ec | Repair decodes from a mix of shard generations |
| E8 | med | store | Bodies unlinked before the removing transaction commits |
| E9 | med | store | `write_fs_object` reports success for a partial write |
| E10 | med | store | `install_version` adopts a body without checking size or CRC |
| E11 | med | store | `crc_file_range` zero-fills past EOF and returns success |
| E12 | med | store | Backwards tip accepted; install idempotency ignores attrs |
| C1 | crit | client | `compact()` truncates the log under a CAS that never fails |
| C2 | crit | client | `flush()` advances the cursor past ops it never applied |
| C3 | high | client | Out-of-order `op_id` records are silently dropped by readers |
| C4 | high | client | Mutex never renews its lease; failed unlock terminates |
| C5 | high | client | A short response body is accepted as a complete body |
| C6 | high | client | No timeout on any socket operation |
| C7 | med | client | Response bodies allocated without a size limit |
| C8 | med | client | Request bodies never signed; redirects followed blindly |
| C9 | med | client | CRLF injection through attribute values into headers |
| C10 | med | client | Inconsistent framing; unvalidated `op_id` can wedge a client |
| C11 | med | client | Malformed responses escape as nlohmann exceptions |
| C12 | low | client | `list`/`deque` `operator[]` mutations are never persisted |
| U1 | crit | util | Attacker-controlled `full_size` crashes the daemon on GET ✓ |
| U2 | med | util | Non-constant-time HMAC comparison for cluster/RPC auth |
| U3 | med | util | Signed messages are replayable within the skew window |
| U4 | med | xrd | Data-plane ops authorize against a stale thread-local caller |
| U5 | low | util | base64 decoder silently accepts malformed padding |
| K1 | crit | aiosfs | `http_backend.c` does not compile — `drop_nlink` redefinition ✓ |
| K2 | crit | aiosfs | `json_get_u64` cannot parse anything ✓ |
| K3 | crit | aiosfs | Reply length never clamped to the caller's buffer |
| K4 | crit | aiosfs | `getattr` clobbers `i_size`; writeback drops the dirty tail |
| K5 | crit | aiosfs | `drop_inode` discards dirty pages instead of writing back |
| K6 | high | aiosfs | `readdir` skips entries; `next_offset` not checked |
| K7 | high | aiosfs | Names interpolated into JSON unescaped |
| K8 | high | aiosfs | Metadata-only writeback issues a data-destroying truncate |
| K9 | med | aiosfs | `mutex_lock()` inside a `wait_event_interruptible` condition |
| K10 | med | aiosfs | Same-dir rename orphans the victim, ignores emptiness |
| K11 | med | aiosfs | Upcall rename maintains no link counts |
| K12 | med | aiosfs | O_DIRECT invalidates the page cache without flushing first |

---

## src/posix

This layer maps POSIX onto flat objects: inodes are JSON documents at
`posix/{vol}/ino/{N}`, file data is striped into `stripe_unit` (default 1 MiB)
chunk objects, and directories are a changelog replayed into memory on every
operation. Almost every multi-object operation is a sequence of independently
committed writes.

### P1 (critical, HIGH confidence ✓) — quota ledger reads its CAS from the wrong attribute

`src/posix/quota_ledger.cpp:232,239,452,564,593`

The ledger writes `aios.posix.cas` (line 95) but reads `snap.cas`, which
`src/client/session.cpp:257` populates only from `aios.stl.cas`. The value read
back is therefore always 0. The file already defines the correct helper,
`cas_from_attrs` (line 22), and uses it when writing — just not when reading.

After the first successful flush, every later flush sends `expected_cas = 0`,
the server sees 1, and the retry loop repeats the identical request eight times.
Usage is never persisted again, so quota drifts arbitrarily far from reality and
is lost on restart. The pending sets are never cleared and grow without bound,
and because `pending_bytes_` stays above the flush threshold forever, every
write on the node performs 8 GETs + 8 HEADs while holding `mu_`.

`QuotaAdminStore` manipulates the same two objects and reads the CAS correctly
(`src/http/quota_admin.cpp:82,117`), which is why the existing tests miss this —
`tests/test_quota.cpp` only exercises the admin path.

*Fix:* use `cas_from_attrs(snap.attrs)` at all five read sites. Separately, stop
classifying conflicts by substring-matching the error message, and add a
backoff so a persistently failing flush cannot be retried on every write.

### P2 (critical, HIGH) — chunk writes are unsynchronized read-modify-write with an unconditional PUT

`src/posix/posix_fs.cpp:796-816`, and the truncate tail-trim at `:870-876`

Each write GETs the whole chunk, patches its bytes into a private copy, and PUTs
it back with `expected_cas = nullopt` and no cluster lock. Two writers touching
disjoint byte ranges of the same chunk silently overwrite each other, and the
loser already returned success with a full byte count. Default stripe is 1 MiB
and FUSE writes are 128 KiB, so eight consecutive requests share a chunk.

*Fix:* read the chunk's CAS, PUT with it, and retry the whole read-modify-write
on conflict. Better, use `Session::put_range` for sub-chunk writes — the API
exists (`session.hpp:93`) and is unused here.

### P3 (critical, HIGH) — `store_inode` answers a CAS conflict by rewriting its stale record

`src/posix/posix_fs.cpp:604-624`

The catch handler detects that another writer changed the inode, then discards
their record and re-PUTs the caller's stale copy under the refreshed CAS. That
converts optimistic concurrency control into last-writer-wins on the whole
inode.

The concrete case is `size`: two concurrent appends both read 0, B commits 200,
A then overwrites with 100. Bytes 100–200 are durably in the chunk object but
`read_file` clamps at `meta.size`, so they are unreadable — silent truncation of
an acknowledged write. `nlink`, xattrs and mode are lost the same way, and the
background rstat thread calls `store_inode` on directories and repaired children
(`posix_rstat.cpp:45,72`), so a timer thread can clobber a live inode.

*Fix:* on conflict, re-read the fresh record and re-apply only the fields this
operation owns, then retry. Passing a mutation lambda into `store_inode` is the
cleanest shape.

### P4 (critical, HIGH) — directory compaction erases the log with no lock and no CAS

`src/posix/posix_fs.cpp:334-345`

`compact_if_needed` writes a snapshot from its own already-stale `entries_`,
then truncates the shared log with an unconditional PUT, holding no cluster
lock. It fires automatically once the log passes `kAutoCompactBytes` (1 MiB).
Any dentry another client appended during the window is destroyed permanently —
files created successfully simply vanish from the directory.

Second failure in the same function: the order is snapshot → erase log → store
meta. If `store_meta()` fails, the durable meta still claims a large log, and
the next `load()` range-gets past the end of a now-empty object, gets a 416, and
returns `-EIO`. The directory is unreadable until the meta object is repaired by
hand.

*Fix:* take the `HeldLocks` on the three oids that `rename_cross_dir` already
uses, reload under the lock, and write all three through `/txn` —
`plan_compact_bodies` and `txn_put_dir` exist for exactly this.

### P5 (high, HIGH) — exceptions can escape FUSE callbacks and terminate the mount

`src/posix/posix_fs.cpp:1053` and every `aios_posix_*` entry point

Entry points catch only `client_error`, and the FUSE callbacks
(`fuse3_ops.cpp:486-513`) add no barrier. Reachable throwers on ordinary paths:
`nlohmann::json::parse` in `inode_from_json`, `super_from_json` and
`DirTable::load`; `std::stoull` in `apply_record` (`:234`); `std::thread`
construction in `write_file` (`:813`); `std::bad_alloc` from a chunk resize.
Throwing through libfuse's C frames calls `std::terminate`, so one corrupt
metadata object takes down the whole mount instead of returning `EIO` for one
path. The thread case additionally destroys unjoined threads, which terminates
unconditionally.

*Fix:* add `catch (const std::exception&)` and `catch (...)` returning `-EIO` to
every entry point, and wrap each FUSE callback as a second barrier. In
`write_file`, join everything on all paths.

### P6 (high, HIGH) — cross-directory rename over an existing file leaks its chunks and quota

`src/posix/posix_fs.cpp:519-547`

The GC step calls `truncate_file` on the victim after the transaction already
deleted its inode (`:514`) and the cache entry was erased (`:535`), so
`load_inode` misses, the GET fails, and it returns `-ENOENT` before deleting a
chunk or calling `note_delta`. Every chunk of the replaced file is orphaned
permanently with its bytes still charged; repeated overwrite-by-rename yields
`EDQUOT` on a volume that looks nearly empty. Same-directory rename and `unlink`
get this right via `drop_nlink` (`:691-694`).

*Fix:* capture the victim's size and chunk count before the commit, delete the
chunks by oid afterwards, and credit the quota back explicitly.

### P7 (high, HIGH) — unlocked shared state, and a dangling pointer from the layout lookup

`src/posix/posix_layout.cpp:48,106-145`; `src/posix/posix_fs.cpp:626-651`

`FsState::mu` protects only `inode_cache`, `flock_tokens` and `rstat_dirty`
(`posix_internal.hpp:139-142`). The 30-second layout refresh calls `clear()` then
`push_back` on `layout_rules`, while `match_posix_layout_rule` returns a raw
pointer into that vector which the caller dereferences — on every metadata and
data write. `layout_rules_loaded` is a non-atomic `time_point` read on the same
path, so several threads can enter the refresh at once. `ensure_super`
reassigns a `SuperMeta` containing a `std::string` on every mutating operation
while others read `super.frozen`.

*Fix:* hold the rules behind a `shared_ptr<const vector<...>>` swapped atomically
on refresh so readers keep their version, and guard `super` with `st.mu`.

### P8 (high, HIGH) — `readdir` gives every entry in a batch the same offset cookie

`src/posix/fuse3_ops.cpp:151-164`

`aios_posix_readdir` advances `off` by the batch size before returning
(`posix_fs.cpp:1095`), so all 64 entries are filled with the batch-end cookie.
When `filler` reports the kernel buffer full at entry *i*, the kernel resumes
past the whole batch and entries *i*..63 are never returned. `ls` silently
misses files and `rm -rf` fails with `ENOTEMPTY` after appearing to succeed.

Separately, the cookie is a positional index into a vector rebuilt and re-sorted
on every call (`:1078-1085`), so concurrent creates shift the order between
pages and a paged listing can skip or duplicate entries. This affects S3
`ListObjects` (`s3_server.cpp:369`) as well as FUSE.

*Fix:* pass `off - n + i + 1` per entry and stop on the first `filler` failure.
For stability, make the cookie order-stable (e.g. resume past the last returned
name) rather than positional.

### P9 (high, HIGH) — QoS burst equals rate, so a large I/O can never be admitted

`src/posix/qos_controller.cpp:130-146`

`burst` is capped at `rate` and `tokens` at `burst`, while `cost` is the full I/O
size. A request larger than the per-second byte limit can never be satisfied —
the bucket is not slow, it is permanently closed. The S3 front-end passes an
entire body to one write (`s3_server.cpp:924`), so with a 10 MB/s limit any PUT
above 10 MB fails immediately and forever, surfacing as `-EAGAIN`
(`posix_fs.cpp:777`) which applications treat as a hard error, not backpressure.

`now_ms()` also uses `system_clock` (`:16`). A backward step cannot mint tokens
(the elapsed clamp at `:137` prevents that) but it does set `last_ms` to the
stepped-back time, so after an NTP correction the buckets stop refilling until
wall time catches up.

The two-pass dry-run/consume structure in `admit` (`:239-246`) was checked for
token leakage on partial admission and is sound.

*Fix:* set `burst` to `max(rate, largest_expected_io)`, admit an oversized single
I/O after draining rather than rejecting forever, switch to `steady_clock`, and
return `-EBUSY` rather than `-EAGAIN`.

### P10 (high, HIGH) — `create`/`mkdir` check-then-act race orphans an inode

`src/posix/posix_fs.cpp:1161-1182`

The `EEXIST` check reads a `DirTable` loaded moments earlier; `dir.link` appends
unconditionally. Two concurrent creates of one name both pass and both append;
replay order picks the winner. The loser gets a success return and a stat for
its own now-unreachable inode. The S3 PUT path does exactly this
(`s3_server.cpp:318-322`), so two concurrent PUTs to one key both return 200
while one body lands in an invisible inode.

Related: if `dir.link` throws after `store_inode` succeeded, the inode is durable
with no dentry and the caller sees `-EAGAIN` — same orphan, reported as failure.

*Fix:* make the name claim atomic — a conditional link op validated server-side,
or the directory cluster lock around load-check-link as `rename_cross_dir` does.

### P11 (medium, HIGH) — long names accepted on create, truncated in `readdir`

`src/posix/posix_fs.cpp:1151` (validation), `:1094` (truncation)

Create checks only non-empty and slash-free; `readdir` `snprintf`s into a
256-byte field. `statfs` advertises `namemax = 255` (`:1489`) but nothing
enforces it, and S3 keys may be 1024 bytes with each `/`-separated segment
becoming a dentry name. The object is then listed under a truncated name, so a
GET or DELETE of the listed key returns 404 — listed but unusable, and
undeletable through listing-driven tooling.

*Fix:* reject names over 255 bytes with `-ENAMETOOLONG` in `create`, `mkdir`,
`link` and `rename`; have `readdir` skip rather than truncate.

### P12 (medium, HIGH) — same-directory rename is not atomic and destroys the destination first

`src/posix/posix_fs.cpp:1346-1358`; `unlink` at `:1213-1214`

Unlink-target, drop-nlink and rename are three independently committed
operations in that order. A crash or thrown error after the first leaves the
destination destroyed and the source at its old name, so write-temp-then-rename
loses the real file. `unlink` has the same shape: if `drop_nlink` throws, the
caller gets an error with the dentry already gone and the inode plus chunks
orphaned with quota still charged. The cross-directory path is correct.

*Fix:* route same-directory replace through the locked `/txn` compact rewrite
that `rename_cross_dir` already uses. For `unlink`, drop the link count first.

---

## src/ec and src/store

### E1 (critical, HIGH ✓) — a third EC read path selects shards by acting-set position and never checks the CRC

`src/object/object_io.cpp:58-78`

The same defect already fixed in `ObjectService::reconstruct_ec_object` and
`repair_ec_object`: shard identity is the stored `aios.ec.i` attribute, not the
slot the target occupies after `place()` reorders the acting set. This third
call site was missed, and unlike the other two it has no `full_crc` guard.

Every caller is a destructive migration — `transition.cpp:87` (re-encodes and
republishes), `archive_pack.cpp:209,337`, `archive_tape.cpp:473` (writes to tape
and drops the local copy). After any topology change the codec is fed mismatched
shards, returns `true`, and the corrupted buffer becomes authoritative.

*Fix:* factor the `shard_index_for` lambda out of `object_service.cpp:1141-1159`
into a shared helper, use it here, and add the CRC guard from `:1206`.

### E2 (critical, HIGH ✓) — `shard_len` truncated to `int` for ISA-L, producing all-zero parity

`src/ec/isal_rs.cpp:100` (encode), `:185` (decode)

`ec_encode_data` takes `int len`; nothing bounds `shard_len` to `INT_MAX`, and
`max_object_bytes` defaults to 64 GiB (`config.hpp:174`). At a 2 GiB shard the
cast yields `INT_MIN` and ISA-L's length loops do not execute, so the parity
buffers stay zero and `encode()` returns `true`. `commit_ec_put` counts those as
successful shards and publishes. The object reads fine while all `k` data shards
survive and is unrecoverable the moment one is lost.

Triggers: `k=2 m=2` at 4 GiB, or `k=4 m=2` at 16 GiB.

*Fix:* reject `shard_len > INT_MAX` explicitly, or chunk the call — `ec_encode_data`
is stateless per byte offset so chunking is safe.

### E3 (high, HIGH) — `decode()` never checks shard length against `full_size`

`src/ec/isal_rs.cpp:194-202`, `src/ec/xor_parity.cpp:93-101`

`shard_len` comes from the caller and is never compared against the required
`ceil(full_size/k)`. Too long produces misaligned output; too short leaves a
zero-filled tail. Both return `true`. The codec has no defence of its own
against mismatched shards, and E1 is a path with no CRC check to compensate.

Reachable because `reconstruct_ec_object` takes `full_size`/`full_crc` from one
target's attrs but pulls bodies from each target's own tip with no seq filter,
so attrs from generation N can meet bodies from N−1.

*Fix:* `if (shard_len != (full_size + k_ - 1) / k_) return false;` at the top of
both `decode` bodies.

### E4 (high, HIGH) — staging and tmp names keyed on seq only, not on the object

`src/store/object_store.cpp:529-531`, `:1054`, `:1026-1027`

Names are `v%016llx.tmp` and `stage-<seq>`, with no oid component. Every object's
first version is seq 1. None of `handle_stage_begin`/`stage_data`/`stage_commit`
takes `ObjectService::mu_`, and they run on the 8-thread RPC pool
(`net/server.hpp:66`). Two concurrent uploads of different oids hashing into the
same shard at the same seq write into one file; one object ends up holding a
blend of both bodies with a correct-looking DB row. `create_staging_file`'s
`upload-<now_ms>-<shard_id>` collides the same way within a millisecond.

*Fix:* include the oid hash in both names — `version_relpath` already computes it.

### E5 (high, HIGH) — no serialization of sqlite transactions

`src/store/object_store.cpp:375-384`; `object_store.hpp:239-291` has no mutex

Every mutating entry point wraps itself in `BEGIN IMMEDIATE … COMMIT` on a single
connection per shard. Only the primary-side `api_*` methods hold
`ObjectService::mu_`; the replica-side handlers do not — `local_install`
(`object_service.cpp:592`), `handle_publish_tip` (`:922`), `handle_stage_commit`
(`:2185`) are all lock-free on the worker pool, and `run_repair` adds a tenth
thread from the gossip io_context.

Three failures follow: a second `BEGIN IMMEDIATE` on a connection already in a
transaction fails, so concurrent writes return spurious errors under load; any
`rollback(s)` discards whichever transaction is open, including another
request's work; and bare SELECTs on that connection see the open transaction's
uncommitted rows, so repair can sample a shard version that never existed.

*Fix:* a `std::mutex` per `Shard`, held across the whole begin/commit span
including the filesystem work. The 256-way sharding keeps the concurrency.

### E6 (high, HIGH) — `shard_len == 0` doubles as the "unset" sentinel

`src/ec/isal_rs.cpp:128-133`, `src/ec/xor_parity.cpp:56-61`

If the *first* present shard is empty, `shard_len` stays 0, the next shard sets
it, and the empty one slips past the mismatch check. Its `.data()` is then read
as `shard_len` bytes — a heap over-read, before any CRC check runs. The XOR
gather does this even when nothing is missing. Empty bodies are reachable
because `ObjectStore::get`'s inline branch (`object_store.cpp:1888-1895`) never
compares `blob_len` against `info->size`, unlike the fs branch at `:1862-1869`.

*Fix:* track presence with a separate `bool have_len`, and make the inline branch
fail on a length mismatch.

### E7 (high, HIGH on the code, MEDIUM on reachability) — repair decodes from a mix of shard generations

`src/object/repair.cpp:310-327`

`run_repair` already computed at `:524` which targets hold a stale seq, but the
collection loop filters on presence (`has[ti]`) only. All generations of a
same-sized object share a shard length, so the E3 check would not catch it
either. The `full_crc` guard at `:332` saves it when the attribute is present;
when it is not, `:337` re-encodes the unverified decode over the shards marked
for repair, turning a recoverable situation into permanent corruption.

*Fix:* skip targets whose seq differs from the authoritative one, and refuse to
repair at all when `!meta->full_crc_known`.

### E8 (medium, HIGH) — bodies unlinked before the removing transaction commits

`src/store/object_store.cpp:903-906`, called from `publish_tip:1715`,
`trim_versions:2347`, `abort_version:1748`, `purge_version:2280`

`delete_version_row_locked` runs its DELETEs inside the caller's transaction and
unlinks the file immediately, outside it. `publish_tip` trims and only then
commits at `:1722`; if that commit fails the rows come back but the files are
gone. A crash before the WAL commit is durable does the same. The
`remove_fs_object` result is discarded, so a failed unlink is a silent leak.

*Fix:* collect relpaths and unlink only after `commit()` returns true.

### E9 (medium, HIGH) — `write_fs_object` reports success for a partial write

`src/store/object_store.cpp:537-566`

The stream state is checked after `write` but not after `flush`, and the
`ofstream` destructor's close is never checked, so an `ENOSPC` or `EIO` at close
is discarded and a truncated file is renamed into place. The parent directory is
never fsynced after the rename, and with `synchronous = NORMAL` in WAL mode
(`:205`) neither the directory entry nor the metadata commit is forced. The
durability block also reopens the file read-only, so its `fflush` is a no-op.

For EC this is the bad case: a short-written shard still counts toward
`total_ok`, `ec_need` is satisfied, and the object publishes.

*Fix:* `::write` loop on an fd with every return checked, `fsync` the data fd,
rename, then `fsync` the parent directory.

### E10 (medium, HIGH) — `install_version` adopts a body without checking size or CRC

`src/store/object_store.cpp:1611-1618`

Existence is the only test; `pv.size` and `pv.crc32c` come from the wire and go
into the row unverified. `handle_stage_commit` (`object_service.cpp:2174-2185`)
relies entirely on this, so an upload truncated mid-transfer installs as a valid
version whose CRC describes bytes the file does not contain. `repair.cpp:164-167`
uses the same pattern.

*Fix:* stat the file against `pv.size` and run `crc_file_range` against
`pv.crc32c` before inserting.

### E11 (medium, HIGH on the path, MEDIUM on intent) — `crc_file_range` zero-fills past EOF

`src/store/object_store.cpp:655-659`

A `pread` returning 0 means the file is shorter than the range. Sparse holes read
as zeros without hitting EOF, so this branch only fires on genuine truncation —
and it pretends the missing tail is zeros and returns `true`. `ftruncate` in
`ensure_fs_size` already guarantees the length, so there is no legitimate
trigger. `recompute_crc32c` (`:2540`) writes the result back, blessing the
truncated content, and `repair.cpp:502-508` calls it on the authoritative
replica exactly when the CRC is unknown.

*Fix:* treat `n == 0` before `done == len` as an error.

### E12 (medium, MEDIUM) — backwards tip accepted; install idempotency ignores attrs

`src/store/object_store.cpp:1668-1686`, `:1571-1579`

`publish_tip` sets the tip unconditionally with no check that `seq` is at least
the current tip, reachable straight from the wire via `handle_publish_tip`
(`:922`) with no lock. `repair.cpp:266` publishes at `auth_seq`, which for a
target legitimately holding a newer version rolls it backwards.

The `install_version` fast path compares body identity only, never attrs, so
re-installing a version with a corrected `aios.ec.i` silently succeeds and
leaves the stale attribute — the one attribute shard identity now depends on.

Confidence is MEDIUM only because no upstream invariant guaranteeing monotonic
seq ordering was found; the code itself is unambiguous.

*Fix:* guard the tip update with `WHERE excluded.tip_seq > object_tips.tip_seq`,
and compare or rewrite attrs in the install fast path.

---

## src/client

`Session` opens a fresh TCP connection per request, hand-rolls HTTP/1.1, signs
with an HMAC over a canonical string, and follows redirects up to 5 hops. The
distributed containers are built on `changelog::Log`, which spreads each
container over a meta document, an append-only op log and a snapshot. The
pervasive weakness is that this protocol is made of non-atomic multi-object
sequences whose races fail silently.

### C1 (critical, HIGH) — `compact()` truncates the log under a CAS that never fails

`src/client/changelog.cpp:351`

The truncating PUT is guarded on `aios.stl.cas`, but ops arrive via
`POST /o/{oid}/append`, which the server handles with `replace_attrs = false`
(`http_server.cpp:2550`), so an append never touches that attribute and the CAS
is identical before and after. Compaction is automatic above 1 MiB
(`map.hpp:214` and the four siblings). Records another client appended during
the window — including ones this client's snapshot does not cover — are
destroyed after that client's append returned success.

*Fix:* hold the object lock on the log oid for the duration and pass the token on
the truncating PUT, or point meta at a fresh log oid instead of truncating in
place.

### C2 (critical, HIGH) — `flush()` advances the cursor past ops it never applied

`src/client/map.hpp:74`, and `set.hpp:59`, `list.hpp:57`, `deque.hpp:57`,
`unordered_map.hpp:74`

It sets `applied_op` to `m.next_op - 1` — the highest id anyone reserved, not
its own last record. In async mode `ensure_fresh_read()` returns immediately
while `local_valid_` is set, so the client never pulled the intervening ops, and
every future `pull()` skips them.

Alone that is a stale local view. With C1 it is permanent corruption: `flush()`
calls `maybe_compact()` on the next line, `compact()` skips its freshness check
for the same reason, and `Log::compact` writes the divergent local state as a
snapshot with `snapshot_op = applied_op` — asserting it contains the skipped ops
— then erases the log holding them.

*Fix:* have `append_ops` return the ids it wrote, or simply `pull()` after
appending and let it apply everything in order.

### C3 (high, HIGH on the code, MEDIUM on frequency) — out-of-order records are silently dropped

`src/client/changelog.cpp:284` (write), `:259` (read)

Reserving the id under meta CAS and appending the record are two separate round
trips, so client A can reserve 5, B reserve 6, and B's append arrive first. The
log holds [6][5]; the reader applies 6, sets the high-water mark to 6, and skips
5 forever — on every client including its author. The op is durably in the log
and permanently invisible.

*Fix:* sort by `op_id` before applying and track an applied set rather than a
high-water mark. Structurally, drop id reservation and use the byte offset that
the atomic append already returns as the total order.

### C4 (high, HIGH) — mutex never renews its lease; failed unlock terminates

`src/client/mutex.cpp:26`

`Session::lock_acquire` discards the `expires_ms` the server returns
(`session.cpp:450`), there is no renewal, and `owns_lock()` is just a non-empty
token. The server's lease is 30s, primary-local and purged on expiry
(`locks_watches.cpp:39-110`), and containers do not pass the lock token on their
writes, so a stale holder cannot be fenced — both clients mutate freely.
`lock()` also spins forever with no deadline. And `unlock()` throws
`client_error` from a `std::lock_guard` destructor, which is `noexcept`, so the
cleanup path for that exact scenario calls `std::terminate`.

*Fix:* store `expires_ms`, renew at roughly TTL/3, make `unlock()` swallow and
log release failures, and give `lock()` a bounded default timeout.

### C5 (high, HIGH) — a short response body is accepted as a complete body

`src/client/session.cpp:206`

The read loop `break`s on error and returns a short body with status 200 and no
error. Over-reads are trimmed; under-reads are not detected. The server can
produce this too: `write_file_body` (`http_server.cpp:380`) emits
`Content-Length: size` and then breaks out of its send loop on a short file
read.

For the changelog this is the worst case: `pull()` hands the buffer to
`decode_records`, which stops at the first incomplete record and returns a
consumed-byte count that `pull()` discards (`changelog.cpp:258`). The client
applies a prefix, marks itself current, and reports success — and a later
compaction writes the dropped tail out of existence.

*Fix:* treat `body.size() != content_length` as an error, and have `pull()` check
`decode_records`' return against the buffer size.

### C6 (high, HIGH) — no timeout on any socket operation

`src/client/session.cpp:151`

Every call is the blocking synchronous form with no deadline — no
`expires_after`, no `SO_RCVTIMEO`, no async-with-timer anywhere in the file.
A server that accepts and never replies blocks the calling thread forever; in
the FUSE-facing paths that is an unkillable operation. It compounds C4, since a
thread stalled inside `request()` while holding the distributed mutex blows past
its lease with no bound.

*Fix:* `SO_SNDTIMEO`/`SO_RCVTIMEO` on the native handle is a small change given
the code is already synchronous — the same fix already applied server-side.

### C7 (medium, HIGH) — response bodies allocated without a size limit

`src/client/session.cpp:186`

`kMaxBodyBytes` (16 MiB) is enforced on every outbound body and never on inbound
ones, and `std::stoull("-1")` parses to 2^64−1 without throwing. `pull()`
compounds it by range-getting a span sized from the server-supplied
`x-aios-size` header. Any node the client talks to — which after a 307 need not
be one it chose — can exhaust its memory.

*Fix:* reject `content_length > kMaxBodyBytes` before reading; page the log.

### C8 (medium, HIGH) — request bodies never signed; redirects followed blindly

`src/client/session.cpp:127`, `:214`

The client always sends the literal `UNSIGNED-PAYLOAD`, though the server hashes
the real body whenever that header says anything else (`http_server.cpp:1036`).
The signature therefore covers only method, path+query, date and two headers,
over plain HTTP. An on-path attacker can rewrite the body of any signed PUT and
the HMAC still verifies; within the skew window a captured request can be
replayed with a substituted body. Separately, `Location` comes from an
unauthenticated response and the full request including the body is re-sent to
whatever host it names.

*Fix:* send `sha256_hex(body)` — `aios_core` already exports the helper the
server uses — and restrict redirect targets to hosts in the cluster map.

### C9 (medium, HIGH on the injection, MEDIUM on exploitability) — CRLF injection through attribute values

`src/client/session.cpp:307`, also `:148` (app label) and `:312` (lock token)

Object ids are URL-encoded, but header keys and values are never filtered, and
only `x-aios-date` and `x-aios-content-sha256` are signed. An attribute value
containing CRLF injects headers that pass authentication — including
`x-aios-attr-aios.stl.cas` or `if-none-match`, subverting the CAS preconditions
the client believes it is enforcing. Full request smuggling is blocked only
incidentally, by `Connection: close`.

*Fix:* reject CR, LF and NUL in header names and values in one choke point at the
top of `Session::request`, before signing.

### C10 (medium, MEDIUM) — inconsistent framing; an unvalidated `op_id` can wedge a client

`src/client/changelog.cpp:104`

The parser is memory-safe — every read is bounds-checked and `size_t + uint32_t`
cannot overflow on 64-bit — but the fixed 16-byte header is read at `h` without
checking it fits inside the declared `header_len`, while the cursor advances by
`header_len`. The op value is `static_cast<Op>` with no range check, and `op_id`
is never validated against `next_op`. A record carrying `UINT64_MAX` sets the
high-water mark there (`:266`), freezing the container for that client
permanently, surviving reconnects, clearable only by `load()`. `parse_index`
(`list.hpp:157`) also throws `std::invalid_argument`/`out_of_range` — not
`client_error` — from inside `apply_op`.

*Fix:* require `header_len >= 16` with the header read bounded, reject `op_id`
outside `(0, next_op)` and out-of-range ops, and use the checked `stl_codec`
path instead of `parse_index`.

### C11 (medium, HIGH) — malformed responses escape as nlohmann exceptions

`src/client/wire.cpp:69`; `changelog.cpp:197`, `:216`

The library's error contract is `client_error` with a `code()`, which
`lock_try_acquire`, `append_op`'s conflict retry and `vbd_registry_upsert` all
switch on. But the parsers throw `json::parse_error` from `parse()`,
`json::out_of_range` from `at()`, and `json::type_error` from `get<std::string>()`
on a non-string element (`wire.cpp:104`, `:112` have no type check). `Log::open`
and `load_meta` parse before any validation, so this is on the hot read path of
every container.

*Fix:* wrap each parse entry point and rethrow as `client_error`; add `is_string()`
checks in `parse_set_doc` and `parse_list_doc`.

### C12 (low, HIGH) — `list`/`deque` `operator[]` mutations are never persisted

`src/client/list.hpp:116`, `deque.hpp:117`

The operator marks the container dirty, but `flush()` writes only
`impl_->pending`, which a write through the returned reference never populates.
So it skips the append, clears the dirty flag and returns normally.
`l[0] = "x"; l.flush();` discards the write and reports success — and it can
appear to work in a single-process test, because the value survives locally
until the next `load()`. `deque` at least offers `set_at()`; `list` has no
persisting element-assignment path at all.

*Fix:* drop non-const `operator[]` in favour of `set_at()`, or return a proxy
whose `operator=` emits an op.

---

## src/xrd, src/cuobject, src/metrics, src/util

What is actually built matters here: `util` and `metrics` compile into `aiosd`
unconditionally; the compression path needs libzstd, which defaults on and is
present in this build; `cuobject` is stub/null because the NVIDIA SDK is not
found; the XRootD plugin builds but loads into an `xrootd` server, not `aiosd`.

### U1 (critical, HIGH ✓ — reproduced live) — attacker-controlled `full_size` crashes the daemon

`src/util/compression.cpp:56`; `src/object/object_service.cpp:97`

`zstd_decompress` calls `out.resize(logical_size)` with a value read straight
from the `aios.compression.full_size` attribute, which a client sets with the
`x-aios-attr-aios.compression.full_size` header. There is no upper bound. The
resulting `std::length_error` (or `bad_alloc`) is caught nowhere — `api_get`
catches only `aios::client_error` and `http_server.cpp:2811` has no surrounding
catch — so it propagates out of the request handler and calls `std::terminate`.

Reproduced: an authenticated PUT with that attribute set to 2^64−1 followed by a
GET printed `terminating due to uncaught exception of type std::length_error`
and the process exited. A value of 1000000 instead returned a clean 500,
confirming the crash is specifically the oversized resize. Because the attribute
is stored, the poisoned object re-crashes the daemon on every later GET or HEAD.

*Fix:* reject `logical_size > cfg.max_object_bytes` and wrap the resize so a bad
size becomes a 4xx, never a terminate.

### U2 (medium, HIGH that it is non-constant-time; impact LOW) — non-constant-time HMAC comparison

`src/util/auth.cpp:91`

Gossip, object-RPC and Hello signatures are verified with
`std::string::operator!=`, which returns at the first differing byte. The HTTP
path already does this correctly with `const_time_eq` (`http_auth.cpp:22`), so
this is an inconsistency rather than a missing concept.

*Fix:* use `const_time_eq`.

### U3 (medium, HIGH that there is no anti-replay) — signed messages are replayable within the skew window

`src/util/auth.cpp:67`; `src/http/http_auth.cpp:105`

Both verifiers accept any correctly-signed request within `auth_skew_ms`,
default 60000 (`config.hpp:101`). There is no nonce and no replay cache, so the
timestamp bounds staleness but not replay. A passive observer can duplicate any
mutating operation for up to a minute — re-issue a DELETE, re-apply a gossip
state, repeat an admin call.

*Fix:* a per-message nonce with a bounded LRU, or a connection-level challenge.
At minimum, document the window as accepted risk.

### U4 (medium, MEDIUM) — XRootD data-plane ops authorize against a stale thread-local caller

`src/xrd/XrdAiosDF.cpp:71-112`

`Open`, `Stat`, `Chmod` call `apply_caller`; `Read`, `Write`, `Fstat`, `Fsync`,
`Ftruncate`, `Fchmod` do not. The identity is a `thread_local` keyed by fs
pointer (`posix_fs.cpp:916`) and XRootD dispatches from a worker pool, so a read
can run on a thread whose identity was last set by another request — or never
set, falling back to the mount default uid 0, which bypasses mode checks.
Open-time checks are correct, so this is inconsistent enforcement rather than a
wholesale bypass. MEDIUM because XRootD's thread dispatch was inferred, not read.

*Fix:* store the opener's uid/gid on the `XrdAiosFile` and assert it before each
data op.

### U5 (low, HIGH — reproduced) — base64 decoder accepts malformed padding

`src/util/base64.cpp:49-61`

Input like `AB=C` — padding in the third slot, data in the fourth — passes,
because the guard checks only the -1 invalid sentinel and misses the -2 padding
sentinel, so the last byte is computed from a negative value. It decodes to
`00 82` with no error. Used for inter-node replicated payloads
(`object_service.cpp:557`) and POSIX xattr values (`posix_fs.cpp:165`), all
HMAC-authenticated, so this is robustness rather than an external hole.

*Fix:* reject any `=` that is not trailing; handle the -2 sentinel explicitly.

---

## kernel/aiosfs

Excluding the five findings the earlier workqueue pass covered (three of which
are still unfixed: the double `end_page_writeback` at `http_backend.c`
`out_pages`, the `struct aios_request` use-after-free in `upcall.c`, and the
unbounded `wait_event_interruptible` there). **None of this can be verified from
this machine — there is no kernel build tree.**

### K1 (critical, HIGH ✓) — `http_backend.c` does not compile

`kernel/aiosfs/http_backend.c:1135`, called at `:1368` and `:1449`

A file-scope `static int drop_nlink(struct aios_sb_info *, u64)` conflicts with
the `static inline void drop_nlink(struct inode *)` that `<linux/fs.h>` — pulled
in via `aiosfs.h` — has defined for decades. Line 1449 then calls it with a
single `struct inode *`, clearly meaning the VFS helper the local definition
shadowed. `http_backend.c` is in `aiosfs-y`, so the module does not build, and
`http_rmdir` has no way to decrement the parent link count as written.

This implies the HTTP backend has never been compiled in its current state,
which is consistent with K2.

*Fix:* rename the local helper to `aios_http_drop_link` and update `:1368`;
leave `:1449` calling the VFS `drop_nlink(dir)`.

### K2 (critical, HIGH ✓) — `json_get_u64` cannot parse anything

`kernel/aiosfs/http_backend.c:111-127`, and `parse_entries` at `:180`

`p` points into the middle of the JSON document, so `kstrtou64` sees
`5,"mode":33188,…}`. `kstrtou64` requires the entire string to be the number and
returns `-EINVAL` on any other trailing byte, and the serializer at `:594-602`
and `:427-430` always emits `,` or `}`. So no call ever succeeds.

Consequences, all silent: `inode_from_json` leaves ino, mode and size at 0, so
`load_inode` returns success with garbage. At mount, `:2913` loads the root, sees
mode 0, takes the non-directory branch, and installs file ops on the root inode —
the mount succeeds and the first `ls` fails with `ENOTDIR`. Every file collapses
onto `iget_locked(sb, 0)`. `dir_load` never picks up `log_bytes`/`snapshot_op`,
so directory logs are never replayed. `alloc_ino` re-reads `next_ino` as 2
forever. And `http_store_xattrs` re-serializes the all-zero record, so setting
one xattr overwrites the inode's real mode and size with zeros.

*Fix:* copy the digit run into a small stack buffer, NUL-terminate, then convert.

### K3 (critical, HIGH) — reply length never clamped to the caller's buffer

`kernel/aiosfs/io.c:31-43`

`hdr->size` is validated against `rep_len` but never against `len`, the caller's
buffer size. `aios_upcall` accepts payloads up to `AIOS_KABI_MAX_PAYLOAD`
(1 MiB), so a daemon answering a 4096-byte READ with `size = 1 MiB - 8` memcpys
~1 MiB into the kmapped page from `aios_readpage` (`pagecache.c:32`) or the
256 KiB buffer in `aios_direct_IO` (`:201`). Out-of-bounds kernel write of
attacker-chosen length and content. The same value is returned as `*out_len`, so
callers' `got` can also exceed `PAGE_SIZE`.

*Fix:* `if (hdr->size > len || hdr->size > rep_len - sizeof(*hdr)) return -EIO;`

### K4 (critical, HIGH on the code, MEDIUM on frequency) — `getattr` clobbers `i_size`, writeback drops the dirty tail

`kernel/aiosfs/inode.c:16` (`aios_stat_to_inode`), reached from
`aios_refresh_inode` and `http_refresh` (`:62-67`)

A bare `inode->i_size = st->size;` on a live inode, with no lock and no
comparison against the in-core value, on every `stat(2)`. Dirty pages only reach
the cluster at flush time, so between a buffered write and the next flush the
server's size is stale and smaller. `aios_write_page_data` then bails at
`pos >= i_size` (`pagecache.c:63-64`) and reports success, and the HTTP
collector sets `len = 0` for the same reason (`http_backend.c:2283-2288`),
leaving `max_end == 0` so the size is never even restored.

Write 1 MiB, run any `stat` — `ls -l`, `tar`, `rsync`, `cp` all do — and every
page is marked clean and the data is gone, with no error to userspace and
nothing in dmesg.

*Fix:* do not touch `i_size` for regular inodes with dirty or writeback pages;
route any genuine shrink through `truncate_setsize()` after
`filemap_write_and_wait()`.

### K5 (critical, MEDIUM) — `drop_inode` discards dirty pages instead of writing back

`kernel/aiosfs/super.c:185-192`; `http_backend.c:2853-2860`

`generic_delete_inode` returns 1 unconditionally, so `iput_final` takes the drop
branch and skips the `write_inode_now(inode, 1)` the caching branch performs;
`aios_evict_inode` then calls `truncate_inode_pages_final()`, cancelling the
dirty bit. This is constant rather than rare because `aios_d_revalidate`
(`inode.c:426-433`) returns 0 for everything, which unhashes the dentry so
`dput()` kills it immediately and the inode's last reference goes away.
`echo hi > /mnt/f; ls /mnt/f` is enough.

MEDIUM only because the exact `iput_final`/`retain_dentry` behaviour was recalled
from the 5.14 sources rather than read.

*Fix:* drop the `.drop_inode` override in both super-op tables so
`generic_drop_inode` applies — which requires `aios_unlink`/`http_unlink` to
actually call `drop_nlink` on the inode. Have `d_revalidate` return 1 within a
short timeout for positive dentries.

### K6 (high, HIGH) — `readdir` skips entries; `next_offset` not checked

`kernel/aiosfs/inode.c:374-387`

`dir_emit` returns false when the caller's buffer is full; the loop breaks but
`ctx->pos` is still advanced to `hdr->next_offset`, the batch-end cookie, so
entries *i*..`count-1` are permanently skipped. With `max_entries = 64` this
fires on any directory that does not fit one `getdents` call. Same defect as P8,
in the other front end. Separately, nothing checks that `next_offset` advances,
so a daemon returning `count > 0` with a non-advancing offset spins the
userspace loop forever.

*Fix:* add a per-entry cookie to `struct aios_kabi_dirent` and set `ctx->pos`
after each successful emit; reject a non-advancing `next_offset`.

### K7 (high, HIGH) — names interpolated into JSON unescaped

`kernel/aiosfs/http_backend.c:420-426`; xattr names at `:879-880`

`dt->ents[i].name` comes straight from `d_name` with only `/` and NUL rejected
(`:1249`), and is written with `%s` inside quotes. POSIX permits `"`, `\` and
newlines. Creating `a"b` writes `{"entries":{"a"b":5}}`; the next `dir_load`
reads the name as `a`, hits `b":5` where it expects digits, and returns
`-EINVAL`. Every subsequent lookup, create, unlink and readdir in that directory
then fails — an unprivileged, permanent, single-`touch` denial of service on the
directory, undoable only by rewriting the object out of band. A name like
`a":1,"victim` instead rewrites another entry's inode number. For xattrs, a
quote also desynchronises the brace counting in `extract_xattrs_object`
(`:623-633`).

*Fix:* add a JSON string escaper and matching unescaper used by both writers and
both parsers; size the snapshot buffer for the worst-case 6× expansion.

### K8 (high, HIGH on the mechanism, MEDIUM on frequency) — metadata-only writeback issues a destructive truncate

`kernel/aiosfs/pagecache.c:341-349`

`aios_write_inode` sends the current in-core `i_size` for any dirty regular
inode, including one dirtied purely by `file_update_time()` or `aios_setattr`.
On the HTTP side that lands in `truncate_file` (`http_backend.c:1885-1899`),
which for a smaller size deletes every chunk past the new end and rewrites the
tail. So whenever the in-core size trails the stored size — after K4, or
whenever another client appends to a cached file — routine writeback destroys
data.

*Fix:* track a `last_synced_size` and skip when unchanged; refuse to shrink
unless the caller is an explicit truncate.

### K9 (medium, HIGH on the pattern, MEDIUM on observability) — `mutex_lock()` inside a wait condition

`kernel/aiosfs/upcall.c:170-181`

`wait_event_interruptible` evaluates its condition after `prepare_to_wait_event()`
has set `TASK_INTERRUPTIBLE`. Taking a mutex there trips the
`CONFIG_DEBUG_ATOMIC_SLEEP` warning, and on contention `mutex_lock` resets the
state to `TASK_RUNNING` so the following `schedule()` returns immediately and
the daemon's read spins instead of sleeping. The final `conn->daemon_open` read
is also outside the lock it just dropped.

*Fix:* an explicit `DEFINE_WAIT`/`prepare_to_wait` loop, or convert `conn->lock`
to a spinlock (nothing under it sleeps) and use
`wait_event_interruptible_lock_irq`.

### K10 (medium, HIGH) — same-directory rename orphans the victim, ignores emptiness

`kernel/aiosfs/http_backend.c:1471-1478`; `apply_dir_op` at `:240-255`

The rename op just removes any entry named `new_name` from the in-memory table.
The victim's inode object, data chunks and directory meta/log/snap objects are
never deleted and its `nlink` is never decremented — they leak forever. There is
no emptiness check either, so `mv dir_a dir_b` within one directory detaches
`dir_b`'s whole subtree (VFS enforces dir↔dir but leaves `-ENOTEMPTY` to the
filesystem). Neither rename path drops the replaced inode's in-core `i_nlink`.
`http_rename_cross_dir` (`:1694-1722`) gets all of this right.

*Fix:* look up `new_name` first, return `-ENOTEMPTY` for a non-empty directory,
and drop the victim's link count after the store.

### K11 (medium, HIGH) — upcall rename maintains no link counts

`kernel/aiosfs/inode.c:222-233`

Two VFS contract violations. When `new_dentry` is positive the filesystem must
`drop_nlink()` the replaced inode (compare `simple_rename`); without it that
inode keeps `nlink == 1` forever, so `fstat` on a still-open replaced file is
wrong and — once K5 is fixed — it is never deleted. And moving a directory
between parents must `drop_nlink(old_dir); inc_nlink(new_dir);` for the `..`
entry; neither happens, so both counts drift until a `getattr` refresh papers
over it.

*Fix:* adjust both after a successful upcall, using `clear_nlink` for a replaced
directory and `drop_nlink` for a replaced file.

### K12 (medium, MEDIUM-HIGH) — O_DIRECT invalidates the page cache without flushing first

`kernel/aiosfs/pagecache.c:263-267` (read), `:296-302` (write)

`invalidate_inode_pages2_range` cannot invalidate a dirty page and returns
`-EBUSY`, so any direct read or write overlapping pages dirtied by an earlier
buffered write fails outright. Where the invalidate does succeed, the ordering
between it and the direct write is undefined. MEDIUM-HIGH because the `-EBUSY`
return is recalled from `mm/truncate.c` rather than read; the missing flush is
verified.

*Fix:* `filemap_write_and_wait_range()` before each invalidate, and invalidate
again after the direct write returns.

---

## Checked and found correct

Recorded so these are not re-reviewed.

- **Erasure coding maths** — see the summary at the top. Additionally: ISA-L's
  `g_tbls` sizing is `k*m*32` for encode and `k*nerrs*32` for decode, both
  correct; the coefficient pointer `encode_matrix_ + k*k` correctly skips the
  identity rows so the code really is systematic; `ec_encode_data` has no
  alignment requirement; `k+m > 255` is rejected so the fixed `[255]` arrays
  cannot be overrun; the no-ISA-L branch returns a null codec with a clear error
  rather than an unsafe fallback; zero-length and single-byte objects round-trip
  through both codecs.
- **`src/util/crc32c.cpp`** — brute-tested aligned and unaligned, incremental
  against one-shot, `crc32c_combine` against 200 random concatenations, and
  `crc32c_update_zeros` against a real zero buffer. All matched.
- **`src/util/aes_gcm.cpp`** — correct key and nonce length checks, tag appended
  and split correctly, `EVP_DecryptFinal_ex` failure surfaced as an
  authentication error, contexts freed and output cleared on every error path.
- **`src/metrics`** — `app_label` is length- and charset-validated so no
  Prometheus label injection is possible; distinct labels are capped at 256 with
  an `_overflow` bucket; `qos_rates`/`frontend_io` keys are numeric or from a
  fixed set, not raw client strings; counters are atomic and snapshot pointers
  stay valid because entries are only inserted, never erased; `/metrics` is
  unauthenticated only under the explicit `admin_metrics_public` opt-in.
- **Client HMAC canonicalization** — the client signs the full request target
  including the query string (`session.cpp:149`) and the server verifies the same
  raw target (`http_server.cpp:1040`); header names are lowercased on both sides
  and the signed-header list is sorted identically; redirect hops are bounded and
  the request is re-signed at each hop with stale `authorization`/`x-aios-date`
  erased; `url_encode_oid` escapes `/` as `%2F` and matches the server's decoder,
  so no CRLF can enter the request line.
- **Request-body memory handling** — the S3 and HTTP body readers grow with bytes
  actually received rather than pre-sizing from `Content-Length`
  (`s3_server.cpp:608`, `http_server.cpp:965`), correctly avoiding the
  allocate-from-attacker-length problem. U1 is the one place that pattern slipped
  back in.
- **`kernel/aiosfs` name handling on the upcall path** — the `len >
  AIOS_KABI_NAME_MAX` guards are safe because every wire `name[]` is
  `AIOS_KABI_NAME_MAX + 1` bytes; `match_strlcpy` in `aios_parse_options`
  truncates safely; `aios_http_encode_oid` percent-encodes everything outside the
  unreserved set, so neither the `volume=` mount option nor any OID can inject
  into the request line; `aios_get_tree_fill`'s error unwind does not double-free;
  `http_put_super` correctly drains `wb_wq` before destroying it.

## Below the bar, but worth knowing

- `.` and `..` are accepted as dentry names in `create`/`mkdir`, so an S3 key like
  `a/../b` creates a literal `..` entry. Not a traversal escape — lookup
  intercepts both names first — but `readdir` then emits duplicates.
- `readdir` reports `..` as the directory's own inode for every non-root
  directory (`posix_fs.cpp:1082`), which breaks inode-based tree walkers and NFS
  re-export.
- `removexattr` is the only mutating operation missing the snapshot-freeze check.
  The freeze is advisory in any case — checked once at entry, no lock held for
  the operation's duration.
- A `write_file` that fails partway leaves committed chunks with no size update
  and no quota accounting: a torn write on overwrite, dead uncharged space on
  append.
- `setattr` lets any user with write permission set explicit atime/mtime; POSIX
  requires ownership. `check_access` ignores supplementary groups —
  over-restrictive rather than a hole.
- `path_of_ino` returns a silently wrong partial path when a lookup fails
  mid-walk (`posix_layout.cpp:86`), so a file can be written with the wrong
  storage class and `layout_domains_differ` can return a spurious `EXDEV`.
- `aios_posix_flock`'s blocking mode is a 300 × 20 ms poll that gives up with
  `-EAGAIN`, so a blocking `flock` is silently a 6-second timeout.
- `g_tls_callers` is keyed by the `aios_posix_fs*` pointer and erased only on the
  unmounting thread, so a reused address could apply stale credentials to a new
  mount.
- `basic_set`/`list`/`deque`/`unordered_map`'s `pull()` discards the `Meta` return
  and so lacks `basic_map`'s `if (!meta.exists) local_.clear()` guard, leaving
  stale local state readable after another client deletes the container.
- `vbd_registry_client.cpp:94` classifies retryable CAS conflicts by
  substring-matching `e.what()` rather than `e.code()`.
- `attach_iinfo` (`http_backend.c:988`) allocates `i_private` without
  synchronisation, so two concurrent lookups on one inode can leak an allocation.
- `http_rename_cross_dir` has roughly a 2.9 KB stack frame, which will trip the
  default `-Wframe-larger-than=2048` that deep in the VFS rename path.
