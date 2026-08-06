# Development statistics

Snapshot generated **2026-08-06** from git history and the Cursor agent transcript
[`4ae526f1-a71a-4795-b75c-1e40ff32bf86`](https://cursor.com).

Sources of uncertainty are called out inline. Token usage is **not** from a
billing meter (Cursor/Grok totals are not available in this environment).

---

## 1. Cursor chat (Aug 1–6)

Calendar span **Sat 1 Aug 09:59 → Thu 6 Aug 12:08 (UTC+2)** (~122 h), across **6**
active calendar days.

| Metric | Value | Notes |
|--------|------:|-------|
| User turns | **~268** | Timestamped `<user_query>` events |
| Tool calls | **~5,050** | Counted in transcript |
| Active time (est.) | **~31–44 h** | Sum of gaps between user turns, capped at 45–90 min |

This is **not** continuous coding time; long idle gaps between sessions are capped so
overnight pauses are not counted as work.

---

## 2. Grok 4.5 tokens

| Scope | Exact billed tokens | Rough lower bound (message text only) |
|-------|---------------------|----------------------------------------|
| Whole chat (Aug 1–6) | **Unknown** | ~**60–65k** (chars ÷ 3.5) |

Message-text estimates **exclude** tool results, file contents, and multi-turn
context reuse, so real Grok/Cursor billed tokens are likely **several times higher**.
Authoritative source: Cursor **Settings → Usage / Billing**.

---

## 3. Repository log (since 2026-08-01)

Range: parent of first Aug 1 commit → `HEAD` at generation time.

| Metric | Value |
|--------|------:|
| Commits | **59** (`f43b067` … `403cda0`) |
| Files changed | **~237** |
| Insertions | **+51,471** |
| Deletions | **−752** |
| Net | **+50,719** |

### Commit log (oldest → newest)

```
f43b067 2026-08-01 14:08:30 +0200 Add object redirect versions that point to another oid.
2e757f5 2026-08-01 19:23:44 +0200 Expand tests with advanced coverage and a live mini-cluster.
66f56d9 2026-08-02 14:38:47 +0200 Add multithreaded HTTP client benchmark with latency percentiles.
3ea02a8 2026-08-02 15:55:48 +0200 Add streaming objects, cluster LIST, primary redirects, and aios CLI.
63e3e2d 2026-08-02 15:58:23 +0200 Stream CLI I/O, chunked remote gets, and multi-RPC TCP sessions.
596b0d5 2026-08-02 22:12:18 +0200 Add cross-object transactions via prepare, commit, and abort.
f56cc4f 2026-08-02 22:20:15 +0200 Add XOR 2+1 erasure coding as an optional durability profile.
6735d64 2026-08-02 22:25:53 +0200 Add optional ISA-L Reed-Solomon erasure coding.
877c906 2026-08-02 22:28:42 +0200 Expand erasure-coding tests for degraded reads and repair.
6adb734 2026-08-02 22:31:33 +0200 Add HTTP EC coverage, 4+2 service tests, and EC+txn reads.
19f216e 2026-08-02 22:43:05 +0200 Add per-object layout selection at PUT time.
c21d711 2026-08-02 22:44:40 +0200 Wire per-object layout through TCP++ Put and aios-bench.
6e0a35a 2026-08-03 09:15:15 +0200 Add oid-prefix layout rules as admin defaults.
5790e0f 2026-08-03 09:37:09 +0200 Add enforced object locks and HTTP long-poll watches.
6808bff 2026-08-03 09:48:38 +0200 Add HTTP topic pub/sub with ephemeral, buffered, and durable modes.
cbc030e 2026-08-03 09:53:08 +0200 Rewrite README into a fuller project guide without RADOS framing.
786a6c8 2026-08-03 10:05:49 +0200 Add STL-like persistent C++ client with SYNC/ASYNC modes.
eca7d2f 2026-08-03 10:10:21 +0200 Add STL client mode to aios-bench and expand README coverage.
fa03855 2026-08-03 16:55:38 +0200 Add atomic append, STL changelog, and admin monitoring.
8dca5f7 2026-08-03 17:01:35 +0200 Add client application labels for traffic tagging and OPS.
545bc07 2026-08-03 21:21:26 +0200 Add consistent-hash placement with storage classes and transitions.
ef3a004 2026-08-03 21:21:48 +0200 Clarify layout.md entry in README documentation table.
a97c88f 2026-08-03 22:15:40 +0200 Add POSIX C ABI filesystem client and optional FUSE3 mount.
690e730 2026-08-04 08:28:29 +0200 Add POSIX xattr, flock, and hard link support.
1263064 2026-08-04 09:19:48 +0200 Make cross-directory rename consistent via multi-object txns.
928a55e 2026-08-04 09:37:34 +0200 Add AlmaLinux 9 aiosfs kernel prototype with userspace bridge.
c311fb5 2026-08-04 10:27:17 +0200 Add in-kernel HTTP backend and page-cache writeback for aiosfs.
e6dcc18 2026-08-04 12:18:19 +0200 Add cross-directory rename to the in-kernel HTTP backend.
3b6d103 2026-08-04 14:24:18 +0200 Add aiosvd block volumes and densify the AlmaLinux 9 kernel stack.
fba8b45 2026-08-04 14:36:53 +0200 Add FS-backed S3-compatible API to aiosd.
ec9e4c0 2026-08-04 14:44:36 +0200 Add browser admin panel with cluster-key session login.
ce729e8 2026-08-04 21:13:11 +0200 Add per-bucket S3 IAM credentials with POSIX ownership.
18270d9 2026-08-04 21:35:03 +0200 Add soft uid/gid and project subtree quotas.
d6c640b 2026-08-04 22:23:38 +0200 Add soft IOPS and bandwidth QoS for FUSE and S3.
9dcc546 2026-08-04 22:32:13 +0200 Separate S3, FS, and VBD IO monitoring in admin.
4c9733c 2026-08-05 09:51:10 +0200 Add optional ZSTD whole-object PUT compression with ratio monitoring.
fca1ea9 2026-08-05 10:44:05 +0200 Add cuObject GPUDirect S3 scaffold with optional RDMA offload.
cf3e3a7 2026-08-05 10:57:39 +0200 Support multi byte-range S3 GetObject with multipart/byteranges.
84f169e 2026-08-05 11:18:43 +0200 Add thread-scoped POSIX caller credentials and mode checks.
ccbf198 2026-08-05 11:28:22 +0200 Add XRootD OSS plugin mapping entity.name through local passwd.
567dde9 2026-08-05 11:32:54 +0200 Document access methods overview and aiosfs NFS export.
ade36e7 2026-08-05 12:02:01 +0200 Add cold archive packing with tape drain and recall.
7e8553e 2026-08-05 12:13:05 +0200 Add s3 and xrdcp tape sink drivers for archive drain/recall.
01ce94c 2026-08-05 14:13:28 +0200 Add POSIX/VBD snapshot backup with archive copy-out.
2ba57b5 2026-08-05 14:19:43 +0200 Expose archive and backup actions in the CLI and admin UI.
d6d080e 2026-08-05 14:39:56 +0200 Add subtree snapshots and live GFS backup policies.
f28fd5b 2026-08-05 14:54:33 +0200 Add whole-bag ZSTD compression and AES-256-GCM encryption.
7437bc3 2026-08-05 15:13:13 +0200 Add parent_ino pointers and lazy recursive directory stats.
2946071 2026-08-05 18:09:40 +0200 Add templated STL keys/values with string wire codecs.
83ddabf 2026-08-05 18:19:59 +0200 Document cold archive, backup, and POSIX virtual attrs in the README.
508b715 2026-08-06 10:36:55 +0200 Add POSIX subtree meta/data layout rules with EXDEV across domains.
27928a9 2026-08-06 10:48:57 +0200 Add cluster VBD volume registry with safer delete and admin backup.
18d0e4c 2026-08-06 11:41:27 +0200 Fix Alma/RHEL configure when FindSQLite3 omits SQLite3::SQLite3.
97e56ab 2026-08-06 11:49:06 +0200 Add node and filesystem lifecycle (up/drain/off) with capacity weights.
344cc43 2026-08-06 11:52:47 +0200 Derive placement weights from capacity and add free-space autotune.
d1513f3 2026-08-06 11:56:54 +0200 Build static libs with -fPIC for libXrdAios.so on Linux.
3576728 2026-08-06 11:59:26 +0200 Add rack failure domain and rename gossip liveness to online/offline.
267049b 2026-08-06 12:02:58 +0200 Silence GCC -Wmissing-field-initializers on partial aggregate inits.
403cda0 2026-08-06 12:10:44 +0200 Add development statistics and August commit log snapshot.
```

Refresh the log with:

```bash
git log --since='2026-08-01' --format='%h %ci %s' --reverse
git diff --shortstat "$(git log --since='2026-08-01' --reverse --format='%H' | head -1)^"..HEAD
```
