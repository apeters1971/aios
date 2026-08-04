# Soft IOPS / bandwidth QoS

Node-local rate limits on POSIX volume I/O (FUSE + S3). Enforcement uses token buckets on the posix read/write path — the same choke point S3 uses. Limits are soft/delayed like quotas: each node admits independently from durable `qos/{volume}/limits`.

## Domains

Same identity model as [quotas](quota.md):

| Domain | Key | Limits |
|--------|-----|--------|
| Volume | uid / gid | `iops` (ops/s), `bps` (bytes/s) |
| Project | `project_id` from inode inheritance | project total + optional per-uid / per-gid |

A request is denied if **any** applicable limit lacks tokens. Denied posix I/O returns `-EAGAIN`; S3 maps that to `503 SlowDown`.

Project QoS keys the same `project_id` stamped by quota project roots (no separate QoS project create).

## Durable object

**`qos/{volume}/limits`**

```json
{
  "volume": {
    "uids": { "1001": { "iops": 1000, "bps": 104857600 } },
    "gids": { "100": { "iops": 5000 } }
  },
  "projects": {
    "1": { "iops": 2000, "bps": 52428800, "uids": { "1001": { "iops": 100 } } }
  }
}
```

Volume defaults to `s3_volume` when S3 is enabled, else `default`.

## Monitoring

`GET /admin/api/qos` includes:

- Configured limits
- **Node OPS rates** derived from successive polls of process `OpsRegistry` (put/get IOPS and B/s)
- **Observed rates** from local posix admit windows (per uid/gid/project)

## Admin

```bash
aios admin qos show
aios admin qos set --uid 1001 --iops 1000 --bps 100M
aios admin qos set --gid 100 --iops 5000
aios admin qos set --uid 1001 --clear
aios admin qos project set --id 1 --iops 2000 --bps 50M
aios admin qos project set --id 1 --uid 1001 --iops 100
```

HTTP: `GET /admin/api/qos`, `PUT /admin/api/qos/limits` with `{uid|gid|project_id, uid?, iops?, bps?, clear?}`.

Web UI: **QoS** tab.
