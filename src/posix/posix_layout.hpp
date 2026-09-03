#pragma once

#include "client/put_layout.hpp"
#include "config.hpp"
#include "posix/posix_internal.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aios {
namespace posix {

PutLayout put_layout_from_spec(const PosixLayoutSpec& spec);

// Normalize volume-relative path: leading /, no trailing / (except root).
std::string normalize_fs_path(std::string path);

// Longest matching rule for volume + path; nullopt if none. Returned by value so it
// cannot dangle when the rule set is refreshed underneath the caller.
std::optional<PosixLayoutRule> match_posix_layout_rule(const std::vector<PosixLayoutRule>& rules,
                                                       const std::string& volume,
                                                       const std::string& path);

// Stable key for EXDEV: compare source vs dest placement domains.
std::string layout_domain_key(const std::optional<PosixLayoutRule>& rule);

std::string path_of_ino(FsState& st, uint64_t ino);

// Returns the current rule set; refreshes it first when the cache has expired.
std::shared_ptr<const std::vector<PosixLayoutRule>> refresh_layout_rules(FsState& st);
PutLayout meta_layout_for_path(FsState& st, const std::string& path);
PutLayout data_layout_for_path(FsState& st, const std::string& path);
PutLayout meta_layout_for_ino(FsState& st, uint64_t ino);
PutLayout data_layout_for_ino(FsState& st, uint64_t ino);

bool layout_domains_differ(FsState& st, const std::string& path_a, const std::string& path_b);

// False when no layout rules are configured: every placement query is then the
// default layout and the parent-chain walk of path_of_ino can be skipped.
bool layout_rules_present(FsState& st);
// Path of a not-yet-linked child for rule matching; nullopt when there are no rules.
std::optional<std::string> child_path_for_layout(FsState& st, uint64_t parent, const char* name);

}  // namespace posix
}  // namespace aios
