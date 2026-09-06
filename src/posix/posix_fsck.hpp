#pragma once

// Consistency check and repair for one libaios_posix volume, working directly
// on the object layout (posix/{vol}/super, ino/, dir/, data/). Nothing here
// needs a mount; every repair is a CAS-guarded write or a delete of an object
// that no reachable inode refers to, so a mount that is active at the same time
// can at worst make a finding obsolete, never lose data. Objects younger than
// FsckOptions::min_age are reported but never repaired: a create under a
// directory lease publishes the inode before the name, a write publishes its
// chunks before the size, and fsck must not race either.

#include "client/session.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aios::posix {

struct FsckOptions {
  bool repair{false};
  // Findings on objects modified less than this long ago are never repaired.
  std::chrono::seconds min_age{600};
  // Progress / per-finding lines. Optional.
  std::function<void(const std::string&)> log;
};

struct FsckFinding {
  enum class Kind {
    NoSuperblock,        // posix/{vol}/super missing or unreadable
    NoRoot,              // inode 1 missing
    DanglingDentry,      // directory names an inode that does not exist
    DirLinkedTwice,      // a directory reachable under two names (or a cycle)
    ParentMismatch,      // child directory's parent_ino is not the directory naming it
    NlinkMismatch,       // nlink differs from the number of names (files) / 2 + subdirs (dirs)
    OrphanInode,         // inode object not reachable from the root
    OrphanChunk,         // data chunk of an inode that does not exist
    StrayChunk,          // data chunk past the end of its file
    StaleDirObjects,     // dir/{ino}/* for an inode that is missing or not a directory
    LogGarbage,          // directory log longer than the committed log_bytes
    NextInoBehind,       // superblock next_ino <= an existing inode number
  };
  Kind kind{};
  std::string subject;  // oid or path
  std::string detail;
  bool repairable{true};
  bool repaired{false};
  bool skipped_young{false};
};

struct FsckReport {
  std::vector<FsckFinding> findings;
  std::uint64_t dirs{0};
  std::uint64_t files{0};
  std::uint64_t symlinks{0};
  std::uint64_t inode_objects{0};
  std::uint64_t chunk_objects{0};
  std::uint64_t bytes{0};
  std::size_t repaired{0};
  bool fatal{false};  // superblock / root unusable, tree walk did not run

  bool clean() const { return findings.empty(); }
  std::size_t unrepaired() const;
};

const char* fsck_kind_name(FsckFinding::Kind k);

FsckReport fsck_volume(Session& session, const std::string& volume, const FsckOptions& opt);

}  // namespace aios::posix
