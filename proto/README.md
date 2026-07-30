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

Maximum body size: 16 MiB.

## Message types

| Code | Name       | Body |
|------|------------|------|
| 1    | Hello      | `{ "node_id", "listen", "ts", "sig" }` |
| 2    | Membership | `{ "members": [ ... ] }` (legacy; prefer Gossip) |
| 3    | FsTable    | `{ "entries": [ ... ] }` (legacy; prefer Gossip) |
| 4    | Gossip     | `{ "membership", "fs_table", "ts", "sig" }` |
| 5    | Ping       | `{}` |
| 6    | Pong       | `{}` |

## Cluster authentication

All `Hello` and `Gossip` bodies carry:

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
4. Server merges, replies with `Gossip` of its view.
5. Connection closes.

## Member entry

```json
{
  "node_id": "node-a",
  "addr": "192.168.1.10:7400",
  "state": "alive",
  "last_seen_ms": 1710000000000
}
```

`state` is one of `alive`, `suspect`, `dead`.

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
