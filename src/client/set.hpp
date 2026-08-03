#pragma once

#include "client/changelog.hpp"
#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aios {

class set : public detail::StlBase {
 public:
  using key_type = std::string;
  using value_type = std::string;

  set(Session& session, std::string name, sync_mode mode = sync_mode::async,
      bool flush_on_destroy = true);
  ~set();

  set(const set&) = delete;
  set& operator=(const set&) = delete;

  void load();
  void flush();
  void compact();

  void clear();
  std::size_t size() const;
  bool empty() const { return size() == 0; }
  bool contains(const std::string& key) const;

  // Returns true if inserted.
  bool insert(const std::string& key);
  std::size_t erase(const std::string& key);

  std::set<std::string> snapshot() const;

 private:
  void ensure_fresh_read() const;
  void pull();
  void persist_op(changelog::Op op, std::vector<std::string> args);
  void maybe_compact();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  mutable std::set<std::string> local_;
  mutable bool local_valid_{false};
};

}  // namespace aios
