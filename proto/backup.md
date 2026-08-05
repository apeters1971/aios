# Backup: POSIX / VBD snapshot then archive copy-out

Do **not** archive live `posix/` or `vd/` tips while mounted. Backup creates an immutable tree, then reuses packed bags + [`archive.md`](archive.md) `tape_sink` drain.

```text
POSIX volume  → freeze → posix/{vol}/.snap/{id}/  → pack bags → tape drain
VBD volume    → clone+seal → vd/{pool}/{dest}/    → pack bags → tape drain
```

## Config

```yaml
backup_rules:
  - kind: posix
    volume: default
    retain_snaps: 3
    from: nvme                 # storage_class of snap tips
    staging_class: archive
    tape_sink: s3              # none | external | s3 | xrdcp
    tape_uri_prefix: s3://backups/aios/
  - kind: vbd
    pool: rbd
    name: disk0
    staging_class: archive
    tape_sink: external
    tape_root: /var/lib/aios/tape
backup_interval_ms: 3600000
backup_batch_oids: 256
```

Declare `.aios` targets for `staging_class` (bags), same as cold archive.

## Consistency

- **Crash-consistent** object cut (not guest `fsfreeze`).
- POSIX: sets `super.frozen=true` during copy; mutating POSIX ops return `-EBUSY`.
- VBD: copies `data.*` stripes into a new volume name, then clears `parent_*` and sets `sealed=true`.

## Admin

| Path | Behavior |
|------|----------|
| `GET /admin/backup` | Rules + intervals |
| `POST /admin/backup/run` | One full backup tick (snap → pack → drain → prune) |
| `POST /admin/backup/snapshot` | `{kind, volume}` or `{kind, pool, name, dest?}` only |

## POSIX ABI

```c
int aios_posix_snapshot(aios_posix_fs* fs, char* snap_id_out, size_t snap_id_len);
```

Creates `posix/{vol}/.snap/{id}/` mirroring live oids (excluding other snaps).

## Related

- Cold archive bags / tape drivers: [`archive.md`](archive.md)
- POSIX layout: [`posix_fuse.md`](posix_fuse.md)
- VBD layout: [`../kernel/README.md`](../kernel/README.md)
