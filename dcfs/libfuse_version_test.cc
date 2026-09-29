// Verifies that the patched libfuse (third_party/libfuse/0001-attr-generation.patch)
// is actually the one being linked: FUSE_CAP_ATTR_GENERATION must be defined
// and fuse_reply_attr_with_generation() must be a linkable symbol.
//
// Drop this test (and the patch) once a released libfuse includes the
// feature upstream; see the TODO next to the single_version_override in
// MODULE.bazel.

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <fuse_lowlevel.h>

// FUSE_ATTR_GENERATION (the wire-protocol INIT flag, as opposed to
// FUSE_CAP_ATTR_GENERATION, the application-facing capability bit) lives in
// this lower-level header, not fuse_lowlevel.h's public surface.
#include <fuse_kernel.h>

#include <gtest/gtest.h>

static_assert(FUSE_CAP_ATTR_GENERATION != 0,
              "FUSE_CAP_ATTR_GENERATION must be defined and non-zero; is "
              "third_party/libfuse/0001-attr-generation.patch applied?");

// Regression test for 36e5fa9: the patch originally carried
// FUSE_ATTR_GENERATION at a bit position from before the kernel's 7.46
// io_uring-bufpool rebase (which took bit 43 for
// FUSE_HAS_IO_URING_BUFPOOL and pushed FUSE_ATTR_GENERATION to bit 44 in
// protocol 7.47) -- so the *wire* INIT flag the patch set/checked did not
// match what a real 7.47 kernel sends/expects (see ~/Sources/linux's
// include/uapi/linux/fuse.h, a sibling checkout of the same kernel series
// this project tracks), silently miscommunicating the capability even
// though FUSE_CAP_ATTR_GENERATION (the unrelated application-facing
// capability bit above) stayed correct throughout. Pinned here as a
// literal, rather than by including the kernel's own uapi header (which
// lives in a separate git repository this Bazel workspace has no
// dependency on and cannot hermetically reference), so a future
// libfuse/patch bump can't silently drift the wire bit back without a
// build break; see 36e5fa9's commit message for the verification against
// a pristine libfuse 3.18.2 checkout and the kernel source.
static_assert(FUSE_ATTR_GENERATION == (1ULL << 44),
              "FUSE_ATTR_GENERATION must be bit 44 (protocol 7.47), "
              "matching the kernel's include/uapi/linux/fuse.h -- see "
              "third_party/libfuse/0001-attr-generation.patch");

namespace {

// Taking the address into a volatile function pointer forces the linker to
// resolve fuse_reply_attr_with_generation, so a missing symbol fails the
// build rather than silently linking without it.
int (*volatile kFuseReplyAttrWithGeneration)(
    fuse_req_t, const struct stat *, double, uint64_t) =
    &fuse_reply_attr_with_generation;

TEST(LibfuseVersionTest, AttrGenerationCapabilityIsDefined) {
  EXPECT_NE(FUSE_CAP_ATTR_GENERATION, 0u);
}

TEST(LibfuseVersionTest, FuseReplyAttrWithGenerationSymbolLinks) {
  EXPECT_NE(kFuseReplyAttrWithGeneration, nullptr);
}

TEST(LibfuseVersionTest, AttrGenerationWireBitIsProtocol747BitFortyFour) {
  EXPECT_EQ(FUSE_ATTR_GENERATION, (1ULL << 44));
}

}  // namespace
