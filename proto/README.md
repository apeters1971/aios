# AIOS TCP++ wire format (v1)

Not protobuf — fixed binary header + UTF-8 JSON body.

## Frame layout

| Offset | Size | Field   | Value |
|--------|------|---------|-------|
| 0      | 4    | magic   | `AIOS` (ASCII) |
| 4      | 1    | version | `1` |
| 5      | 1    | type    | message type (see below) |
| 6      | 2    | flags   | reserved, must be 0 |
| 8      | 4    | length  | big-endian body length |
| 12     | N    | body    | UTF-8 JSON |

Maximum frame body size: 16 MiB (object payloads may exceed this via staging).

## Message types

| Code | Name         | Body |
|------|--------------|------|
| 1    | Hello        | `{ "node_id", "listen", "ts", "sig" }` |
| 2    | Membership   | `{ "members": [ ... ] }` (legacy; prefer Gossip) |
| 3    | FsTable      | `{ "entries": [ ... ] }` (legacy; prefer Gossip) |
| 4    | Gossip       | `{ "membership", "fs_table", "cluster_map"?, "ts", "sig" }` |
| 5    | Ping         | `{}` |
| 6    | Pong         | `{}` |
| 7    | ObjectPut    | see below |
| 8    | ObjectGet    | see below |
| 9    | ObjectDel    | see below |
| 10   | ObjectStat   | see below |
| 11   | ObjectReply | see below |
| 12   | ObjectPutRange | JSON header + raw body (`flags` bit0); see below |
| 13   | ObjectPublishTip | publish prepared `seq` as tip |
| 14   | ObjectAbortVersion | drop unpublished / non-tip `seq` |
| 15   | ObjectListVersions | list versions for oid |
| 16   | ObjectPurgeVersions | trim (`keep`) or purge one `seq` |
| 17   | ObjectStageBegin | start FS body staging for replica install |
| 18   | ObjectStageData | raw chunk (`kFlagRawBody`); JSON has `offset` |
| 19   | ObjectStageCommit | place staged file → `install_version` |
| 20   | ObjectList | list tip objects on one node (scatter-gather leaf) |

HTTP front-end (external clients): [`http.md`](http.md).

Maximum **frame** body remains 16 MiB; object bodies may be larger via HTTP streaming and `ObjectStage*` chunks. `ObjectGet` with `offset`/`length` returns a raw trailer (no base64) for ranged pulls.

## Cluster authentication

All `Hello`, `Gossip`, object requests, and `ObjectReply` bodies carry:

- `ts` — sender wall-clock milliseconds since Unix epoch
- `sig` — lowercase hex HMAC-SHA256 over a canonical string, keyed by the shared `cluster_key`

Canonical string:

```
<MsgTypeName>\n
<ts>\n
<json dump of body without ts/sig, compact, sorted object keys>
```

Example for Hello:

```
Hello
1710000000000
{"listen":"127.0.0.1:7400","node_id":"node-a"}
```

Receivers reject messages when:

- `ts`/`sig` missing
- `|now - ts| > auth_skew_ms` (default 60s)
- HMAC does not match the local `cluster_key`

`Ping`/`Pong` are unsigned (optional liveness only).

## Gossip exchange

Short-lived TCP sessions:

1. Client connects and sends `Hello`.
2. Server replies with `Hello`.
3. Client sends `Gossip` with its membership + FS table.
4. Server merges, rebuilds cluster map, replies with `Gossip` (may include `cluster_map`).
5. Connection closes.

## Cluster map

Built locally from Online members × usable `fs_table` entries. `epoch` is a content hash of the sorted target list and `replica_count`.

```json
{
  "epoch": 123456789,
  "replica_count": 3,
  "targets": [
    {
      "node_id": "node-a",
      "addr": "192.168.1.10:7400",
      "aios_path": "/data/aios",
      "mount": "/data",
      "bavail": 450000
    }
  ]
}
```

## Object RPC (server-side primary replication)

Short-lived TCP sessions: `Hello` → object request → `ObjectReply`.

Clients compute `place(oid)` from the cluster map and send mutating ops to the **primary** (`acting_set[0]`). The primary **prepares** a new immutable version (tip unpublished), fans out **install** to secondaries (`role=replica` + `seq`), and on `write_quorum` **publishes** the tip (`ObjectPublishTip`). On quorum failure it **aborts** the version (`ObjectAbortVersion`) so tip is unchanged.

### ObjectPut

```json
{
  "epoch": 123456789,
  "aios_path": "/data/aios",
  "oid": "my-object",
  "data_b64": "...",
  "attrs": { "k": "v" },
  "crc32c": 123456789,
  "role": "primary",
  "layout": "replica",
  "ec_k": 2,
  "ec_m": 1,
  "ec_codec": "xor",
  "ts": 0,
  "sig": "..."
}
```

`role` is `primary` (prepare → quorum install → publish) or `replica` with `seq` (install unpublished version only). Replica install fields: `seq`, `base_seq`, `size`, `inline_body`, `fs_path`, `is_delete`, `redirect`, `crc32c`.
Optional `crc32c` is verified against the body before accept; replicas should send it.
Optional `layout` / `ec_k` / `ec_m` / `ec_codec` select per-version durability (same semantics as HTTP `x-aios-*` headers; omit → cluster defaults). See [`layout.md`](layout.md).
Reply may include `seq`. Set `"redirect":"other-oid"` (no `data_b64`) to create a redirect version.

### ObjectPutRange (raw body)

When `flags & 0x0001` is set, the frame body is:

```
[u32be json_len][json bytes][raw octets]
```

JSON fields: `epoch`, `aios_path`, `oid`, `offset`, `attrs`, `replace_attrs`, `range_crc32c`, `role`, `layout`, `ec_*`, `ts`, `sig`.
Raw octets are the range payload (not base64). HMAC covers JSON only.
`range_crc32c` is CRC32C of the raw range bytes; the store recomputes the **whole-object** CRC after applying the overwrite.
Ranged PUT requires replica layout (EC tips / `layout=ec` → error).

### ObjectGet / ObjectDel / ObjectStat

```json
{
  "epoch": 123456789,
  "aios_path": "/data/aios",
  "oid": "my-object",
  "seq": 3,
  "role": "primary"
}
```

Optional `seq` selects a version for Get/Stat. `ObjectDel` prepares a delete-marker version and uses the same publish-after-quorum flow. Replica `ObjectDel` with `seq` installs that delete marker without publishing.

### ObjectPublishTip / ObjectAbortVersion

```json
{ "epoch": 1, "aios_path": "/data/aios", "oid": "my-object", "seq": 3, "role": "replica" }
```

Publish moves tip to `seq` (then trims to `max_versions`). Abort drops an unpublished (or non-tip) version and unlinks its FS body.

### ObjectListVersions / ObjectPurgeVersions

List returns `versions: [{seq,size,crc32c,ctime_ms,is_delete}, ...]` newest first. Purge: `{keep:N}` trims, or `{seq, allow_tip}` removes one version.

### ObjectReply

```json
{
  "ok": true,
  "epoch": 123456789,
  "replicas": 3,
  "data_b64": "...",
  "size": 12,
  "mtime_ms": 1710000000000,
  "code": "epoch_mismatch",
  "error": "...",
  "cluster_map": { },
  "acting_set": [ ],
  "ts": 0,
  "sig": "..."
}
```

Error `code` values include: `epoch_mismatch`, `not_primary`, `not_replica`, `not_found`, `quorum_failed`, `no_targets`, `bad_request`, `store_error`.

On `epoch_mismatch`, clients should refresh the cluster map and retry. On `not_primary`, use the returned `acting_set`.

## Member entry

```json
{
  "node_id": "node-a",
  "addr": "192.168.1.10:7400",
  "rack": "row-a",
  "state": "online",
  "last_seen_ms": 1710000000000
}
```

`state` is one of `online`, `suspect`, `offline` (legacy `alive`/`dead` accepted on parse).

## FS table entry

```json
{
  "node_id": "node-a",
  "mount": "/data",
  "target_path": "/data",
  "aios_path": "/data/aios",
  "bsize": 4096,
  "blocks": 1000000,
  "bfree": 500000,
  "bavail": 450000,
  "files": 100000,
  "ffree": 90000,
  "usable": true,
  "updated_ms": 1710000000000
}
```
