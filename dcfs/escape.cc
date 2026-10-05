#include "dcfs/escape.h"

#include <optional>
#include <string>
#include <string_view>

namespace dcfs {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

// The value of a lowercase hex digit, or -1.
int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

}  // namespace

std::string EscapeBytes(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  AppendEscapedBytes(bytes, out);
  return out;
}

void AppendEscapedBytes(std::string_view bytes, std::string &out) {
  for (char ch : bytes) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c >= 0x20 && c <= 0x7e) {
          out += ch;
        } else {
          out += "\\x";
          out += kHexDigits[c >> 4];
          out += kHexDigits[c & 15];
        }
    }
  }
}

std::optional<std::string> UnescapeBytes(std::string_view escaped) {
  std::string out;
  out.reserve(escaped.size());
  if (!AppendUnescapedBytes(escaped, out)) return std::nullopt;
  return out;
}

bool AppendUnescapedBytes(std::string_view escaped, std::string &out) {
  for (size_t i = 0; i < escaped.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(escaped[i]);
    if (c != '\\') {
      // EscapeBytes never leaves these bytes bare.
      if (c < 0x20 || c > 0x7e || c == '"') return false;
      out += escaped[i];
      continue;
    }
    if (++i == escaped.size()) return false;
    switch (escaped[i]) {
      case '\\':
        out += '\\';
        break;
      case '"':
        out += '"';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'x': {
        if (escaped.size() - i < 3) return false;
        int hi = HexValue(escaped[i + 1]);
        int lo = HexValue(escaped[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
        break;
      }
      default:
        return false;
    }
  }
  return true;
}

}  // namespace dcfs
