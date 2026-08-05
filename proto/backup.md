# Backup: POSIX / VBD snapshot then archive copy-out

Do **not** archive live `posix/` or `vd/` tips while mounted. Backup creates an immutable tree, then reuses packed bags + [`archive.md`](archive.md) `tape_sink` drain.

```text
POSIX volume/subtree  → freeze → posix/{vol}/.snap/{id}/  → pack bags → tape drain
VBD volume            → clone+seal → vd/{pool}/{dest}/    → pack bags → tape drain
```

## Two policy sources (both run)

1. **YAML `backup_rules`** — interval timer (`backup_interval_ms`), count retention (`retain_snaps`). Restart to change.
2. **Live policies** — cluster CAS object `backup/policies`. Editable via CLI / Web UI without restart. Daily UTC schedule + GFS retention.

```yaml
backup_rules:
  - kind: posix
    volume: default
    retain_snaps: 3
    from: nvme
    staging_class: archive
    tape_sink: s3
    tape_uri_prefix: s3://backups/aios/
    bag_compression: zstd
    bag_encryption: aes-256-gcm
# bag_encryption_key: "<64 hex chars>"   # cluster-wide; required for aes-256-gcm
backup_interval_ms: 3600000
backup_batch_oids: 256
```

Bag compression/encryption uses the same whole-bag AITF path as cold archive ([`archive.md`](archive.md)).

Live policy example (JSON in `backup/policies`):

```json
{
  "aios_backup_policies": 1,
  "policies": [{
    "id": "abc",
    "enabled": true,
    "kind": "posix",
    "volume": "default",
    "path": "/home",
    "schedule": { "at": "00:00", "tz": "UTC" },
    "retain": { "keep_days": 7, "keep_monthly": 12 },
    "staging_class": "archive",
    "tape_sink": "s3",
    "tape_uri_prefix": "s3://backups/aios/",
    "bag_compression": "zstd",
    "bag_encryption": "aes-256-gcm"
  }]
}
```

- **Schedule:** one daily trigger at `HH:MM` UTC. The backup timer polls (YAML interval, or 60s when only live policies exist); a policy runs when that daily instant is after `last_run_ms`.
- **GFS retain:** keep every snap newer than `keep_days`; also keep the newest snap of each UTC calendar month for `keep_monthly` months. Prune only after tips are frozen/drained.
- **Subtree:** `path` `/` (or empty) = whole volume; otherwise volume-relative directory (e.g. `/home/alice`). Freeze remains volume-wide.

## Consistency

- **Crash-consistent** object cut (not guest `fsfreeze`).
- POSIX: sets `super.frozen=true` during copy; mutating POSIX ops return `-EBUSY`.
- VBD: copies `data.*` stripes into a new volume name, then clears `parent_*` and sets `sealed=true`.

## Admin

| Path | Behavior |
|------|----------|
| `GET /admin/backup` | YAML rules + live `policies` + intervals |
| `GET /admin/api/backup/policies` | Live policies only |
| `POST /admin/api/backup/policies` | Create/upsert policy JSON |
| `DELETE /admin/api/backup/policies/{id}` | Remove policy |
| `POST /admin/backup/run` | Force YAML + all enabled live policies |
| `POST /admin/backup/snapshot` | `{kind, volume, path?}` or `{kind, pool, name, dest?}` |

CLI:

```text
aios admin backup show|run
aios admin backup snapshot posix --volume V [--path /subdir]
aios admin backup policy list|set|rm
aios admin backup policy set --volume default --path /home --at 00:00 --keep-days 7 --keep-monthly 12
```

## POSIX ABI

```c
int aios_posix_snapshot(aios_posix_fs* fs, char* snap_id_out, size_t snap_id_len);
int aios_posix_snapshot_at(aios_posix_fs* fs, const char* path, char* snap_id_out, size_t snap_id_len);
```

Manifest under each snap includes `path`, `root_ino`, `created_ms`, and optional `policy_id`.

## Related

- Cold archive bags / tape drivers: [`archive.md`](archive.md)
- POSIX layout: [`posix_fuse.md`](posix_fuse.md)
- VBD layout: [`../kernel/README.md`](../kernel/README.md)
- Admin UI/CLI: [`admin.md`](admin.md)
