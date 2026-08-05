# Cold archive: packed bags (not per-object tape)

Warm class transitions remain **1:1** copies ([`layout.md`](layout.md)). The cold path **packs** many small tips into large **bag** objects, then replaces each tip with a **frozen stub**. Tape / cold backends only ever see bags.

```text
tips on `from` class  →  packer  →  archive/bag/{id} on staging_class
                                   →  each tip: empty body + freeze attrs
                          drain  →  bag body copied via tape_sink driver
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
    tape_sink: none               # none | external | s3 | xrdcp
archive_interval_ms: 30000
archive_batch_oids: 64
```

Declare `.aios` targets with `storage_class: archive` (or reuse `hdd`) for bag bodies.

### Tape sinks

| `tape_sink` | Drain / recall tool | Required fields |
|-------------|---------------------|-----------------|
| `none` / empty | Bags stay on staging (`bagged`) | — |
| `external` | Filesystem under `tape_root`, or custom cmds | `tape_root` |
| `s3` | `aws s3 cp` (or `tape_bin`) | `tape_uri_prefix` (`s3://…`) |
| `xrdcp` | `xrdcp -f -s` (or `tape_bin`) | `tape_uri_prefix` (`root://…` / `xroot://…`) |

```yaml
# Filesystem / HSM mount (or custom put/get executables)
tape_sink: external
tape_root: /var/lib/aios/tape
# tape_put_cmd: /usr/local/bin/aios-tape-put   # <bag_oid> <local_path> → stdout URI
# tape_get_cmd: /usr/local/bin/aios-tape-get   # <uri> <local_path>

# AWS CLI (or any aws-compatible binary)
tape_sink: s3
tape_uri_prefix: s3://cold-archive/aios-bags/
tape_bin: aws                          # optional; PATH lookup via execvp
tape_s3_endpoint: https://s3.example   # optional --endpoint-url
tape_root: /var/tmp/aios-tape          # optional local scratch (else system temp)

# XRootD copy
tape_sink: xrdcp
tape_uri_prefix: root://eos.example//eos/archive/bags/
tape_bin: xrdcp
tape_root: /var/tmp/aios-tape
```

**Driver argv (no shell):**

| Sink | Put | Get |
|------|-----|-----|
| `s3` | `aws s3 cp <local> <s3uri> [--endpoint-url …]` | `aws s3 cp <s3uri> <local> […]` |
| `xrdcp` | `xrdcp -f -s <local> <rooturi>` | `xrdcp -f -s <rooturi> <local>` |
| `external` + cmds | `tape_put_cmd <bag_oid> <local>` → stdout URI | `tape_get_cmd <uri> <local>` |

Stored `aios.tape_uri` is the full remote URI (`s3://…` / `root://…`) or a path relative to `tape_root` for the filesystem sink.

## Stub / bag attrs

| Attr | Meaning |
|------|---------|
| `aios.frozen` | `1` while archived (member stubs) |
| `aios.bag_id` | bag oid (`archive/bag/…`) |
| `aios.bag_offset` / `aios.bag_length` | member slice in bag |
| `aios.content_sha256` | member (or bag) checksum |
| `aios.archive_state` | `bagged` \| `on_tape` \| `restoring` |
| `aios.tape_sink` | `external` \| `s3` \| `xrdcp` |
| `aios.tape_root` | local scratch / fs library root (snapshot) |
| `aios.tape_uri_prefix` | remote prefix snapshot (`s3` / `xrdcp`) |
| `aios.tape_bin` / `aios.tape_s3_endpoint` | tool overrides snapshot |
| `aios.tape_uri` | locator after successful drain |

## Runtime

- Background tick packs tips, then drains staged bags; or admin:
  - `POST /admin/archive/run` — pack
  - `POST /admin/archive/drain` — copy bags out and reclaim staging
  - `POST /admin/archive/recall` `{"oid":"…"}` — restore bag if needed, rehydrate tip
- **GET** frozen + `bagged`: range-read member from bag (transparent)
- **GET** `on_tape` / `restoring`: HTTP **503** + `Retry-After`, code `restoring`
- **PUT** / ranged put on frozen tip: **409** `frozen`

### Drain / recall lifecycle

1. **Seal** — bag + members marked `on_tape`; bag body still on `staging_class`; sink attrs recorded.
2. **Drain** — copy bag via driver, set `aios.tape_uri`, install **empty** bag tip (staging reclaim).
3. **Recall** — if bag tip size is 0, fetch via `tape_uri`, reinstall bag on staging, extract member, clear freeze on that oid. Bag may be drained again on a later tick.

## Bag format

Binary `AIAB` v1: header → concatenated payloads → trailing index (`oid`, offset, length, sha256 hex, attrs JSON).
