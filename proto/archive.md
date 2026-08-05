# Cold archive: packed bags (not per-object tape)

Warm class transitions remain **1:1** copies ([`layout.md`](layout.md)). The cold path **packs** many small tips into large **bag** objects, then replaces each tip with a **frozen stub**. Tape systems only ever see bags.

```text
tips on `from` class  →  packer  →  archive/bag/{id} on staging_class
                                   →  each tip: empty body + freeze attrs
                          drain  →  bag body copied to tape_root (or tape_put_cmd)
                                   →  staging bag body reclaimed (empty)
                         recall  →  restore bag to staging if needed → rehydrate tip
```

## Config

```yaml
archive_rules:
  - prefix: "cold/"
    from: hdd
    staging_class: archive
    min_age_days: 30
    min_bag_bytes: 68719476736    # 64 GiB
    max_bag_bytes: 274877906944   # 256 GiB
    max_members: 0                # 0 = unlimited
    max_open_ms: -1               # 0 = seal undersized bags each tick
    tape_sink: none               # none | external
    # When tape_sink: external:
    tape_root: /var/lib/aios/tape # fs library / HSM mount (required)
    # Optional external driver (absolute path to executable):
    # tape_put_cmd: /usr/local/bin/aios-tape-put   # args: <bag_oid> <local_path>; stdout: URI
    # tape_get_cmd: /usr/local/bin/aios-tape-get   # args: <uri> <local_path>
archive_interval_ms: 30000
archive_batch_oids: 64
```

Declare `.aios` targets with `storage_class: archive` (or reuse `hdd`) for bag bodies.

## Stub / bag attrs

| Attr | Meaning |
|------|---------|
| `aios.frozen` | `1` while archived (member stubs) |
| `aios.bag_id` | bag oid (`archive/bag/…`) |
| `aios.bag_offset` / `aios.bag_length` | member slice in bag |
| `aios.content_sha256` | member (or bag) checksum |
| `aios.archive_state` | `bagged` \| `on_tape` \| `restoring` |
| `aios.tape_sink` | `external` when a tape drain is configured |
| `aios.tape_root` | snapshot of rule `tape_root` at seal |
| `aios.tape_uri` | locator after successful drain (relative under `tape_root`, absolute path, or cmd URI) |

## Runtime

- Background tick packs tips, then drains staged bags; or admin:
  - `POST /admin/archive/run` — pack
  - `POST /admin/archive/drain` — copy bags to tape and reclaim staging
  - `POST /admin/archive/recall` `{"oid":"…"}` — restore bag if needed, rehydrate tip
- **GET** frozen + `bagged`: range-read member from bag (transparent)
- **GET** `on_tape` / `restoring`: HTTP **503** + `Retry-After`, code `restoring`
- **PUT** / ranged put on frozen tip: **409** `frozen`

### `tape_sink: external` lifecycle

1. **Seal** — bag + members marked `on_tape`; bag body still on `staging_class`; attrs record `tape_root`.
2. **Drain** — copy bag bytes to `tape_root/bags/<safe_oid>` (or `tape_put_cmd`), set `aios.tape_uri`, install **empty** bag tip (staging reclaim). Members stay frozen stubs.
3. **Recall** — if bag tip size is 0, fetch via `tape_uri` (`tape_get_cmd` or filesystem), reinstall bag body on staging, extract member, clear freeze on that oid. Bag may be drained again on a later tick.

Without `tape_put_cmd` / `tape_get_cmd`, the built-in driver is a durable filesystem tree under `tape_root` (typical for an HSM-managed mount or test library).

### External commands

| Command | Args | Contract |
|---------|------|----------|
| `tape_put_cmd` | `<bag_oid> <local_path>` | Exit 0; first stdout line = durable URI |
| `tape_get_cmd` | `<uri> <local_path>` | Exit 0; write full bag bytes to `<local_path>` |

Commands are executed with `execv` (no shell). Paths must be absolute.

## Bag format

Binary `AIAB` v1: header → concatenated payloads → trailing index (`oid`, offset, length, sha256 hex, attrs JSON).
