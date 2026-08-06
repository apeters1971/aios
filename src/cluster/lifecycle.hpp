#pragma once

#include <algorithm>
#include <string>

namespace aios {

// Operator lifecycle for nodes and filesystem targets (independent of gossip
// Online/Suspect/Offline).
enum class LifecycleState { Up, Drain, Off };

inline const char* lifecycle_state_name(LifecycleState s) {
  switch (s) {
    case LifecycleState::Up:
      return "up";
    case LifecycleState::Drain:
      return "drain";
    case LifecycleState::Off:
      return "off";
  }
  return "up";
}

inline LifecycleState lifecycle_state_from_string(const std::string& s) {
  if (s == "drain") return LifecycleState::Drain;
  if (s == "off") return LifecycleState::Off;
  return LifecycleState::Up;
}

inline bool valid_lifecycle_state_string(const std::string& s) {
  return s == "up" || s == "drain" || s == "off";
}

// Worse of two states: off > drain > up.
inline LifecycleState worse_lifecycle(LifecycleState a, LifecycleState b) {
  const auto rank = [](LifecycleState s) {
    switch (s) {
      case LifecycleState::Off:
        return 2;
      case LifecycleState::Drain:
        return 1;
      case LifecycleState::Up:
        return 0;
    }
    return 0;
  };
  return rank(a) >= rank(b) ? a : b;
}

}  // namespace aios
