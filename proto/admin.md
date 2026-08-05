# Admin API & console

Nodes started with **`admin: true`** (YAML) or **`--admin`** expose monitoring and configuration endpoints on `http_listen`. Non-admin nodes return `404` for these paths.

## Enable

```yaml
admin: true
# Optional: allow Prometheus to scrape without HMAC (restrict the port!)
admin_metrics_public: true
```

```bash
aiosd --cluster-key "$KEY" --admin --admin-metrics-public ...
```

## Web UI

Open **`http://HOST:PORT/admin/`** in a browser (same `http_listen`).

1. Sign in with the **cluster key** (password field).
2. A session cookie (`aios_admin`, HttpOnly, SameSite=Strict, 12h) authenticates subsequent `/admin/api/*` calls.
3. Panels: Overview (OPS), Cluster peers, Config (read-only; secrets redacted), Actions (toggle `admin_metrics_public`, run transitions, run repair).

Static assets live in [`web/admin/`](../web/admin/) (installed to `share/aios/admin`). Override search path with **`AIOS_ADMIN_WEB`**.

Logout: `POST /admin/logout`.

## HTTP endpoints

### Browser / session

| Path | Auth | Response |
|------|------|----------|
| `GET /admin/`, `/admin/index.html`, `app.js`, `style.css`, `aios-icon.png` | none | Web UI assets |
| `POST /admin/login` | body `{"cluster_key"}` | Sets session cookie |
| `POST /admin/logout` | session optional | Clears cookie |
| `GET /admin/api/status` | cookie **or** HMAC | Same as `/admin/status` |
| `GET /admin/api/ops` | cookie **or** HMAC | Same as `/admin/ops` |
| `GET /admin/api/config` | cookie **or** HMAC | Same as `/admin/config` |
| `GET /admin/api/cluster` | cookie **or** HMAC | Same as `/admin/cluster` |
| `GET /admin/api/transitions` | cookie **or** HMAC | Transition rules |
| `POST /admin/api/transitions/run` | cookie **or** HMAC | One transition tick |
| `GET /admin/api/archive` | cookie **or** HMAC | Archive (pack-to-bag) rules |
| `POST /admin/api/archive/run` | cookie **or** HMAC | One archive pack tick |
| `POST /admin/api/archive/drain` | cookie **or** HMAC | Drain staged bags to tape_root / tape_put_cmd |
| `POST /admin/api/archive/recall` | cookie **or** HMAC | `{oid}` restore bag if needed, rehydrate tip |
| `POST /admin/api/repair/run` | cookie **or** HMAC | One repair tick |
| `POST /admin/api/settings` | cookie **or** HMAC | `{admin_metrics_public: bool}` (in-memory) |
| `GET /admin/api/s3/credentials` | cookie **or** HMAC | List S3 IAM keys (secrets redacted); requires `s3_listen` |
| `POST /admin/api/s3/credentials` | cookie **or** HMAC | Create `{access_key_id, uid, gid, buckets[, secret]}` — secret returned once |
| `DELETE /admin/api/s3/credentials/{id}` | cookie **or** HMAC | Remove IAM key |
| `GET /admin/api/quota` | cookie **or** HMAC | Soft quota limits + usage |
| `PUT /admin/api/quota/limits` | cookie **or** HMAC | `{uid\|gid, bytes\|null}` |
| `POST /admin/api/quota/projects` | cookie **or** HMAC | `{name, root_ino, bytes?}` |
| `PUT /admin/api/quota/projects/{id}` | cookie **or** HMAC | `{uid, bytes\|null}` project-uid limit |
| `DELETE /admin/api/quota/projects/{id}` | cookie **or** HMAC | Remove project |
| `POST /admin/api/quota/reconcile` | cookie **or** HMAC | Rebuild usage from inodes |
| `GET /admin/api/qos` | cookie **or** HMAC | Soft IOPS/BW limits + monitoring rates |
| `PUT /admin/api/qos/limits` | cookie **or** HMAC | `{uid\|gid\|project_id, iops?, bps?, clear?}` |

CLI: `aios admin s3-cred …`, `aios admin quota …`, `aios admin qos …`. Web UI: **S3 credentials** / **Quotas** / **QoS** tabs. See [`proto/s3.md`](s3.md), [`proto/quota.md`](quota.md), [`proto/qos.md`](qos.md).

### Legacy JSON (CLI / HMAC)

All of these require the normal AIOS HMAC Authorization header, except `GET /metrics` when `admin_metrics_public` is true.

| Path | Response |
|------|----------|
| `GET /admin/status` | Node identity, map epoch, membership, local `ops` |
| `GET /admin/ops` | `{node_id, ops, ops_by_label, compression, io_frontends}` |
| `GET /admin/config` | Effective config (`cluster_key` → `"***"`). Most changes need restart; `admin_metrics_public` can be toggled via `/admin/api/settings`. |
| `GET /admin/cluster` | Local status + `admin_peers` (`node_id`, `http_addr`) for cluster scrape |
| `GET /admin/transitions` | Configured storage-class `transition_rules` + intervals |
| `POST /admin/transitions/run` | Run one local transition tick; returns `{matched,migrated,drained,failed}` |
| `GET /admin/archive` | Configured `archive_rules` + intervals |
| `POST /admin/archive/run` | Pack tick; returns `{matched,packed,bags_sealed,failed}` |
| `POST /admin/archive/drain` | Tape drain tick; returns `{bags_scanned,drained,skipped,failed}` |
| `POST /admin/archive/recall` | Restore bag from tape if needed; rehydrate `{oid}` |
| `GET /metrics` | Prometheus counters (`aios_*_total`) |

## OPS counters

Process-local monotonic counters (see `src/metrics/ops_counters.hpp`), including:

- HTTP requests, put / put_range / append / get / head / del / list
- put/get/append bytes
- compress_puts / compress_skipped / compress_logical_bytes / compress_stored_bytes
- lock_acquire, watch, pubsub_publish
- gossip_rounds, repair_scanned / repaired / failed
- errors (failed API results; excludes `not_primary` redirects)

### Compression

When whole-object compression is enabled (`compression: zstd` in config), `/admin/ops` also includes a top-level `compression` object:

| Field | Meaning |
|-------|---------|
| `puts` | Objects stored compressed |
| `skipped` | Puts that skipped compression (too small or no gain) |
| `logical_bytes` | Uncompressed payload bytes of compressed puts |
| `stored_bytes` | On-disk payload bytes after compression |
| `ratio` | `logical_bytes / stored_bytes` (overall; `0` if nothing stored compressed) |

Prometheus exposes the same counters plus gauge `aios_compress_ratio`.

**Cluster-wide totals:** scrape `/admin/ops` (or use the console `cluster` command) across alive peers listed by `/admin/cluster`. Gossip does not carry counters.

Counters are also written into `status_file` under `ops` / `ops_by_label` when that path is configured.

### Application labels

Clients send `x-aios-app-label: <label>` (see [`http.md`](http.md)). Each distinct label gets its own counter bucket (max 256; further labels roll into `_overflow`). Totals always include labeled + unlabeled traffic; `ops_by_label` is the breakdown.

**Reserved frontend labels** (defaults when unset):

| Label | Frontend |
|-------|----------|
| `s3` | S3 API (posix mount inside aiosd) |
| `fs` | FUSE / posix filesystem clients |
| `vbd` | Block volumes (`aiosvd`) |

### IO by frontend

`GET /admin/ops` (and `/admin/api/ops`) includes `io_frontends`:

- `logical` — S3/FS posix read/write ops+bytes; VBD aggregated from mapped `aiosvd` devices when `/dev/aiosvd_ctl` is present
- `object_ops` — object-API OPS sliced by the same labels (chunk-level; higher than logical for striped FS/S3)
- `vbd_devices` — per-device kernel counters (`ops_read` / `ops_write` / bytes / errors / …)

Prometheus series add `app_label="…"` for per-label samples (node totals remain without that label).

## CLI console

```bash
# Interactive
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin

# One-shot
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin status
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin ops
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin config
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin cluster
aios --cluster-key "$KEY" --endpoint 127.0.0.1:7480 admin metrics
```

Console commands: `status`, `ops`, `config`, `cluster`, `metrics`, `help`, `quit`.

## Prometheus

Point Prometheus at an admin node's `/metrics`. Prefer `admin_metrics_public: true` plus network ACLs, or configure a scraper that signs AIOS HMAC. The web UI Actions tab can toggle public metrics at runtime (in-memory; not persisted to YAML).

Example metric names: `aios_http_requests_total`, `aios_ops_put_total`, `aios_ops_get_bytes_total`, `aios_gossip_rounds_total`, `aios_repair_repaired_total`.

Labeled example:

```
aios_ops_put_total{node_id="node-a"} 100
aios_ops_put_total{node_id="node-a",app_label="etl"} 40
```
