namespace dcfs {

template <typename RawType, typename BaseClass>
class StrongNumber {
 public:
  using RawType = RawType;

  explicit StrongNumber(RawType value) : value_(value) {}

  auto operator<=>(const BaseClass&) const = default;

  RawType value() const { return value_; }

  template <typename H>
  friend H AbslHashValue(H h, const BaseClass &num) {
    return H::combine(std::move(h), num.value_);
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const BaseClass &num) {
    absl::Format(&sink, "%d", num.value_);
  }

 private:
  RawType value_;
};

}  // namespace dcfs
