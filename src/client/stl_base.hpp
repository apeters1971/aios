#pragma once

#include "client/error.hpp"
#include "client/mode.hpp"
#include "client/session.hpp"

#include <string>

namespace aios {
namespace detail {

// Shared SYNC/ASYNC persistence for whole-document STL objects.
class StlBase {
 public:
  StlBase(Session& session, std::string type, std::string name, sync_mode mode,
          bool flush_on_destroy = true)
      : session_(&session),
        type_(std::move(type)),
        name_(std::move(name)),
        mode_(mode),
        flush_on_destroy_(flush_on_destroy),
        oid_(Session::stl_oid(type_, name_)) {}

  StlBase(const StlBase&) = delete;
  StlBase& operator=(const StlBase&) = delete;

  sync_mode mode() const { return mode_; }
  const std::string& name() const { return name_; }
  const std::string& oid() const { return oid_; }
  bool dirty() const { return dirty_; }
  std::uint64_t cas() const { return cas_; }
  bool flush_on_destroy() const { return flush_on_destroy_; }
  void set_flush_on_destroy(bool v) { flush_on_destroy_ = v; }

  void set_mode(sync_mode m) {
    if (m == mode_) return;
    if (mode_ == sync_mode::async && dirty_) {
      throw client_error("bad_request", "flush or discard before set_mode(sync)");
    }
    mode_ = m;
  }

  void discard() {
    dirty_ = false;
    // Local state left as-is; caller should load() to refresh.
  }

 protected:
  Session& session() { return *session_; }
  const Session& session() const { return *session_; }

  void mark_dirty() {
    if (mode_ == sync_mode::async) dirty_ = true;
  }

  void clear_dirty() { dirty_ = false; }

  void set_cas(std::uint64_t c) { cas_ = c; }

  std::string put_body(const std::string& body) {
    cas_ = session_->put_object(oid_, body, type_, cas_);
    dirty_ = false;
    return body;
  }

  ObjectSnapshot fetch() { return session_->get_object(oid_); }

 private:
  Session* session_;
  std::string type_;
  std::string name_;
  sync_mode mode_;
  bool flush_on_destroy_{true};
  std::string oid_;
  std::uint64_t cas_{0};
  bool dirty_{false};
};

}  // namespace detail
}  // namespace aios
