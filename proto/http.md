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
| `PUT` | `/o/{oid}` | Full replace, or partial with `Content-Range: bytes start-end/*` |
| `GET` | `/o/{oid}` | Full or `Range: bytes=start-end` → 206 |
| `HEAD` | `/o/{oid}` | Stat + attr headers |
| `DELETE` | `/o/{oid}` | Delete |
| `GET` | `/o?prefix=&limit=&cursor=&attr_eq=k:v&attrs=1` | LIST (local stores; shard fan-out) |
| `GET` | `/map` | Cluster map JSON |

### Attrs

Request/response: `x-aios-attr-<key>: <value>`

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
