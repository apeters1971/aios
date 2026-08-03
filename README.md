# AIOS

AIOS is a small C++20 **cluster object store**: durable objects on local filesystems, gossip membership, **consistent-hash placement** with storage classes, and a placement-aware HTTP API.

Clients talk to the **primary** for an object (HTTP or TCP++); the primary replicates or erasure-codes across the acting set. There are no pools or placement groups—layout and storage class are chosen **per object version** at write time.

| Binary / lib | Role |
|--------------|------|
| `aiosd` | Cluster daemon (gossip, storage targets, object RPC, HTTP API, repair) |
| `aios` | Thin HTTP client (put/get/del/stat/list/map/admin; follows `307`) |
| `aios-bench` | Multithreaded HTTP create/update/read benchmark |
| `aios-store-bench` | Local hybrid-store microbenchmark (no cluster) |
| `libaios_client` | STL-like persistent C++ API (`string` / `map` / `unordered_map` / `set` / `list` / `deque` / `mutex`) |

Protocol details: [`proto/http.md`](proto/http.md) (HTTP), [`proto/admin.md`](proto/admin.md) (admin/metrics), [`proto/README.md`](proto/README.md) (TCP++), [`proto/layout.md`](proto/layout.md) (per-object layout), [`proto/stl_client.md`](proto/stl_client.md) (STL client).

---

## Contents

- [Features](#features)
- [Architecture](#architecture)
- [Build](#build)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Storage targets (`.aios`)](#storage-targets-aios)
- [Placement, storage classes, and layout](#placement-storage-classes-and-layout)
- [HTTP object API](#http-object-api)
- [Local object store](#local-object-store)
- [Tools](#tools)
- [STL-like C++ client](#stl-like-c-client)
- [Authentication](#authentication)
- [Logging](#logging)
- [Documentation](#documentation)

---

## Features

**Cluster**

- Peer join from config/CLI (empty peer list = bootstrap node)
- Membership via signed TCP gossip (“TCP++”)
- Cluster map (epoch + storage targets + classes) rebuilt from gossiped capacity
- **Consistent hashing with virtual nodes** on a **storage-class** ring: `place(oid, map, n, class) → acting_set` (primary = first target); membership changes remap ~`1/N` of objects

**Storage**

- Discover mounts via top-level `.aios` markers (`storage_class` required); prepare `…/aios/` targets
- Hybrid local store: SQLite metadata + inline or filesystem bodies, sharded by oid hash
- Versioned objects (`max_versions`), attrs, ranged PUT/GET, delete markers, atomic append
- Server-side **replica** or **erasure-coded** durability, with background repair
- **Class transitions** (`transition_rules`): background tip migration between classes (e.g. `nvme` → `hdd`)

**HTTP API** (`http_listen`, default `:7480`)

- Object CRUD, LIST (cluster scatter-gather or local), cluster map
- Per-PUT layout / storage class headers (`x-aios-layout`, `x-aios-storage-class`, `x-aios-ec-*`)
- Attr preconditions (`If-Match` / `If-None-Match` / attr predicates)
- Cross-object transactions (`/txn`)
- Enforced object locks (TTL leases)
- Long-poll watches (per-oid and prefix)
- Topic pub/sub (`/pubsub`: ephemeral, buffered, or durable)

**Ops**

- Status JSON file, HMAC shared-secret auth on gossip/RPC/HTTP
- Admin console + Prometheus (`/admin/*`, `/metrics`); application labels for per-workload OPS
- Optional Intel ISA-L Reed–Solomon for EC with `m > 1`

---

## Architecture

```text
                    ┌─────────────┐
                    │  aios /     │
                    │  HTTP apps  │
                    └──────┬──────┘
                           │ HTTP :7480 (HMAC)
              ┌────────────▼────────────┐
              │     primary aiosd       │
              │  CH place(oid,class,n)  │
              └─────┬──────────┬────────┘
                    │          │ TCP++ object RPC
           ┌────────▼──┐  ┌────▼────────┐
           │ local     │  │ peer aiosd  │
           │ …/aios/   │  │ replicas/EC │
           │ (nvme/hdd)│  │             │
           └───────────┘  └─────────────┘
                    ▲
                    │ gossip :7400 (membership + fs_table + map)
```

1. Daemons gossip membership and `statvfs` for each usable target (with `storage_class` / weight).
2. Each node builds the same **cluster map** for a given epoch (class-scoped vnode rings).
3. Writes consistent-hash the oid onto the class ring → acting set; the primary installs and publishes the tip, then fans out.
4. Reads go to the primary (or any node that can reconstruct EC); wrong node → HTTP **307** with `Location`.
5. Optional **transition** workers move tips between classes under `transition_rules`.

---

## Build

**Requirements**

- CMake ≥ 3.24
- C++20 compiler
- Boost (Asio headers)
- OpenSSL (HMAC-SHA256)
- SQLite3
- Network on first configure (FetchContent: yaml-cpp, nlohmann/json)
- Optional: [ISA-L](https://github.com/intel/isa-l) for Reed–Solomon EC (`m > 1`)

```bash
export PATH="/opt/homebrew/bin:$PATH"   # macOS Homebrew
# Optional for RS EC: brew install isa-l

cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/sqlite;/opt/homebrew/opt/openssl@3;/opt/homebrew/opt/isa-l"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Outputs: `build/aiosd`, `build/aios`, `build/aios-bench`, `build/aios-store-bench`, `build/libaios_client.a`, `build/aios_tests`.

---

## Quick start

Shared secret (UUID recommended)—every node and client must use the same key:

```bash
KEY=550e8400-e29b-41d4-a716-446655440000
```

**Terminal A** (bootstrap, no peers):

```bash
./build/aiosd --cluster-key "$KEY" --node-id a --listen 127.0.0.1:7400 \
  --http-listen 127.0.0.1:7480 --status-file /tmp/aios-a.json
```

**Terminal B**:

```bash
./build/aiosd --cluster-key "$KEY" --node-id b --listen 127.0.0.1:7401 \
  --peer 127.0.0.1:7400 --http-listen 127.0.0.1:7481 \
  --status-file /tmp/aios-b.json
```

Within a few seconds both status files should list `a` and `b` as `alive`. A daemon with a different `--cluster-key` will not join.

**Client** (against A’s HTTP port; follows redirects to the primary):

```bash
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" put demo/hello ./file.bin
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" get demo/hello -o ./out.bin
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" list --prefix demo/
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" map
```

For real capacity you need at least one mount with a [`.aios` marker](#storage-targets-aios). Without targets, membership still works but object puts fail with `no_targets`.

---

## Configuration

Full example: [`config/aiosd.example.yaml`](config/aiosd.example.yaml).

```yaml
node_id: "node-a"
listen: "0.0.0.0:7400"
cluster_key: "550e8400-e29b-41d4-a716-446655440000"
peers: []
gossip_interval_ms: 1000
suspect_after_ms: 5000
dead_after_ms: 15000
scan_interval_ms: 5000
replica_count: 3
default_storage_class: nvme
placement:
  vnodes_per_target: 128
  min_vnodes: 16
  max_vnodes: 1024
# durability: replica       # or ec
# ec_k: 2
# ec_m: 1                   # m=1 → XOR; m>1 needs ISA-L
http_listen: "0.0.0.0:7480"
# max_versions: 16
# max_object_bytes: 68719476736
# layout_rules:
#   - prefix: "hot/"
#     layout: replica
#     storage_class: nvme
#   - prefix: "cold/"
#     layout: ec
#     storage_class: hdd
#     ec_k: 2
#     ec_m: 1
# transition_rules:
#   - prefix: "cold/"
#     from: nvme
#     to: hdd
status_file: "/tmp/aios-a.json"
```

**CLI overrides:** `--config`, `--cluster-key`, `--listen`, `--peer` (repeatable), `--node-id`, `--status-file`, `--replica-count`, `--write-quorum`, `--http-listen`, `--admin`, `--admin-metrics-public`.

### Admin & monitoring

Enable on selected nodes with `admin: true` / `--admin`. That exposes `/admin/status|ops|config|cluster|transitions` and Prometheus `/metrics` (optionally unauthenticated via `admin_metrics_public`). OPS counters are process-local; use `aios admin cluster` to sum them across peers. `POST /admin/transitions/run` forces one local class-migration tick.

```bash
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin status
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin   # interactive console
```

Clients may tag traffic with `x-aios-app-label` / `--app-label` / `SessionConfig::app_label` for per-workload OPS counters (QoS limits later).

Details: [`proto/admin.md`](proto/admin.md).

---

## Storage targets (`.aios`)

Place a file named `.aios` in the **top level** of a mounted filesystem the daemon can see. **`storage_class` is required.**

| Field | Effect |
|-------|--------|
| `storage_class: nvme` | Placement pool / class-scoped consistent-hash ring (required) |
| `weight: 2` | Optional vnode weight (default 1) |
| `targets: [data, scratch]` | `<mount>/data/aios/` and `<mount>/scratch/aios/` (omit → `<mount>/aios/`) |

The daemon creates each `aios/` directory if missing, then requires ownership to match the process **euid/egid**. Mismatches are logged and the target is **not** advertised.

```bash
cat > /path/to/nvme-mount/.aios <<'EOF'
storage_class: nvme
targets: [data]
EOF
cat > /path/to/hdd-mount/.aios <<'EOF'
storage_class: hdd
weight: 1
EOF
```

---

## Placement, storage classes, and layout

### Consistent hashing

Each storage class has its own **vnode ring**. Targets advertise a class and optional `weight` in `.aios`; the map expands each target to `clamp(weight × vnodes_per_target, min_vnodes, max_vnodes)` virtual nodes.

```text
sha256(oid) → start on class ring → walk clockwise
  → prefer distinct node_id, then fill same-node mounts
  → acting_set[0] is primary
```

API: `place(oid, map, n, storage_class)`. If the class has fewer than `n` targets, placement fails with `no_targets` (no silent under-protection). Adding or removing a target remaps roughly **`1/N`** of objects—not a full reshuffle.

### Storage classes

Devices declare a class (convention: `nvme`, `ssd`, `hdd`, … — free-form `[a-z0-9_-]+`). Writes pick a class by:

1. Request: `x-aios-storage-class` / JSON `storage_class`
2. Longest matching `layout_rules` prefix
3. Cluster `default_storage_class`

The tip stores `aios.storage_class`. Reads and repair use that attr (attrs win over cluster defaults).

### Durability (replica vs EC)

| Mode | Behavior |
|------|----------|
| **`durability: replica`** (default) | Primary writes the full object and fans out identical copies; ACK when `write_quorum` succeeds |
| **`durability: ec`** | Primary stripes into `ec_k + ec_m` shards (one per acting-set target). GET reconstructs from any `k` shards; repair rebuilds missing shards |

EC codec is auto-selected (`xor` when `ec_m=1`, else `isal` / Reed–Solomon). Build with ISA-L available for `m>1`. Ranged PUT is rejected under EC; staged PUT is capped (see HTTP docs).

**Per-object layout** (no pools): each PUT may override via `x-aios-layout` / `x-aios-ec-*`. Cluster `durability` / `ec_*` are defaults. `layout_rules` can set both layout and `storage_class` by oid prefix (longest match; request headers still win).

### Class transitions

Move tips between classes under policy (or with a client PUT that changes class):

```yaml
transition_rules:
  - prefix: "cold/"
    from: nvme
    to: hdd
    layout: ec          # optional layout change on migrate
    ec_k: 2
    ec_m: 1
```

The destination primary copies/re-encodes the tip, sets `aios.storage_class_prev` while dual-homed, then drains the source once the destination has quorum. Background interval: `transition_interval_ms`. Admin: `GET /admin/transitions`, `POST /admin/transitions/run`.

Full design: [`proto/layout.md`](proto/layout.md).

---

## HTTP object API

Listen address: `http_listen` (empty disables HTTP). Bodies are raw octets. Auth: HMAC (see [Authentication](#authentication)).

| Area | Endpoints (summary) |
|------|---------------------|
| Objects | `PUT/GET/HEAD/DELETE /o/{oid}`, versions, purge, ranged I/O |
| List / map | `GET /o?prefix=…`, `GET /map` |
| Transactions | `POST /txn`, prepare put/delete, commit/abort |
| Locks | `POST/GET/DELETE /o/{oid}/lock` (+ renew); mutates need `x-aios-lock-token` |
| Watches | Long-poll `GET /o/{oid}/watch`, `GET /watch?prefix=` |
| Pub/sub | `PUT/GET /pubsub/{topic}`, `POST …/publish`, `GET …/subscribe` |

Wrong primary → **307** with `Location` to the coordinator. Full contract: [`proto/http.md`](proto/http.md).

**Pub/sub delivery modes** (sticky per topic):

| Mode | Behavior |
|------|----------|
| `ephemeral` | Fanout to current long-poll waiters only |
| `buffered` | In-memory ring; catch up with `after_id` |
| `durable` | Messages stored as objects under `pubsub/{topic}/m/{id}` |

**Locks / watches** are primary-local and in-memory (lost on restart or primary move), except durable pub/sub message bodies.

---

## Local object store

Each `…/aios/` target holds a sharded hybrid store:

```text
aios/
  store.json                 # shard_count, inline_max_bytes (immutable after create)
  shards/
    0/ … ff/                 # hex shard ids (count is power of two)
      meta.sqlite            # objects + attrs for this shard
      objects/ab/cd/<hash>   # large bodies only
      tmp/
```

- **Shard** = low bits of `SHA-256(oid)` (`shard_count` power of two; default 256)
- **Inline** (`size ≤ inline_max_bytes`, default 64 KiB): body BLOB in SQLite
- **Filesystem**: large bodies on disk; metadata/attrs always in SQLite

Objects larger than 256 KiB are streamed to disk on the HTTP path (default max 64 GiB via `max_object_bytes`).

---

## Tools

### `aios` — HTTP client

```bash
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" put OID FILE
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" get OID [-o FILE]
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" del OID
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" stat OID
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" list [--prefix P]
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" map
```

Follows `307` redirects. Put/get stream file bytes (no full-object client buffer).

### `aios-bench` — HTTP / STL benchmark

**Object mode** (default): raw PUT/GET create/update/read. Reports IOPS, MiB/s, latency p50/p95/p99. Optional `--layout ec`.

**STL mode** (`--mode stl`): benchmarks `aios_client` types (`string`, `map`, `unordered_map`, `set`, `list`, `deque`) in SYNC and/or ASYNC. `--sizes` is string byte length or entry count (default `16,64,256,1k,4k`).

```bash
# Object store
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --threads 16 --ops 200
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --sizes 1k,64k,1M --ops-mix create,read --json

# STL client
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --mode stl --stl-sync both --ops 100 --threads 8
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --mode stl --stl-types string,map --stl-sync async \
  --sizes 64,256,1k --ops-mix create,read --json
```

### `aios-store-bench` — local store microbench

```bash
./build/aios-store-bench --root /tmp/aios-bench --mode both --shards 16 --count 1000
./build/aios-store-bench --root /tmp/aios-bench --mode inline --small-size 256 --count 5000
./build/aios-store-bench --root /tmp/aios-bench --mode fs --large-size 262144 --count 200 --keep
```

---

## STL-like C++ client

Library **`aios_client`** (`#include "client/stl.hpp"`) maps **named** STL-style objects onto the HTTP object API. Element/value type is `std::string`.

| Class | Role | Persistence |
|-------|------|-------------|
| `aios::string` | Persistent string | Whole JSON tip (`aios_stl: 1`) |
| `aios::map` / `unordered_map` / `set` / `list` / `deque` | Containers | Append-only changelog via `POST /o/{oid}/append` (`aios_stl: 2`) |
| `aios::mutex` | Cluster-shared mutex (`BasicLockable`) | HTTP lock API |

Changelog containers store meta at `stl/{type}/{name}`, an op log at `…/log`, and optional snapshot at `…/snap`. Call **`compact()`** (or rely on auto-compact past ~1 MiB) to snapshot and truncate the log. Opening a v1 whole-document tip migrates on open.

### SYNC vs ASYNC

Containers (not mutex) take a `aios::sync_mode` (default **ASYNC**):

| Mode | Behavior |
|------|----------|
| **SYNC** | Every mutate appends a framed op (visible to peers on their next read/follow). |
| **ASYNC** | Mutations stay local (`dirty`). **`load()`** pulls meta/snap/log; **`flush()`** batch-appends pending ops. |

Mode switch: `set_mode(sync)` while dirty fails until `flush()` or `discard()`. Dirty `load()` also fails. ASYNC destructors flush by default (`flush_on_destroy`).

`string` still uses CAS on `aios.stl.cas`. Stale string flush → `aios::client_error` with `code() == "conflict"`.

`aios::mutex` uses HTTP object locks on `stl/mutex/{name}` (TTL lease, default 30s). Containers do **not** take it automatically—compose with `std::lock_guard` for critical sections.

### Example

```cpp
#include "client/stl.hpp"

aios::Session sess({"127.0.0.1:7480", key});

// ASYNC: edit locally, then atomic publish
aios::string s(sess, "greeting", aios::sync_mode::async);
s.assign("hello");
s.flush();

// SYNC: each mutate appends an op (cluster-visible)
aios::map m(sess, "users", aios::sync_mode::sync);
m["alice"] = "1";
m.compact();

aios::mutex mx(sess, "users");
{
  std::lock_guard lock(mx);
  // exclusive section across processes/nodes
}
```

### Build / link

```bash
cmake --build build --target aios_client
# link: aios_client (pulls aios_core for HTTP HMAC helpers)
```

Wire format, append, and API notes: [`proto/stl_client.md`](proto/stl_client.md), [`proto/http.md`](proto/http.md).

---

## Authentication

Every daemon and HTTP client in a cluster must share `cluster_key`.

| Surface | Mechanism |
|---------|-----------|
| TCP++ `Hello` / `Gossip` / object RPC | HMAC-SHA256 over canonical body + timestamp |
| HTTP | `Authorization: AIOS-HMAC-SHA256 …` + `x-aios-date` + content hash |

Skew window: `auth_skew_ms` (default 60s). This is shared-secret clustering, not mutual TLS. Details: [`proto/README.md`](proto/README.md), [`proto/http.md`](proto/http.md).

---

## Logging

```bash
AIOS_LOG=debug|info|warn|error   # default: info
```

---

## Documentation

| Doc | Topic |
|-----|-------|
| [`proto/http.md`](proto/http.md) | HTTP API, locks, watches, pub/sub, txns, preconditions |
| [`proto/admin.md`](proto/admin.md) | Admin console, OPS counters, Prometheus `/metrics` |
| [`proto/README.md`](proto/README.md) | TCP++ framing, gossip, object RPC |
| [`proto/layout.md`](proto/layout.md) | Placement (CH + classes), layout, and transitions |
| [`proto/stl_client.md`](proto/stl_client.md) | STL-like C++ client (SYNC/ASYNC) |
| [`config/aiosd.example.yaml`](config/aiosd.example.yaml) | Daemon config reference |

Run the unit suite after changes:

```bash
./build/aios_tests
# or
ctest --test-dir build --output-on-failure
```
