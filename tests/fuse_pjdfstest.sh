#!/usr/bin/env bash
# Run pjdfstest (https://github.com/pjd/pjdfstest) against an `aios-fuse`
# mount. pjdfstest must run as root and switches uids, so the mount is made
# with allow_other,default_permissions and the suite runs under sudo.
#
# Usage: tests/fuse_pjdfstest.sh BUILD_DIR PJDFSTEST_DIR [TEST_SUBDIRS...]
#   PJDFSTEST_DIR: a checkout with `pjdfstest` already built (autoreconf -ifs &&
#                  ./configure && make pjdfstest).
#   TEST_SUBDIRS:  default: the groups a network filesystem without device
#                  nodes / chflags is expected to pass.
# Writes the TAP archive to $PJD_OUT (default: $PWD/pjdfstest-results) and exits
# with prove's status.
set -euo pipefail
cd "$(dirname "$0")/.."
PJD=${2:?pjdfstest dir}
shift 2 || true
GROUPS_=("$@")
if [[ ${#GROUPS_[@]} -eq 0 ]]; then
  GROUPS_=(chmod chown ftruncate link mkdir open rename rmdir symlink truncate unlink utimensat)
fi
[[ -x "$PJD/pjdfstest" ]] || { echo "$PJD/pjdfstest not built" >&2; exit 2; }
command -v prove > /dev/null || { echo "prove (perl) missing" >&2; exit 2; }
PJD_OUT=${PJD_OUT:-$PWD/pjdfstest-results}

# shellcheck source=tests/fuse_env.sh
source tests/fuse_env.sh "$1"
VOLUME=pjd
fuse_env_start
MNT=$WORK/mnt
fuse_env_mount "$MNT" "$WORK/fuse.log" "allow_other,default_permissions"

# pjdfstest's `fstest` binary must be reachable from inside the mount cwd.
step "pjdfstest groups: ${GROUPS_[*]}"
mkdir -p "$PJD_OUT"
rc=0
(
  cd "$MNT"
  args=()
  for g in "${GROUPS_[@]}"; do args+=("$PJD/tests/$g"); done
  sudo env PATH="$PATH" prove -r --archive "$PJD_OUT/tap.tgz" "${args[@]}"
) || rc=$?
echo "pjdfstest exit: $rc (archive: $PJD_OUT/tap.tgz)"
exit $rc
