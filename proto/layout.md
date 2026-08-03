# Per-object layout (design)

AIOS does **not** use Ceph-style pools or placement groups. Durability and data layout are chosen **per object version at write time**. Cluster configuration supplies **defaults and safety caps** only.

Per-object layout is implemented for HTTP/`api_put*` and TCP++ `ObjectPut` / `ObjectPutRange`: clients may send `x-aios-layout` / `x-aios-ec-*` headers or JSON `layout` / `ec_k` / `ec_m` / `ec_codec`; omit → longest matching `layout_rules` prefix, else cluster `durability` / `ec_*` defaults. Layout attrs (`aios.layout`, `aios.n`, and `aios.ec.*` when EC) are stored on each version.

## Goals

- Client (or admin default / prefix rule) picks `replica` or `ec` (and EC `k` / `m` / codec) **per PUT**
- Layout is stored on the version and is authoritative for GET / HEAD / repair
- The same oid may change layout across versions (each full PUT chooses anew)
- No pool namespace and no PG map

## Non-goals (v1)

- Background layout migration / re-encode of existing tips
- Hard deny of client overrides (prefix rules are defaults, not locks)
- Runtime rule reload / gossip of rules (config load / restart only)
- Silently using fewer targets than requested (`n` larger than the cluster → hard fail)
- Changing acting-set size for an already-written version

## Current baseline

```
PUT  →  cfg.durability  →  replica: full-copy × map.replica_count
                        →  ec: stripe with cfg.ec_*

GET  →  tip has aios.ec.* ?  →  reconstruct from shards
                             →  else return tip body
```

- Placement: `place(oid, map)` sizes the acting set from **`map.replica_count` only**
- EC shard metadata (when used): `aios.ec.k`, `aios.ec.m`, `aios.ec.i`, `aios.ec.codec`, `aios.ec.full_size`, `aios.ec.full_crc`

## Target model

### Layout descriptor (version attrs)

Stored on every new version:

| Attr | Meaning |
|------|---------|
| `aios.layout` | `replica` or `ec` |
| `aios.ec.*` | Present iff `layout=ec` (same keys as today) |
| `aios.n` | Optional: copy/shard count actually used (`replica` count or `k+m`) |

- Replica versions set `aios.layout=replica` and omit EC attrs
- EC versions set `aios.layout=ec` plus the existing `aios.ec.*` keys

Legacy objects without `aios.layout` remain valid: presence of `aios.ec.k` + `aios.ec.m` means EC; otherwise replica.

### HTTP request API

On `PUT /o/{oid}` (and later txn prepare when wired):

| Header | Role |
|--------|------|
| `x-aios-layout: replica \| ec` | Select layout; omit → prefix rule / cluster default |
| `x-aios-ec-k`, `x-aios-ec-m` | EC parameters; omit → prefix rule / cluster defaults |
| `x-aios-ec-codec: xor \| isal` | Omit → auto (`xor` if `m==1`, else `isal`) |

GET/HEAD already return attrs as `x-aios-attr-*`, so clients can discover layout after the fact.

TCP++ `ObjectPut` / `ObjectPutRange` accept the same fields as JSON (`layout`, `ec_k`, `ec_m`, `ec_codec`).

### Cluster config (defaults, caps, prefix rules)

| Knob | Role |
|------|------|
| `default_layout` (`replica` \| `ec`) | Cluster default when no request field and no matching rule. Today’s `durability` maps here |
| `default_ec_k`, `default_ec_m`, `default_ec_codec` | Defaults for EC writes |
| `max_ec_k`, `max_ec_m`, `max_replica_count` | Reject oversized client / rule requests |
| `layout_rules` | Optional list of `{prefix, layout, ec_*?}` admin defaults |
| `replica_count` / map field | Default **N** for replica layout and upper bound for the placement ring width — **not** “every object uses N” |

**Resolve precedence (field-by-field):** request → longest matching `layout_rules` prefix → cluster defaults. Equal-length prefixes: first rule in YAML wins. Empty prefix `""` is allowed as a catch-all.

```yaml
layout_rules:
  - prefix: "hot/"
    layout: replica
  - prefix: "cold/"
    layout: ec
    ec_k: 2
    ec_m: 1
    ec_codec: xor
```

### Placement

```cpp
Placement place(const std::string& oid, const ClusterMap& map, int n);
// place(oid, map) == place(oid, map, map.replica_count)  // compat
```

- Same SHA-256 start index and distinct-node walk as today
- `n` is the replica count or `k+m` for **that write**
- If `n > map.targets.size()` → fail `no_targets` (no silent under-protection)
- Primary remains `acting_set[0]`; HTTP 307 redirects unchanged

### Write path

1. Resolve `ObjectLayout` from request + prefix rules + config defaults + caps
2. `place(oid, map, layout.n)`
3. Branch:
   - **replica** → prepare + quorum install of full copies + publish tip
   - **ec** → encode, install one shard per acting-set member, publish tips
4. Persist layout attrs on every copy/shard

`Content-Range` / ranged PUT: allowed only when the tip (or new version) layout is `replica`. EC versions reject ranged PUT (`400`).

### Read and repair

- Tip (or selected version) has EC attrs → reconstruct from any `k` shards
- Otherwise → normal tip body read (including txn-prepared full copies on an EC-default cluster)
- Repair: **attrs win**. EC reconstruct/rebuild if EC attrs present; else byte-copy replica repair. Global default layout is not the sole signal

### Transactions

- Coordinator object `txn/<id>`: force **replica** (small JSON control plane)
- Prepared data ops: layout from prepare-request headers, or cluster default
- A single txn may prepare objects with different layouts

### Versioning

- Layout is **per version**, not a permanent oid property
- Tip GET/HEAD use the tip version’s layout
- `?version=` / `x-aios-version` use that version’s attrs

## Target flow

```
PUT (+ optional layout headers)
  → resolve ObjectLayout
  → place(oid, map, n)
  → replica: full-copy quorum + attrs aios.layout=replica
  → ec: stripe shards + attrs aios.layout=ec + aios.ec.*

GET
  → tip attrs
  → ec → fetch any k shards, decode, return full object
  → replica → return tip body
```

## Compatibility

| Existing data | Behavior |
|---------------|----------|
| EC shards with `aios.ec.*` but no `aios.layout` | Treated as EC via `attrs_are_ec` |
| Full-copy tips with no EC attrs | Treated as replica |
| YAML `durability: ec` | Becomes `default_layout: ec` so deployments keep current behavior until clients send headers |

## Implementation rollout (code follow-up)

1. `ObjectLayout` + resolve helper (headers / config / caps)
2. `place(oid, map, n)` with compat overload
3. Wire HTTP PUT (and TCP++ Put) to per-request layout; stop forcing all writes from global durability alone
4. Repair attrs-first cleanup if any global-only branches remain
5. Tests: same cluster hosts both a replica object and an EC object; GET + repair both; HTTP header round-trip
6. Optional: `aios-bench --layout …`

## Related docs

- HTTP API: [`http.md`](http.md)
- TCP++ wire format: [`README.md`](README.md)
