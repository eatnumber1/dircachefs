#ifndef DCFS_ATTRIBUTES_H_
#define DCFS_ATTRIBUTES_H_

#ifndef absl_nullable
//#define absl_nullable __attribute__((null))
#define absl_nullable
#endif  // absl_nullable

#ifndef absl_nonnull
//#define absl_nonnull __attribute__((nonnull))
#define absl_nonnull
#endif  // absl_nonnull

#endif  // DCFS_ATTRIBUTES_H_
