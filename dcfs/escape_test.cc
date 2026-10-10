#include "dcfs/escape.h"

#include <optional>
#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace dcfs {
namespace {

using ::testing::Eq;
using ::testing::Optional;

std::string HexForMessage(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) {
    out += kDigits[c >> 4];
    out += kDigits[c & 15];
  }
  return out;
}

TEST(EscapeBytesTest, PlainTextIsUnchanged) {
  EXPECT_EQ(EscapeBytes(""), "");
  EXPECT_EQ(EscapeBytes("plain name.txt"), "plain name.txt");
  EXPECT_EQ(EscapeBytes("~!#$%&'()*+,-./:;<=>?@[]^_`{|}"),
            "~!#$%&'()*+,-./:;<=>?@[]^_`{|}");
}

TEST(EscapeBytesTest, SpecificEscapes) {
  EXPECT_EQ(EscapeBytes("a\nb"), "a\\nb");
  EXPECT_EQ(EscapeBytes("a\rb"), "a\\rb");
  EXPECT_EQ(EscapeBytes("a\tb"), "a\\tb");
  EXPECT_EQ(EscapeBytes("a\\b"), "a\\\\b");
  EXPECT_EQ(EscapeBytes("a\"b"), "a\\\"b");
  EXPECT_EQ(EscapeBytes("\x01"), "\\x01");
  EXPECT_EQ(EscapeBytes("\x1f"), "\\x1f");
  EXPECT_EQ(EscapeBytes("\x7f"), "\\x7f");
  EXPECT_EQ(EscapeBytes("\x80"), "\\x80");
  EXPECT_EQ(EscapeBytes("\xff"), "\\xff");
  // A name that already looks like an escape is escaped itself.
  EXPECT_EQ(EscapeBytes("\\n"), "\\\\n");
  EXPECT_EQ(EscapeBytes("\\x41"), "\\\\x41");
  // A NUL (never in a name, but in an xattr value) is an ordinary byte.
  EXPECT_EQ(EscapeBytes(std::string_view("a\0b", 3)), "a\\x00b");
  // Valid UTF-8 is escaped byte by byte: it is not text.
  EXPECT_EQ(EscapeBytes("caf\xc3\xa9"), "caf\\xc3\\xa9");
}

TEST(UnescapeBytesTest, Inverts) {
  EXPECT_THAT(UnescapeBytes("a\\nb\\\\\\x41\\xff\\\""),
              Optional(Eq("a\nb\\A\xff\"")));
  EXPECT_THAT(UnescapeBytes(""), Optional(Eq("")));
}

TEST(UnescapeBytesTest, RejectsMalformedInput) {
  EXPECT_EQ(UnescapeBytes("\\"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("abc\\"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("\\x"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("\\x4"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("\\xzz"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("\\q"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("\\040"), std::nullopt);
  // Raw bytes the escaper never emits.
  EXPECT_EQ(UnescapeBytes("a\nb"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("a\xff"), std::nullopt);
  EXPECT_EQ(UnescapeBytes("\""), std::nullopt);
  // Upper-case hex digits are not what the escaper emits either.
  EXPECT_EQ(UnescapeBytes("\\xFF"), std::nullopt);
}

// Every byte string of length 0 to 3: the escape is printable ASCII on one
// line, and unescaping gives the bytes back.
TEST(EscapeBytesTest, ExhaustiveRoundTripUpToLengthThree) {
  // Plain ifs, not ASSERT_*: 16.7 million strings, and an unoptimized build.
  bool ok = true;
  std::string escaped, back;
  auto check = [&](std::string_view bytes) {
    escaped.clear();
    AppendEscapedBytes(bytes, escaped);
    bool printable = true;
    for (unsigned char c : escaped) {
      if (c < 0x20 || c > 0x7e) printable = false;
    }
    back.clear();
    if (!printable || !AppendUnescapedBytes(escaped, back) || back != bytes) {
      if (ok) {
        ADD_FAILURE() << "does not round-trip: " << HexForMessage(bytes)
                      << " escapes to " << escaped;
      }
      ok = false;
    }
  };
  check("");
  char buf[3];
  for (int a = 0; a < 256; ++a) {
    buf[0] = static_cast<char>(a);
    check(std::string_view(buf, 1));
    for (int b = 0; b < 256; ++b) {
      buf[1] = static_cast<char>(b);
      check(std::string_view(buf, 2));
      for (int c = 0; c < 256; ++c) {
        buf[2] = static_cast<char>(c);
        check(std::string_view(buf, 3));
      }
    }
  }
}

}  // namespace
}  // namespace dcfs
