# Per-object layout and placement (design)

AIOS has no pools or placement groups. Durability, storage class, and data layout are chosen **per object version at write time**. Cluster configuration supplies **defaults, caps, and transition policies**.

## Goals

- Client (or admin default / prefix rule) picks `replica` or `ec`, and a **storage class** (e.g. `nvme`, `hdd`), **per PUT**
- Placement uses **consistent hashing with virtual nodes** on a **class-scoped** ring
- Layout and storage class are stored on the version and are authoritative for GET / HEAD / repair
- Tips can **transition** between storage classes under `transition_rules` (background) or via a new PUT with a different class

## Placement

```
target T  →  weight w (from .aios; default 1; capacity-proportional, operator-set)
           →  only LifecycleState::Up enters the place() ring
           →  vnodes = clamp(w * vnodes_per_target, min_vnodes, max_vnodes)
oid key    =  sha256(oid) → u64
acting set =  clockwise on the class ring from oid key; unique physical targets,
              prefer distinct rack, then node_id, then same-node mounts
primary    =  acting_set[0]
```

```cpp
Placement place(const std::string& oid, const ClusterMap& map, int n,
                const std::string& storage_class);
```

- One vnode ring **per storage class**
- `n` is replica count or `k+m`
- Map includes **up** and **drain** targets; **off** is omitted. `place()` uses **up** only.
- Failure domains: **rack → node → mount**. Node `rack:` (YAML) defaults each target; `.aios` `rack:` overwrites. Empty node rack → `node_id`.
- Effective lifecycle = worse of node `node_state` and target `.aios` `state` (`off` > `drain` > `up`)
- Drain: still serves existing data; repair **evacuates** tips that are no longer in the acting set
- If the class has fewer than `n` **up** targets → empty acting set → `no_targets`
- Adding/removing a target remaps roughly `O(1/N)` of objects (consistent hashing)

### Device declaration (`.aios`)

```yaml
storage_class: nvme   # required; [a-z0-9_-]+
weight: 4             # optional; TiB units. Omit → total FS size (statvfs)
state: up             # up | drain | off (default up)
rack: row-a           # optional; overwrites node rack for this FS
targets: [data]       # optional; default = mount root
```

Daemon knobs (YAML / live admin):

```yaml
rack: row-a                            # node default failure domain (else node_id)
weight_autotune: false                 # true → weight from free space (TiB)
weight_autotune_threshold_pct: 20      # relative hysteresis
weight_autotune_min_delta: 1           # absolute floor on |Δweight|
```

Autotune updates advertised weight only when
`|Δ| ≥ max(min_delta, ceil(current × threshold_pct / 100))`.

Gossip liveness (separate from lifecycle): **online** / **suspect** / **offline** (legacy `alive`/`dead` still accepted on parse).
## Layout descriptor (version attrs)

| Attr | Meaning |
|------|---------|
| `aios.layout` | `replica` or `ec` |
| `aios.n` | copy/shard count |
| `aios.storage_class` | authoritative home class |
| `aios.storage_class_prev` | source class while dual-homed during transition |
| `aios.transition` | `copying` \| `done` (omit when idle) |
| `aios.ec.*` | Present iff `layout=ec` |

## Resolve precedence

Request fields → longest matching `layout_rules` prefix → cluster defaults
(`default_storage_class`, `durability` / `ec_*`).

HTTP: `x-aios-layout`, `x-aios-storage-class`, `x-aios-ec-*`.  
TCP++ JSON: `layout`, `storage_class`, `ec_k`, `ec_m`, `ec_codec`.

```yaml
layout_rules:
  - prefix: "hot/"
    layout: replica
    storage_class: nvme
  - prefix: "cold/"
    layout: ec
    storage_class: hdd
    ec_k: 2
    ec_m: 1
```

## Class transitions

```yaml
transition_rules:
  - prefix: "cold/"
    from: nvme
    to: hdd
    layout: ec      # optional layout change on migrate
    ec_k: 2
    ec_m: 1
```

Background worker (dest primary):

1. Match tip on `from` class under prefix
2. Copy / re-encode onto `to` class acting set (new tip version)
3. Set `aios.storage_class=to`, `aios.storage_class_prev=from`, `aios.transition=copying`
4. After dest quorum: clear `prev`, set `transition=done`

Reads prefer the tip class; fall back to `storage_class_prev` while dual-homed.

Admin:

- `GET /admin/transitions` — configured rules
- `POST /admin/transitions/run` — run one local transition tick

Client-driven transition: full PUT with `x-aios-storage-class` for the destination.

## Repair

Attrs win: place using tip `aios.storage_class` (and `n`); heal under-replication / missing EC shards on that class ring. During transition, may also consider `storage_class_prev`.

## Cold archive (packed bags)

Do **not** transition 1:1 onto tape. For cold data, use [`archive.md`](archive.md): pack many tips into large `archive/bag/*` objects, leave frozen stubs, then drain bag bodies via `tape_sink` (`external` / `s3` / `xrdcp`).

## Related docs

- HTTP API: [`http.md`](http.md)
- TCP++ wire format: [`README.md`](README.md)
- Admin: [`admin.md`](admin.md)
- Cold archive: [`archive.md`](archive.md)
