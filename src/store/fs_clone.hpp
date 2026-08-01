#pragma once

#include <string>

namespace aios {

// Reflink/clone src -> dst (must not exist). Uses FICLONE (Linux) or clonefile (macOS).
// Returns false with err on failure. Never silently full-copies.
bool clone_file(const std::string& src, const std::string& dst, std::string& err);

// True if this build/OS exposes a clone syscall we can attempt.
bool clone_file_supported();

// Full byte copy fallback (only for tests when clone_required=false).
bool copy_file_full(const std::string& src, const std::string& dst, std::string& err);

// Clone if possible; if clone fails and allow_copy, fall back to full copy.
bool clone_or_copy_file(const std::string& src, const std::string& dst, bool allow_copy,
                        std::string& err);

}  // namespace aios
