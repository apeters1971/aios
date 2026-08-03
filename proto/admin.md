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

## HTTP endpoints

All admin JSON routes still require the normal AIOS HMAC Authorization header, except `GET /metrics` when `admin_metrics_public` is true.

| Path | Response |
|------|----------|
| `GET /admin/status` | Node identity, map epoch, membership, local `ops` |
| `GET /admin/ops` | `{node_id, ops, ops_by_label}` |
| `GET /admin/config` | Effective config (`cluster_key` → `"***"`). Read-only; changes need restart. |
| `GET /admin/cluster` | Local status + `admin_peers` (`node_id`, `http_addr`) for cluster scrape |
| `GET /metrics` | Prometheus counters (`aios_*_total`) |

## OPS counters

Process-local monotonic counters (see `src/metrics/ops_counters.hpp`), including:

- HTTP requests, put / put_range / append / get / head / del / list
- put/get/append bytes
- lock_acquire, watch, pubsub_publish
- gossip_rounds, repair_scanned / repaired / failed
- errors (failed API results; excludes `not_primary` redirects)

**Cluster-wide totals:** scrape `/admin/ops` (or use the console `cluster` command) across alive peers listed by `/admin/cluster`. Gossip does not carry counters.

Counters are also written into `status_file` under `ops` / `ops_by_label` when that path is configured.

### Application labels

Clients send `x-aios-app-label: <label>` (see [`http.md`](http.md)). Each distinct label gets its own counter bucket (max 256; further labels roll into `_overflow`). Totals always include labeled + unlabeled traffic; `ops_by_label` is the breakdown.

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

Point Prometheus at an admin node's `/metrics`. Prefer `admin_metrics_public: true` plus network ACLs, or configure a scraper that signs AIOS HMAC.

Example metric names: `aios_http_requests_total`, `aios_ops_put_total`, `aios_ops_get_bytes_total`, `aios_gossip_rounds_total`, `aios_repair_repaired_total`.

Labeled example:

```
aios_ops_put_total{node_id="node-a"} 100
aios_ops_put_total{node_id="node-a",app_label="etl"} 40
```
