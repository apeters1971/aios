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
3. Panels: Overview (OPS), Cluster peers, Config (read-only; secrets redacted), Actions (metrics toggle, transitions, repair, archive pack/drain/recall, backup run/snapshot/live policies), S3 / Quotas / QoS.

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
| `GET /admin/api/backup` | cookie **or** HMAC | Backup (snap+pack) rules |
| `POST /admin/api/backup/run` | cookie **or** HMAC | One backup tick |
| `POST /admin/api/backup/snapshot` | cookie **or** HMAC | `{kind,volume}` or `{kind,pool,name}` |
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
| `GET /admin/api/posix-layout` | cookie **or** HMAC | Live POSIX path → meta/data layout rules |
| `PUT /admin/api/posix-layout` | cookie **or** HMAC | Replace rules (`{posix_layout_rules:[…]}` or array) |
| `GET /admin/api/lifecycle` | cookie **or** HMAC | Node + target lifecycle (effective state, weight, class, path) |
| `PUT /admin/api/lifecycle/node` | cookie **or** HMAC | `{state: up\|drain\|off}` — live `node_state` (triggers rescan) |
| `PUT /admin/api/lifecycle/target` | cookie **or** HMAC | `{aios_path\|mount, state?, weight?}` — rewrite local `.aios` + rescan |
| `PUT /admin/api/lifecycle/autotune` | cookie **or** HMAC | `{enabled?, threshold_pct?, min_delta?}` — free-space weight autotune + hysteresis |

CLI: `aios admin archive …`, `aios admin backup …`, `aios admin posix-layout …`, `aios admin lifecycle …`, `aios admin s3-cred …`, `aios admin quota …`, `aios admin qos …`. Web UI: **Actions** (archive/backup), **S3 credentials** / **Quotas** / **QoS** / **POSIX layout** / **Lifecycle** tabs. See [`archive.md`](archive.md), [`backup.md`](backup.md), [`posix_fuse.md`](posix_fuse.md), [`layout.md`](layout.md), [`s3.md`](s3.md), [`quota.md`](quota.md), [`qos.md`](qos.md).

```bash
aios admin archive show
aios admin archive run
aios admin archive drain
aios admin archive recall cold/file
aios admin backup show
aios admin backup run
aios admin backup snapshot posix --volume default --path /home
aios admin backup snapshot vbd --pool rbd --name disk0
aios admin backup policy set --volume default --path / --at 00:00 --keep-days 7 --keep-monthly 12
aios admin backup policy list
aios admin backup policy rm ID
aios admin posix-layout show
aios admin posix-layout set --file rules.json
aios admin lifecycle show
aios admin lifecycle node drain
aios admin lifecycle target --mount /mnt/disk0 --state drain
aios admin lifecycle target --aios-path /mnt/disk0/aios --weight 16
aios admin lifecycle autotune show
aios admin lifecycle autotune on --threshold 20 --min-delta 1
aios admin lifecycle autotune off
```

Archive **rules** are YAML-only (restart to change). Backup has YAML `backup_rules` plus live policies in cluster object `backup/policies` (CLI/UI CRUD, daily UTC + GFS retention). POSIX subtree layout uses YAML `posix_layout_rules` as seed plus live object `posix/layout_rules` (CLI/UI replace-all).

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
| `GET /admin/backup` | YAML `backup_rules` + live `policies` + intervals |
| `GET/POST /admin/api/backup/policies` | List / upsert live policies |
| `DELETE /admin/api/backup/policies/{id}` | Remove live policy |
| `POST /admin/backup/run` | Force YAML + enabled live policies |
| `POST /admin/backup/snapshot` | Create posix/vbd snapshot (`path` optional for posix) |
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
