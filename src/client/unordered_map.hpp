#pragma once

#include "client/mode.hpp"
#include "client/session.hpp"
#include "client/stl_base.hpp"

#include <string>
#include <unordered_map>

namespace aios {

class unordered_map : public detail::StlBase {
 public:
  using key_type = std::string;
  using mapped_type = std::string;
  using value_type = std::pair<const std::string, std::string>;

  class reference {
   public:
    reference(unordered_map& m, std::string key) : m_(&m), key_(std::move(key)) {}
    reference& operator=(const std::string& v) {
      m_->set(key_, v);
      return *this;
    }
    operator std::string() const { return m_->at(key_); }

   private:
    unordered_map* m_;
    std::string key_;
  };

  unordered_map(Session& session, std::string name, sync_mode mode = sync_mode::async,
                bool flush_on_destroy = true);
  ~unordered_map();

  void load();
  void flush();

  void clear();
  std::size_t size() const;
  bool empty() const { return size() == 0; }
  bool contains(const std::string& key) const;

  std::string at(const std::string& key) const;
  reference operator[](const std::string& key);

  void insert_or_assign(const std::string& key, const std::string& value);
  void set(const std::string& key, const std::string& value) { insert_or_assign(key, value); }
  std::size_t erase(const std::string& key);

  std::unordered_map<std::string, std::string> snapshot() const;

 private:
  void ensure_fresh_read() const;
  void persist_if_sync();

  mutable std::unordered_map<std::string, std::string> local_;
  mutable bool local_valid_{false};
};

}  // namespace aios
