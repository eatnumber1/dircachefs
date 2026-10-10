// Known-good input of .clang-tidy: no check of the configuration finds
// anything here.
namespace {

[[maybe_unused]] int ReturnsEarly(int x) {
  if (x > 0) {
    return 1;
  }
  return 2;
}

[[maybe_unused]] int Straight(int x) {
  if (x > 0) {
    x = 1;
  }
  x = 2;
  return x;
}

class Explicit {
 public:
  explicit Explicit(int value) : value_(value) {}
  [[nodiscard]] int Value() const { return value_; }

 private:
  int value_;
};

[[maybe_unused]] int UsesExplicit() { return Explicit(1).Value(); }

}  // namespace
