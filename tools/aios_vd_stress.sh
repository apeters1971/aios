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
  if [[ -n "${CLONE_ID:-}" ]]; then
    "$AIOS_VD" unmap "$CLONE_ID" 2>/dev/null || true
  fi
  if [[ -n "${DEV_ID:-}" ]]; then
    "$AIOS_VD" unmap "$DEV_ID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

dev_id_of() {
  echo "$1" | sed -n 's|.*/aiosvd\([0-9][0-9]*\).*|\1|p' | head -1
}

# Read LEN bytes at OFF from DEV and fail unless they are all zero.
assert_zero() {
  local dev="$1" off="$2" len="$3" what="$4"
  if ! cmp -n "$len" <(dd if="$dev" bs=1M iflag=skip_bytes,count_bytes skip="$off" count="$len" status=none) /dev/zero \
      >/dev/null 2>&1; then
    echo "FAIL: $what: $dev @$off+$len is not zero" >&2
    exit 1
  fi
}

# Read LEN bytes at OFF from two devices and fail unless they are identical.
assert_same() {
  local a="$1" b="$2" off="$3" len="$4" what="$5"
  if ! cmp -n "$len" <(dd if="$a" bs=1M iflag=skip_bytes,count_bytes skip="$off" count="$len" status=none) \
      <(dd if="$b" bs=1M iflag=skip_bytes,count_bytes skip="$off" count="$len" status=none) >/dev/null 2>&1; then
    echo "FAIL: $what: $a and $b differ @$off+$len" >&2
    exit 1
  fi
}

echo "==> map $POOL/$NAME size=$SIZE"
MAP_OUT=$("$AIOS_VD" map --endpoint "$ENDPOINT" --key "$KEY" --pool "$POOL" --name "$NAME" \
  --size "$SIZE" --create --excl)
echo "$MAP_OUT"
DEV_ID=$(dev_id_of "$MAP_OUT")
DEV="/dev/aiosvd${DEV_ID}"
if [[ -z "$DEV_ID" || ! -b "$DEV" ]]; then
  echo "failed to resolve device from: $MAP_OUT" >&2
  exit 1
fi

echo "==> fio randrw on $DEV (${FIO_RUNTIME}s)"
fio --name=aiosvd_stress --filename="$DEV" --rw=randrw --bs=4k --iodepth=16 \
  --runtime="$FIO_RUNTIME" --time_based --direct=1 --ioengine=libaio \
  --size=100% --group_reporting

# COW clone semantics: zeroing / discarding on a child must never expose the
# parent's data again (the child used to DELETE its object and fall through).
echo "==> clone → $POOL/${NAME}-clone and zero read-back check"
# Object 0 and 1 of the parent get known non-zero content first.
OBJ=$((4 * 1024 * 1024))
dd if=/dev/urandom of="$DEV" bs=1M count=8 conv=fsync status=none
CLONE_OUT=$("$AIOS_VD" clone "$DEV_ID" --endpoint "$ENDPOINT" --key "$KEY" \
  --pool "$POOL" --name "${NAME}-clone")
echo "$CLONE_OUT"
CLONE_ID=$(dev_id_of "$CLONE_OUT")
CDEV="/dev/aiosvd${CLONE_ID}"
if [[ -z "$CLONE_ID" || ! -b "$CDEV" ]]; then
  echo "failed to resolve clone device from: $CLONE_OUT" >&2
  exit 1
fi
assert_same "$DEV" "$CDEV" 0 $((2 * OBJ)) "fresh clone reads parent data"
# 1. explicit zero write covering a whole object
dd if=/dev/zero of="$CDEV" bs=1M count=4 conv=fsync status=none
assert_zero "$CDEV" 0 "$OBJ" "clone zero-write (full object)"
# 2. partial zero write inside the second object
dd if=/dev/zero of="$CDEV" bs=4k seek=$((OBJ / 4096 + 3)) count=16 conv=fsync status=none
assert_zero "$CDEV" $((OBJ + 3 * 4096)) $((16 * 4096)) "clone zero-write (partial)"
# 3. discard / write-zeroes on the clone
if command -v blkdiscard >/dev/null 2>&1; then
  blkdiscard -o "$OBJ" -l "$OBJ" "$CDEV" || blkdiscard -f -o "$OBJ" -l "$OBJ" "$CDEV" || true
  blkdiscard -z -o 0 -l "$OBJ" "$CDEV" 2>/dev/null || true
  assert_zero "$CDEV" "$OBJ" "$OBJ" "clone discard (full object)"
fi
# The parent is untouched by all of the above.
if cmp -n "$OBJ" <(dd if="$DEV" bs=1M iflag=count_bytes count="$OBJ" status=none) /dev/zero >/dev/null 2>&1; then
  echo "FAIL: parent object 0 became zero after clone writes" >&2
  exit 1
fi
# Drop caches and re-read so the check covers the stored objects, not the page cache.
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
blockdev --flushbufs "$CDEV" 2>/dev/null || true
assert_zero "$CDEV" 0 "$OBJ" "clone zero-write (full object, re-read)"
assert_zero "$CDEV" $((OBJ + 3 * 4096)) $((16 * 4096)) "clone zero-write (partial, re-read)"

echo "==> unmap clone"
"$AIOS_VD" unmap "$CLONE_ID"
CLONE_ID=""

echo "==> discard (blkdiscard)"
if command -v blkdiscard >/dev/null 2>&1; then
  blkdiscard -f "$DEV" || blkdiscard "$DEV" || true
else
  echo "blkdiscard not found; skipping discard step"
fi

echo "==> resize → $RESIZE_TO"
"$AIOS_VD" resize "$DEV_ID" --endpoint "$ENDPOINT" --key "$KEY" --size "$RESIZE_TO"

echo "==> info"
"$AIOS_VD" info "$DEV_ID"

echo "==> unmap"
"$AIOS_VD" unmap "$DEV_ID"
DEV_ID=""

echo "OK: aios_vd_stress completed for $POOL/$NAME"
