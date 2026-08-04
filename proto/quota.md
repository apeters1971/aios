# Soft uid/gid and project quotas

Logical stored-byte quotas on a POSIX volume (shared by FUSE and S3). Enforcement is **soft / delayed**: local atomics on the write path, periodic CAS flush of usage, optional reconcile.

## Domains

| Domain | `project_id` | Limits |
|--------|--------------|--------|
| Volume | `0` | Per-uid and optional per-gid |
| Project | `> 0` | Subtree rooted at a directory; total bytes + optional uid/gid inside the project |

Inodes store inherited `project_id` (copied from the parent on create). Renames across projects update the moved inode’s `project_id` and move its size between project counters. Nested projects are not supported in v1.

## Durable objects

- `quota/{volume}/limits` — admin-written limits
- `quota/{volume}/usage` — aggregated usage (nodes flush deltas)

Volume defaults to `s3_volume` when S3 is enabled, else `default`.

## Enforcement

On file grow (`write` / `truncate` up), posix returns `-EDQUOT` if any applicable limit would be exceeded. S3 maps that to `403 QuotaExceeded`. Shrinks and deletes always apply (negative deltas).

## Admin

```bash
aios admin quota show
aios admin quota set --uid 1001 --bytes 10G
aios admin quota set --gid 100 --bytes 50G
aios admin quota set --uid 1001 --clear
aios admin quota project create --name photos --root-ino 42 --bytes 20G
aios admin quota project set --id 1 --uid 1001 --bytes 5G
aios admin quota project delete --id 1
aios admin quota reconcile
```

HTTP (cookie or HMAC): `GET /admin/api/quota`, `PUT /admin/api/quota/limits`, `POST /admin/api/quota/projects`, `PUT /admin/api/quota/projects/{id}` (`{uid, bytes|null}`), `DELETE /admin/api/quota/projects/{id}`, `POST /admin/api/quota/reconcile`.

Web UI: **Quotas** tab.
