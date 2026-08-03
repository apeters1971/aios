#pragma once

namespace aios {

enum class sync_mode {
  sync,   // every mutate persists immediately; reads fetch tip
  async,  // local buffer; load()/flush() are atomic snapshots
};

}  // namespace aios
