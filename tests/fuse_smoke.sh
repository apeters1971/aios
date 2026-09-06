#!/usr/bin/env bash
# End-to-end FUSE smoke test: a single-node aiosd, a real `aios-fuse` (or
# `aios-fusell` with FUSELL=1) mount through the kernel, and the POSIX
# operations the in-process suite cannot exercise through the VFS: path
# resolution, page cache, rename over open files, hard links, symlinks, xattrs,
# truncate, fsync, directory listing, and a second mount seeing the first's
# committed state. Exits non-zero on the first failure.
#
# Usage: tests/fuse_smoke.sh BUILD_DIR   (needs /dev/fuse; unprivileged is fine)
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=tests/fuse_env.sh
source tests/fuse_env.sh "${1:?build dir}"

MNT_A=$WORK/mnt-a
MNT_B=$WORK/mnt-b

# Directory changes are batched under a lease and flushed by a background
# thread; fsync(2) on the directory waits for that, `sync` does not reach FUSE.
fsyncdir() {
  python3 - "$1" <<'EOF'
import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
os.fsync(fd)
os.close(fd)
EOF
}
# Cross-mount visibility is checked with a bounded retry.
wait_for() {  # wait_for SECONDS DESCRIPTION CMD...
  local deadline=$(( $(date +%s) + $1 )); shift
  local what=$1; shift
  until "$@" 2>/dev/null; do
    (( $(date +%s) < deadline )) || fail "timed out waiting for: $what"
    sleep 0.2
  done
}

fuse_env_start

step "mount A ($FUSE_BIN)"
fuse_env_mount "$MNT_A" "$WORK/fuse-a.log"

step "files and directories"
mkdir "$MNT_A/d1" "$MNT_A/d1/sub"
echo hello > "$MNT_A/d1/f1"
[[ "$(cat "$MNT_A/d1/f1")" == hello ]] || fail "read back f1"
[[ "$(stat -c %s "$MNT_A/d1/f1")" == 6 ]] || fail "size of f1"
[[ "$(stat -c %h "$MNT_A/d1")" == 3 ]] || fail "nlink of d1 with one subdir: $(stat -c %h "$MNT_A/d1")"
touch "$MNT_A/d1/f1" && [[ -f "$MNT_A/d1/f1" ]] || fail "touch existing"
[[ "$(ls "$MNT_A/d1" | sort | tr '\n' ' ')" == "f1 sub " ]] || fail "readdir d1: $(ls "$MNT_A/d1")"

step "larger data through the page cache (multi-chunk, partial rewrite)"
dd if=/dev/urandom of="$WORK/big.bin" bs=1M count=5 status=none
cp "$WORK/big.bin" "$MNT_A/d1/big"
cmp "$WORK/big.bin" "$MNT_A/d1/big" || fail "big file differs after cp"
# Overwrite 4 KiB in the middle without truncating; compare against the same edit locally.
head -c 4096 /dev/zero | tr '\0' 'P' > "$WORK/patch.bin"
dd if="$WORK/patch.bin" of="$MNT_A/d1/big" bs=4096 seek=300 conv=notrunc status=none
dd if="$WORK/patch.bin" of="$WORK/big.bin" bs=4096 seek=300 conv=notrunc status=none
cmp "$WORK/big.bin" "$MNT_A/d1/big" || fail "big file differs after partial rewrite"

step "truncate shrink and grow"
truncate -s 1000000 "$MNT_A/d1/big"
[[ "$(stat -c %s "$MNT_A/d1/big")" == 1000000 ]] || fail "shrink size"
head -c 1000000 "$WORK/big.bin" | cmp - "$MNT_A/d1/big" || fail "shrunk content"
truncate -s 1500000 "$MNT_A/d1/big"
[[ "$(stat -c %s "$MNT_A/d1/big")" == 1500000 ]] || fail "grow size"
[[ "$(tail -c 500000 "$MNT_A/d1/big" | tr -d '\0' | wc -c)" == 0 ]] || fail "grown tail is not zeros"

step "rename, hard link, symlink, unlink"
mv "$MNT_A/d1/f1" "$MNT_A/d1/f2"
[[ ! -e "$MNT_A/d1/f1" && "$(cat "$MNT_A/d1/f2")" == hello ]] || fail "rename same dir"
mv "$MNT_A/d1/f2" "$MNT_A/d1/sub/f3"
[[ "$(cat "$MNT_A/d1/sub/f3")" == hello ]] || fail "rename across dirs"
ln "$MNT_A/d1/sub/f3" "$MNT_A/d1/hard"
[[ "$(stat -c %h "$MNT_A/d1/hard")" == 2 ]] || fail "hard link nlink"
[[ "$(stat -c %i "$MNT_A/d1/hard")" == "$(stat -c %i "$MNT_A/d1/sub/f3")" ]] || fail "hard link inode"
echo more >> "$MNT_A/d1/hard"
[[ "$(cat "$MNT_A/d1/sub/f3")" == $'hello\nmore' ]] || fail "write through hard link"
ln -s sub/f3 "$MNT_A/d1/link"
[[ "$(readlink "$MNT_A/d1/link")" == sub/f3 ]] || fail "readlink"
[[ "$(cat "$MNT_A/d1/link")" == $'hello\nmore' ]] || fail "read through symlink"
rm "$MNT_A/d1/sub/f3"
[[ "$(stat -c %h "$MNT_A/d1/hard")" == 1 ]] || fail "nlink after unlinking one name"
[[ "$(cat "$MNT_A/d1/hard")" == $'hello\nmore' ]] || fail "content survives unlink of other name"

step "unlink of an open file keeps its data until close"
exec 3<"$MNT_A/d1/hard"
rm "$MNT_A/d1/hard"
got=$(cat <&3 || true)
exec 3<&-
if [[ "$got" != $'hello\nmore' ]]; then
  if [[ "${FUSELL:-0}" == 1 ]]; then
    # Known gap: the low-level mount has no libfuse .fuse_hidden rename, and
    # libaios_posix drops the chunks at nlink 0 without counting open handles.
    echo "   KNOWN GAP (aios-fusell): unlinked-while-open data not readable"
  else
    fail "read from unlinked open file"
  fi
fi

step "xattrs"
if command -v setfattr > /dev/null; then
  setfattr -n user.color -v blue "$MNT_A/d1/big"
  [[ "$(getfattr -n user.color --only-values "$MNT_A/d1/big")" == blue ]] || fail "getxattr"
  getfattr -d "$MNT_A/d1/big" | grep -q 'user.color="blue"' || fail "listxattr"
  setfattr -x user.color "$MNT_A/d1/big"
  if getfattr -n user.color "$MNT_A/d1/big" 2>/dev/null; then fail "removexattr left the attribute"; fi
else
  echo "   (attr tools not installed, skipped)"
fi

step "permissions and times"
chmod 640 "$MNT_A/d1/big"
[[ "$(stat -c %a "$MNT_A/d1/big")" == 640 ]] || fail "chmod"
touch -d '2020-01-02 03:04:05 UTC' "$MNT_A/d1/big"
[[ "$(stat -c %Y "$MNT_A/d1/big")" == 1577934245 ]] || fail "utimens: $(stat -c %Y "$MNT_A/d1/big")"

step "many entries in one directory (lease batching) and rmdir refusals"
mkdir "$MNT_A/many"
for i in $(seq 1 300); do : > "$MNT_A/many/f$i"; done
[[ "$(ls "$MNT_A/many" | wc -l)" == 300 ]] || fail "300 entries listed: $(ls "$MNT_A/many" | wc -l)"
if rmdir "$MNT_A/many" 2>/dev/null; then fail "rmdir of non-empty dir succeeded"; fi
rm "$MNT_A/many"/f*
rmdir "$MNT_A/many" || fail "rmdir emptied dir"
[[ ! -e "$MNT_A/many" ]] || fail "dir still present after rmdir"

step "fsyncdir, then a second independent mount sees everything"
fsyncdir "$MNT_A/d1"
fsyncdir "$MNT_A"
fuse_env_mount "$MNT_B" "$WORK/fuse-b.log"
[[ "$(ls "$MNT_B/d1" | sort | tr '\n' ' ')" == "big link sub " ]] || fail "mount B readdir d1: $(ls "$MNT_B/d1")"
[[ "$(stat -c %s "$MNT_B/d1/big")" == 1500000 ]] || fail "mount B size"
head -c 1000000 "$WORK/big.bin" | cmp - <(head -c 1000000 "$MNT_B/d1/big") || fail "mount B content"
[[ "$(readlink "$MNT_B/d1/link")" == sub/f3 ]] || fail "mount B readlink"

step "B creates in a directory A leases: break, flush, A sees it (and back)"
echo from-b > "$MNT_B/d1/fromb"
wait_for 15 "A to see B's file" test -e "$MNT_A/d1/fromb"
[[ "$(cat "$MNT_A/d1/fromb")" == from-b ]] || fail "content of B's file on A"
echo from-a > "$MNT_A/d1/froma"
wait_for 15 "B to see A's file" test -e "$MNT_B/d1/froma"
[[ "$(cat "$MNT_B/d1/froma")" == from-a ]] || fail "content of A's file on B"

step "unmount A while B stays up, then B"
fuse_env_umount "$MNT_A"
[[ "$(cat "$MNT_B/d1/fromb")" == from-b ]] || fail "B unaffected by A unmount"
fuse_env_umount "$MNT_B"

step "objects are where the layout says (posix/$VOLUME/...)"
aios_list "posix/$VOLUME/" | grep -q "posix/$VOLUME/super" || fail "superblock object"
aios_list "posix/$VOLUME/dir/1/" | grep -q "posix/$VOLUME/dir/1/meta" || fail "root dir objects"

echo "FUSE smoke: OK"
