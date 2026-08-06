#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aios {

// Admin default layout for oids under `prefix` (longest match wins).
struct LayoutRule {
  std::string prefix;
  std::string layout;  // required: "replica" | "ec"
  std::optional<std::string> storage_class;
  std::optional<int> ec_k;
  std::optional<int> ec_m;
  std::optional<std::string> ec_codec;
};

// Background tip migration between storage classes (prefix match).
struct TransitionRule {
  std::string prefix;
  std::string from;  // source storage_class
  std::string to;    // destination storage_class
  std::optional<std::string> layout;  // optional layout change on transition
  std::optional<int> ec_k;
  std::optional<int> ec_m;
  std::optional<std::string> ec_codec;
};

// Snapshot a POSIX volume or VBD, then pack+drain the immutable tree (see proto/backup.md).
struct BackupRule {
  std::string kind;                 // "posix" | "vbd"
  std::string volume;               // posix volume name
  std::string pool;                 // vbd pool
  std::string name;                 // vbd volume name
  int retain_snaps{3};              // local posix .snap trees to keep (-1 = keep all)
  std::string from;                 // storage_class of snap tips (default: default_storage_class)
  std::string staging_class{"archive"};
  std::uint64_t max_bag_bytes{0};   // 0 = unlimited
  int max_members{0};
  std::string tape_sink;
  std::string tape_root;
  std::string tape_uri_prefix;
  std::string tape_bin;
  std::string tape_s3_endpoint;
  std::string tape_put_cmd;
  std::string tape_get_cmd;
  std::string bag_compression{"none"};  // none | zstd
  int bag_compression_level{0};         // 0 = use Config.compression_level
  std::string bag_encryption{"none"};   // none | aes-256-gcm
};

// Per-request layout knobs for POSIX meta (ino/dir) vs data (chunks) under a path prefix.
struct PosixLayoutSpec {
  std::string layout;  // empty = omit (cluster default); "replica" | "ec"
  std::optional<std::string> storage_class;
  std::optional<int> ec_k;
  std::optional<int> ec_m;
  std::optional<std::string> ec_codec;
};

// Longest-match volume-relative path prefix (default configure "/").
// Rename across rules that differ in meta/data placement returns EXDEV (copy instead).
struct PosixLayoutRule {
  std::string path{"/"};
  std::optional<std::string> volume;  // omit = all volumes
  PosixLayoutSpec meta;
  PosixLayoutSpec data;
};

// Pack many small tips into large bag objects, then stub members (cold/tape path).
struct ArchiveRule {
  std::string prefix;
  std::string from;                 // source storage_class of candidates
  std::string staging_class{"archive"};  // class for bag bodies
  int min_age_days{0};
  std::uint64_t min_bag_bytes{64ull * 1024ull * 1024ull * 1024ull};  // 64 GiB
  std::uint64_t max_bag_bytes{256ull * 1024ull * 1024ull * 1024ull}; // 256 GiB
  int max_members{0};               // 0 = unlimited
  int max_open_ms{-1};              // 0 = seal undersized bags each tick; -1 = wait for min
  // empty/"none" | external (fs/cmd) | s3 (aws s3 cp) | xrdcp
  std::string tape_sink;
  std::string tape_root;            // fs sink dest, or local scratch for s3/xrdcp/cmd
  std::string tape_uri_prefix;      // s3://bucket/prefix/ or root://host//path/ (s3|xrdcp)
  std::string tape_bin;             // override binary (default aws / xrdcp)
  std::string tape_s3_endpoint;     // optional --endpoint-url for aws s3 cp
  std::string tape_put_cmd;         // external only: exec <bag_oid> <local_path> → stdout URI
  std::string tape_get_cmd;         // external only: exec <uri> <local_path>
  std::string bag_compression{"none"};  // none | zstd
  int bag_compression_level{0};         // 0 = use Config.compression_level
  std::string bag_encryption{"none"};   // none | aes-256-gcm
};

struct Config {
  std::string node_id;
  std::string listen{"0.0.0.0:7400"};
  std::vector<std::string> peers;
  // Shared cluster secret (UUID recommended). Required for Hello/Gossip HMAC.
  std::string cluster_key;
  int auth_skew_ms{60000};  // reject |now-ts| larger than this
  int gossip_interval_ms{1000};
  int suspect_after_ms{5000};
  int dead_after_ms{15000};
  int scan_interval_ms{5000};
  std::string status_file;
  // Server-side replication: primary fans out to replica_count targets.
  int replica_count{3};
  // Successful copies required for Put/Del ACK (including primary). 0 => replica_count.
  int write_quorum{0};
  // Default layout when a PUT omits x-aios-layout ("replica" or "ec").
  std::string durability{"replica"};
  // Default storage class for new writes when not overridden.
  std::string default_storage_class{"nvme"};
  // Consistent-hash virtual nodes.
  int vnodes_per_target{128};
  int min_vnodes{16};
  int max_vnodes{1024};
  // Default EC parameters when layout is ec (request may override).
  int ec_k{2};
  int ec_m{1};
  // EC codec: "" (auto: xor if m==1 else isal), "xor", or "isal".
  std::string ec_codec;
  // Safety caps for per-request layout (0 = no cap).
  int max_ec_k{16};
  int max_ec_m{16};
  int max_replica_count{64};
  // Prefix → layout / storage_class defaults (longest matching prefix).
  std::vector<LayoutRule> layout_rules;
  // POSIX path-prefix → separate meta/data placement (longest match). Seed for live store.
  std::vector<PosixLayoutRule> posix_layout_rules;
  // Prefix → class transition policies.
  std::vector<TransitionRule> transition_rules;
  // Prefix → archive (pack-to-bag) policies.
  std::vector<ArchiveRule> archive_rules;
  // Snapshot + archive copy-out policies (POSIX / VBD).
  std::vector<BackupRule> backup_rules;
  int repair_interval_ms{30000};
  // Max oids scanned per local store each repair tick.
  int repair_batch_oids{256};
  int transition_interval_ms{30000};
  int transition_batch_oids{64};
  int archive_interval_ms{30000};
  int archive_batch_oids{64};
  int backup_interval_ms{3600000};
  int backup_batch_oids{256};
  // HTTP object API listen address; empty disables HTTP front-end.
  std::string http_listen{"0.0.0.0:7480"};
  // S3-compatible API listen address; empty disables. Uses libaios_posix on s3_volume.
  std::string s3_listen;
  // POSIX volume backing S3 buckets (top-level dirs). Default "s3".
  std::string s3_volume{"s3"};
  // AWS SigV4 access key id; secret is always cluster_key.
  std::string s3_access_key{"aios"};
  // cuObjServer RDMA listen (IP:port). Empty disables S3 GPUDirect offload.
  std::string cuobject_listen;
  // Body durability for ranged FS puts: none | data | full (informational; store fsyncs).
  std::string http_body_sync{"data"};
  // Retain newest N object versions per oid (default 16).
  int max_versions{16};
  // If true, FS COW requires reflink/clonefile; if false, allow full-copy fallback.
  bool clone_required{true};
  // Max object body size for HTTP streaming PUT (bytes). Default 64 GiB.
  std::uint64_t max_object_bytes{64ull * 1024ull * 1024ull * 1024ull};
  // When true, expose /admin/* and /metrics on the HTTP listener.
  bool admin{false};
  // When true (and admin), GET /metrics skips HMAC so Prometheus can scrape.
  bool admin_metrics_public{false};
  // Operator lifecycle for this node (up | drain | off). Folded into local target states.
  std::string node_state{"up"};
  // When true, advertise placement weights from free space (TiB), with hysteresis.
  // When false: explicit .aios weight, or total capacity (TiB) if weight omitted.
  bool weight_autotune{false};
  // Autotune applies a new weight only if |Δ| >= max(min_delta, ceil(cur*pct/100)).
  int weight_autotune_threshold_pct{20};
  int weight_autotune_min_delta{1};
  // Whole-object PUT compression: "none" | "zstd" (range/append rejected on compressed tips).
  std::string compression{"none"};
  int compression_level{3};                 // zstd level 1..22
  std::uint64_t compression_min_bytes{256}; // skip smaller objects
  // 64 hex chars (AES-256) for archive bag encryption; empty disables encrypting seals.
  std::string bag_encryption_key;
};

// Build dialable http host:port from TCP advertise + http_listen.
std::string derive_http_addr(const std::string& advertise_tcp, const std::string& http_listen);

// Load YAML config file (optional). Returns false on parse failure.
bool load_config_file(const std::string& path, Config& cfg, std::string& err);

// Apply argv overrides. Returns false on bad args / --help.
bool parse_cli(int argc, char** argv, Config& cfg, std::string& err, bool& help);

// Validate durability/EC knobs and derive replica_count for EC profiles.
bool normalize_config(Config& cfg, std::string& err);

// Split "host:port" into host and port. IPv6 not supported in v0 (host may not contain ':').
bool split_host_port(const std::string& addr, std::string& host, std::string& port);

}  // namespace aios
