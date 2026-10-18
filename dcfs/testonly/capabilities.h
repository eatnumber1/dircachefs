// SPDX-License-Identifier: MIT
#ifndef DCFS_TESTONLY_CAPABILITIES_H_
#define DCFS_TESTONLY_CAPABILITIES_H_

#include <linux/capability.h>

#include "dcfs/capability_set.h"

namespace dcfs::testonly {

// `set` without capability `cap`: what a test that drops one capability
// hands to capset. dcfs itself never removes one, so CapabilitySet has no
// way to.
inline CapabilitySet Without(CapabilitySet set, int cap) {
  CapabilitySet without;
  for (int c = 0; c <= CAP_LAST_CAP; ++c) {
    if (c != cap && set.Has(c)) without.Add(c);
  }
  return without;
}

}  // namespace dcfs::testonly

#endif  // DCFS_TESTONLY_CAPABILITIES_H_
