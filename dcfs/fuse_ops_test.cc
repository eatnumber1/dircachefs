// SPDX-License-Identifier: MIT
#include "dcfs/fuse_ops.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "dcfs/protocol_events.h"
#include "fuse_lowlevel.h"

namespace dcfs {
namespace {

TEST(FuseOpsTest, MakeFuseOpsPopulatesHandlers) {
  const fuse_lowlevel_ops ops = MakeFuseOps();
  EXPECT_NE(ops.lookup, nullptr);
  EXPECT_NE(ops.getattr, nullptr);
  EXPECT_NE(ops.open, nullptr);
}

TEST(FuseOpsTest, OpAbslStringifyAllOps) {
  EXPECT_EQ(absl::StrCat(events::Op::kOther), "Other");
  EXPECT_EQ(absl::StrCat(events::Op::kLookup), "Lookup");
  EXPECT_EQ(absl::StrCat(events::Op::kGetattr), "Getattr");
  EXPECT_EQ(absl::StrCat(events::Op::kSetattr), "Setattr");
  EXPECT_EQ(absl::StrCat(events::Op::kReadlink), "Readlink");
  EXPECT_EQ(absl::StrCat(events::Op::kMknod), "Mknod");
  EXPECT_EQ(absl::StrCat(events::Op::kMkdir), "Mkdir");
  EXPECT_EQ(absl::StrCat(events::Op::kUnlink), "Unlink");
  EXPECT_EQ(absl::StrCat(events::Op::kRmdir), "Rmdir");
  EXPECT_EQ(absl::StrCat(events::Op::kSymlink), "Symlink");
  EXPECT_EQ(absl::StrCat(events::Op::kRename), "Rename");
  EXPECT_EQ(absl::StrCat(events::Op::kLink), "Link");
  EXPECT_EQ(absl::StrCat(events::Op::kOpen), "Open");
  EXPECT_EQ(absl::StrCat(events::Op::kRead), "Read");
  EXPECT_EQ(absl::StrCat(events::Op::kWrite), "Write");
  EXPECT_EQ(absl::StrCat(events::Op::kFlush), "Flush");
  EXPECT_EQ(absl::StrCat(events::Op::kRelease), "Release");
  EXPECT_EQ(absl::StrCat(events::Op::kFsync), "Fsync");
  EXPECT_EQ(absl::StrCat(events::Op::kOpendir), "Opendir");
  EXPECT_EQ(absl::StrCat(events::Op::kReaddir), "Readdir");
  EXPECT_EQ(absl::StrCat(events::Op::kReaddirplus), "Readdirplus");
  EXPECT_EQ(absl::StrCat(events::Op::kReleasedir), "Releasedir");
  EXPECT_EQ(absl::StrCat(events::Op::kFsyncdir), "Fsyncdir");
  EXPECT_EQ(absl::StrCat(events::Op::kStatfs), "Statfs");
  EXPECT_EQ(absl::StrCat(events::Op::kSetxattr), "Setxattr");
  EXPECT_EQ(absl::StrCat(events::Op::kGetxattr), "Getxattr");
  EXPECT_EQ(absl::StrCat(events::Op::kListxattr), "Listxattr");
  EXPECT_EQ(absl::StrCat(events::Op::kRemovexattr), "Removexattr");
  EXPECT_EQ(absl::StrCat(events::Op::kAccess), "Access");
  EXPECT_EQ(absl::StrCat(events::Op::kCreate), "Create");
  EXPECT_EQ(absl::StrCat(events::Op::kFallocate), "Fallocate");
  EXPECT_EQ(absl::StrCat(events::Op::kCopyFileRange), "CopyFileRange");
  EXPECT_EQ(absl::StrCat(events::Op::kIoctl), "Ioctl");
  EXPECT_EQ(absl::StrCat(events::Op::kTmpfile), "Tmpfile");
  EXPECT_EQ(absl::StrCat(events::Op::kLinkTmpfile), "LinkTmpfile");
  EXPECT_EQ(absl::StrCat(events::Op::kForget), "Forget");
  EXPECT_EQ(absl::StrCat(events::Op::kBatchForget), "BatchForget");
}

TEST(FuseOpsTest, OpAbslStringifyOutOfRange) {
  EXPECT_EQ(absl::StrCat(static_cast<events::Op>(-1)), "Other");
  EXPECT_EQ(absl::StrCat(static_cast<events::Op>(9999)), "Other");
}

TEST(FuseOpsTest, RequestAbslStringifyWithoutNames) {
  events::Request req;
  req.op = events::Op::kLookup;
  req.ino = 1;
  EXPECT_EQ(absl::StrCat(req), "Lookup(ino=1)");

  req.op = events::Op::kGetattr;
  req.ino = 42;
  EXPECT_EQ(absl::StrCat(req), "Getattr(ino=42)");
}

TEST(FuseOpsTest, RequestAbslStringifyWithName) {
  events::Request req;
  req.op = events::Op::kLookup;
  req.ino = 1;
  req.name = "child";
  EXPECT_EQ(absl::StrCat(req), "Lookup(ino=1, name=\"child\")");
}

TEST(FuseOpsTest, RequestAbslStringifyWithNewName) {
  events::Request req;
  req.op = events::Op::kLink;
  req.ino = 10;
  req.newparent = 20;
  req.newname = "link_target";
  EXPECT_EQ(absl::StrCat(req),
            "Link(ino=10, newparent=20, newname=\"link_target\")");
}

TEST(FuseOpsTest, RequestAbslStringifyWithNameAndNewName) {
  events::Request req;
  req.op = events::Op::kRename;
  req.ino = 2;
  req.name = "src";
  req.newparent = 3;
  req.newname = "dst";
  EXPECT_EQ(absl::StrCat(req),
            "Rename(ino=2, name=\"src\", newparent=3, newname=\"dst\")");
}

TEST(FuseOpsTest, RequestAbslStringifyEscaping) {
  events::Request req;
  req.op = events::Op::kCreate;
  req.ino = 5;
  req.name = "hello\n\"world\"\\and\ttabs\x01\xff";
  EXPECT_EQ(
      absl::StrCat(req),
      "Create(ino=5, name=\"hello\\n\\\"world\\\"\\\\and\\ttabs\\x01\\xff\")");

  static constexpr char kNewName[] = {'n', 'e', 'w', '\0', 'n', 'a', 'm', 'e'};
  req.op = events::Op::kRename;
  req.name = "old\rname";
  req.newparent = 6;
  req.newname = std::string_view(kNewName, sizeof(kNewName));
  EXPECT_EQ(absl::StrCat(req),
            "Rename(ino=5, name=\"old\\rname\", newparent=6, "
            "newname=\"new\\x00name\")");
}

}  // namespace
}  // namespace dcfs
