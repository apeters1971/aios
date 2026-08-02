# AIOS

Pragmatic cluster object-storage foundation (RADOS-inspired, intentionally small).

This repository currently ships **`aiosd`**: a standalone C++20 daemon that

1. Joins a peer set from config/CLI (including an empty seed list for the first node)
2. Maintains a shared membership table via custom TCP gossip (“TCP++”)
3. Scans mount points for a top-level `.aios` marker
4. Prepares `…/aios/` object-storage target directories
5. Gossips `statvfs` capacity for usable targets
6. Builds a **cluster map** (epoch + targets) and serves object **Put/Get/Del/Stat** RPCs
7. Performs **server-side primary replication** (`replica_count`) or optional **XOR erasure coding** (`durability: ec`, v1 `2+1`) with a background repair loop
8. Exposes an **HTTP object API** (`http_listen`, default `:7480`) with ranged PUT/GET, attr preconditions, LIST, DELETE, and cross-object transactions (`/txn`)

Plus a local **hybrid object store** library and **`aios-bench`**. Clients are thin and placement-aware: they contact the primary (HTTP or TCP++); the primary fans out replicas. See [`proto/http.md`](proto/http.md).

## Build

Requirements:

- CMake ≥ 3.24
- C++20 compiler
- Boost (Asio headers)
- OpenSSL (HMAC-SHA256 for cluster auth)
- SQLite3
- Network for first configure (FetchContent pulls yaml-cpp + nlohmann/json)

```bash
export PATH="/opt/homebrew/bin:$PATH"   # macOS Homebrew
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/sqlite;/opt/homebrew/opt/openssl@3"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Binaries: `build/aiosd`, `build/aios`, `build/aios-bench`, `build/aios-store-bench`

## Configuration

See [`config/aiosd.example.yaml`](config/aiosd.example.yaml).

```yaml
node_id: "node-a"
listen: "0.0.0.0:7400"
cluster_key: "550e8400-e29b-41d4-a716-446655440000"  # required shared secret
peers: []          # first node: empty
gossip_interval_ms: 1000
suspect_after_ms: 5000
dead_after_ms: 15000
scan_interval_ms: 5000
replica_count: 3
status_file: "/tmp/aios-a.json"
```

CLI overrides: `--cluster-key`, `--config`, `--listen`, `--peer` (repeatable), `--node-id`, `--status-file`, `--replica-count`, `--write-quorum`, `--http-listen`.

### Redundancy

Placement is deterministic: `place(oid, cluster_map) → acting_set` (primary = `[0]`).

- **`durability: replica` (default):** primary writes the full object and fans out identical copies; ACK when `write_quorum` copies succeed.
- **`durability: ec` (v1):** XOR `ec_k + ec_m` with `m=1` (typical `2+1`). Primary stripes the object, installs one shard per acting-set target, and publishes tips together. GET reconstructs from any `k` shards; repair rebuilds a missing shard. Ranged PUT is rejected; objects larger than 16 MiB via staging are rejected in v1.

See [`proto/README.md`](proto/README.md).

### Cluster key

Every daemon in a cluster must share the same `cluster_key` (a UUID is a good choice). `Hello` and `Gossip` messages are signed with **HMAC-SHA256** over a canonical body + timestamp; peers with the wrong key are rejected. This is minimum shared-secret clustering, not full mutual TLS.

## `.aios` marker

Place a file named `.aios` in the **top level** of a mounted filesystem.

| Contents | Effect |
|----------|--------|
| empty / `{}` / no `targets` | Whole mount is one target → `<mount>/aios/` |
| `targets: [data, scratch]` | `<mount>/data/aios/` and `<mount>/scratch/aios/` |

The daemon creates each `aios/` directory if missing, then checks that its ownership matches the process **euid/egid**. Mismatches are logged and the target is **not** advertised.

## Two-node smoke test

Terminal A (first node, no peers):

```bash
KEY=550e8400-e29b-41d4-a716-446655440000
./build/aiosd --cluster-key "$KEY" --node-id a --listen 127.0.0.1:7400 \
  --status-file /tmp/aios-a.json
```

Terminal B:

```bash
KEY=550e8400-e29b-41d4-a716-446655440000
./build/aiosd --cluster-key "$KEY" --node-id b --listen 127.0.0.1:7401 \
  --peer 127.0.0.1:7400 --status-file /tmp/aios-b.json
```

Within a few seconds both status files should list members `a` and `b` as `alive`. A third daemon with a different `--cluster-key` will fail to join.

Optional FS fixture (requires a directory you own that is a mount root, or use a bind/disk image in real deployments). For a quick local marker on a writable mount you control:

```bash
# Example only if /path/to/mount is a real mount root you own:
echo 'targets: []' > /path/to/mount/.aios
# or empty file:
: > /path/to/mount/.aios
```

Then check `fs_table.entries` in the status JSON for `…/aios` paths and `statvfs` fields.

Wire format: [`proto/README.md`](proto/README.md).

## Local object store

Each `…/aios/` target can hold a sharded hybrid store:

```text
aios/
  store.json                 # shard_count, inline_max_bytes (immutable after create)
  shards/
    0/ … ff/                 # hex shard ids (count is power of two)
      meta.sqlite            # objects + attrs for this shard
      objects/ab/cd/<hash>   # large bodies only
      tmp/
```

- **Shard** = `SHA-256(oid)` low bits (`shard_count` must be power of two; default 256)
- **Inline** (`size ≤ inline_max_bytes`, default 64 KiB): body BLOB in that shard’s SQLite
- **Filesystem**: body as a file under the shard; metadata/attrs always in SQLite
- Arbitrary object attrs in table `attrs(oid,key,value)`

### Client CLI (`aios`)

Placement-aware HTTP client (follows `307` redirects to the primary):

```bash
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" put myoid ./file.bin
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" get myoid -o ./out.bin
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" list --prefix my
./build/aios --endpoint 127.0.0.1:7480 --cluster-key "$KEY" map
```

Objects larger than 256 KiB are streamed to disk on the server (default max 64 GiB via `max_object_bytes`). `GET /o` lists cluster-wide (scatter-gather); use `?scope=local` for this node only.

### Client benchmark (`aios-bench`)

Multithreaded HTTP create/update/read across object sizes (default 1 KiB … 16 MiB). Reports IOPS, MiB/s, and latency p50/p95/p99.

```bash
# against a running aiosd with http_listen and matching cluster_key
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --threads 16 --ops 200
./build/aios-bench --endpoint 127.0.0.1:7480 --cluster-key "$KEY" \
  --sizes 1k,64k,1M --ops-mix create,read --json
```

### Local store microbench (`aios-store-bench`)

```bash
./build/aios-store-bench --root /tmp/aios-bench --mode both --shards 16 --count 1000
./build/aios-store-bench --root /tmp/aios-bench --mode inline --small-size 256 --count 5000
./build/aios-store-bench --root /tmp/aios-bench --mode fs --large-size 262144 --count 200 --keep
```

## Logging

`AIOS_LOG=debug|info|warn|error` (default `info`).
