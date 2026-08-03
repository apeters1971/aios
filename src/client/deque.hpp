#pragma once

#include "client/changelog.hpp"
#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aios {

class deque : public detail::StlBase {
 public:
  deque(Session& session, std::string name, sync_mode mode = sync_mode::async,
        bool flush_on_destroy = true);
  ~deque();

  deque(const deque&) = delete;
  deque& operator=(const deque&) = delete;

  void load();
  void flush();
  void compact();

  void clear();
  std::size_t size() const;
  bool empty() const { return size() == 0; }

  void push_back(const std::string& v);
  void push_front(const std::string& v);
  void pop_back();
  void pop_front();

  // Non-const access marks dirty (ASYNC) or requires set_at for SYNC persist.
  std::string& operator[](std::size_t i);
  const std::string& operator[](std::size_t i) const;
  std::string at(std::size_t i) const;
  void set_at(std::size_t i, const std::string& v);

  void insert(std::size_t index, const std::string& v);
  void erase(std::size_t index);

  std::vector<std::string> snapshot() const;

 private:
  void ensure_fresh_read() const;
  void pull();
  void persist_op(changelog::Op op, std::vector<std::string> args);
  void maybe_compact();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  mutable std::vector<std::string> local_;
  mutable bool local_valid_{false};
};

}  // namespace aios
