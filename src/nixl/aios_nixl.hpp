#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios {

// cuObjServer accepts one RDMA transfer up to 1 GiB and no byte range.
// Kept equal to kCuObjectMaxTransferBytes (asserted in tests).
inline constexpr std::size_t kNixlMaxRdmaBytes = 1ull << 30;

// Numeric values match nixl_status_t so the plugin can cast.
enum class NixlStatus : int {
  InProg = 1,
  Success = 0,
  NotPosted = -1,
  InvalidParam = -2,
  Backend = -3,
  NotFound = -4,
  Mismatch = -5,
  NotAllowed = -6,
  RepostActive = -7,
  Unknown = -8,
  NotSupported = -9,
  RemoteDisconnect = -10,
  Canceled = -11,
};

enum class NixlMem { Dram, Vram, Obj };
enum class NixlOp { Read, Write };

struct NixlConfig {
  std::string s3_endpoint;
  std::string access_key;
  std::string secret_key;
  std::string region{"us-east-1"};
  // Used when a DRAM/VRAM registration has an empty metaInfo token.
  std::string rdma_token;
  int socket_timeout_ms{30000};
};

struct NixlReg {
  NixlMem mem{NixlMem::Dram};
  std::uint64_t addr{0};
  std::size_t len{0};
  std::uint64_t dev_id{0};
  // DRAM/VRAM: cuObject RDMA token. OBJ: "bucket/key".
  std::string meta;
};

struct NixlMemInfo {
  NixlMem mem{NixlMem::Dram};
  std::uint64_t addr{0};
  std::size_t len{0};
  std::uint64_t dev_id{0};
  std::string obj_key;
  std::string rdma_token;
};

struct NixlXferDesc {
  std::uint64_t addr{0};
  std::size_t len{0};
  std::uint64_t dev_id{0};
  const NixlMemInfo* info{nullptr};
};

// One whole-object cuObject transfer. READ pulls the object into the buffer
// named by the token; WRITE pushes that buffer into the object.
class NixlRdmaTransport {
 public:
  virtual ~NixlRdmaTransport() = default;
  virtual bool read_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                           std::string& err) = 0;
  virtual bool write_object(const std::string& key, std::size_t len, const std::string& rdma_token,
                            std::string& err) = 0;
};

struct S3RdmaRequest {
  std::string method;
  std::string target;
  std::string canonical_uri;
  std::string host;
  std::string port;
  std::unordered_map<std::string, std::string> headers;
  std::string payload_hash;
  std::string error;
};

struct NixlReq;

// Local storage engine. The NIXL plugin is a thin adapter over this type.
// Object identity lives on NixlMemInfo, not on devId.
class NixlEngine {
 public:
  NixlEngine(NixlConfig cfg, std::shared_ptr<NixlRdmaTransport> transport);
  ~NixlEngine();
  NixlEngine(const NixlEngine&) = delete;
  NixlEngine& operator=(const NixlEngine&) = delete;

  bool ok() const { return ok_; }
  const std::string& init_error() const { return init_error_; }

  NixlStatus register_mem(const NixlReg& reg, NixlMemInfo& out, std::string& err) const;

  NixlStatus prep(NixlOp op, const std::vector<NixlXferDesc>& local,
                  const std::vector<NixlXferDesc>& remote, NixlReq*& out, std::string& err);
  NixlStatus post(NixlReq* req);
  NixlStatus check(NixlReq* req);
  // Non-blocking. Returns Backend while an in-flight transfer is still inside
  // the transport; a later call frees the handle and returns Success.
  NixlStatus release(NixlReq*& req);
  std::string last_error(const NixlReq* req) const;

 private:
  NixlConfig cfg_;
  std::shared_ptr<NixlRdmaTransport> transport_;
  bool ok_{false};
  std::string init_error_;
  struct Live;
  std::unique_ptr<Live> live_;
};

std::shared_ptr<NixlRdmaTransport> make_s3_cuobject_transport(const NixlConfig& cfg);

// SigV4 request for an empty-body GET or PUT that carries x-amz-rdma-token.
// `amz_date` is `YYYYMMDDTHHMMSSZ`. canonical_uri is what AIOS S3 verifies:
// the request-target with each path segment encoded.
S3RdmaRequest build_s3_rdma_request(const NixlConfig& cfg, bool is_put, const std::string& bucket,
                                    const std::string& object, std::size_t len,
                                    const std::string& rdma_token, const std::string& amz_date);

}  // namespace aios
