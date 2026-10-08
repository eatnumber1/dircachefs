// Fixture for operators_test.py (plan step 26.5d): one small function per
// operator case. The test compiles it with the pinned clang (-ast-dump=json,
// the command mutate.py uses on dcfs/) and runs the operators on the result.
// Nothing here is linked or run, so the absl and logging pieces are stubs
// with the names and types the operators look for.

#define LOG(severity) Stream()
#define CHECK(cond) CheckStream((cond))

namespace absl {

class Status {
 public:
  bool ok() const;
};

Status OkStatus();
Status InternalError(const char* message);

template <typename T>
class StatusOr {
 public:
  StatusOr(T value);
  StatusOr(Status status);
};

}  // namespace absl

struct Stream {
  Stream& operator<<(int value);
  Stream& operator<<(const char* value);
  Stream& operator<<(bool value);
};

struct CheckStream {
  explicit CheckStream(bool ok);
  CheckStream& operator<<(int value);
  CheckStream& operator<<(const char* value);
  CheckStream& operator<<(bool value);
};

enum class Kind { kFound, kNegative };

namespace dcfs {

struct Mutation {
  void End();
  absl::Status Mark(int id);
};

absl::Status BeginThing();
void Touch(int id);

bool Relational(int a, int b) { return a < b; }

bool Logical(bool a, bool b) { return a && b; }

int Negations(int a) {
  if (a) {
    return 1;
  }
  while (a) {
    --a;
  }
  for (int i = 0; i < 3; ++i) {
  }
  return a ? 1 : 2;
}

Kind Swaps() { return Kind::kFound; }

int Constants(int a) { return a + 7; }

int ConstantsOutsideArithmetic(int a) {
  int table[4] = {1, 2, 3, 4};
  return table[a];
}

void Statements(Mutation& m, int a) {
  m.End();
  a = 5;
  ++a;
  Touch(a);
  (void)m.Mark(a);
}

absl::Status Marks(Mutation& m) {
  absl::Status status = m.Mark(1);
  absl::Status begun = BeginThing();
  return status;
}

absl::Status ErrorOut(int a) { return absl::InternalError("boom"); }

absl::Status OkOut(int a) { return absl::OkStatus(); }

absl::StatusOr<int> ValueOut(int a) { return a; }

absl::StatusOr<int> StatusOut(absl::Status status) { return status; }

int Inner(int a) {
  auto lambda = [](int x) -> absl::Status { return absl::OkStatus(); };
  return a;
}

int TwoSame(int a, int b) { return a; }

int Swapping(int x, int y, long z) {
  int i = TwoSame(x, y);
  int j = TwoSame(x, x);
  long k = TwoSame(x, z);
  return i + j + static_cast<int>(k);
}

void Logging(int a, int b) {
  LOG(INFO) << "a is " << (a < b) << (a + 3);
  CHECK(a == b) << "differ " << (a != b);
}

struct Printable {
  int ToString() { return 4 + 1; }
  int DebugString() { return 6 * 2; }
  int AbslStringify() { return 8 - 1; }
  int Other() { return 3 - 1; }
};

absl::Status ErrorBuilder(const char* message);
Stream& Builder();

int Messages(int a) {
  Builder() << "count " << (a < 3);
  return a;
}

}  // namespace dcfs
