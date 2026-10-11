// SPDX-License-Identifier: MIT
#include "dcfs/fuse_ops.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "dcfs/protocol_events.h"

namespace dcfs {
namespace {

TEST(FuseOpsTest, OpNameAllOps) {
  EXPECT_EQ(OpName(events::Op::kOther), "Other");
  EXPECT_EQ(OpName(events::Op::kLookup), "Lookup");
  EXPECT_EQ(OpName(events::Op::kGetattr), "Getattr");
  EXPECT_EQ(OpName(events::Op::kSetattr), "Setattr");
  EXPECT_EQ(OpName(events::Op::kReadlink), "Readlink");
  EXPECT_EQ(OpName(events::Op::kMknod), "Mknod");
  EXPECT_EQ(OpName(events::Op::kMkdir), "Mkdir");
  EXPECT_EQ(OpName(events::Op::kUnlink), "Unlink");
  EXPECT_EQ(OpName(events::Op::kRmdir), "Rmdir");
  EXPECT_EQ(OpName(events::Op::kSymlink), "Symlink");
  EXPECT_EQ(OpName(events::Op::kRename), "Rename");
  EXPECT_EQ(OpName(events::Op::kLink), "Link");
  EXPECT_EQ(OpName(events::Op::kOpen), "Open");
  EXPECT_EQ(OpName(events::Op::kRead), "Read");
  EXPECT_EQ(OpName(events::Op::kWrite), "Write");
  EXPECT_EQ(OpName(events::Op::kFlush), "Flush");
  EXPECT_EQ(OpName(events::Op::kRelease), "Release");
  EXPECT_EQ(OpName(events::Op::kFsync), "Fsync");
  EXPECT_EQ(OpName(events::Op::kOpendir), "Opendir");
  EXPECT_EQ(OpName(events::Op::kReaddir), "Readdir");
  EXPECT_EQ(OpName(events::Op::kReaddirplus), "Readdirplus");
  EXPECT_EQ(OpName(events::Op::kReleasedir), "Releasedir");
  EXPECT_EQ(OpName(events::Op::kFsyncdir), "Fsyncdir");
  EXPECT_EQ(OpName(events::Op::kStatfs), "Statfs");
  EXPECT_EQ(OpName(events::Op::kSetxattr), "Setxattr");
  EXPECT_EQ(OpName(events::Op::kGetxattr), "Getxattr");
  EXPECT_EQ(OpName(events::Op::kListxattr), "Listxattr");
  EXPECT_EQ(OpName(events::Op::kRemovexattr), "Removexattr");
  EXPECT_EQ(OpName(events::Op::kAccess), "Access");
  EXPECT_EQ(OpName(events::Op::kCreate), "Create");
  EXPECT_EQ(OpName(events::Op::kFallocate), "Fallocate");
  EXPECT_EQ(OpName(events::Op::kCopyFileRange), "CopyFileRange");
  EXPECT_EQ(OpName(events::Op::kIoctl), "Ioctl");
  EXPECT_EQ(OpName(events::Op::kTmpfile), "Tmpfile");
  EXPECT_EQ(OpName(events::Op::kLinkTmpfile), "LinkTmpfile");
  EXPECT_EQ(OpName(events::Op::kForget), "Forget");
  EXPECT_EQ(OpName(events::Op::kBatchForget), "BatchForget");
}

TEST(FuseOpsTest, OpNameOutOfRange) {
  EXPECT_EQ(OpName(static_cast<events::Op>(-1)), "Other");
  EXPECT_EQ(OpName(static_cast<events::Op>(9999)), "Other");
}

TEST(FuseOpsTest, DescribeRequestWithoutNames) {
  events::Request req;
  req.op = events::Op::kLookup;
  req.ino = 1;
  EXPECT_EQ(DescribeRequest(req), "Lookup(ino=1)");

  req.op = events::Op::kGetattr;
  req.ino = 42;
  EXPECT_EQ(DescribeRequest(req), "Getattr(ino=42)");
}

TEST(FuseOpsTest, DescribeRequestWithName) {
  events::Request req;
  req.op = events::Op::kLookup;
  req.ino = 1;
  req.name = "child";
  EXPECT_EQ(DescribeRequest(req), "Lookup(ino=1, name=\"child\")");
}

TEST(FuseOpsTest, DescribeRequestWithNewName) {
  events::Request req;
  req.op = events::Op::kLink;
  req.ino = 10;
  req.newparent = 20;
  req.newname = "link_target";
  EXPECT_EQ(DescribeRequest(req),
            "Link(ino=10, newparent=20, newname=\"link_target\")");
}

TEST(FuseOpsTest, DescribeRequestWithNameAndNewName) {
  events::Request req;
  req.op = events::Op::kRename;
  req.ino = 2;
  req.name = "src";
  req.newparent = 3;
  req.newname = "dst";
  EXPECT_EQ(DescribeRequest(req),
            "Rename(ino=2, name=\"src\", newparent=3, newname=\"dst\")");
}

TEST(FuseOpsTest, DescribeRequestEscaping) {
  events::Request req;
  req.op = events::Op::kCreate;
  req.ino = 5;
  req.name = "hello\n\"world\"\\and\ttabs\x01\xff";
  EXPECT_EQ(
      DescribeRequest(req),
      "Create(ino=5, name=\"hello\\n\\\"world\\\"\\\\and\\ttabs\\x01\\xff\")");

  static constexpr char kNewName[] = {'n', 'e', 'w', '\0', 'n', 'a', 'm', 'e'};
  req.op = events::Op::kRename;
  req.name = "old\rname";
  req.newparent = 6;
  req.newname = std::string_view(kNewName, sizeof(kNewName));
  EXPECT_EQ(DescribeRequest(req),
            "Rename(ino=5, name=\"old\\rname\", newparent=6, "
            "newname=\"new\\x00name\")");
}

}  // namespace
}  // namespace dcfs
