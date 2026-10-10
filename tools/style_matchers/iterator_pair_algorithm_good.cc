// Known-good input of iterator_pair_algorithm.query: the range form, and
// ranges that are not the whole of one container.
#include <algorithm>
#include <vector>

#include "absl/algorithm/container.h"

void Ranges(std::vector<int> &in, std::vector<int> &out) {
  absl::c_sort(in);
  std::sort(in.begin(), in.begin() + 2);
  std::copy(in.begin(), out.end(), in.begin());
}
