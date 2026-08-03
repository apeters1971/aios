# AIOS HTTP object API (v1)

Listen address: `http_listen` (default `0.0.0.0:7480`). Separate from TCP++ gossip/object RPC.

Bodies are **raw octets** (never base64). Connections are HTTP/1.1 keep-alive by default.

## Authorization

```
Authorization: AIOS-HMAC-SHA256 Credential=<id>, SignedHeaders=<list>, Signature=<hex>
x-aios-date: <unix-ms>
x-aios-content-sha256: <sha256-hex of body> | UNSIGNED-PAYLOAD
```

### Application label (optional)

Clients may tag requests with a workload label:

```
x-aios-app-label: my-app
```

Rules: 1–64 chars matching `[A-Za-z0-9_.:/-]+`. Empty/absent = unlabeled. Invalid → `400`.

Labels feed per-app OPS counters (and are reserved for future QoS). See [`admin.md`](admin.md). The `aios` CLI accepts `--app-label`; `SessionConfig::app_label` / `Session::set_app_label` for `aios_client`.

Canonical string:

```
<METHOD>\n
<PATH?QUERY>\n
<date>\n
<header>:<value>\n   # for each signed header, lowercased, sorted
...
<SignedHeaders>\n
<payload_hash>
```

`Signature = HMAC-SHA256(cluster_key, canonical)` as lowercase hex.

## Endpoints

| Method | Path | Notes |
|--------|------|-------|
| `PUT` | `/o/{oid}` | Full replace, or partial with `Content-Range: bytes start-end/*` (new version) |
| `POST` | `/o/{oid}/append` | Atomic byte-append at tip size (primary-serialized) → `200` `{offset,size,seq,epoch}` |
| `GET` | `/o/{oid}` | Tip, or `?version={seq}` / `x-aios-version`; `Range` → 206 |
| `HEAD` | `/o/{oid}` | Stat + attr headers (+ version selection as GET) |
| `DELETE` | `/o/{oid}` | Delete-marker version at tip |
| `DELETE` | `/o/{oid}?version={seq}` | Purge one non-tip version |
| `GET` | `/o/{oid}/versions` | List versions newest first (`seq,size,crc32c,ctime_ms,is_delete`) |
| `POST` | `/o/{oid}/purge?keep={n}` | Keep newest `n` versions (default `max_versions`) |
| `POST` | `/o/{oid}/lock` | Acquire enforced lease → `201` `{oid,token,expires_ms}` |
| `POST` | `/o/{oid}/lock/renew` | Renew (`x-aios-lock-token` required) |
| `DELETE` | `/o/{oid}/lock` | Release (`x-aios-lock-token` required) |
| `GET` | `/o/{oid}/lock` | Stat held lock (no token in response) or `404` |
| `GET` | `/o/{oid}/watch` | Long-poll tip change (`timeout_ms`, `after_seq`) → event or `204` |
| `GET` | `/watch?prefix=` | Long-poll events for oids this node is primary for under `prefix` |
| `PUT` | `/pubsub/{topic}?delivery=&capacity=` | Create/configure topic (`ephemeral`\|`buffered`\|`durable`) |
| `GET` | `/pubsub/{topic}` | Topic stat `{topic,delivery,next_id,buffered,capacity}` |
| `POST` | `/pubsub/{topic}/publish` | Publish raw body (≤1 MiB) → `201` `{topic,id,delivery,ts_ms}` |
| `GET` | `/pubsub/{topic}/subscribe` | Long-poll (`timeout_ms`, `after_id`) → messages or `204` |
| `GET` | `/o?prefix=&limit=&cursor=&attr_eq=k:v&attrs=1&scope=` | LIST tip objects (default **cluster** scatter-gather; `scope=local` for this node) |
| `GET` | `/map` | Cluster map JSON (targets include `http_addr`) |
| `GET` | `/admin/status` | **Admin nodes only:** local status + OPS counters + membership |
| `GET` | `/admin/ops` | **Admin:** `{node_id,ops}` |
| `GET` | `/admin/config` | **Admin:** effective config (`cluster_key` redacted) |
| `GET` | `/admin/cluster` | **Admin:** status + list of alive peers with `http_addr` |
| `GET` | `/metrics` | **Admin:** Prometheus/OpenMetrics text (optional public scrape) |
| `POST` | `/txn` | Begin cross-object txn → JSON `{txn_id,state,ops}` (201) |
| `GET` | `/txn/{id}` | Txn coordinator state JSON |
| `PUT` | `/txn/{id}/o/{oid}` | Prepare put (install without tip publish) |
| `DELETE` | `/txn/{id}/o/{oid}` | Prepare delete-marker (unpublished) |
| `POST` | `/txn/{id}/commit` | Publish prepared tips in oid-sorted order |
| `POST` | `/txn/{id}/abort` | Abort prepared versions; mark txn aborted |

### Cross-object transactions

Coordinator is the primary for object `txn/{id}`. Prepare installs a new version on each oid’s primary/replicas but leaves the tip unchanged, so tip GET cannot see ops until commit. Commit publishes tips in **oid-sorted** order; a publish failure aborts remaining ops and marks the txn `aborted` (already-published tips stay visible — v1 torn window). Large staged PUTs in a txn require the request to land on the oid primary (same node as coordinator when that oid is local). Wrong coordinator → **307** like other mutating APIs.

### Erasure coding / layout

When the daemon runs with `durability: ec`, ordinary `PUT /o/{oid}` stripes the object across the acting set. `GET`/`HEAD` return the **reconstructed full object** (`Content-Length` / `x-aios-size` are the logical size). EC metadata appears as `x-aios-attr-aios.ec.*`. Partial `PUT` with `Content-Range` is rejected (`400`). Partial `GET` with `Range` is supported (reconstruct then slice). Transactions (`/txn`) work on EC clusters; prepared txn objects use full-copy install until publish (readable as non-EC tips).

**Per-object layout:** on `PUT /o/{oid}`, optional headers select durability for that version:

| Header | Role |
|--------|------|
| `x-aios-layout: replica \| ec` | Select layout; omit → prefix rule / cluster `durability` |
| `x-aios-ec-k`, `x-aios-ec-m` | EC params; omit → prefix rule / cluster `ec_k` / `ec_m` |
| `x-aios-ec-codec: xor \| isal` | Omit → auto (`xor` if `m==1`, else `isal`) |

Layout is stored on the version (`x-aios-attr-aios.layout`, etc.). Optional YAML `layout_rules` set admin defaults by oid prefix. See [`layout.md`](layout.md).

### Large objects

Full PUT/GET bodies are streamed to/from filesystem staging when larger than 256 KiB (no full-object RAM buffer on the server). Cap with config `max_object_bytes` (default 64 GiB). TCP++ frame size remains 16 MiB; replicas use chunked `ObjectStage*` messages.

### Wrong-node routing

Mutating requests on a non-primary return **307** with:

- `Location: http://{primary.http_addr}/o/{oid}…`
- `x-aios-primary`, `x-aios-acting-set` (JSON)
- JSON body `{ code: "not_primary", acting_set, epoch }`

### Versions

Every successful mutating write creates an immutable `seq` (uint64). Responses include `x-aios-version: {seq}`. Tip GET/HEAD/DELETE (without `version`) operate on the highest non-deleted tip, or a delete-marker tip after `DELETE`. Large (FS) versions use clone/COW (`FICLONE` / `clonefile`); set `clone_required: false` to allow full-copy fallback. Config: `max_versions` (default 16).

### Redirects

Create or replace an object as a pointer to another oid:

```
PUT /o/{oid}
x-aios-redirect: {target_oid}
```

Body must be empty (no `Content-Range`). Creates a new version with no body. `GET`/`HEAD` on a redirect tip returns `307 Temporary Redirect` with `Location: /o/{target}` and `x-aios-redirect: {target}`. A normal `PUT` with a body replaces the redirect with a real object. Self-redirects are rejected.

### Attrs

Request/response: `x-aios-attr-<key>: <value>`

### Checksums (CRC32C)

Whole-object Castagnoli CRC-32C is stored with each object and updated on full and ranged PUTs.

- Response / HEAD: `x-aios-crc32c: <uint32 decimal>`
- Optional request on PUT: `x-aios-crc32c` must match the request body (full object or the ranged patch bytes). Mismatch → `400` / `crc_mismatch`.
- Replication carries `crc32c` / `range_crc32c`; replicas verify before accepting. Scrub repairs size or CRC mismatches.

### Locks (enforced leases)

Primary-local, in-memory leases (lost on restart / primary change). While held:

- Mutating ops (`PUT` / ranged PUT / redirect PUT / `DELETE` / txn prepare) without matching `x-aios-lock-token` → **409** `lock_held`
- With the token → allowed

| Header | Role |
|--------|------|
| `x-aios-lock-ttl-ms` | Acquire/renew TTL (default 30000, max 300000) |
| `x-aios-lock-token` | Present lock token for renew/release/mutate |

Wrong coordinator → **307** like other mutating APIs.

### Watches (HTTP long-poll)

- `GET /o/{oid}/watch?timeout_ms=30000&after_seq=N` — wait on oid primary until tip `seq > after_seq` (default: current tip, i.e. next change), then `200` `{"oid","seq","op","ts_ms"}`; timeout → **204**
- `GET /watch?prefix=foo/&timeout_ms=30000` — events for oids **this node commits as primary** under `prefix` (not a cluster-wide stream)

Long-polls run on worker threads so they do not block the daemon accept loop.

### Pub/sub (HTTP long-poll)

Coordinator is the primary for object `pubsub/{topic}` (wrong node → **307**). Delivery mode is sticky per topic (set on create or first publish; conflicting `delivery` → **409** `mode_mismatch`).

| `delivery` | Behavior |
|------------|----------|
| `ephemeral` | Fanout to current subscribers only; no backlog (in-memory; lost on restart) |
| `buffered` | In-memory ring (default `capacity=256`, max `4096`); catch up via `after_id` |
| `durable` | Messages stored as objects `pubsub/{topic}/m/{id}` + meta tip `pubsub/{topic}`; survives restart |

- `PUT /pubsub/{topic}?delivery=buffered&capacity=256` — create/configure → `201` stat JSON
- `POST /pubsub/{topic}/publish?delivery=` — raw body (≤1 MiB; larger → **413**); optional `delivery` only if topic unset (default `buffered`)
- `GET /pubsub/{topic}/subscribe?timeout_ms=30000&after_id=N` — wait for messages with `id > after_id` (default: current tip = next message); `200` `{"topic","messages":[{"id","ts_ms","content_type","data_b64"},...]}` or **204** on timeout
- `GET /pubsub/{topic}` — stat

Subscribe long-polls use worker threads (same as watches).

### Preconditions (412 on failure)

- `If-None-Match: *` — create only
- `If-Match: *` — must exist
- `x-aios-if-attr-eq: key=value`
- `x-aios-if-attr-ne: key=value`
- `x-aios-if-attr-absent: key`
- `x-aios-if-attr-present: key`

### Partial PUT (random overwrite)

`Content-Range: bytes 1000-1999/*` with body length 1000 writes at offset 1000, growing the object; holes are sparse zeros. Always FS-backed.

### Atomic append

`POST /o/{oid}/append` with a raw body appends at the current tip size under the object primary lock. Concurrent appends never share an offset. Response JSON: `{offset,size,seq,epoch}`. Rejected for erasure-coded tips and redirect tips (`400`). Honors `x-aios-lock-token` like other mutates (`409` if lock held without token). Creates the object when missing (append at offset `0`).

### Partial GET

`Range: bytes=0-99` → `206` + `Content-Range: bytes 0-99/<size>`. Unsatisfiable → `416`.

## Example (UNSIGNED-PAYLOAD)

```bash
DATE=$(date +%s000)
BODY='hello'
HASH=UNSIGNED-PAYLOAD
# Build signature with the same canonical rules, then:
curl -X PUT "http://127.0.0.1:7480/o/myobj" \
  -H "Authorization: AIOS-HMAC-SHA256 Credential=cli, SignedHeaders=x-aios-date;x-aios-content-sha256, Signature=..." \
  -H "x-aios-date: $DATE" \
  -H "x-aios-content-sha256: UNSIGNED-PAYLOAD" \
  --data-binary "$BODY"
```
