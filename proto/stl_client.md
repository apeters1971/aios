# STL-like persistent client (v2)

C++ library target **`aios_client`**. Include [`src/client/stl.hpp`](../src/client/stl.hpp).

## Types

| Class | Persistence | Notes |
|-------|-------------|--------|
| `aios::string` | Whole JSON tip `stl/string/{name}` (`aios_stl: 1`) | Unchanged from v1 |
| `aios::map` / `unordered_map` / `set` / `list` / `deque` | Append-only **changelog** (below) | `aios_stl: 2` meta |
| `aios::mutex` | HTTP lock API (no body) | Unchanged |

Value type for map/list/deque elements is `std::string`.

## Changelog layout (containers)

Per container name:

| Oid | Role |
|-----|------|
| `stl/{type}/{name}` | **Meta** JSON: `aios_stl=2`, `type`, `next_op`, `log_bytes`, `snapshot_op`, `snapshot_oid` |
| `stl/{type}/{name}/log` | Opaque framed op log (atomic `POST /o/{oid}/append`) |
| `stl/{type}/{name}/snap` | Compacted snapshot (same JSON shape as v1 payload: `entries` / `keys` / `items`) |

### Record framing (little-endian)

```text
u32 magic = 'AOPk' (0x6b504f41)
u32 header_len
u64 op_id
u32 op
u32 payload_len
u8  payload[payload_len]   # length-prefixed UTF-8 strings (u32 len + bytes)
```

Ops: map/unordered_map `Put`/`Erase`/`Clear`; set `Insert`/`Erase`/`Clear`; list/deque `PushBack`/`PushFront`/`PopBack`/`PopFront`/`Insert`/`EraseAt`/`SetAt`/`Clear`; plus `Compact` fence.

### Client path

- Local materialized state + `applied_op` cursor
- **SYNC** mutate: append one framed record, apply locally
- **ASYNC** mutate: local + pending ops; `flush()` batch-appends; `load()` pulls meta/snap/log
- **Follow**: each SYNC read (and ASYNC `load`) range-gets new log bytes and applies
- **`compact()`**: write snapshot, truncate log (`log_bytes=0`), keep `next_op` monotonic; also auto when log exceeds ~1 MiB

### Compat

Opening a v1 whole-document tip (`aios_stl: 1`) **migrates on open**: seed snap from the body, empty log, rewrite meta as v2.

## SYNC vs ASYNC

| Mode | Mutate | Read | Persist |
|------|--------|------|---------|
| **SYNC** | Immediate append (+ follow on next read) | Pull log from cursor | Every mutating call |
| **ASYNC** | Local only (`dirty`) | Local after `load()` | `flush()` batch append; `load()` pull |

Default mode on construct: **ASYNC**.

- `set_mode(sync)` while dirty → error (flush or `discard()` first).
- `load()` while dirty → error.
- Destructor: ASYNC dirty objects `flush()` by default (`flush_on_destroy`).

`string` still uses CAS on `aios.stl.cas` for whole-document PUT. Changelog containers use meta CAS around `next_op` / `log_bytes`.

## Mutex

`aios::mutex` wraps `POST/DELETE /o/{oid}/lock` (TTL lease, default 30s). Models `BasicLockable` (`std::lock_guard`). Shared across processes/nodes. Containers do **not** take the mutex automatically.

## Example

```cpp
#include "client/stl.hpp"

aios::Session sess({"127.0.0.1:7480", cluster_key});

aios::string s(sess, "greeting", aios::sync_mode::async);
s.assign("hello");
s.flush();

aios::map m(sess, "users", aios::sync_mode::sync);
m["alice"] = "1";   // append Put; visible cluster-wide
m.compact();        // optional snapshot + log truncate

aios::mutex mx(sess, "users");
{
  std::lock_guard lock(mx);
  // critical section
}
```

## Notes

- Oids use `/` separators; the client URL-encodes them as `%2F` so `/o/{oid}/lock|append` routing stays unambiguous.
- Atomic append: [`proto/http.md`](http.md) `POST /o/{oid}/append`.
- `aios_client` links `aios_core` for shared HTTP HMAC helpers.

## Benchmark

`aios-bench --mode stl` exercises the client types (see `--stl-types`, `--stl-sync`):

```bash
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --mode stl --stl-sync both --ops 100
```

## Build

```bash
cmake --build build --target aios_client aios-bench
```
