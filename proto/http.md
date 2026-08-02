# AIOS HTTP object API (v1)

Listen address: `http_listen` (default `0.0.0.0:7480`). Separate from TCP++ gossip/object RPC.

Bodies are **raw octets** (never base64). Connections are HTTP/1.1 keep-alive by default.

## Authorization

```
Authorization: AIOS-HMAC-SHA256 Credential=<id>, SignedHeaders=<list>, Signature=<hex>
x-aios-date: <unix-ms>
x-aios-content-sha256: <sha256-hex of body> | UNSIGNED-PAYLOAD
```

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
| `GET` | `/o/{oid}` | Tip, or `?version={seq}` / `x-aios-version`; `Range` → 206 |
| `HEAD` | `/o/{oid}` | Stat + attr headers (+ version selection as GET) |
| `DELETE` | `/o/{oid}` | Delete-marker version at tip |
| `DELETE` | `/o/{oid}?version={seq}` | Purge one non-tip version |
| `GET` | `/o/{oid}/versions` | List versions newest first (`seq,size,crc32c,ctime_ms,is_delete`) |
| `POST` | `/o/{oid}/purge?keep={n}` | Keep newest `n` versions (default `max_versions`) |
| `GET` | `/o?prefix=&limit=&cursor=&attr_eq=k:v&attrs=1&scope=` | LIST tip objects (default **cluster** scatter-gather; `scope=local` for this node) |
| `GET` | `/map` | Cluster map JSON (targets include `http_addr`) |
| `POST` | `/txn` | Begin cross-object txn → JSON `{txn_id,state,ops}` (201) |
| `GET` | `/txn/{id}` | Txn coordinator state JSON |
| `PUT` | `/txn/{id}/o/{oid}` | Prepare put (install without tip publish) |
| `DELETE` | `/txn/{id}/o/{oid}` | Prepare delete-marker (unpublished) |
| `POST` | `/txn/{id}/commit` | Publish prepared tips in oid-sorted order |
| `POST` | `/txn/{id}/abort` | Abort prepared versions; mark txn aborted |

### Cross-object transactions

Coordinator is the primary for object `txn/{id}`. Prepare installs a new version on each oid’s primary/replicas but leaves the tip unchanged, so tip GET cannot see ops until commit. Commit publishes tips in **oid-sorted** order; a publish failure aborts remaining ops and marks the txn `aborted` (already-published tips stay visible — v1 torn window). Large staged PUTs in a txn require the request to land on the oid primary (same node as coordinator when that oid is local). Wrong coordinator → **307** like other mutating APIs.

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

### Preconditions (412 on failure)

- `If-None-Match: *` — create only
- `If-Match: *` — must exist
- `x-aios-if-attr-eq: key=value`
- `x-aios-if-attr-ne: key=value`
- `x-aios-if-attr-absent: key`
- `x-aios-if-attr-present: key`

### Partial PUT (random overwrite)

`Content-Range: bytes 1000-1999/*` with body length 1000 writes at offset 1000, growing the object; holes are sparse zeros. Always FS-backed.

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
