// Known-bad input of iterator_pair_algorithm.query.
#include <algorithm>
#include <vector>

void IteratorPairs(std::vector<int> &in, std::vector<int> &out) {
  std::sort(in.begin(), in.end());  // HIT
  std::copy(std::begin(in), std::end(in), out.begin());  // HIT
}
