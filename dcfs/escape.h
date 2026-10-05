#ifndef DCFS_ESCAPE_H_
#define DCFS_ESCAPE_H_

#include <optional>
#include <string>
#include <string_view>

namespace dcfs {

// File names, symlink targets and xattr names are bytes (README.md, "File
// names are bytes"): any byte but NUL (and, for names, '/'). Never print one
// raw. A name containing a newline would forge a log line, and a terminal
// would obey its control characters.
//
// EscapeBytes is the one escaping for logs, error messages and any other
// text meant for people. The result is printable ASCII (0x20 to 0x7e) on one
// line: bytes 0x20 to 0x7e print as themselves, except that '\\' and '"'
// (so a caller can wrap the result in quotes) are backslash-escaped, and
// "\n", "\r" and "\t" use their usual letters. Every other byte, NUL
// included, becomes "\xNN" with two lowercase hex digits.
std::string EscapeBytes(std::string_view bytes);

// EscapeBytes, appending to `out` (no allocation of its own).
void AppendEscapedBytes(std::string_view bytes, std::string &out);

// The inverse of EscapeBytes: accepts exactly what EscapeBytes can produce,
// and returns nullopt for anything else (a lone backslash, an unknown
// escape, a bad hex digit, an unescaped byte EscapeBytes would have escaped).
std::optional<std::string> UnescapeBytes(std::string_view escaped);

// UnescapeBytes, appending to `out`; returns false (leaving `out` in an
// unspecified state) for malformed input.
bool AppendUnescapedBytes(std::string_view escaped, std::string &out);

// Phase 15 plugs in next to these: mountinfo, fstab and exports(5) text use
// octal escapes ("\040" for a space), which is a different format from the
// above and gets its own pair of functions here, not a mode of these. They do
// not exist yet.

}  // namespace dcfs

#endif  // DCFS_ESCAPE_H_
