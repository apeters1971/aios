#pragma once

#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"

#include <set>
#include <string>

namespace aios {

class set : public detail::StlBase {
 public:
  using key_type = std::string;
  using value_type = std::string;

  set(Session& session, std::string name, sync_mode mode = sync_mode::async,
      bool flush_on_destroy = true);
  ~set();

  void load();
  void flush();

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
  void persist_if_sync();

  mutable std::set<std::string> local_;
  mutable bool local_valid_{false};
};

}  // namespace aios
