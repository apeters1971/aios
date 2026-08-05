# cuObject / GPUDirect S3 (scaffold)

AIOS can offload S3 object **payloads** to NVIDIA **cuObject** RDMA while keeping SigV4 HTTP as the control plane.

```text
GPU / host client
    |  S3 HTTP + x-amz-rdma-token
    v
S3Server (auth, locate object)
    |  host buffer via aios_posix
    v
cuObjServer  ==== RDMA ====  client memory
```

## Build

Optional; the tree builds without the NVIDIA SDK.

```bash
cmake -DAIOS_WITH_CUOBJECT=ON -DCUOBJECT_ROOT=/path/to/cuobjserver ..
# or: export CUOBJECT_ROOT=...
```

When headers + `libcuobjserver` are found, CMake sets `AIOS_HAVE_CUOBJECT=1` and links the real backend. Otherwise only null/stub endpoints are available (CI).

## Config

```yaml
s3_listen: "0.0.0.0:7481"
# RDMA listen for cuObjServer (separate from S3 HTTP):
cuobject_listen: "0.0.0.0:18515"
```

CLI: `--cuobject-listen HOST:PORT`. Empty (default) disables RDMA; S3 behaves as today.

## Headers

| Header | Direction | Meaning |
|--------|-----------|---------|
| `x-amz-rdma-token` | request | Opaque RDMA descriptor from cuObjClient (must be in SigV4 `SignedHeaders`) |
| `x-amz-rdma-reply` | response | Present when the server completed RDMA offload |

## Server behaviour

### GET

1. Authenticate as usual.
2. If `x-amz-rdma-token` is set, object fits in one transfer (≤ 1 GiB), no `Range`, and cuobject is available:
   - Read object into a host buffer via posix
   - `rdma_get` / `handleGetObject` (RDMA_WRITE to client)
   - HTTP `200` with **empty body**, `Content-Length` = logical size, `x-amz-rdma-reply`
3. On RDMA failure or size/range limits: **fall back to TCP body** (no reply header).

Clients that see `x-amz-rdma-reply` must **not** read a TCP body; `Content-Length` is the logical object size delivered over RDMA.

### PUT

1. `Content-Length` = object size; TCP body empty; prefer `x-amz-content-sha256: UNSIGNED-PAYLOAD`.
2. If token present and endpoint available: `rdma_put` then posix write; reply includes `x-amz-rdma-reply`.
3. If token present but endpoint unavailable: **`501 NotImplemented`** (retry without token / use TCP PUT).

Multipart UploadPart and ranged RDMA GET are out of scope for this scaffold.

## Client example

```bash
aios-cuobj-s3 put --endpoint 127.0.0.1:7481 --access-key aios --secret "$KEY" \
  s3://bucket/obj --file ./data.bin --tcp

aios-cuobj-s3 get --endpoint 127.0.0.1:7481 --access-key aios --secret "$KEY" \
  s3://bucket/obj --file ./out.bin --rdma
```

`--rdma` injects a synthetic token when the NVIDIA **cuObjClient** SDK is not linked. With a real SDK, replace that token with `cuMemObjGetRDMAToken` / `cuObjGet`/`cuObjPut` output. Clients must treat a response **without** `x-amz-rdma-reply` as TCP fallback (use the HTTP body).

## Hardware

- NVIDIA GPU with GPUDirect RDMA (for GPU buffers)
- RDMA NIC (InfiniBand or RoCEv2), preferably same PCIe root complex as the GPU
- CUDA toolkit (client) + cuObjServer partner package (server)

## Code map

| Path | Role |
|------|------|
| `src/cuobject/cuobject_endpoint.hpp` | `CuObjectEndpoint` API |
| `src/cuobject/cuobject_{null,stub,nvidia}.cpp` | Backends |
| `src/http/s3_server.cpp` | Control-plane hooks |
| `tools/aios_cuobj_s3.cpp` | Example SigV4 client |
| `tests/test_cuobject_s3.cpp` | Stub-backed GET/PUT tests |
