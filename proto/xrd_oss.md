# XRootD OSS plugin (`libXrdAios`)

AIOS exposes hierarchical storage through `libaios_posix`. The XRootD gateway is a stock **XrdOfs** process with an **OSS plugin** that maps filesystem ops onto that ABI.

```text
client → XrdOfs → libXrdAios (OSS) → aios_posix_* → AIOS HTTP
```

## Identity

On each client op, the plugin reads `XrdOucEnv::secEnv()` → `XrdSecEntity::name`, maps it with the **local passwd database** (`getpwnam_r`), and calls `aios_posix_set_caller(fs, uid, gid)`.

| Case | Result |
|------|--------|
| `name` missing / unknown in PWD | `-EACCES` (fail closed) |
| Mapped uid `0` | POSIX root bypass in libaios_posix |
| No env (internal server call) | Clear caller → mount defaults |

Configure a `sec.protocol` that fills `entity.name` with a local account (e.g. `unix`, or your site mapper).

## Build

```bash
cmake -S . -B build -DXRootD_ROOT=/path/to/xrootd   # or install prefix
cmake --build build --target XrdAios
# → build/libXrdAios.so
```

Requires XRootD headers (`XrdOss/XrdOss.hh`, `XrdVersion.hh`) and `libXrdUtils`. Disable with `-DAIOS_WITH_XROOTD=OFF`.

## Config

See [`config/xrootd.aios.example.cf`](../config/xrootd.aios.example.cf). Minimal:

```text
all.export /
ofs.osslib /path/to/libXrdAios.so
aios.endpoint 127.0.0.1:7480
aios.cluster_key $KEY
aios.volume default
```

Parms after the library path also work: `ofs.osslib libXrdAios.so endpoint=… cluster_key=…`. Env fallbacks: `AIOS_ENDPOINT`, `AIOS_CLUSTER_KEY`.

## Manual check

1. Start `aiosd` with HTTP listen.
2. Start `xrootd -c xrootd.aios.example.cf` with `libXrdAios.so` on `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`.
3. `xrdcp` / `xrdfs` as a user whose login name exists in the server passwd DB; mode bits on AIOS inodes apply to the mapped uid/gid.

## Scope (v1)

Create/open/read/write/stat/mkdir/rmdir/unlink/rename/truncate/chmod/readdir. No TPC, AIO, or proxy FSctl.
