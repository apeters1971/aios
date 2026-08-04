#!/usr/bin/env bash
# Stress aiosvd: map → fio randrw → discard → resize → unmap
set -euo pipefail

ENDPOINT="${ENDPOINT:-127.0.0.1:7480}"
KEY="${KEY:-${AIOS_CLUSTER_KEY:-}}"
POOL="${POOL:-default}"
NAME="${NAME:-stress$$}"
SIZE="${SIZE:-256M}"
RESIZE_TO="${RESIZE_TO:-512M}"
FIO_RUNTIME="${FIO_RUNTIME:-20}"
AIOS_VD="${AIOS_VD:-./build/aios-vd}"

if [[ -z "$KEY" ]]; then
  echo "Set KEY or AIOS_CLUSTER_KEY" >&2
  exit 1
fi
if [[ ! -x "$AIOS_VD" ]]; then
  echo "aios-vd not found at $AIOS_VD (build first)" >&2
  exit 1
fi
if ! command -v fio >/dev/null 2>&1; then
  echo "fio is required" >&2
  exit 1
fi
if [[ ! -e /dev/aiosvd_ctl ]]; then
  echo "load aios_http.ko and aiosvd.ko first" >&2
  exit 1
fi

cleanup() {
  if [[ -n "${DEV_ID:-}" ]]; then
    "$AIOS_VD" unmap "$DEV_ID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "==> map $POOL/$NAME size=$SIZE"
MAP_OUT=$("$AIOS_VD" map --endpoint "$ENDPOINT" --key "$KEY" --pool "$POOL" --name "$NAME" \
  --size "$SIZE" --create --excl)
echo "$MAP_OUT"
DEV_ID=$(echo "$MAP_OUT" | sed -n 's|.*/aiosvd\([0-9][0-9]*\).*|\1|p' | head -1)
DEV="/dev/aiosvd${DEV_ID}"
if [[ -z "$DEV_ID" || ! -b "$DEV" ]]; then
  echo "failed to resolve device from: $MAP_OUT" >&2
  exit 1
fi

echo "==> fio randrw on $DEV (${FIO_RUNTIME}s)"
fio --name=aiosvd_stress --filename="$DEV" --rw=randrw --bs=4k --iodepth=16 \
  --runtime="$FIO_RUNTIME" --time_based --direct=1 --ioengine=libaio \
  --size=100% --group_reporting

echo "==> discard (blkdiscard)"
if command -v blkdiscard >/dev/null 2>&1; then
  blkdiscard -f "$DEV" || blkdiscard "$DEV" || true
else
  echo "blkdiscard not found; skipping discard step"
fi

echo "==> resize → $RESIZE_TO"
"$AIOS_VD" resize "$DEV_ID" --size "$RESIZE_TO"

echo "==> info"
"$AIOS_VD" info "$DEV_ID"

echo "==> unmap"
"$AIOS_VD" unmap "$DEV_ID"
DEV_ID=""

echo "OK: aios_vd_stress completed for $POOL/$NAME"
