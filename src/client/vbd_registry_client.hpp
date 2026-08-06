#pragma once

#include "client/session.hpp"
#include "http/vbd_registry.hpp"

#include <string>

namespace aios {

// Session-based registry updates for aios-vd (same CAS document as VbdRegistryStore).
bool vbd_registry_upsert(Session& session, VbdVolume v, std::string& err);
bool vbd_registry_rename(Session& session, const std::string& old_pool,
                         const std::string& old_name, VbdVolume neu, std::string& err);

}  // namespace aios
