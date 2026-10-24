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
| `x-amz-rdma-token` | request | Opaque RDMA descriptor from cuObjClient (include it in SigV4 `SignedHeaders`) |
| `x-amz-rdma-reply` | response | `200` whole object, `206` byte range, `501` declined, `400`/`500` on a failed attempt |
| `x-amz-rdma-bytes-transferred` | response | Decimal byte count actually moved. Set only after the RDMA call succeeds |

The token is colon-delimited hex. Field 1 is the remote address (`uint64`, at most 16 digits). Field 2 is the registered buffer size (`uint32`, at most 8 digits). The rest (rkey, LID, DCT number, GID) is passed through to `handleGetObject` / `handlePutObject` unchanged. A first field longer than 16 hex digits is not this scheme; aiosd answers `501` with `x-amz-rdma-reply: 501`.

## Server behaviour

### GET

1. Authenticate as usual.
2. If `x-amz-rdma-token` is set and the token is a DC descriptor, read the span into a host buffer and `rdma_get` (`handleGetObject`, RDMA_WRITE). NIXL always sends `Range`, including `bytes=0-(n-1)` for a full read.
   - The span is the whole object (no `Range`, or a range that covers it): HTTP `200`, `x-amz-rdma-reply: 200`.
   - The span is a sub-range: HTTP `206`, `x-amz-rdma-reply: 206`, plus `Content-Range`.
   - `Content-Length` is `0`. The count is `x-amz-rdma-bytes-transferred`. The TCP body is empty.
3. One transfer is at most 1 GiB. A token buffer smaller than the span is `400`. A transfer error falls back to the TCP body and does **not** set `x-amz-rdma-reply`.

### PUT

1. TCP body empty. `x-amz-content-sha256: UNSIGNED-PAYLOAD`.
2. Object size is `Content-Length` when that is non-zero. NIXL's accelerated client sends `Content-Length: 0`; the size is then field 2 of the token. The two must agree when both are present.
3. On success: HTTP `200`, `ETag`, `x-amz-rdma-reply: 200`, `x-amz-rdma-bytes-transferred`. The key is created only after the RDMA read completes, so a decline does not store a 0-byte object.
4. No size, or cuObjServer is not available: **`501`** and `x-amz-rdma-reply: 501`. A malformed token is `400` with reply `400`. A failed RDMA call is `500` with reply `500`.

Multipart UploadPart (`uploadId`) stays on the TCP body. A token on that request is ignored.

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

A NIXL agent does not talk to cuObjServer itself. `libplugin_AIOS.so` ([`nixl.md`](nixl.md)) sends the S3 request with `x-amz-rdma-token`, and this endpoint performs the RDMA.
