#pragma once

#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"

#include <string>

namespace aios {

class string : public detail::StlBase {
 public:
  string(Session& session, std::string name, sync_mode mode = sync_mode::async,
         bool flush_on_destroy = true);

  ~string();

  void load();
  void flush();

  string& assign(std::string v);
  string& append(const std::string& v);
  string& operator=(std::string v) { return assign(std::move(v)); }
  void clear();

  std::size_t size() const;
  bool empty() const { return size() == 0; }
  const std::string& str() const;
  const char* data() const { return str().c_str(); }
  const char* c_str() const { return str().c_str(); }

  bool operator==(const std::string& o) const { return str() == o; }
  bool operator==(const string& o) const { return str() == o.str(); }

 private:
  void ensure_fresh_read() const;
  void persist_if_sync();

  mutable std::string local_;
  mutable bool local_valid_{false};
};

}  // namespace aios
