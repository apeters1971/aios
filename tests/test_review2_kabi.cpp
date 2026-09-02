// Regression tests for the 2026-09-02 code review (kabi slice).
//
// KRN-8: pins the kernel <-> userspace ABI (kernel/aios_kabi.h,
// kernel/aiosvd_uapi.h) so that any layout drift fails to compile, and checks
// that aios-kbridge frames a reply as header+payload in one buffer (the
// /dev/aios_bridge write path consumes exactly one frame per write(2)).
#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include "../kernel/aios_kabi.h"
#include "../kernel/aiosvd_uapi.h"
#include "../tools/aios_kbridge_wire.hpp"

// ---- kernel/aios_kabi.h ----------------------------------------------------

static_assert(AIOS_KABI_MAGIC == 0x41494F53u);
static_assert(AIOS_KABI_VERSION == 1);
static_assert(AIOS_KABI_NAME_MAX == 255);
static_assert(AIOS_KABI_SYMLINK_MAX == 4095);
static_assert(AIOS_KABI_MAX_PAYLOAD == 1024u * 1024u);
static_assert(AIOS_KABI_XATTR_VALUE_MAX == 64u * 1024u);
static_assert(AIOS_OP_MOUNT == 1 && AIOS_OP_READLINK == 23);

static_assert(sizeof(aios_kabi_stat) == 56);
static_assert(offsetof(aios_kabi_stat, ino) == 0);
static_assert(offsetof(aios_kabi_stat, mode) == 8);
static_assert(offsetof(aios_kabi_stat, nlink) == 12);
static_assert(offsetof(aios_kabi_stat, uid) == 16);
static_assert(offsetof(aios_kabi_stat, gid) == 20);
static_assert(offsetof(aios_kabi_stat, size) == 24);
static_assert(offsetof(aios_kabi_stat, atime_ns) == 32);
static_assert(offsetof(aios_kabi_stat, mtime_ns) == 40);
static_assert(offsetof(aios_kabi_stat, ctime_ns) == 48);
static_assert(sizeof(aios_kabi_dirent) == 272);
static_assert(offsetof(aios_kabi_dirent, ino) == 0);
static_assert(offsetof(aios_kabi_dirent, mode) == 8);
static_assert(offsetof(aios_kabi_dirent, _pad) == 12);
static_assert(offsetof(aios_kabi_dirent, name) == 16);
static_assert(sizeof(aios_kabi_statvfs) == 48);
static_assert(offsetof(aios_kabi_statvfs, blocks) == 0);
static_assert(offsetof(aios_kabi_statvfs, bfree) == 8);
static_assert(offsetof(aios_kabi_statvfs, bavail) == 16);
static_assert(offsetof(aios_kabi_statvfs, files) == 24);
static_assert(offsetof(aios_kabi_statvfs, ffree) == 32);
static_assert(offsetof(aios_kabi_statvfs, bsize) == 40);
static_assert(offsetof(aios_kabi_statvfs, namemax) == 44);
static_assert(sizeof(aios_kabi_req_hdr) == 32);
static_assert(offsetof(aios_kabi_req_hdr, magic) == 0);
static_assert(offsetof(aios_kabi_req_hdr, version) == 4);
static_assert(offsetof(aios_kabi_req_hdr, unique) == 8);
static_assert(offsetof(aios_kabi_req_hdr, opcode) == 16);
static_assert(offsetof(aios_kabi_req_hdr, payload_len) == 20);
static_assert(offsetof(aios_kabi_req_hdr, mount_id) == 24);
static_assert(offsetof(aios_kabi_req_hdr, _pad) == 28);
static_assert(sizeof(aios_kabi_rep_hdr) == 24);
static_assert(offsetof(aios_kabi_rep_hdr, magic) == 0);
static_assert(offsetof(aios_kabi_rep_hdr, version) == 4);
static_assert(offsetof(aios_kabi_rep_hdr, unique) == 8);
static_assert(offsetof(aios_kabi_rep_hdr, result) == 16);
static_assert(offsetof(aios_kabi_rep_hdr, payload_len) == 20);
static_assert(sizeof(aios_kabi_mount_in) == 664);
static_assert(offsetof(aios_kabi_mount_in, endpoint) == 0);
static_assert(offsetof(aios_kabi_mount_in, cluster_key) == 256);
static_assert(offsetof(aios_kabi_mount_in, volume) == 512);
static_assert(offsetof(aios_kabi_mount_in, app_label) == 576);
static_assert(offsetof(aios_kabi_mount_in, stripe_unit) == 640);
static_assert(offsetof(aios_kabi_mount_in, stripe_width) == 648);
static_assert(offsetof(aios_kabi_mount_in, uid) == 652);
static_assert(offsetof(aios_kabi_mount_in, gid) == 656);
static_assert(sizeof(aios_kabi_mount_out) == 64);
static_assert(offsetof(aios_kabi_mount_out, mount_id) == 0);
static_assert(offsetof(aios_kabi_mount_out, _pad) == 4);
static_assert(offsetof(aios_kabi_mount_out, root) == 8);
static_assert(sizeof(aios_kabi_lookup_in) == 264);
static_assert(offsetof(aios_kabi_lookup_in, parent) == 0);
static_assert(offsetof(aios_kabi_lookup_in, name) == 8);
static_assert(sizeof(aios_kabi_ino_in) == 8);
static_assert(offsetof(aios_kabi_ino_in, ino) == 0);
static_assert(sizeof(aios_kabi_readdir_in) == 24);
static_assert(offsetof(aios_kabi_readdir_in, ino) == 0);
static_assert(offsetof(aios_kabi_readdir_in, offset) == 8);
static_assert(offsetof(aios_kabi_readdir_in, max_entries) == 16);
static_assert(offsetof(aios_kabi_readdir_in, _pad) == 20);
static_assert(sizeof(aios_kabi_readdir_out) == 16);
static_assert(offsetof(aios_kabi_readdir_out, next_offset) == 0);
static_assert(offsetof(aios_kabi_readdir_out, count) == 8);
static_assert(offsetof(aios_kabi_readdir_out, _pad) == 12);
static_assert(sizeof(aios_kabi_create_in) == 272);
static_assert(offsetof(aios_kabi_create_in, parent) == 0);
static_assert(offsetof(aios_kabi_create_in, mode) == 8);
static_assert(offsetof(aios_kabi_create_in, _pad) == 12);
static_assert(offsetof(aios_kabi_create_in, name) == 16);
static_assert(sizeof(aios_kabi_unlink_in) == 264);
static_assert(offsetof(aios_kabi_unlink_in, parent) == 0);
static_assert(offsetof(aios_kabi_unlink_in, name) == 8);
static_assert(sizeof(aios_kabi_rename_in) == 536);
static_assert(offsetof(aios_kabi_rename_in, old_parent) == 0);
static_assert(offsetof(aios_kabi_rename_in, new_parent) == 8);
static_assert(offsetof(aios_kabi_rename_in, old_name) == 16);
static_assert(offsetof(aios_kabi_rename_in, new_name) == 272);
static_assert(offsetof(aios_kabi_rename_in, flags) == 528);
static_assert(offsetof(aios_kabi_rename_in, _pad) == 532);
static_assert(sizeof(aios_kabi_symlink_in) == 4360);
static_assert(offsetof(aios_kabi_symlink_in, parent) == 0);
static_assert(offsetof(aios_kabi_symlink_in, name) == 8);
static_assert(offsetof(aios_kabi_symlink_in, target) == 264);
static_assert(sizeof(aios_kabi_readlink_in) == 16);
static_assert(offsetof(aios_kabi_readlink_in, ino) == 0);
static_assert(offsetof(aios_kabi_readlink_in, size) == 8);
static_assert(offsetof(aios_kabi_readlink_in, _pad) == 12);
static_assert(sizeof(aios_kabi_link_in) == 528);
static_assert(offsetof(aios_kabi_link_in, old_parent) == 0);
static_assert(offsetof(aios_kabi_link_in, new_parent) == 8);
static_assert(offsetof(aios_kabi_link_in, old_name) == 16);
static_assert(offsetof(aios_kabi_link_in, new_name) == 272);
static_assert(sizeof(aios_kabi_rw_in) == 24);
static_assert(offsetof(aios_kabi_rw_in, ino) == 0);
static_assert(offsetof(aios_kabi_rw_in, offset) == 8);
static_assert(offsetof(aios_kabi_rw_in, size) == 16);
static_assert(offsetof(aios_kabi_rw_in, _pad) == 20);
static_assert(sizeof(aios_kabi_rw_out) == 8);
static_assert(offsetof(aios_kabi_rw_out, size) == 0);
static_assert(offsetof(aios_kabi_rw_out, _pad) == 4);
static_assert(sizeof(aios_kabi_truncate_in) == 16);
static_assert(offsetof(aios_kabi_truncate_in, ino) == 0);
static_assert(offsetof(aios_kabi_truncate_in, size) == 8);
static_assert(sizeof(aios_kabi_setattr_in) == 72);
static_assert(offsetof(aios_kabi_setattr_in, ino) == 0);
static_assert(offsetof(aios_kabi_setattr_in, to_set) == 8);
static_assert(offsetof(aios_kabi_setattr_in, _pad) == 12);
static_assert(offsetof(aios_kabi_setattr_in, st) == 16);
static_assert(sizeof(aios_kabi_setxattr_in) == 272);
static_assert(offsetof(aios_kabi_setxattr_in, ino) == 0);
static_assert(offsetof(aios_kabi_setxattr_in, flags) == 8);
static_assert(offsetof(aios_kabi_setxattr_in, value_len) == 12);
static_assert(offsetof(aios_kabi_setxattr_in, name) == 16);
static_assert(sizeof(aios_kabi_getxattr_in) == 272);
static_assert(offsetof(aios_kabi_getxattr_in, ino) == 0);
static_assert(offsetof(aios_kabi_getxattr_in, size) == 8);
static_assert(offsetof(aios_kabi_getxattr_in, _pad) == 12);
static_assert(offsetof(aios_kabi_getxattr_in, name) == 16);
static_assert(sizeof(aios_kabi_xattr_out) == 8);
static_assert(offsetof(aios_kabi_xattr_out, size) == 0);
static_assert(offsetof(aios_kabi_xattr_out, _pad) == 4);
static_assert(sizeof(aios_kabi_listxattr_in) == 16);
static_assert(offsetof(aios_kabi_listxattr_in, ino) == 0);
static_assert(offsetof(aios_kabi_listxattr_in, size) == 8);
static_assert(offsetof(aios_kabi_listxattr_in, _pad) == 12);
static_assert(sizeof(aios_kabi_removexattr_in) == 264);
static_assert(offsetof(aios_kabi_removexattr_in, ino) == 0);
static_assert(offsetof(aios_kabi_removexattr_in, name) == 8);

// ---- kernel/aiosvd_uapi.h --------------------------------------------------

static_assert(AIOSVD_NAME_MAX == 63);
static_assert(AIOSVD_POOL_MAX == 63);
static_assert(AIOSVD_KEY_ID_MAX == 63);
static_assert(AIOSVD_MAX_DEVS == 32);
static_assert(AIOSVD_DEFAULT_OBJ_ORDER == 22);

static_assert(sizeof(aiosvd_map_arg) == 800);
static_assert(offsetof(aiosvd_map_arg, endpoint) == 0);
static_assert(offsetof(aiosvd_map_arg, cluster_key) == 256);
static_assert(offsetof(aiosvd_map_arg, pool) == 512);
static_assert(offsetof(aiosvd_map_arg, name) == 576);
static_assert(offsetof(aiosvd_map_arg, app_label) == 640);
static_assert(offsetof(aiosvd_map_arg, key_id) == 704);
static_assert(offsetof(aiosvd_map_arg, size) == 768);
static_assert(offsetof(aiosvd_map_arg, obj_order) == 776);
static_assert(offsetof(aiosvd_map_arg, flags) == 780);
static_assert(offsetof(aiosvd_map_arg, queue_depth) == 784);
static_assert(offsetof(aiosvd_map_arg, max_clients) == 788);
static_assert(offsetof(aiosvd_map_arg, dev_id) == 792);
static_assert(offsetof(aiosvd_map_arg, _pad) == 796);
static_assert(sizeof(aiosvd_unmap_arg) == 8);
static_assert(offsetof(aiosvd_unmap_arg, dev_id) == 0);
static_assert(offsetof(aiosvd_unmap_arg, _pad) == 4);
static_assert(sizeof(aiosvd_info_arg) == 416);
static_assert(offsetof(aiosvd_info_arg, dev_id) == 0);
static_assert(offsetof(aiosvd_info_arg, _pad) == 4);
static_assert(offsetof(aiosvd_info_arg, pool) == 8);
static_assert(offsetof(aiosvd_info_arg, name) == 72);
static_assert(offsetof(aiosvd_info_arg, parent_pool) == 136);
static_assert(offsetof(aiosvd_info_arg, parent_name) == 200);
static_assert(offsetof(aiosvd_info_arg, key_id) == 264);
static_assert(offsetof(aiosvd_info_arg, size) == 328);
static_assert(offsetof(aiosvd_info_arg, obj_order) == 336);
static_assert(offsetof(aiosvd_info_arg, flags) == 340);
static_assert(offsetof(aiosvd_info_arg, bytes_read) == 344);
static_assert(offsetof(aiosvd_info_arg, bytes_written) == 352);
static_assert(offsetof(aiosvd_info_arg, ops_read) == 360);
static_assert(offsetof(aiosvd_info_arg, ops_write) == 368);
static_assert(offsetof(aiosvd_info_arg, ops_discard) == 376);
static_assert(offsetof(aiosvd_info_arg, errors) == 384);
static_assert(offsetof(aiosvd_info_arg, timeouts) == 392);
static_assert(offsetof(aiosvd_info_arg, reconnects) == 400);
static_assert(offsetof(aiosvd_info_arg, cache_hits) == 408);
static_assert(sizeof(aiosvd_list_arg) == 13320);
static_assert(offsetof(aiosvd_list_arg, count) == 0);
static_assert(offsetof(aiosvd_list_arg, _pad) == 4);
static_assert(offsetof(aiosvd_list_arg, entries) == 8);
static_assert(sizeof(aiosvd_resize_arg) == 16);
static_assert(offsetof(aiosvd_resize_arg, dev_id) == 0);
static_assert(offsetof(aiosvd_resize_arg, _pad) == 4);
static_assert(offsetof(aiosvd_resize_arg, new_size) == 8);
static_assert(sizeof(aiosvd_clone_arg) == 144);
static_assert(offsetof(aiosvd_clone_arg, src_dev_id) == 0);
static_assert(offsetof(aiosvd_clone_arg, _pad) == 4);
static_assert(offsetof(aiosvd_clone_arg, pool) == 8);
static_assert(offsetof(aiosvd_clone_arg, name) == 72);
static_assert(offsetof(aiosvd_clone_arg, dest_dev_id) == 136);
static_assert(offsetof(aiosvd_clone_arg, _pad2) == 140);
static_assert(sizeof(aiosvd_rename_arg) == 136);
static_assert(offsetof(aiosvd_rename_arg, dev_id) == 0);
static_assert(offsetof(aiosvd_rename_arg, _pad) == 4);
static_assert(offsetof(aiosvd_rename_arg, pool) == 8);
static_assert(offsetof(aiosvd_rename_arg, name) == 72);

// Every struct crossing the boundary must be trivially copyable (memcpy'd
// through copy_to_user / copy_from_user).
static_assert(std::is_trivially_copyable_v<aios_kabi_req_hdr>);
static_assert(std::is_trivially_copyable_v<aios_kabi_rep_hdr>);
static_assert(std::is_trivially_copyable_v<aios_kabi_mount_in>);
static_assert(std::is_trivially_copyable_v<aios_kabi_mount_out>);
static_assert(std::is_trivially_copyable_v<aiosvd_map_arg>);
static_assert(std::is_trivially_copyable_v<aiosvd_list_arg>);

// ---- kbridge reply framing (KRN-8) ------------------------------------------

TEST(Review2Kabi, FrameReplyHeaderOnly) {
  const auto buf = aios_kbridge::frame_reply(42, -ENOENT, nullptr, 0);
  ASSERT_EQ(buf.size(), sizeof(aios_kabi_rep_hdr));
  aios_kabi_rep_hdr h{};
  std::memcpy(&h, buf.data(), sizeof(h));
  EXPECT_EQ(h.magic, AIOS_KABI_MAGIC);
  EXPECT_EQ(h.version, AIOS_KABI_VERSION);
  EXPECT_EQ(h.unique, 42u);
  EXPECT_EQ(h.result, -ENOENT);
  EXPECT_EQ(h.payload_len, 0u);
}

TEST(Review2Kabi, FrameReplyHeaderPlusPayloadContiguous) {
  aios_kabi_mount_out out{};
  out.mount_id = 7;
  out.root.ino = 1;
  out.root.mode = 040755;
  out.root.nlink = 2;
  out.root.size = 4096;

  const auto buf = aios_kbridge::frame_reply(99, 0, &out, sizeof(out));
  // Exactly what aios_dev_write requires: count == sizeof(hdr) + payload_len.
  ASSERT_EQ(buf.size(), sizeof(aios_kabi_rep_hdr) + sizeof(out));

  aios_kabi_rep_hdr h{};
  std::memcpy(&h, buf.data(), sizeof(h));
  EXPECT_EQ(h.magic, AIOS_KABI_MAGIC);
  EXPECT_EQ(h.version, AIOS_KABI_VERSION);
  EXPECT_EQ(h.unique, 99u);
  EXPECT_EQ(h.result, 0);
  EXPECT_EQ(h.payload_len, sizeof(out));

  aios_kabi_mount_out back{};
  std::memcpy(&back, buf.data() + sizeof(h), sizeof(back));
  EXPECT_EQ(std::memcmp(&back, &out, sizeof(out)), 0);
}

TEST(Review2Kabi, FrameReplyIgnoresPayloadPointerWhenLengthZero) {
  const char junk[8] = "ignored";
  const auto buf = aios_kbridge::frame_reply(1, 0, junk, 0);
  EXPECT_EQ(buf.size(), sizeof(aios_kabi_rep_hdr));
}

TEST(Review2Kabi, WriteReplyIsSingleWrite) {
  // A pipe records write boundaries: one read must return the whole frame.
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const std::string payload(1000, 'x');
  ASSERT_TRUE(aios_kbridge::write_reply(fds[1], 5, 0, payload.data(),
                                        static_cast<uint32_t>(payload.size())));
  std::vector<unsigned char> got(sizeof(aios_kabi_rep_hdr) + payload.size() + 64);
  const ssize_t n = read(fds[0], got.data(), got.size());
  ASSERT_EQ(n, static_cast<ssize_t>(sizeof(aios_kabi_rep_hdr) + payload.size()));
  aios_kabi_rep_hdr h{};
  std::memcpy(&h, got.data(), sizeof(h));
  EXPECT_EQ(h.unique, 5u);
  EXPECT_EQ(h.payload_len, payload.size());
  EXPECT_EQ(std::memcmp(got.data() + sizeof(h), payload.data(), payload.size()), 0);
  close(fds[0]);
  close(fds[1]);
}
