#pragma once

#include "ec/erasure_codec.hpp"

#include <memory>
#include <string>

namespace aios {

// True when built with AIOS_HAVE_ISAL and linked against libisal.
bool isal_ec_available();

// name: "" (auto), "xor", or "isal".
// Auto: m==1 → xor, else → isal (requires ISA-L).
std::unique_ptr<ErasureCodec> make_erasure_codec(int k, int m, const std::string& name,
                                                 std::string& err);

}  // namespace aios
