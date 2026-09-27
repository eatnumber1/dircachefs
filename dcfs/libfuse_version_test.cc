// Verifies that the patched libfuse (third_party/libfuse/0001-attr-generation.patch)
// is actually the one being linked: FUSE_CAP_ATTR_GENERATION must be defined
// and fuse_reply_attr_with_generation() must be a linkable symbol.
//
// Drop this test (and the patch) once a released libfuse includes the
// feature upstream; see the TODO next to the single_version_override in
// MODULE.bazel.

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include <fuse_lowlevel.h>

#include <gtest/gtest.h>

static_assert(FUSE_CAP_ATTR_GENERATION != 0,
              "FUSE_CAP_ATTR_GENERATION must be defined and non-zero; is "
              "third_party/libfuse/0001-attr-generation.patch applied?");

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

}  // namespace
