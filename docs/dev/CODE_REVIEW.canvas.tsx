import {
  Callout,
  CollapsibleSection,
  Divider,
  Grid,
  H1,
  H2,
  Pill,
  Row,
  Spacer,
  Stack,
  Stat,
  Swatch,
  Text,
  useCanvasState,
  useHostTheme,
  type Color,
} from "cursor/canvas";

type Sev = "critical" | "high" | "medium" | "low";

type Area =
  | "src/posix"
  | "src/ec + src/store"
  | "src/client"
  | "util / xrd / metrics"
  | "kernel/aiosfs";

interface Finding {
  id: string;
  area: Area;
  sev: Sev;
  title: string;
  loc: string;
  /** What the code does vs what it should do. */
  detail: string;
  /** The user-visible consequence. */
  impact: string;
  conf: "HIGH" | "MEDIUM" | "LOW";
  /** True when I re-checked the claim against the source myself. */
  verified?: boolean;
}

const SEV_COLOR: Record<Sev, Color> = {
  critical: "red",
  high: "orange",
  medium: "yellow",
  low: "gray",
};

const SEV_ORDER: Sev[] = ["critical", "high", "medium", "low"];

const AREAS: Area[] = [
  "src/posix",
  "src/ec + src/store",
  "src/client",
  "util / xrd / metrics",
  "kernel/aiosfs",
];

const FINDINGS: Finding[] = [
  // ---------------- src/posix ----------------
  {
    id: "P1",
    area: "src/posix",
    sev: "critical",
    title: "Quota ledger reads its CAS from the wrong attribute",
    loc: "quota_ledger.cpp:232,239,452,564,593",
    detail:
      "The ledger writes `aios.posix.cas` but reads `snap.cas`, which `session.cpp:257` populates only from `aios.stl.cas`. So the CAS it reads back is always 0. The file already defines the correct helper, `cas_from_attrs`, and uses it when writing — just not when reading.",
    impact:
      "Usage is never persisted after the first flush, so quota drifts arbitrarily far from reality and is lost on restart. Pending sets grow without bound, and every single write then performs 8 GETs + 8 HEADs while holding the ledger mutex.",
    conf: "HIGH",
    verified: true,
  },
  {
    id: "P2",
    area: "src/posix",
    sev: "critical",
    title: "Chunk writes are unsynchronized read-modify-write with an unconditional PUT",
    loc: "posix_fs.cpp:796-816, 870-876",
    detail:
      "Each write GETs the whole 1 MiB chunk, patches its bytes into a private copy, and PUTs it back with `expected_cas = nullopt` and no lock held.",
    impact:
      "Two writers touching disjoint ranges of the same chunk silently overwrite each other. The loser already returned success with a full byte count. Default stripe is 1 MiB and FUSE writes are 128 KiB, so eight consecutive requests share a chunk.",
    conf: "HIGH",
  },
  {
    id: "P3",
    area: "src/posix",
    sev: "critical",
    title: "store_inode answers a CAS conflict by rewriting its stale record",
    loc: "posix_fs.cpp:604-624",
    detail:
      "The catch handler detects that another writer changed the inode, then discards their record entirely and re-PUTs the caller's stale copy under the refreshed CAS. That converts optimistic concurrency control into last-writer-wins on the whole inode.",
    impact:
      "Concurrent appends lose size updates: bytes are durably in the chunk object but the file is shorter than them, so reads clamp them away. Also loses nlink, xattr, and mode changes. The background rstat thread can clobber a live inode the same way.",
    conf: "HIGH",
  },
  {
    id: "P4",
    area: "src/posix",
    sev: "critical",
    title: "Directory changelog compaction erases the log with no lock and no CAS",
    loc: "posix_fs.cpp:334-345",
    detail:
      "`compact_if_needed` writes a snapshot from its own stale `entries_`, then truncates the shared log with an unconditional PUT, holding no cluster lock. It fires automatically once the log passes 1 MiB.",
    impact:
      "Any dentry another client appended during the window is destroyed permanently — files that were created successfully simply vanish from the directory. If `store_meta` then fails, the directory becomes unreadable until the meta object is repaired by hand.",
    conf: "HIGH",
  },
  {
    id: "P5",
    area: "src/posix",
    sev: "high",
    title: "Exceptions can escape FUSE callbacks and terminate the mount",
    loc: "posix_fs.cpp:1053 and every aios_posix_* entry point",
    detail:
      "Entry points catch only `client_error`. `nlohmann::json::parse`, `std::stoull` in `apply_record`, `std::thread` construction in `write_file`, and `bad_alloc` all throw other types straight through libfuse's C frames.",
    impact:
      "One corrupt metadata object takes down the whole mount for every process using it, instead of returning EIO for one path. The thread-construction case destroys unjoined threads, which is an unconditional terminate.",
    conf: "HIGH",
  },
  {
    id: "P6",
    area: "src/posix",
    sev: "high",
    title: "Cross-directory rename over an existing file leaks its chunks and quota",
    loc: "posix_fs.cpp:519-547",
    detail:
      "GC calls `truncate_file` on the victim after the transaction already deleted its inode and the cache entry was erased, so it returns ENOENT before deleting a single chunk or crediting quota back.",
    impact:
      "Every chunk of the replaced file is orphaned permanently and its bytes stay charged. Repeated overwrite-by-rename eventually yields EDQUOT on a volume that looks nearly empty. Same-directory rename and unlink get this right.",
    conf: "HIGH",
  },
  {
    id: "P7",
    area: "src/posix",
    sev: "high",
    title: "layout_rules and super are shared mutable state with no lock, and the layout path returns a dangling pointer",
    loc: "posix_layout.cpp:48,106-145; posix_fs.cpp:626-651",
    detail:
      "`FsState::mu` covers only the inode cache, flock tokens and rstat set. The 30-second layout refresh calls `clear()` + `push_back` while `match_posix_layout_rule` hands callers a raw pointer into that same vector. `ensure_super` reassigns a `SuperMeta` containing a `std::string` on every mutating operation.",
    impact:
      "Use-after-free on a hot path — layout lookup runs on every metadata and data write.",
    conf: "HIGH",
  },
  {
    id: "P8",
    area: "src/posix",
    sev: "high",
    title: "readdir gives every entry in a batch the same offset cookie",
    loc: "fuse3_ops.cpp:151-164",
    detail:
      "`aios_posix_readdir` advances `off` past the whole batch before the fill loop runs, so all 64 entries are emitted with the batch-end cookie. When `filler` reports the kernel buffer full at entry i, the kernel resumes past the batch.",
    impact:
      "Entries are silently skipped on any directory large enough to fill the readdir buffer. `ls` misses files and `rm -rf` fails with ENOTEMPTY after appearing to succeed. Paging is also unstable because the cookie is a positional index into a freshly re-sorted vector.",
    conf: "HIGH",
  },
  {
    id: "P9",
    area: "src/posix",
    sev: "high",
    title: "QoS burst equals rate, so a single large I/O can never be admitted",
    loc: "qos_controller.cpp:130-146",
    detail:
      "`burst` is capped at `rate` and `tokens` at `burst`, while `cost` is the full I/O size. If one request exceeds the per-second byte limit the bucket is not slow, it is permanently closed. `now_ms()` also uses `system_clock` rather than a monotonic clock.",
    impact:
      "With a 10 MB/s limit, any S3 PUT above 10 MB fails immediately and forever — and it surfaces as EAGAIN from a FUSE write, which applications treat as a hard error rather than backpressure.",
    conf: "HIGH",
  },
  {
    id: "P10",
    area: "src/posix",
    sev: "high",
    title: "create/mkdir have a check-then-act race that orphans an inode",
    loc: "posix_fs.cpp:1161-1182",
    detail:
      "The EEXIST check reads a DirTable loaded moments earlier, then `dir.link` appends unconditionally. Two concurrent creates of one name both pass and both append; replay order decides the winner.",
    impact:
      "The loser is never told. Its inode is orphaned and invisible to readdir. Two concurrent S3 PUTs to the same key both return 200 while one body is written into the unreachable inode.",
    conf: "HIGH",
  },
  {
    id: "P11",
    area: "src/posix",
    sev: "medium",
    title: "Names over 255 bytes are accepted on create but truncated in readdir",
    loc: "posix_fs.cpp:1151, 1094",
    detail:
      "Create validates only that the name is non-empty and slash-free; readdir `snprintf`s into a 256-byte field. statfs advertises namemax 255 but nothing enforces it, and S3 key segments can be far longer.",
    impact:
      "The object is listed under a truncated name, so a client that takes the listed key and issues a GET or DELETE gets 404 — listed but unusable and undeletable through listing-driven tooling.",
    conf: "HIGH",
  },
  {
    id: "P12",
    area: "src/posix",
    sev: "medium",
    title: "Same-directory rename is not atomic and destroys the destination first",
    loc: "posix_fs.cpp:1346-1358, 1213-1214",
    detail:
      "Unlink-target, drop-nlink and rename are three independently committed operations. The cross-directory path does this correctly with cluster locks and /txn; only the same-directory fast path is unprotected. Unlink has the same shape.",
    impact:
      "A failure after the unlink leaves the destination destroyed and the source still at its old name, so the write-temp-then-rename idiom loses the real file. A failed unlink reports an error with the dentry already gone.",
    conf: "HIGH",
  },

  // ---------------- src/ec + src/store ----------------
  {
    id: "E1",
    area: "src/ec + src/store",
    sev: "critical",
    title: "A third EC read path still selects shards by acting-set position and never checks the CRC",
    loc: "object_io.cpp:58-78",
    detail:
      "The same defect already fixed in `reconstruct_ec_object` and `repair_ec_object`: shard identity is the stored `aios.ec.i` attr, not the slot the target occupies after `place()` reorders the set. This call site was missed, and unlike the other two it has no `full_crc` guard.",
    impact:
      "Every caller is a destructive migration — `transition.cpp`, `archive_pack.cpp`, `archive_tape.cpp`. After any topology change the codec is fed mismatched shards, returns true, and the corrupted buffer becomes the new authoritative copy.",
    conf: "HIGH",
    verified: true,
  },
  {
    id: "E2",
    area: "src/ec + src/store",
    sev: "critical",
    title: "shard_len is truncated to int for ISA-L, producing all-zero parity",
    loc: "isal_rs.cpp:100, 185",
    detail:
      "`ec_encode_data` takes `int len` and nothing bounds `shard_len` to INT_MAX, while `max_object_bytes` defaults to 64 GiB. At 2 GiB the cast yields INT_MIN and the length loops do not execute.",
    impact:
      "Parity buffers stay zero, encode returns success, the write satisfies quorum and publishes. The object reads fine while all k data shards survive and is unrecoverable the moment one is lost.",
    conf: "HIGH",
    verified: true,
  },
  {
    id: "E3",
    area: "src/ec + src/store",
    sev: "high",
    title: "decode() never checks that shard length agrees with full_size",
    loc: "isal_rs.cpp:194-202, xor_parity.cpp:93-101",
    detail:
      "`shard_len` is taken from whatever the caller supplied and never compared against the required `ceil(full_size/k)`. Shards that are too long produce misaligned output; too short leaves a zero-filled tail. Both return true.",
    impact:
      "The codec offers no defence of its own against mismatched shards — today only the caller's `full_crc` check catches it, and E1 shows a path with no such check.",
    conf: "HIGH",
  },
  {
    id: "E4",
    area: "src/ec + src/store",
    sev: "high",
    title: "Staging and tmp file names are keyed on seq only, not on the object",
    loc: "object_store.cpp:529-531, 1054",
    detail:
      "Names are `v%016llx.tmp` and `stage-<seq>`, with no oid component. Every object's first version is seq 1, and the staging handlers take no lock while running on the 8-thread RPC pool.",
    impact:
      "Two concurrent uploads of different oids that hash into the same shard at the same seq write into one file. One object ends up holding a blend of both bodies with a correct-looking DB row.",
    conf: "HIGH",
  },
  {
    id: "E5",
    area: "src/ec + src/store",
    sev: "high",
    title: "No serialization of sqlite transactions — one connection per shard, many threads",
    loc: "object_store.cpp:375-384; object_store.hpp has no mutex",
    detail:
      "Every mutating entry point wraps itself in BEGIN IMMEDIATE on a single connection per shard. Replica-side handlers do not hold `ObjectService::mu_`, and repair adds a further thread.",
    impact:
      "Three failures: spurious store errors under load; a rollback discarding another request's uncommitted work; and reads on the same connection seeing uncommitted rows, so repair can sample a shard version that never existed.",
    conf: "HIGH",
  },
  {
    id: "E6",
    area: "src/ec + src/store",
    sev: "high",
    title: "shard_len == 0 doubles as the 'not yet determined' sentinel",
    loc: "isal_rs.cpp:128-133, xor_parity.cpp:56-61",
    detail:
      "If the first present shard is empty, `shard_len` stays 0 and the next shard sets it, so the empty one slips past the mismatch check. Its `data()` is then read as `shard_len` bytes.",
    impact:
      "Heap over-read before any CRC check runs. `ObjectStore::get`'s inline branch makes an empty body reachable because it never compares `blob_len` against the recorded size, unlike the fs branch.",
    conf: "HIGH",
  },
  {
    id: "E7",
    area: "src/ec + src/store",
    sev: "high",
    title: "Repair decodes from a mix of shard generations",
    loc: "repair.cpp:310-327",
    detail:
      "`run_repair` already knows which targets hold a stale seq, but the shard-collection loop filters on presence only. Same-sized generations share a shard length, so a length check would not help either.",
    impact:
      "When `full_crc` is absent the unverified decode is re-encoded over the shards marked for repair, turning a recoverable stale-shard situation into permanent corruption.",
    conf: "HIGH",
  },
  {
    id: "E8",
    area: "src/ec + src/store",
    sev: "medium",
    title: "Version bodies are unlinked before the transaction removing their rows commits",
    loc: "object_store.cpp:903-906",
    detail:
      "`delete_version_row_locked` runs the DELETEs inside the caller's transaction and unlinks immediately, outside it. The `remove_fs_object` result is also discarded.",
    impact:
      "If the commit fails the rows come back but the files are gone, so the DB advertises versions whose bodies do not exist. A crash before the WAL commit is durable has the same result.",
    conf: "HIGH",
  },
  {
    id: "E9",
    area: "src/ec + src/store",
    sev: "medium",
    title: "write_fs_object reports success for a body it may not have fully written",
    loc: "object_store.cpp:537-566",
    detail:
      "The stream state is checked after `write` but not after `flush`, and the destructor's close is never checked. The parent directory is never fsync'd after the rename, and the durability barrier reopens the file read-only.",
    impact:
      "A silently short-written shard still counts toward the EC quorum and the object publishes. The truncation only surfaces later, at the short-read check in get().",
    conf: "HIGH",
  },
  {
    id: "E10",
    area: "src/ec + src/store",
    sev: "medium",
    title: "install_version adopts a pre-placed body without checking size or CRC",
    loc: "object_store.cpp:1611-1618",
    detail:
      "Existence is the only test; `size` and `crc32c` come from the wire and are written into the row unverified. `handle_stage_commit` relies entirely on this branch.",
    impact:
      "A streamed upload truncated mid-transfer installs as a valid version whose recorded CRC describes bytes the file does not contain, so every later integrity check compares against a fiction.",
    conf: "HIGH",
  },
  {
    id: "E11",
    area: "src/ec + src/store",
    sev: "medium",
    title: "crc_file_range zero-fills past EOF and returns success",
    loc: "object_store.cpp:655-659",
    detail:
      "A `pread` returning 0 means the file is shorter than the range being checksummed. Instead of reporting it, the function pretends the missing tail is zeros. `ftruncate` in `ensure_fs_size` already guarantees the length, so the branch has no legitimate trigger.",
    impact:
      "`recompute_crc32c` writes the result back, so a truncated body gets a freshly blessed CRC matching its truncated content. Repair calls this on the authoritative replica exactly when the CRC is unknown.",
    conf: "HIGH",
  },
  {
    id: "E12",
    area: "src/ec + src/store",
    sev: "medium",
    title: "publish_tip accepts a backwards tip; install_version idempotency ignores attrs",
    loc: "object_store.cpp:1668-1686, 1571-1579",
    detail:
      "The tip update has no check that seq is at least the current tip, and is reachable straight from the wire with no lock. The install fast path compares body identity only, never attrs.",
    impact:
      "Repair publishing at auth_seq can roll a target that legitimately holds a newer version backwards. A re-install with a corrected `aios.ec.i` silently leaves the old attr in place — the one attribute shard identity now depends on.",
    conf: "MEDIUM",
  },

  // ---------------- src/client ----------------
  {
    id: "C1",
    area: "src/client",
    sev: "critical",
    title: "changelog compact() truncates the shared log with a CAS concurrent appends cannot invalidate",
    loc: "changelog.cpp:351",
    detail:
      "The empty PUT is guarded on `aios.stl.cas`, but ops arrive via `POST /o/{oid}/append`, which the server handles with `replace_attrs=false` — so an append never changes that attr and the guard always passes. Compaction is automatic above 1 MiB.",
    impact:
      "Records another client appended during the window are destroyed, after that client's append returned success. Permanent silent loss of committed writes.",
    conf: "HIGH",
  },
  {
    id: "C2",
    area: "src/client",
    sev: "critical",
    title: "flush() advances applied_op past ops it never applied",
    loc: "map.hpp:74 and the four sibling containers",
    detail:
      "The cursor is set to `next_op - 1`, the highest id anyone reserved, not the client's own last record. In async mode `ensure_fresh_read()` short-circuits, so the client never pulled those ops.",
    impact:
      "On its own, a silently stale local view. Combined with C1 it is permanent corruption: the very next `maybe_compact()` writes the divergent local state as a snapshot claiming to contain the ops it skipped, then truncates the log.",
    conf: "HIGH",
  },
  {
    id: "C3",
    area: "src/client",
    sev: "high",
    title: "Records can land out of op_id order and readers silently drop the late one",
    loc: "changelog.cpp:284 (write), 259 (read)",
    detail:
      "Reserving the id and appending the record are two separate round trips, so the log can physically contain [6][5]. The reader keys off a single high-water mark and skips anything at or below it.",
    impact:
      "A durably written op becomes permanently invisible to every client including its author. Silent lost update under ordinary write contention.",
    conf: "HIGH",
  },
  {
    id: "C4",
    area: "src/client",
    sev: "high",
    title: "Distributed mutex never renews its lease, and a failed unlock terminates the process",
    loc: "mutex.cpp:26",
    detail:
      "`lock_acquire` discards the `expires_ms` the server returns, there is no renewal, and `owns_lock()` is just a non-empty token. Containers do not pass the lock token on writes, so the server cannot fence a stale holder. `unlock()` throws from a `lock_guard` destructor.",
    impact:
      "Any stall longer than the 30s TTL — including one caused by C6 — silently gives two clients the lock. The cleanup path for that same scenario then crashes the process.",
    conf: "HIGH",
  },
  {
    id: "C5",
    area: "src/client",
    sev: "high",
    title: "A short response body is accepted silently as a complete body",
    loc: "session.cpp:206",
    detail:
      "The read loop breaks on error and returns whatever it has with status 200. Over-reads are trimmed; under-reads are not even detected. The server can produce this too, since `write_file_body` commits to a Content-Length before a possibly short file read.",
    impact:
      "`pull()` applies a prefix of the changelog, marks itself current, and reports success. A later compaction then writes the dropped tail out of existence.",
    conf: "HIGH",
  },
  {
    id: "C6",
    area: "src/client",
    sev: "high",
    title: "No timeout on any socket operation",
    loc: "session.cpp:151",
    detail:
      "Every call is the blocking synchronous form with no deadline — no `expires_after`, no SO_RCVTIMEO, no async-with-timer. This is the same unbounded-wait class already fixed on the server side.",
    impact:
      "One unresponsive node hangs application threads forever; in the FUSE-facing paths that is an unkillable operation. It also compounds C4 by blowing past the lock lease with no bound.",
    conf: "HIGH",
  },
  {
    id: "C7",
    area: "src/client",
    sev: "medium",
    title: "Response bodies are allocated without any size limit",
    loc: "session.cpp:186",
    detail:
      "`kMaxBodyBytes` (16 MiB) is enforced on every outbound body but never on inbound ones, and `std::stoull(\"-1\")` parses without throwing. `pull()` range-gets a span sized by a server-supplied header.",
    impact:
      "Trivial memory exhaustion of any client by the node it is talking to — which, after a 307, need not be a node the client chose.",
    conf: "HIGH",
  },
  {
    id: "C8",
    area: "src/client",
    sev: "medium",
    title: "Request bodies are never signed, and redirects are followed to unauthenticated hosts",
    loc: "session.cpp:127, 214",
    detail:
      "The client always sends the literal `UNSIGNED-PAYLOAD`, even though the server hashes the real body whenever that header says anything else. Transport is plain HTTP, and Location comes from an unauthenticated response.",
    impact:
      "No integrity on writes: an on-path attacker can rewrite the body of any signed PUT and the HMAC still verifies. A tampered Location exfiltrates the body to a chosen host.",
    conf: "HIGH",
  },
  {
    id: "C9",
    area: "src/client",
    sev: "medium",
    title: "CRLF injection through attribute values into request headers",
    loc: "session.cpp:307, 148, 312",
    detail:
      "Object ids are URL-encoded but header keys and values are not filtered anywhere, and only two headers are signed. App label and lock token have the same exposure.",
    impact:
      "An application storing user-controlled data in attrs lets that data inject headers that pass authentication — including the CAS preconditions the client believes it is enforcing.",
    conf: "HIGH",
  },
  {
    id: "C10",
    area: "src/client",
    sev: "medium",
    title: "Changelog framing decoded inconsistently; an unvalidated op_id can wedge a client",
    loc: "changelog.cpp:104",
    detail:
      "The parser is memory-safe, but the 16-byte header is read without checking it fits inside the declared `header_len`, the op value is not range-checked, and `op_id` is never validated against `next_op`.",
    impact:
      "One record carrying `op_id = UINT64_MAX` freezes the container for that client permanently, surviving reconnects. `parse_index` also throws non-`client_error` exceptions from inside apply.",
    conf: "MEDIUM",
  },
  {
    id: "C11",
    area: "src/client",
    sev: "medium",
    title: "Malformed server responses escape as nlohmann exceptions, not client_error",
    loc: "wire.cpp:69; changelog.cpp:197, 216",
    detail:
      "Parsers throw `json::parse_error`, `json::out_of_range` and `json::type_error` on the hot read path of every container, before any validation. The library's error contract is `client_error` with a `code()`.",
    impact:
      "A truncated document surfaces as an exception type callers do not catch, and `append_op`'s conflict-retry `catch (const client_error&)` no longer matches.",
    conf: "HIGH",
  },
  {
    id: "C12",
    area: "src/client",
    sev: "low",
    title: "list/deque mutations through non-const operator[] are never persisted",
    loc: "list.hpp:116, deque.hpp:117",
    detail:
      "`operator[]` marks the container dirty, but `flush()` only writes `pending`, which a write through the returned reference never populates. It then clears the dirty flag and returns normally.",
    impact:
      "`l[0] = \"x\"; l.flush();` discards the write and reports success. It can appear to work in a single-process test and vanish in production.",
    conf: "HIGH",
  },

  // ---------------- util / xrd / metrics ----------------
  {
    id: "U1",
    area: "util / xrd / metrics",
    sev: "critical",
    title: "Attacker-controlled compression full_size crashes the daemon on GET",
    loc: "compression.cpp:56; object_service.cpp:97",
    detail:
      "`zstd_decompress` calls `out.resize(logical_size)` with a value taken straight from the client-settable `aios.compression.full_size` attribute, with no cap. The resulting `std::length_error` is not caught anywhere up the stack.",
    impact:
      "Any client with the cluster key can terminate aiosd on demand, and can store one poisoned object whose every later GET or HEAD re-crashes the daemon. The reviewing agent reproduced this live against a running instance.",
    conf: "HIGH",
    verified: true,
  },
  {
    id: "U2",
    area: "util / xrd / metrics",
    sev: "medium",
    title: "Non-constant-time HMAC comparison for cluster and RPC auth",
    loc: "util/auth.cpp:91",
    detail:
      "Gossip, object-RPC and Hello signatures are verified with `std::string::operator!=`, which returns at the first differing byte. The HTTP path already does this correctly with `const_time_eq`.",
    impact:
      "A timing side channel on HMAC verification. Low exploitability over a network, but the codebase already has the primitive and uses it elsewhere.",
    conf: "HIGH",
  },
  {
    id: "U3",
    area: "util / xrd / metrics",
    sev: "medium",
    title: "Signed messages are replayable within the skew window",
    loc: "util/auth.cpp:67; http_auth.cpp:105",
    detail:
      "Both verifiers accept any correctly-signed request inside `auth_skew_ms`, which defaults to 60 seconds. There is no nonce and no replay cache — the timestamp bounds staleness, not replay.",
    impact:
      "A passive observer can duplicate any mutating operation for up to a minute: re-issue a DELETE, re-apply a gossip state, repeat an admin call.",
    conf: "HIGH",
  },
  {
    id: "U4",
    area: "util / xrd / metrics",
    sev: "medium",
    title: "XRootD data-plane ops authorize against a stale thread-local caller",
    loc: "XrdAiosDF.cpp:71-112",
    detail:
      "Open and the metadata ops call `apply_caller`, but Read/Write/Fstat/Fsync/Ftruncate do not. Identity lives in a thread_local keyed by fs pointer, and XRootD dispatches from a worker pool.",
    impact:
      "Data-plane access is checked against whichever identity that thread last saw — a spurious EACCES, or via the uid=0 default, the AIOS-level check effectively skipped.",
    conf: "MEDIUM",
  },
  {
    id: "U5",
    area: "util / xrd / metrics",
    sev: "low",
    title: "base64 decoder silently accepts malformed padding",
    loc: "util/base64.cpp:49-61",
    detail:
      "Input like `AB=C` (pad in the third slot, data in the fourth) passes: the guard checks only for -1 and misses the -2 pad sentinel, so the last byte is computed from a negative value.",
    impact:
      "Malformed base64 yields garbage bytes instead of an error. Used for inter-node replicated payloads and POSIX xattr values, all HMAC-authenticated, so it is robustness rather than an external hole.",
    conf: "HIGH",
  },

  // ---------------- kernel/aiosfs ----------------
  {
    id: "K1",
    area: "kernel/aiosfs",
    sev: "critical",
    title: "http_backend.c does not compile — drop_nlink redefinition",
    loc: "http_backend.c:1135, 1449",
    detail:
      "A file-scope `static int drop_nlink(struct aios_sb_info *, u64)` conflicts with the `drop_nlink(struct inode *)` that `<linux/fs.h>` has defined for decades, and line 1449 then calls it with one argument intending the VFS helper.",
    impact:
      "The HTTP backend of aiosfs has never been built in its current state. That also means my own workqueue changes to this file are unverified, and it explains why K4 went unnoticed.",
    conf: "HIGH",
    verified: true,
  },
  {
    id: "K2",
    area: "kernel/aiosfs",
    sev: "critical",
    title: "json_get_u64 cannot parse anything",
    loc: "http_backend.c:111-127, 180",
    detail:
      "`kstrtou64` requires the entire string to be the number, but it is handed a pointer into the middle of the JSON document, so it always sees trailing `,` or `}` and returns -EINVAL. `parse_entries` has the same defect.",
    impact:
      "Every HTTP-backend metadata field reads back as 0. The root inode gets mode 0 so the mount succeeds and the first ls fails with ENOTDIR; all files collapse onto ino 0; setting one xattr rewrites the inode's real mode and size with zeros.",
    conf: "HIGH",
    verified: true,
  },
  {
    id: "K3",
    area: "kernel/aiosfs",
    sev: "critical",
    title: "Reply length is never clamped to the caller's buffer",
    loc: "io.c:31-43",
    detail:
      "`hdr->size` is validated against the reply length but never against `len`, the caller's buffer size. Upcall payloads may be up to 1 MiB.",
    impact:
      "A daemon answering a 4096-byte read with size = 1 MiB writes ~1 MiB of chosen content into the kmapped page cache page. Out-of-bounds kernel write: corruption or an oops.",
    conf: "HIGH",
  },
  {
    id: "K4",
    area: "kernel/aiosfs",
    sev: "critical",
    title: "getattr overwrites i_size from the server and writeback then drops the dirty tail",
    loc: "inode.c:16, 62-67",
    detail:
      "`aios_stat_to_inode` stores `st->size` into a live inode with no lock and no comparison. Between a buffered write and the next flush the server's size is stale and smaller, and both writeback paths bail at `pos >= i_size`.",
    impact:
      "Write 1 MiB, then any stat — `ls -l`, tar, rsync, cp — and the flusher marks every page clean and discards the data. No error to userspace, nothing in dmesg.",
    conf: "HIGH",
  },
  {
    id: "K5",
    area: "kernel/aiosfs",
    sev: "critical",
    title: "drop_inode = generic_delete_inode discards dirty pages instead of writing them back",
    loc: "super.c:185-192; http_backend.c:2853-2860",
    detail:
      "Returning 1 unconditionally makes iput_final take the drop branch, skipping `write_inode_now`. `aios_d_revalidate` returns 0 for everything, so dentries are killed immediately and inodes reach zero refcount constantly.",
    impact:
      "`echo hi > /mnt/f; ls /mnt/f` is enough to lose the write: the lookup invalidates the dentry, the inode is evicted, and the dirty page is freed before the 30-second flush.",
    conf: "MEDIUM",
  },
  {
    id: "K6",
    area: "kernel/aiosfs",
    sev: "high",
    title: "readdir skips entries when the user buffer fills, and trusts next_offset",
    loc: "inode.c:374-387",
    detail:
      "The loop breaks when `dir_emit` reports the buffer full, but `ctx->pos` is still advanced to the batch-end cookie. `next_offset` is never checked for forward progress either.",
    impact:
      "ls silently misses files in any directory that does not fit one getdents call — the same defect as P8, in the other front end. A daemon returning a non-advancing offset spins getdents forever.",
    conf: "HIGH",
  },
  {
    id: "K7",
    area: "kernel/aiosfs",
    sev: "high",
    title: "File and xattr names are interpolated into JSON unescaped",
    loc: "http_backend.c:420-426, 879-880",
    detail:
      "Names come straight from `d_name` with only `/` and NUL rejected, and are written into the directory snapshot with `%s` inside quotes. POSIX permits quotes, backslashes and newlines in filenames.",
    impact:
      "`touch 'a\"b'` makes the snapshot unparseable, so every later lookup, create, unlink and readdir in that directory fails with EINVAL — an unprivileged permanent DoS on the directory. A crafted name can instead rewrite another entry's inode number.",
    conf: "HIGH",
  },
  {
    id: "K8",
    area: "kernel/aiosfs",
    sev: "high",
    title: "Metadata-only inode writeback issues a truncate that deletes chunk objects",
    loc: "pagecache.c:341-349",
    detail:
      "Any dirty inode — including one dirtied only by `file_update_time()` — sends the current in-core `i_size` to the backend, where `truncate_file` deletes every chunk past the new end.",
    impact:
      "Whenever the in-core size trails the stored size, routine writeback destroys data. That happens after K4, and independently whenever another client appends to a file this node has cached.",
    conf: "HIGH",
  },
  {
    id: "K9",
    area: "kernel/aiosfs",
    sev: "medium",
    title: "mutex_lock() inside the wait_event_interruptible condition",
    loc: "upcall.c:170-181",
    detail:
      "The condition is evaluated with the task already in TASK_INTERRUPTIBLE. Taking a mutex there trips the atomic-sleep debug check and, on contention, resets the state to TASK_RUNNING so the following schedule() returns immediately.",
    impact:
      "The daemon's read spins in a tight re-evaluation loop instead of sleeping. The final `daemon_open` read is also outside the lock it just dropped.",
    conf: "MEDIUM",
  },
  {
    id: "K10",
    area: "kernel/aiosfs",
    sev: "medium",
    title: "Same-directory rename orphans the victim and ignores emptiness",
    loc: "http_backend.c:1471-1478",
    detail:
      "`apply_dir_op(RENAME)` just drops any entry with the target name from the in-memory table. No inode, chunk or directory object is deleted, no nlink is decremented, and there is no emptiness check. The cross-directory path gets all of this right.",
    impact:
      "The replaced file's objects leak in the cluster forever, and renaming over a non-empty directory detaches its whole subtree instead of returning ENOTEMPTY.",
    conf: "HIGH",
  },
  {
    id: "K11",
    area: "kernel/aiosfs",
    sev: "medium",
    title: "Upcall rename maintains no link counts",
    loc: "inode.c:222-233",
    detail:
      "When the target dentry is positive the filesystem must `drop_nlink()` the replaced inode, and moving a directory between parents must adjust both parents for the `..` entry. Neither happens.",
    impact:
      "An overwritten inode keeps nlink 1 forever, so fstat on a still-open replaced file is wrong and — once K5 is fixed — the inode is never deleted. Parent link counts drift.",
    conf: "HIGH",
  },
  {
    id: "K12",
    area: "kernel/aiosfs",
    sev: "medium",
    title: "O_DIRECT invalidates the page cache without flushing it first",
    loc: "pagecache.c:263-267, 296-302",
    detail:
      "`invalidate_inode_pages2_range` cannot invalidate a dirty page and returns -EBUSY. The standard sequence is flush, then invalidate.",
    impact:
      "Any O_DIRECT read or write overlapping pages dirtied by an earlier buffered write fails with EBUSY rather than doing the I/O, and ordering between the two is undefined where it does proceed.",
    conf: "MEDIUM",
  },
];

const CLEAN: { area: string; note: string }[] = [
  {
    area: "Erasure coding maths",
    note: "Stripe arithmetic, last-stripe padding and the gather/scatter were traced by hand for k=4/m=2 at sizes 0, 1, 5 and 100 with no off-by-one. `gen_decode_matrix` is a faithful port of ISA-L's reference, matrix-inversion failure is surfaced, and the nerrs guard correctly caps erasures at m. XOR is a correct systematic code for m==1 and the factory rejects m>1.",
  },
  {
    area: "util/crc32c and util/aes_gcm",
    note: "crc32c was brute-tested aligned and unaligned, incremental against one-shot, and `crc32c_combine` against 200 random concatenations — all matched. AES-GCM checks key and nonce lengths, surfaces tag failure, and frees contexts on every error path.",
  },
  {
    area: "Metrics cardinality",
    note: "App labels are charset- and length-validated so no Prometheus label injection is possible, distinct labels are capped at 256 with an overflow bucket, counters are atomic, and /metrics is unauthenticated only under the explicit opt-in.",
  },
  {
    area: "Client HMAC canonicalization",
    note: "The canonical string agrees end to end — the client signs the full target including the query string and the server verifies the same raw target, header names are lowercased on both sides, and redirect hops are bounded and correctly re-signed.",
  },
];

function SeverityLegend() {
  return (
    <Row gap={16} wrap align="center">
      {SEV_ORDER.map((s) => {
        const n = FINDINGS.filter((f) => f.sev === s).length;
        return (
          <div key={s}>
            <Row gap={6} align="center">
              <Swatch color={SEV_COLOR[s]} />
              <Text size="small" tone="secondary">
                {n} {s}
              </Text>
            </Row>
          </div>
        );
      })}
    </Row>
  );
}

function FindingRow({ f }: { f: Finding; key?: string }) {
  const t = useHostTheme();
  return (
    <CollapsibleSection
      title={f.title}
      leading={<Swatch color={SEV_COLOR[f.sev]} />}
      trailing={
        <Row gap={8} align="center">
          {f.verified ? <Pill size="sm">re-verified</Pill> : null}
          <Text size="small" tone="tertiary">
            {f.conf}
          </Text>
        </Row>
      }
    >
      <Stack gap={8} style={{ paddingBottom: 8 }}>
        <Text size="small" tone="tertiary" style={{ fontFamily: "monospace" }}>
          {f.id} · {f.loc}
        </Text>
        <Text size="small" tone="secondary">
          {f.detail}
        </Text>
        <Text size="small" style={{ color: t.text.primary }}>
          {f.impact}
        </Text>
      </Stack>
    </CollapsibleSection>
  );
}

export default function AiosFullCodeReview() {
  const t = useHostTheme();
  const [area, setArea] = useCanvasState<Area | "all">("area", "all");
  const [minSev, setMinSev] = useCanvasState<Sev | "all">("minSev", "all");

  const shown = FINDINGS.filter((f) => {
    if (area !== "all" && f.area !== area) return false;
    if (minSev !== "all") {
      const cut = SEV_ORDER.indexOf(minSev);
      if (SEV_ORDER.indexOf(f.sev) > cut) return false;
    }
    return true;
  });

  const critical = FINDINGS.filter((f) => f.sev === "critical").length;
  const high = FINDINGS.filter((f) => f.sev === "high").length;

  return (
    <Stack gap={20} style={{ padding: 24, maxWidth: 1000 }}>
      <Stack gap={6}>
        <H1>aios — first full-codebase review</H1>
        <Text tone="secondary">
          Five parallel reviews covering the areas never previously audited: `src/posix`,
          `src/ec` + `src/store`, `src/client`, `src/xrd` / `cuobject` / `metrics` /
          `util`, and the parts of `kernel/aiosfs` outside the earlier workqueue pass.
        </Text>
        <Text size="small" tone="tertiary">
          Source: Opus 5 subagent reviews of ~47k lines of non-test source · findings marked
          &ldquo;re-verified&rdquo; were re-checked against the source directly · confidence is the
          reviewer&rsquo;s own rating
        </Text>
      </Stack>

      <Grid columns={4} gap={16}>
        <Stat value={FINDINGS.length} label="Total findings" />
        <Stat value={critical} label="Critical" tone="danger" />
        <Stat value={high} label="High" tone="warning" />
        <Stat value="5" label="Re-verified by hand" tone="info" />
      </Grid>

      <Callout tone="danger" title="Two of these change what you can trust today">
        <Text size="small">
          `kernel/aiosfs`&rsquo;s HTTP backend does not compile (K1), so that whole backend — including
          the workqueue changes I committed earlier — has never been built. And any client holding the
          cluster key can terminate `aiosd` on demand, or poison a single object so every later GET
          re-crashes it (U1), which was reproduced live against a running instance.
        </Text>
      </Callout>

      <Stack gap={10}>
        <H2>Findings</H2>
        <Row gap={8} wrap align="center">
          <Pill active={area === "all"} onClick={() => setArea("all")}>
            All areas
          </Pill>
          {AREAS.map((a) => (
            <div key={a}>
              <Pill active={area === a} onClick={() => setArea(a)}>
                {a}
              </Pill>
            </div>
          ))}
        </Row>
        <Row gap={8} wrap align="center">
          <Pill active={minSev === "all"} onClick={() => setMinSev("all")}>
            All severities
          </Pill>
          {SEV_ORDER.map((s) => (
            <div key={s}>
              <Pill active={minSev === s} onClick={() => setMinSev(s)}>
                {s} and above
              </Pill>
            </div>
          ))}
          <Spacer />
          <Text size="small" tone="tertiary">
            {shown.length} shown
          </Text>
        </Row>
        <SeverityLegend />
        <Divider />
        <Stack gap={2}>
          {shown.map((f) => (
            <FindingRow key={f.id} f={f} />
          ))}
        </Stack>
      </Stack>

      <Stack gap={10}>
        <H2>Suggested order</H2>
        <Text size="small" tone="secondary">
          Ordered by irreversibility of the damage rather than by severity label. Silent, permanent
          data loss first; things that fail loudly last.
        </Text>
        <Stack gap={8}>
          {[
            {
              n: "1",
              head: "Stop the silent EC corruption",
              body: "E1 and E2. E1 is the third instance of a bug already fixed twice, and it feeds the tiering and archive paths that overwrite the authoritative copy. E2 makes large objects encode zero parity while reporting success. Both are small, contained changes.",
            },
            {
              n: "2",
              head: "Close the remote crash",
              body: "U1. One bounds check plus a try around the resize. It is the only finding an outside party can trigger at will.",
            },
            {
              n: "3",
              head: "Decide what kernel/aiosfs is for",
              body: "K1 means the HTTP backend has never compiled, and K2 means it could not have worked if it had. Before fixing K3–K12 individually, it is worth deciding whether that backend is a live target — if it is, it needs a build in CI first, because nothing here can be verified from this machine.",
            },
            {
              n: "4",
              head: "Make POSIX writes safe under concurrency",
              body: "P1 through P4. P1 is a one-line fix with disproportionate effect. P2, P3 and P4 are the same shape — unconditional writes where a CAS or a lock is needed — and are the reason a second writer silently loses data today.",
            },
            {
              n: "5",
              head: "Harden the store beneath it",
              body: "E4, E5, E9 and E11. The sqlite serialization gap (E5) is the one most likely to be producing confusing errors under load already.",
            },
            {
              n: "6",
              head: "Client library",
              body: "C1 through C6. C1 and C2 destroy other clients' committed writes; C6 is the same missing-timeout fix already applied server-side and would take minutes.",
            },
          ].map((s) => (
            <div key={s.n}>
              <Row gap={12} align="start">
                <Text
                  size="small"
                  weight="semibold"
                  style={{ color: t.accent.primary, minWidth: 16 }}
                >
                  {s.n}
                </Text>
                <Stack gap={2} style={{ minWidth: 0 }}>
                  <Text weight="semibold">{s.head}</Text>
                  <Text size="small" tone="secondary">
                    {s.body}
                  </Text>
                </Stack>
              </Row>
            </div>
          ))}
        </Stack>
      </Stack>

      <Stack gap={10}>
        <H2>Checked and found correct</H2>
        <Text size="small" tone="secondary">
          Recording these so they are not re-reviewed later.
        </Text>
        <Stack gap={2}>
          {CLEAN.map((c) => (
            <div key={c.area}>
              <CollapsibleSection title={c.area} leading={<Swatch color="green" />}>
                <Text size="small" tone="secondary" style={{ paddingBottom: 8 }}>
                  {c.note}
                </Text>
              </CollapsibleSection>
            </div>
          ))}
        </Stack>
      </Stack>
    </Stack>
  );
}
