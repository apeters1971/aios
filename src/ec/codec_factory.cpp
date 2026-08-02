#include "ec/codec_factory.hpp"

#include "ec/isal_rs.hpp"
#include "ec/xor_parity.hpp"

namespace aios {

bool isal_ec_available() {
#if defined(AIOS_HAVE_ISAL)
  return true;
#else
  return false;
#endif
}

std::unique_ptr<ErasureCodec> make_erasure_codec(int k, int m, const std::string& name,
                                                 std::string& err) {
  if (k < 1 || m < 1) {
    err = "ec_k and ec_m must be >= 1";
    return nullptr;
  }

  std::string chosen = name;
  if (chosen.empty()) {
    chosen = (m == 1) ? "xor" : "isal";
  }

  if (chosen == "xor") {
    if (m != 1) {
      err = "xor codec requires ec_m=1";
      return nullptr;
    }
    return make_xor_parity_codec(k, err);
  }
  if (chosen == "isal" || chosen == "rs") {
    return make_isal_rs_codec(k, m, err);
  }
  err = "unknown ec codec '" + chosen + "' (use xor or isal)";
  return nullptr;
}

}  // namespace aios
