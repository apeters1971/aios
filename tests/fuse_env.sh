# Shared setup for the FUSE end-to-end scripts (sourced, not executed):
# one single-node aiosd on loopback and helpers to mount `aios-fuse` /
# `aios-fusell` against it. Cleanup (unmount, stop daemon, remove workdir
# unless KEEP=1) runs from an EXIT trap.
#
#   source tests/fuse_env.sh BUILD_DIR
#   fuse_env_start
#   fuse_env_mount "$WORK/mnt" "$WORK/fuse.log" [extra -o keys]

BUILD=${1:?build dir}
FUSE_BIN=${FUSE_BIN:-$BUILD/aios-fuse}
if [[ "${FUSELL:-0}" == 1 ]]; then FUSE_BIN=$BUILD/aios-fusell; fi
AIOSD=$BUILD/aiosd
AIOS=$BUILD/aios

for b in "$AIOSD" "$AIOS" "$FUSE_BIN"; do
  [[ -x "$b" ]] || { echo "missing binary: $b" >&2; exit 2; }
done
[[ -e /dev/fuse ]] || { echo "no /dev/fuse on this host" >&2; exit 2; }

KEY=550e8400-e29b-41d4-a716-446655440000
VOLUME=${VOLUME:-smoke}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aios-fuse.XXXXXX")
mkdir -p "$WORK/disk"
printf 'storage_class: nvme\nweight: 1\nstate: up\n' > "$WORK/disk/.aios"

_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
RPC=$(_port); HTTP=$(_port)
EP=127.0.0.1:$HTTP

_fuse_pids=()
_fuse_mnts=()
_daemon_pid=""
fuse_env_cleanup() {
  set +e
  local i
  for (( i=${#_fuse_mnts[@]}-1; i>=0; i-- )); do
    local m=${_fuse_mnts[$i]}
    if mountpoint -q "$m" 2>/dev/null; then
      fusermount3 -u "$m" 2>/dev/null || fusermount -u "$m" 2>/dev/null || sudo umount "$m" 2>/dev/null
    fi
  done
  for p in ${_fuse_pids[@]+"${_fuse_pids[@]}"}; do kill "$p" 2>/dev/null; done
  sleep 0.5
  if [[ -n "$_daemon_pid" ]]; then kill -TERM "$_daemon_pid" 2>/dev/null; wait "$_daemon_pid" 2>/dev/null; fi
  if [[ "${KEEP:-0}" != 1 ]]; then rm -rf "$WORK"; else echo "kept $WORK"; fi
}
trap fuse_env_cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  echo "--- aiosd log (tail)"; tail -50 "$WORK/aiosd.log" 2>/dev/null
  echo "--- fuse logs"; cat "$WORK"/fuse*.log 2>/dev/null
  exit 1
}
step() { echo "== $*"; }

aios_list() { "$AIOS" --endpoint "$EP" --cluster-key "$KEY" list --prefix "$1"; }

fuse_env_start() {
  step "start aiosd on $EP"
  printf 'gossip_interval_ms: 500\nrepair_interval_ms: 0\n' > "$WORK/aiosd.yaml"
  "$AIOSD" --config "$WORK/aiosd.yaml" --cluster-key "$KEY" --node-id smoke \
    --listen "127.0.0.1:$RPC" --http-listen "$EP" --scan-root "$WORK/disk" \
    --replica-count 1 --write-quorum 1 --no-fsync \
    --status-file "$WORK/status.json" > "$WORK/aiosd.log" 2>&1 &
  _daemon_pid=$!
  local _i
  for _i in $(seq 1 100); do
    if aios_list "$VOLUME/" > /dev/null 2>&1; then return 0; fi
    kill -0 "$_daemon_pid" 2>/dev/null || fail "aiosd exited early"
    sleep 0.1
  done
  fail "aiosd not ready"
}

# fuse_env_mount MNT LOG [extra -o list]
fuse_env_mount() {
  local mnt=$1 log=$2 extra=${3:-}
  mkdir -p "$mnt"
  local opts="endpoint=$EP,cluster_key=$KEY,volume=$VOLUME"
  [[ -n "$extra" ]] && opts="$opts,$extra"
  "$FUSE_BIN" -f -o "$opts" "$mnt" > "$log" 2>&1 &
  local pid=$!
  _fuse_pids+=("$pid")
  _fuse_mnts+=("$mnt")
  local _i
  for _i in $(seq 1 100); do
    if mountpoint -q "$mnt"; then return 0; fi
    kill -0 "$pid" 2>/dev/null || { cat "$log"; fail "fuse process for $mnt exited"; }
    sleep 0.1
  done
  fail "mount of $mnt did not appear"
}

fuse_env_umount() {
  fusermount3 -u "$1" 2>/dev/null || fusermount -u "$1" 2>/dev/null || sudo umount "$1"
  mountpoint -q "$1" && fail "$1 still mounted"
  return 0
}
