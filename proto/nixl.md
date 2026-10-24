# NIXL plugin (AIOS)

Out-of-tree [NIXL](https://github.com/ai-dynamo/nixl) storage plugin. A NIXL agent on the GPU host loads `libplugin_AIOS.so` and names backend **`AIOS`**. Transfers are loopback: the plugin is an S3 client, and the payload moves with the existing cuObject RDMA path ([`cuobject.md`](cuobject.md)).

```text
NIXL agent
  local  DRAM or VRAM  (metaInfo = cuObject RDMA token)
  remote OBJ           (metaInfo = bucket/key)
        |
        |  GET or PUT, empty TCP body, x-amz-rdma-token
        v
aiosd S3  -- cuObjServer RDMA -->  the registered buffer
```

`NIXL_READ` is object → buffer (`rdma_get`). `NIXL_WRITE` is buffer → object (`rdma_put`). The plugin does not copy through a host bounce buffer. cuObjServer still stages the object in host memory on the storage node, then RDMA-writes or RDMA-reads the client buffer named by the token.

## What a transfer may be

cuObject accepts one whole object, at most 1 GiB, and no byte range.

- Remote `addr` (object offset) must be 0.
- Local `addr` and `len` must be exactly the registered buffer. The RDMA token describes that buffer, not a subrange.
- DRAM and VRAM `metaInfo` is the token from `cuMemObjGetRDMAToken` (or the backend option `rdma_token` when metaInfo is empty).
- OBJ `metaInfo` is `bucket/key`. The plugin reads the key from registration metadata. Two objects may share a `devId`.

A 200 response without `x-amz-rdma-reply` is a failure. That is the TCP fallback, and the registered buffer was not filled.

## Build

Optional. The tree builds without NIXL. The transfer engine and its tests always compile; `libplugin_AIOS.so` is produced only when the NIXL headers are found. The plugin must be built with the same `NIXL_PLUGIN_API_VERSION` as the `libnixl` that loads it (the plugin manager rejects any other version).

```bash
cmake --preset default -DNIXL_ROOT=/path/to/nixl
# headers used: src/api/cpp, src, src/core/telemetry
```

Installs to `<prefix>/lib/nixl/plugins/libplugin_AIOS.so`. Point the agent at that directory with `NIXL_PLUGIN_DIR` (or the plugin list your NIXL build uses).

## Backend options

| Key | Meaning |
|-----|---------|
| `s3_endpoint` | `host:port` of `s3_listen` |
| `access_key` | SigV4 access key id |
| `secret_key` | SigV4 secret (`cluster_key` for the root key) |
| `region` | SigV4 region (default `us-east-1`) |
| `rdma_token` | Token used when a DRAM/VRAM registration has empty metaInfo |
| `socket_timeout_ms` | Connect and header timeout (default 30000) |

`aiosd` must be started with `cuobject_listen` and a build that found cuObjServer. Without that, RDMA PUT returns 501 and RDMA GET falls back to a TCP body; the plugin treats both as errors.

## Code map

| Path | Role |
|------|------|
| `src/nixl/aios_nixl.hpp` | Engine, registration, transfer state |
| `src/nixl/aios_nixl_s3.cpp` | SigV4 GET/PUT with `x-amz-rdma-token` |
| `src/nixl/nixl_plugin.cpp` | `nixl_plugin_init`, backend name `AIOS` |
| `tests/test_nixl.cpp` | Descriptor routing, cancel, SigV4, RDMA reply |
