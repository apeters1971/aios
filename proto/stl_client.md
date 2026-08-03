# STL-like persistent client (v1)

C++ library target **`aios_client`**. Include [`src/client/stl.hpp`](../src/client/stl.hpp).

Named objects are stored as JSON tips under oid `stl/{type}/{name}` via the HTTP API. Each container is either **SYNC** or **ASYNC**.

## Types

| Class | Oid | Payload |
|-------|-----|---------|
| `aios::string` | `stl/string/{name}` | `{"data":"…"}` |
| `aios::map` | `stl/map/{name}` | `{"entries":[[k,v],…]}` (ordered) |
| `aios::unordered_map` | `stl/unordered_map/{name}` | `{"entries":[[k,v],…]}` (hash; wire sorted for CAS) |
| `aios::set` | `stl/set/{name}` | `{"keys":[…]}` (ordered unique) |
| `aios::list` | `stl/list/{name}` | `{"items":[…]}` |
| `aios::deque` | `stl/deque/{name}` | `{"items":[…]}` |
| `aios::mutex` | `stl/mutex/{name}` | HTTP lock API (no body) |

Envelope fields: `aios_stl: 1`, `type`, `mode_hint`. Attrs: `aios.stl.type`, `aios.stl.v`, `aios.stl.cas` (optimistic concurrency).

Value type for map/list/deque elements is `std::string`. Max document size: 16 MiB.

## SYNC vs ASYNC

| Mode | Mutate | Read | Persist |
|------|--------|------|---------|
| **SYNC** | Immediate PUT (CAS on `aios.stl.cas`) | Fetch tip (or use just-written local) | Every mutating call |
| **ASYNC** | Local only (`dirty`) | Local after `load()` | `flush()` atomic snapshot; `load()` atomic pull |

Default mode on construct: **ASYNC**.

- `set_mode(sync)` while dirty → error (flush or `discard()` first).
- `load()` while dirty → error.
- Destructor: ASYNC dirty objects `flush()` by default (`flush_on_destroy`).

Conflict on stale CAS → `aios::client_error` with `code() == "conflict"`.

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
m["alice"] = "1";   // visible cluster-wide immediately

aios::mutex mx(sess, "users");
{
  std::lock_guard lock(mx);
  // critical section
}
```

## Notes

- Oids use `/` separators; the client URL-encodes them as `%2F` so `/o/{oid}/lock` routing stays unambiguous.
- `aios_client` links `aios_core` for shared HTTP HMAC helpers (and so tests can host `HttpServer`).

## Benchmark

`aios-bench --mode stl` exercises the client types (see `--stl-types`, `--stl-sync`):

```bash
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --mode stl --stl-sync both --ops 100
```

## Build

```bash
cmake --build build --target aios_client aios-bench
./build/aios_tests
```
